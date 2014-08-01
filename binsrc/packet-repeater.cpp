/*
    packet repeater
*/
#include <cybozu/option.hpp>
#include <cybozu/socket.hpp>
#include <cybozu/log.hpp>
#include <cybozu/time.hpp>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>
#include "sma.hpp"

std::atomic<int> g_quit;
std::atomic<bool> g_stop;

struct Option {
    std::string serverAddr;
    uint16_t serverPort;
    uint16_t recvPort;
    uint16_t cmdPort;
    uint32_t delaySec;
    double rateMbps;
    size_t threadNum;
    bool verbose;
    Option(int argc, char *argv[])
        : serverPort(0)
        , cmdPort(0)
        , delaySec(0)
        , rateMbps(0)
        , threadNum(0)
        , verbose(false)
    {
        cybozu::SetLogPriority(cybozu::LogInfo);
        cybozu::Option opt;
        bool vv = false;
        std::string logPath;
        opt.appendParam(&serverAddr, "server", ": server address");
        opt.appendParam(&serverPort, "port", ": server port");
        opt.appendParam(&recvPort, "recvPort", ": port to receive");
        opt.appendParam(&cmdPort, "cmdPort", ": port for command");
        opt.appendOpt(&delaySec, 0, "d", ": delay second");
        opt.appendOpt(&rateMbps, 0, "r", ": data rate(mega bit per second)");
        opt.appendOpt(&threadNum, 10, "t", ": num of thread");
        opt.appendOpt(&logPath, "-", "l", ": log path (default stderr)");
        opt.appendBoolOpt(&verbose, "v", ": verbose message");
        opt.appendBoolOpt(&vv, "vv", ": more verbose message");
        opt.appendHelp("h");
        if (!opt.parse(argc, argv)) {
            opt.usage();
            exit(1);
        }
        if (vv) cybozu::SetLogPriority(cybozu::LogDebug);
        if (logPath == "-") {
            cybozu::SetLogFILE(::stderr);
        } else {
            cybozu::OpenLogFile(logPath);
        }
        opt.put();
    }
};

class ThreadRunner
{
    std::thread thread_;
public:
    void set(std::thread&& thread) {
        thread_ = std::move(thread);
    }
    ~ThreadRunner() noexcept try {
        join();
    } catch (std::exception& e) {
        cybozu::PutLog(cybozu::LogError, "ThreadRunner: error: %s", e.what());
    } catch (...) {
        cybozu::PutLog(cybozu::LogError, "ThreadRunner: unknown error");
    }
    void join() {
        g_quit = true;
        if (thread_.joinable()) thread_.join();
    }
};

void cmdThread(const Option& opt)
    try
{
    if (opt.verbose) cybozu::PutLog(cybozu::LogInfo, "cmdThread start port=%d", opt.cmdPort);
    cybozu::Socket server;
    server.bind(opt.cmdPort);
    while (!g_quit) {
        while (!server.queryAccept()) {
            if (g_quit) break;
        }
        if (g_quit) break;
        try {
            cybozu::SocketAddr addr;
            cybozu::Socket client;
            server.accept(client, &addr);
            if (opt.verbose) cybozu::PutLog(cybozu::LogInfo, "cmdThread accept addr %s", addr.toStr().c_str());
            char buf[128];
            size_t readSize = client.readSome(buf, sizeof(buf));
            if (readSize > 0) {
                if (buf[readSize - 1] == '\n') readSize--;
                if (readSize > 0 && buf[readSize - 1] == '\r') readSize--;
                const std::string cmd(buf, readSize);
                if (cmd == "quit") {
                    if (opt.verbose) cybozu::PutLog(cybozu::LogInfo, "cmdThread quit");
                    g_quit = true;
                } else
                if (cmd == "stop") {
                    if (opt.verbose) cybozu::PutLog(cybozu::LogInfo, "cmdThread stop");
                    g_stop = true;
                } else
                if (cmd == "start") {
                    if (opt.verbose) cybozu::PutLog(cybozu::LogInfo, "cmdThread start");
                    g_stop = false;
                } else
                {
                    if (opt.verbose) cybozu::PutLog(cybozu::LogInfo, "bad command `%s'", cmd.c_str());
                }
            }
            const char ack = 'a';
            client.write(&ack, 1);
        } catch (std::exception& e) {
            cybozu::PutLog(cybozu::LogInfo, "cmdThread ERR %s (continue)", e.what());
        }
    }
    if (opt.verbose) cybozu::PutLog(cybozu::LogInfo, "cmdThread stop");
} catch (std::exception& e) {
    cybozu::PutLog(cybozu::LogInfo, "cmdThread ERR %s", e.what());
}

void waitMsec(int msec)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(msec));
}

class Repeater {
    cybozu::Socket s_[2]; // s_[0] : client, s_[1] : server
    enum {
        Sleep,
        Ready,
        Running,
        Error0,
        Error1,
        Closing0,
        Closing1,
        Close0,
        Close1,
    };
    const Option& opt_;
    std::atomic<int> state_;
    ThreadRunner threadRunner_[2];
    std::exception_ptr ep_[2];
    void loop(int dir)
        try
    {
        if (opt_.verbose) cybozu::PutLog(cybozu::LogInfo, "loop %d start", dir);
        assert(dir == 0 || dir == 1);
        cybozu::Socket &from = s_[dir];
        cybozu::Socket &to = s_[1 - dir];
        const int intervalSec = 3;
        SMAverage sma(intervalSec);
        std::vector<char> buf(1024);
        while (!g_quit) {
            switch ((int)state_) {
            case Sleep:
                waitMsec(10);
                continue;
            case Ready:
                waitMsec(1);
                continue;
            case Error0:
                handleError(dir == 0, dir, from);
                continue;
            case Error1:
                handleError(dir == 1, dir, from);
                continue;
            case Closing0:
                handleClosing(dir == 0, dir, from, to, buf, sma);
                continue;
            case Closing1:
                handleClosing(dir == 1, dir, from, to, buf, sma);
                continue;
            case Close0:
                handleClose(dir == 0, dir, from);
                continue;
            case Close1:
                handleClose(dir == 1, dir, from);
                continue;
            case Running:
                if (!from.isValid()) {
                    cybozu::PutLog(cybozu::LogInfo, "loop %d %d from is not valid", dir, (int)state_);
                    changeStateToError(dir);
                    continue;
                }
                try {
                    while (!from.queryAccept()) {
                    }
                    if (g_quit) continue;
                    if (readAndWrite(dir, from, to, buf, sma) > 0) continue;
                    if (changeStateToClosing(dir)) shutdown(dir, to);
                } catch (std::exception& e) {
                    cybozu::PutLog(cybozu::LogInfo, "loop %d %d ERR %s", dir, (int)state_, e.what());
                    from.close();
                    changeStateToError(dir);
                }
            }
        }
        if (opt_.verbose) cybozu::PutLog(cybozu::LogInfo, "loop %d end", dir);
    } catch (...) {
        ep_[dir] = std::current_exception();
        s_[0].close();
        s_[1].close();
        state_ = Sleep;
    }
    bool changeStateToClosing(int dir) {
        int expected = Running;
        const int after = dir == 0 ? Closing0 : Closing1;
        const bool ret = state_.compare_exchange_strong(expected, after);
        if (!ret && opt_.verbose) {
            cybozu::PutLog(cybozu::LogInfo, "changeStateToClosing failed %d %d", dir, expected);
        }
        return ret;
    }
    void shutdown(int dir, cybozu::Socket& to) {
        if (opt_.verbose) cybozu::PutLog(cybozu::LogInfo, "shutdown %d", dir);
        const bool dontThrow = true;
        to.shutdown(1, dontThrow); // write disallow
    }
    bool changeStateToError(int dir) {
        int expected = Running;
        const int after = dir == 0 ? Error0 : Error1;
        const bool ret = state_.compare_exchange_strong(expected, after);
        if (!ret && opt_.verbose) {
            cybozu::PutLog(cybozu::LogInfo, "changeStateToError failed %d %d", dir, expected);
        }
        return ret;
    }
    void handleError(bool doesSetError, int dir, cybozu::Socket& from) {
        if (doesSetError) {
            assert(!from.isValid());
            waitMsec(1);
        } else {
            if (opt_.verbose) cybozu::PutLog(cybozu::LogInfo, "handleError %d %d", dir, (int)state_);
            from.close();
            state_ = Sleep;
        }
    }
    void handleClosing(bool doesSetClose, int dir, cybozu::Socket& from, cybozu::Socket& to, std::vector<char>& buf, SMAverage& sma) {
        if (doesSetClose) {
            waitMsec(1);
            return;
        }
        if (readAndWrite(dir, from, to, buf, sma) > 0) return;
        if (opt_.verbose) cybozu::PutLog(cybozu::LogInfo, "handleClosing %d %d", dir, (int)state_);
        from.close();
        state_ = dir == 1 ? Close0 : Close1;
    }
    void handleClose(bool doesSetClose, int dir, cybozu::Socket& from) {
        if (!doesSetClose) {
            waitMsec(1);
            return;
        }
        if (opt_.verbose) cybozu::PutLog(cybozu::LogInfo, "handleClose %d %d", dir, (int)state_);
        from.close();
        state_ = Sleep;
    }
    size_t readAndWrite(int dir, cybozu::Socket& from, cybozu::Socket& to, std::vector<char>& buf, SMAverage& sma) {
        const size_t readSize = from.readSome(buf.data(), buf.size());
        if (opt_.verbose) cybozu::PutLog(cybozu::LogDebug, "loop %d %d readSize %d", dir, (int)state_, (int)readSize);
        if (opt_.rateMbps > 0) {
            sma.append(readSize, cybozu::GetCurrentTimeSec());
            while (const double rate = sma.getBps(cybozu::GetCurrentTimeSec()) > opt_.rateMbps * 1e6) {
                if (opt_.verbose) cybozu::PutLog(cybozu::LogDebug, "loop %d %d rate %f", dir, (int)state_, rate);
                waitMsec(1);
            }
        }
        if (readSize == 0) return 0;
        if (!g_stop && to.isValid()) {
            if (opt_.delaySec) {
                waitMsec(opt_.delaySec * 1000);
            }
            to.write(buf.data(), readSize);
        }
        return readSize;
    }
public:
    int getState() const { return state_; }
    Repeater(const Option& opt)
        : opt_(opt)
        , state_(Sleep)
        , threadRunner_()
    {
        for (size_t i = 0; i < 2; i++) {
            threadRunner_[i].set(std::thread(&Repeater::loop, this, i));
        }
    }
    ~Repeater() noexcept {
        join();
    }
    bool tryAndRun(cybozu::Socket& client)
    {
        int expected = Sleep;
        if (!state_.compare_exchange_strong(expected, Ready)) return false;
        if (opt_.verbose) cybozu::PutLog(cybozu::LogInfo, "tryAndRun:in");
        try {
            s_[0].moveFrom(client);
            s_[1].connect(opt_.serverAddr, opt_.serverPort);
            state_ = Running;
            return true;
        } catch (std::exception& e) {
            cybozu::PutLog(cybozu::LogInfo, "tryAndRun::connect err %s", e.what());
            s_[0].close();
            s_[1].close();
            state_ = Sleep;
            return true;
        }
    }
    void join() noexcept {
        for (size_t i = 0; i < 2; i++) {
            try {
                threadRunner_[i].join();
                if (ep_[i]) std::rethrow_exception(ep_[i]);
            } catch (std::exception& e) {
                cybozu::PutLog(cybozu::LogError, "Repeater::join:error: %s", e.what());
            } catch (...) {
                cybozu::PutLog(cybozu::LogError, "Repeater::join:unknow error");
            }
        }
    }
};

int main(int argc, char *argv[]) try
{
    const Option opt(argc, argv);
    cybozu::Socket server;
    server.bind(opt.recvPort);
    ThreadRunner cmdRunner;
    cmdRunner.set(std::thread(cmdThread, opt));
    std::vector<std::unique_ptr<Repeater>> worker;
    try {
        for (size_t i = 0; i < opt.threadNum; i++) {
            worker.emplace_back(new Repeater(opt));
        }
        for (;;) {
    RETRY:
            while (!g_quit && !server.queryAccept()) {
#if 0
                if (opt.verbose) {
                    printf("worker state ");
                    for (size_t i = 0; i < opt.threadNum; i++) {
                        printf("%d ", worker[i]->getState());
                    }
                    printf("\n");
                }
#endif
            }
            if (g_quit) break;
            cybozu::SocketAddr addr;
            cybozu::Socket client;
            server.accept(client, &addr);
            if (opt.verbose) cybozu::PutLog(cybozu::LogInfo, "accept addr %s", addr.toStr().c_str());
            while (!g_quit) {
                for (size_t i = 0; i < opt.threadNum; i++) {
//                    if (opt.verbose) fprintf(stderr, "worker[%d] state=%d", (int)i, worker[i]->getState());
                    if (worker[i]->tryAndRun(client)) {
                        if (opt.verbose) cybozu::PutLog(cybozu::LogInfo, "start %d repeater", (int)i);
                        goto RETRY;
                    }
                }
                waitMsec(100);
            }
            waitMsec(100);
        }
    } catch (std::exception& e) {
        cybozu::PutLog(cybozu::LogError, "ERR %s", e.what());
    }
    if (opt.verbose) puts("main end");
} catch (std::exception& e) {
    cybozu::PutLog(cybozu::LogError, "error: %s", e.what());
    return 1;
} catch (...) {
    cybozu::PutLog(cybozu::LogError, "unknown error");
    return 1;
}

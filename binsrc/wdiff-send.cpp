/**
 * @file
 * @brief To send wlog to a proxy.
 * @author
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include <stdexcept>
#include <cstdio>
#include <time.h>
#include "cybozu/option.hpp"
#include "cybozu/socket.hpp"
#include "cybozu/atoi.hpp"
#include "cybozu/log.hpp"
#include "protocol.hpp"
#include "meta.hpp"
#include "file_path.hpp"
#include "time.hpp"
#include "net_util.hpp"
#include "meta.hpp"
#include "walb_log_file.hpp"
#include "walb_log_net.hpp"

struct Option : cybozu::Option
{
    std::string serverHostPort;
    std::string name;
    uint64_t gid;
    std::vector<std::string> wdiffPathV;
    std::string clientId;
    bool canNotMerge;
    std::string timeStampStr;

    Option() {
        appendMust(&serverHostPort, "server", "server host:port");
        appendMust(&name, "name", "volume identifier");
        appendOpt(&gid, 0, "gid", "begin gid.");
        appendParamVec(&wdiffPathV, "wdiff_path_list", "wdiff path list");
        std::string hostName = cybozu::net::getHostName();
        appendOpt(&clientId, hostName, "id", "client identifier");
        appendBoolOpt(&canNotMerge, "m", "clear canMerge flag.");
        appendOpt(&timeStampStr, "", "t", "timestamp in YYYYmmddHHMMSS format.");
        appendHelp("h");
    }
};

void sendWdiff(cybozu::Socket &sock, const std::string &clientId,
              const std::string &name, int wdiffFd, walb::MetaDiff &diff)
{
    std::string diffFileName = createDiffFileName(diff);
    LOGi("try to send %s...", diffFileName.c_str());

    walb::diff::Reader reader(wdiffFd);

    std::string serverId = walb::protocol::run1stNegotiateAsClient(
        sock, clientId, "wdiff-send");
    walb::ProtocolLogger logger(clientId, serverId);
    std::atomic<bool> forceQuit(false);

    walb::diff::FileHeaderRaw fileH;
    reader.readHeader(fileH);

    /* wdiff-send negotiation */
	walb::packet::Packet packet(sock);
	packet.write(name);
	packet.write(diff);

    /* Send diff packs. */

	walb::packet::StreamControl ctrl(sock);
	walb::diff::PackHeader packH;
    while (reader.readPackHeader(packH)) {
		ctrl.next();
		sock.write(packH.rawData(), packH.rawSize());
        for (size_t i = 0; i < packH.nRecords(); i++) {
			const walb::diff::RecordWrapConst rec(&packH.record(i));
			walb::diff::IoData io;
			reader.readDiffIo(rec, io);
			if (rec.dataSize() > 0) {
				sock.write(io.rawData(), rec.dataSize());
			}
        }
	}
	ctrl.end();

    /* The wdiff-send protocol has finished.
       You can close the socket. */
};

int main(int argc, char *argv[])
try {
    cybozu::SetLogFILE(::stderr);

    Option opt;
    if (!opt.parse(argc, argv)) {
        opt.usage();
        throw std::runtime_error("option error.");
    }
    std::string host;
    uint16_t port;
    std::tie(host, port) = cybozu::net::parseHostPortStr(opt.serverHostPort);

    uint64_t ts = ::time(0);
    if (!opt.timeStampStr.empty()) {
        ts = cybozu::strToUnixTime(opt.timeStampStr);
    }

    uint64_t gid = opt.gid;
    for (const std::string &wdiffPath : opt.wdiffPathV) {
        cybozu::util::FileOpener fo(wdiffPath, O_RDONLY);
        walb::MetaDiff diff;
        diff.init();
        diff.setSnap0(gid);
        diff.setSnap1(gid + 1);
        diff.setTimestamp(ts);
        diff.setCanMerge(!opt.canNotMerge);
        cybozu::Socket sock;
        sock.connect(host, port);
        sendWdiff(sock, opt.clientId, opt.name, fo.fd(), diff);
        gid++;
    }
    return 0;
} catch (std::exception &e) {
    ::fprintf(::stderr, "exception: %s\n", e.what());
    return 1;
} catch (...) {
    ::fprintf(::stderr, "caught an other error.\n");
    return 1;
}

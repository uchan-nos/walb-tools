/**
 * @file
 * @brief WalB log pretty printer.
 * @author HOSHINO Takashi
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include "cybozu/option.hpp"
#include "walb_logger.hpp"
#include "util.hpp"
#include "fileio.hpp"
#include "walb_log_file.hpp"
#include "aio_util.hpp"
#include "linux/walb/walb.h"
#include "walb_util.hpp"

using namespace walb;

/**
 * Command line configuration.
 */
struct Option
{
    std::string inWlogPath;
    uint64_t beginLsid;
    uint64_t endLsid;
    bool showHead, showPack, showStat, showNone;
    bool doValidate;
    bool isDebug;

    Option(int argc, char* argv[])
        : inWlogPath("-")
        , beginLsid(0)
        , endLsid(-1)
        , showHead(false)
        , showPack(false)
        , showStat(false)
        , isDebug(false) {

        cybozu::Option opt;
        opt.setDescription("wlog-show: pretty-print wlog input.");
        opt.appendOpt(&beginLsid, 0, "b", "LSID: begin lsid. (default: 0)");
        opt.appendOpt(&endLsid, uint64_t(-1), "e", "LSID: end lsid. (default: 0xffffffffffffffff)");
        opt.appendParamOpt(&inWlogPath, "-", "WLOG_PATH", ": input wlog path. '-' for stdin. (default: '-')");
        opt.appendBoolOpt(&showHead, "head", ": show file header.");
        opt.appendBoolOpt(&showPack, "pack", ": show packs.");
        opt.appendBoolOpt(&showStat, "stat", ": show statistics.");
        opt.appendBoolOpt(&showNone, "none", ": show nothing.");
        opt.appendBoolOpt(&doValidate, "validate", ": validate each IO checksum.");
        opt.appendBoolOpt(&isDebug, "debug", ": put debug messages to stderr.");
        opt.appendHelp("h", ": show this message.");
        if (!opt.parse(argc, argv)) {
            opt.usage();
            ::exit(1);
        }

        if (endLsid <= beginLsid) {
            throw RT_ERR("beginLsid must be < endLsid.");
        }

        // In default, show all.
        if (!showHead && !showPack && !showStat) {
            showHead = showPack = showStat = true;
        }
        if (showNone) {
            showHead = showPack = showStat = false;
        }
    }
    bool isInputStdin() const { return inWlogPath == "-"; }
};

void setupInputFile(LogFile &fileR, const Option &opt)
{
    if (opt.isInputStdin()) {
        fileR.setFd(0);
        fileR.setSeekable(false);
    } else {
        fileR.open(opt.inWlogPath, O_RDONLY);
        fileR.setSeekable(true);
    }
}

void validateAndPrintLogPackIos(const LogPackHeader &packH, std::queue<AlignedArray>& ioQ)
{
    for (size_t i = 0; i < packH.nRecords(); i++) {
        const WlogRecord &rec = packH.record(i);

        if (!rec.isExist()) {
            throw cybozu::Exception("validateAndPrintLogPackIos")
                << "exist flag not set" << rec;
        }
        if (rec.isDiscard()) continue;
        if (rec.isPadding()) {
            ioQ.pop();
            continue;
        }

        const AlignedArray &buf = ioQ.front();
        const uint32_t csum = cybozu::util::calcChecksum(buf.data(), rec.io_size * LBS, packH.salt());
        if (rec.checksum == csum) {
            LOGs.debug() << "OK" << rec;
        } else {
            LOGs.error() << "NG" << rec << cybozu::util::intToHexStr(csum);
        }
        ioQ.pop();
    }
}

int doMain(int argc, char* argv[])
{
    Option opt(argc, argv);
    util::setLogSetting("-", opt.isDebug);

    LogFile fileR;
    setupInputFile(fileR, opt);

    WlogFileHeader wh;
    wh.readFrom(fileR);
    if (opt.showHead) std::cout << wh.str() << std::endl;
    uint64_t lsid = wh.beginLsid();

    LogStatistics logStat;
    logStat.init(wh.beginLsid(), wh.endLsid());
    LogPackHeader packH(wh.pbs(), wh.salt());
    while (readLogPackHeader(fileR, packH, lsid)) {
        if (opt.showPack) std::cout << packH << std::endl;
        if (opt.doValidate) {
            std::queue<AlignedArray> ioQ;
            readAllLogIos(fileR, packH, ioQ, false);
            validateAndPrintLogPackIos(packH, ioQ);
        } else {
            skipAllLogIos(fileR, packH);
        }
        if (opt.showStat) logStat.update(packH);
        lsid = packH.nextLogpackLsid();
    }

    if (opt.showStat) std::cout << logStat << std::endl;
    return 0;
}

DEFINE_ERROR_SAFE_MAIN("wlog-show")

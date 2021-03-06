#pragma once
#include "walb_logger.hpp"
#include "util.hpp"
#include "fileio.hpp"
#include "bdev_util.hpp"
#include "walb_log_file.hpp"
#include "aio_util.hpp"
#include "linux/walb/walb.h"
#include "walb_util.hpp"
#include "bdev_writer.hpp"

namespace walb {

struct WlogRedoConfig
{
    std::string ddevPath;
    bool isVerbose;
    bool isDiscard;
    bool isZeroDiscard;

    uint32_t pbs;
    uint32_t salt;
    uint64_t bgnLsid;

    bool doShrink;
    bool doSkipCsum;
};

template <typename BdevWriter>
class WlogApplyer
{
private:
    const WlogRedoConfig &cfg_;
    cybozu::util::File ddevFile_;
    BdevWriter ddevWriter_;
    LogPackHeader packH_;

    struct Stat {
        uint64_t normalLb;
        uint64_t discardLb;
        uint64_t allZeroLb;
        uint64_t ignoredLb;
        uint64_t paddingPb;

        Stat() {
            normalLb = 0;
            discardLb = 0;
            allZeroLb = 0;
            ignoredLb = 0;
            paddingPb = 0;
        }
        void print(::FILE *fp = ::stdout) const {
            ::fprintf(fp,
                      "normal:  %10" PRIu64 " LB\n"
                      "discard: %10" PRIu64 " LB\n"
                      "allZero: %10" PRIu64 " LB\n"
                      "ignored: %10" PRIu64 " LB\n"
                      "padding: %10" PRIu64 " PB\n"
                      , normalLb
                      , discardLb
                      , allZeroLb
                      , ignoredLb
                      , paddingPb);
        }
    } stat_;

public:
    explicit WlogApplyer(const WlogRedoConfig &cfg)
        : cfg_(cfg)
        , ddevFile_(cfg_.ddevPath, O_RDWR | O_DIRECT)
        , ddevWriter_(ddevFile_.fd())
        , packH_()
        , stat_() {
    }
    /**
     * RETURN:
     *   true if shrinked.
     */
    template <typename LogReader>
    bool run(LogReader &reader, uint64_t *writtenLsidP = nullptr) {
        const uint32_t pbs = cfg_.pbs;
        const uint32_t salt = cfg_.salt;
        bdev_writer_local::verifyApplicablePbs(pbs, cybozu::util::getPhysicalBlockSize(ddevFile_.fd()));

        uint64_t lsid = cfg_.bgnLsid;
        if (writtenLsidP) *writtenLsidP = lsid;
        LogPackHeader packH(pbs, salt);
        bool isShrinked = false;
        while (readLogPackHeader(reader, packH, lsid) && !isShrinked) {
            packH_.copyFrom(packH);
            if (cfg_.isVerbose) std::cout << packH.str() << std::endl;
            AlignedArray buf;
            for (size_t i = 0; i < packH.nRecords(); i++) {
                if (!readLogIo(reader, packH, i, buf)) {
                    if (cfg_.doShrink) {
                        packH.shrink(i);
                        packH_.copyFrom(packH);
                        isShrinked = true;
                        break;
                    } else if (!cfg_.doSkipCsum) {
                        throw cybozu::Exception(__func__) << "invalid log IO" << i << packH;
                    }
                }
                redoLogIo(packH, i, std::move(buf));
                buf.clear();
            }
            lsid = packH.nextLogpackLsid();
        }
        ddevWriter_.waitForAll();
        ddevFile_.fdatasync();
        if (writtenLsidP) *writtenLsidP = lsid;

        ::printf("Applied lsid range [%" PRIu64 ", %" PRIu64 ")\n", cfg_.bgnLsid, lsid);
        stat_.print();
        ddevWriter_.getStat().print();

        return isShrinked;
    }
    void getPackHeader(LogPackHeader &packH) {
        if (packH_.isValid()) packH.copyFrom(packH_);
    }
private:
    void redoLogIo(const LogPackHeader &packH, size_t idx, AlignedArray &&buf) {
        const WlogRecord &rec = packH.record(idx);
        assert(rec.isExist());

        if (rec.isPadding()) {
            /* Do nothing. */
            stat_.paddingPb += rec.ioSizePb(packH.pbs());
            return;
        }
        if (rec.isDiscard()) {
            if (cfg_.isDiscard) {
                redoDiscard(rec);
                stat_.discardLb += rec.ioSizeLb();
                return;
            }
            if (!cfg_.isZeroDiscard) {
                /* Ignore discard logs. */
                stat_.ignoredLb += rec.ioSizeLb();
                return;
            }
            /* zero-discard will use redoNormalIo(). */
            stat_.allZeroLb += rec.ioSizeLb();
            redoNormalIo(packH, idx, std::move(buf));
            return;
        }
        stat_.normalLb += rec.ioSizeLb();
        redoNormalIo(packH, idx, std::move(buf));
    }
    void redoDiscard(const WlogRecord &rec) {
        assert(cfg_.isDiscard);
        assert(rec.isDiscard());
        ddevWriter_.discard(rec.offset, rec.ioSizeLb());
    }
    void redoNormalIo(const LogPackHeader &packH, size_t idx, AlignedArray &&buf) {
        const WlogRecord &rec = packH.record(idx);
        assert(!rec.isPadding());
        assert(cfg_.isZeroDiscard || !rec.isDiscard());

        const uint64_t offLb = rec.offset;
        const uint32_t sizeLb = rec.ioSizeLb();
        bool ret;
        if (rec.isDiscard()) {
            ret = prepareZeroWrite(offLb, sizeLb);
        } else {
            ret = ddevWriter_.prepare(offLb, sizeLb, std::move(buf));
        }
        if (cfg_.isVerbose && !ret) {
            ::printf("CLIPPED\t\t%" PRIu64 "\t%u\n", offLb, sizeLb);
        }
        ddevWriter_.submit();

        if (cfg_.isVerbose) {
            ::printf("CREATE\t\t%" PRIu64 "\t%u\n",
                     rec.offset, rec.ioSizeLb());
        }
    }
    bool prepareZeroWrite(uint64_t offLb, uint32_t sizeLb) {
        if (ddevWriter_.isClipped(offLb, sizeLb)) return false;
        const AlignedArray& zero = util::zeroedAlignedArray();
        const uint32_t unitLb = zero.size() / LOGICAL_BLOCK_SIZE;
        while (sizeLb > 0) {
            const uint32_t lb = std::min(unitLb, sizeLb);
            if (!ddevWriter_.prepare(offLb, lb, zero.data())) {
                throw cybozu::Exception("prepareZeroWrite") << "something wrong";
            }
            offLb += lb;
            sizeLb -= lb;
        }
        return true;
    }
};

} // namespace walb

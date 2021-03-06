#pragma once
/**
 * @file
 * @brief Walb log device utilities.
 */
#include <cassert>
#include <memory>
#include <queue>
#include <cstdlib>
#include <functional>
#include <type_traits>

#include "util.hpp"
#include "checksum.hpp"
#include "fileio.hpp"
#include "walb_log_file.hpp"
#include "walb_util.hpp"
#include "aio_util.hpp"
#include "bdev_util.hpp"
#include "bdev_reader.hpp"
#include "random.hpp"
#include "linux/walb/super.h"
#include "linux/walb/log_device.h"
#include "linux/walb/log_record.h"
#include "walb_log.h"
#include "wdev_util.hpp"

namespace walb {
namespace device {

namespace local {

inline void verifySizeIsMultipleOfPbs(size_t size, uint32_t pbs, const char *msg)
{
    if (size % pbs == 0) return;
    throw cybozu::Exception(msg) << "size must be multiples of pbs" << size << pbs;
}

} // namespace local

/**
 * WalB super sector.
 *
 * You should call read(), copyFrom(), or format() at first.
 */
class SuperBlock
{
private:
    /* Physical block size */
    uint32_t pbs_;
    /* Super block offset in the log device [physical block]. */
    uint64_t offset_;

    /* Super block data. */
    walb::AlignedArray data_;

public:
    uint16_t getSectorType() const { return super()->sector_type; }
    uint16_t getVersion() const { return super()->version; }
    uint32_t getChecksum() const { return super()->checksum; }
    uint32_t getLogicalBlockSize() const { return super()->logical_bs; }
    uint32_t getPhysicalBlockSize() const { return super()->physical_bs; }
    uint32_t pbs() const { return super()->physical_bs; } // alias.
    uint32_t getMetadataSize() const { return super()->metadata_size; }
    uint32_t getLogChecksumSalt() const { return super()->log_checksum_salt; }
    uint32_t salt() const { return super()->log_checksum_salt; } // alias.
    cybozu::Uuid getUuid() const { return cybozu::Uuid(super()->uuid); }
    const char* getName() const { return super()->name; }
    uint64_t getRingBufferSize() const { return super()->ring_buffer_size; }
    uint64_t getOldestLsid() const { return super()->oldest_lsid; }
    uint64_t getWrittenLsid() const { return super()->written_lsid; }
    uint64_t getDeviceSize() const { return super()->device_size; }

    void setOldestLsid(uint64_t oldestLsid) {
        super()->oldest_lsid = oldestLsid;
    }
    void setWrittenLsid(uint64_t writtenLsid) {
        super()->written_lsid = writtenLsid;
    }
    void setDeviceSize(uint64_t deviceSize) {
        super()->device_size = deviceSize;
    }
    void setLogChecksumSalt(uint32_t salt) {
        super()->log_checksum_salt = salt;
    }
    void setUuid(const cybozu::Uuid &uuid) {
        uuid.copyTo(super()->uuid);
    }
    void updateChecksum() {
        super()->checksum = 0;
        super()->checksum = ::checksum(data_.data(), pbs_, 0);
    }

    /*
     * Offset and size.
     */

    uint64_t get1stSuperBlockOffset() const {
        return offset_;
    }
#if 0
    uint64_t getMetadataOffset() const {
        return ::get_metadata_offset_2(super());
    }
#endif

    uint64_t get2ndSuperBlockOffset() const {
        const uint64_t oft = ::get_super_sector1_offset_2(super());
        unusedVar(oft);
#if 0
        assert(oft == getMetadataOffset() + getMetadataSize());
#endif
        return ::get_super_sector1_offset_2(super());
    }

    uint64_t getRingBufferOffset() const {
        uint64_t oft = ::get_ring_buffer_offset_2(super());
        assert(oft == get2ndSuperBlockOffset() + 1);
        return oft;
    }

    /**
     * Convert lsid to the position in the log device.
     *
     * @lsid target log sequence id.
     *
     * RETURN:
     *   Offset in the log device [physical block].
     */
    uint64_t getOffsetFromLsid(uint64_t lsid) const;

    void format(uint32_t pbs, uint64_t ddevLb, uint64_t ldevLb, const std::string &name);

    void copyFrom(const SuperBlock &rhs) {
        init(rhs.pbs(), false);
        data_ = rhs.data_;
    }
    /**
     * Read super block from the log device.
     */
    void read(int fd);

    /**
     * Write super block to the log device.
     */
    void write(int fd);

    std::string str() const;

    friend inline std::ostream& operator<<(std::ostream& os, const SuperBlock& super) {
        os << super.str();
        return os;
    }
    void print(::FILE *fp = ::stdout) const {
        ::fprintf(fp, "%s", str().c_str());
    }
private:
    void init(uint32_t pbs, bool doZeroClear);

    static uint64_t get1stSuperBlockOffsetStatic(uint32_t pbs) {
        return ::get_super_sector0_offset(pbs);
    }

    walb_super_sector* super() {
        return reinterpret_cast<walb_super_sector*>(data_.data());
    }

    const walb_super_sector* super() const {
        return reinterpret_cast<const walb_super_sector*>(data_.data());
    }

    bool isValid(bool isChecksum = true) const;
};


/**
 * Walb log device reader using synchronous read() system call.
 */
class SimpleWldevReader
{
private:
    cybozu::util::File file_;
    SuperBlock super_;
    uint32_t pbs_;
    uint64_t lsid_;
public:
    explicit SimpleWldevReader(cybozu::util::File &&file)
        : file_(std::move(file))
        , super_(), pbs_(), lsid_() {
        init();
    }
    explicit SimpleWldevReader(const std::string &wldevPath)
        : SimpleWldevReader(cybozu::util::File(wldevPath, O_RDONLY | O_DIRECT)) {
    }
    SuperBlock &super() { return super_; }
    void reset(uint64_t lsid, uint64_t maxSizePb = UINT64_MAX) {
        unusedVar(maxSizePb);
        lsid_ = lsid;
        seek();
    }
    void read(void *data, size_t size) {
        assert(size % pbs_ == 0);
        readPb(data, size / pbs_);
    }
    void skip(size_t size) {
        assert(size % pbs_ == 0);
        skipPb(size / pbs_);
    }
private:
    void init() {
        super_.read(file_.fd());
        pbs_ = super_.getPhysicalBlockSize();
        lsid_ = 0;
    }
    void readBlock(void *data) {
        file_.read(data, pbs_);
        lsid_++;
        if (lsid_ % ringBufPb() == 0) {
            seek();
        }
    }
    void seek() {
        file_.lseek(super_.getOffsetFromLsid(lsid_) * pbs_);
    }
    void verifySizePb(size_t sizePb) {
        if (sizePb >= ringBufPb()) {
            throw cybozu::Exception(__func__)
                << "too large sizePb" << sizePb << ringBufPb();
        }
    }
    uint64_t ringBufPb() const {
        return super_.getRingBufferSize();
    }

    void readPb(void *data, size_t sizePb) {
        if (sizePb == 0) return;
        verifySizePb(sizePb);

        char *p = (char *)data;
        while (sizePb > 0) {
            readBlock(p);
            p += pbs_;
            sizePb--;
        }
    }

    void skipPb(size_t sizePb) {
        if (sizePb == 0) return;
        verifySizePb(sizePb);
        lsid_ += sizePb;
        seek();
    }
};


/**
 * Walb log device reader using aio.
 */
class AsyncWldevReader
{
private:
    cybozu::util::File file_;
    const size_t pbs_;
    const size_t maxIoSize_;

    SuperBlock super_;
    cybozu::aio::Aio aio_;
    uint64_t aheadLsid_;
    RingBufferForSeqRead ringBuf_;

    struct Io
    {
        uint32_t key;
        size_t size;
    };
    std::queue<Io> ioQ_;

    uint64_t readAheadPb_; // read ahead size [physical block]

    static constexpr size_t DEFAULT_BUFFER_SIZE = 4U << 20; /* 4MiB */
    static constexpr size_t DEFAULT_MAX_IO_SIZE = 64U << 10; /* 64KiB. */
public:
    static constexpr const char *NAME() { return "AsyncWldevReader"; }
    /**
     * @wldevPath walb log device path.
     * @bufferSize buffer size to read ahead [byte].
     * @maxIoSize max IO size [byte].
     */
    AsyncWldevReader(cybozu::util::File &&wldevFile,
                     size_t bufferSize = DEFAULT_BUFFER_SIZE,
                     size_t maxIoSize = DEFAULT_MAX_IO_SIZE)
        : file_(std::move(wldevFile))
        , pbs_(cybozu::util::getPhysicalBlockSize(file_.fd()))
        , maxIoSize_(maxIoSize)
        , super_()
        , aio_(file_.fd(), bufferSize / pbs_ + 1)
        , aheadLsid_(0)
        , ringBuf_()
        , ioQ_()
        , readAheadPb_(UINT64_MAX) {
        assert(pbs_ != 0);
        verifyMultiple(bufferSize, pbs_, "bad bufferSize");
        verifyMultiple(maxIoSize_, pbs_, "bad maxIoSize");
        super_.read(file_.fd());
        ringBuf_.init(bufferSize);
    }
    AsyncWldevReader(const std::string &wldevPath,
                     size_t bufferSize = DEFAULT_BUFFER_SIZE,
                     size_t maxIoSize = DEFAULT_MAX_IO_SIZE)
        : AsyncWldevReader(
            cybozu::util::File(wldevPath, O_RDONLY | O_DIRECT),
            bufferSize, maxIoSize) {
    }
    ~AsyncWldevReader() noexcept {
        while (!ioQ_.empty()) {
            try {
                waitForIo();
            } catch (...) {
            }
        }
    }
    SuperBlock &super() { return super_; }
    /**
     * Reset current IOs and start read from a lsid.
     */
    void reset(uint64_t lsid, uint64_t maxSizePb = UINT64_MAX);
    void read(void *data, size_t size);
    void skip(size_t size);
private:
    void verifyMultiple(uint64_t size, size_t pbs, const char *msg) const {
        if (size == 0 || size % pbs != 0) {
            throw cybozu::Exception(NAME()) << msg << size << pbs;
        }
    }
    size_t waitForIo() {
        assert(!ioQ_.empty());
        const Io io = ioQ_.front();
        ioQ_.pop();
        aio_.waitFor(io.key);
        return io.size;
    }
    void prepareReadableData() {
        if (ringBuf_.getReadableSize() > 0) return;
        if (ioQ_.empty()) readAhead();
        if (ioQ_.empty()) {
            assert(readAheadPb_ == 0);
            throw cybozu::Exception(NAME()) << "reached max read size.";
        }
        assert(!ioQ_.empty());
        ringBuf_.complete(waitForIo());
    }
    void readAhead() {
        size_t n = 0;
        while (prepareAheadIo()) n++;
        if (n > 0) aio_.submit();
    }
    bool prepareAheadIo();
    size_t decideIoSize() const;
};


void initWalbMetadata(
    int fd, uint32_t pbs, uint64_t ddevLb, uint64_t ldevLb, const std::string &name);


/**
 * Wlogs area for the range [bgnLsid, endLsid] will be zero-filled in ldev.
 * buffer size is 1MiB (hard-coded).
 */
void fillZeroToLdev(const std::string& wdevName, uint64_t bgnLsid, uint64_t endLsid);


}} //namespace walb::device

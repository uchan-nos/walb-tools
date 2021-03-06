#pragma once
/**
 * @file
 * @brief walb diff base utilities.
 * @author HOSHINO Takashi
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include <memory>
#include <map>
#include <queue>

#include <snappy.h>

#include "util.hpp"
#include "range_util.hpp"
#include "fileio.hpp"
#include "checksum.hpp"
#include "walb_types.hpp"
#include "walb_util.hpp"
#include "walb_diff.h"
#include "walb_logger.hpp"
#include "linux/walb/block_size.h"
#include "backtrace.hpp"
#include "compressor.hpp"
#include "compression_type.hpp"
#include "address_util.hpp"

static_assert(::WALB_DIFF_FLAGS_SHIFT_MAX <= 8, "Too many walb diff flags.");
static_assert(::WALB_DIFF_CMPR_MAX <= 256, "Too many walb diff cmpr types.");

namespace walb {

/*
    you can freely cast this class to DiffRecord safely and vice versa.
*/
struct DiffRecord : public walb_diff_record
{
    constexpr static const char *NAME = "DiffRecord";
    DiffRecord() {
        init();
    }
    void init() {
        ::memset(this, 0, sizeof(struct walb_diff_record));
    }
    uint64_t endIoAddress() const { return io_address + io_blocks; }
    bool isCompressed() const { return compression_type != ::WALB_DIFF_CMPR_NONE; }

    bool isAllZero() const { return (flags & WALB_DIFF_FLAG(ALLZERO)) != 0; }
    bool isDiscard() const { return (flags & WALB_DIFF_FLAG(DISCARD)) != 0; }
    bool isNormal() const { return !isAllZero() && !isDiscard(); }
    bool isValid() const;
    void verify() const;

    void print(::FILE *fp = ::stdout) const;
    void printOneline(::FILE *fp = ::stdout) const {
        ::fprintf(fp, "%s\n", toStr("wdiff_rec:\t").c_str());
    }
    std::string toStr(const char *prefix = "") const;
    friend inline std::ostream &operator<<(std::ostream &os, const DiffRecord &rec) {
        os << rec.toStr();
        return os;
    }
    static void printHeader(::FILE *fp = ::stdout) {
        ::fprintf(fp, "%s\n", getHeader());
    }
    static const char* getHeader() {
        return "#wdiff_rec: addr blks cmpr offset size csum allzero discard";
    }
    void setNormal() {
        flags &= ~WALB_DIFF_FLAG(ALLZERO);
        flags &= ~WALB_DIFF_FLAG(DISCARD);
    }
    void setAllZero() {
        flags |= WALB_DIFF_FLAG(ALLZERO);
        flags &= ~WALB_DIFF_FLAG(DISCARD);
    }
    void setDiscard() {
        flags &= ~WALB_DIFF_FLAG(ALLZERO);
        flags |= WALB_DIFF_FLAG(DISCARD);
    }
    bool isOverwrittenBy(const DiffRecord &rhs) const {
        return cybozu::isOverwritten(io_address, io_blocks, rhs.io_address, rhs.io_blocks);
    }
    bool isOverlapped(const DiffRecord &rhs) const {
        return cybozu::isOverlapped(io_address, io_blocks, rhs.io_address, rhs.io_blocks);
    }
    /**
     * Split a record into several records
     * where all splitted records' ioBlocks will be <= a specified one.
     *
     * CAUSION:
     *   The checksum of splitted records will be invalid state.
     *   Only non-compressed records can be splitted.
     */
    std::vector<DiffRecord> splitAll(uint32_t ioBlocks0) const;
};


inline uint32_t calcDiffIoChecksum(const AlignedArray &io)
{
    if (io.empty()) return 0;
    return cybozu::util::calcChecksum(io.data(), io.size(), 0);
}

inline bool calcDiffIoIsAllZero(const AlignedArray &io)
{
    if (io.size() == 0) return false;
    return cybozu::util::isAllZero(io.data(), io.size());
}


std::vector<AlignedArray> splitIoDataAll(const AlignedArray &buf, uint32_t ioBlocks);


inline void printOnelineDiffIo(const AlignedArray &buf, ::FILE *fp = ::stdout)
{
    ::fprintf(fp, "size %zu checksum %08x\n"
              , buf.size(), calcDiffIoChecksum(buf));
}


int compressData(
    const char *inData, size_t inSize, AlignedArray &outData, size_t &outSize,
    int type = ::WALB_DIFF_CMPR_SNAPPY, int level = 0);
void uncompressData(
    const char *inData, size_t inSize, AlignedArray &outData, int type);

void compressDiffIo(
    const DiffRecord &inRec, const char *inData,
    DiffRecord &outRec, AlignedArray &outData, int type = ::WALB_DIFF_CMPR_SNAPPY, int level = 0);
void uncompressDiffIo(
    const DiffRecord &inRec, const char *inData,
    DiffRecord &outRec, AlignedArray &outData, bool calcChecksum = true);


/**
 * sizeof(IndexedDiffRecord) == sizeof(walb_indexed_diff_record)
 */
struct IndexedDiffRecord : public walb_indexed_diff_record
{
    void init() {
        ::memset(this, 0, sizeof(*this));
        // Now isNormal() will be true.
    }

    uint64_t endIoAddress() const { return io_address + io_blocks; }
    bool isCompressed() const { return compression_type != ::WALB_DIFF_CMPR_NONE; }

    bool isAllZero() const { return (flags & WALB_DIFF_FLAG(ALLZERO)) != 0; }
    bool isDiscard() const { return (flags & WALB_DIFF_FLAG(DISCARD)) != 0; }
    bool isNormal() const { return !isAllZero() && !isDiscard(); }

    bool isValid(bool doChecksum = true) const { return verifyDetail(false, doChecksum); }
    void verify(bool doChecksum = true) const { verifyDetail(true, doChecksum); }

    static constexpr const char *NAME = "IndexedDiffRecord";

    void printOneline(::FILE *fp = ::stdout) const {
        ::fprintf(fp, "%s\n", toStr("wdiff_idx_rec:\t").c_str());
    }
    std::string toStr(const char *prefix = "") const;
    friend inline std::ostream &operator<<(std::ostream &os, const IndexedDiffRecord &rec) {
        os << rec.toStr();
        return os;
    }

    void setNormal() {
        flags &= ~WALB_DIFF_FLAG(ALLZERO);
        flags &= ~WALB_DIFF_FLAG(DISCARD);
    }
    void setAllZero() {
        flags |= WALB_DIFF_FLAG(ALLZERO);
        flags &= ~WALB_DIFF_FLAG(DISCARD);
    }
    void setDiscard() {
        flags &= ~WALB_DIFF_FLAG(ALLZERO);
        flags |= WALB_DIFF_FLAG(DISCARD);
    }

    bool isOverwrittenBy(const IndexedDiffRecord &rhs) const {
        return cybozu::isOverwritten(io_address, io_blocks, rhs.io_address, rhs.io_blocks);
    }
    bool isOverlapped(const IndexedDiffRecord &rhs) const {
        return cybozu::isOverlapped(io_address, io_blocks, rhs.io_address, rhs.io_blocks);
    }

    void verifyAligned() const {
        if (!isAlignedSize(io_blocks)) {
            throw cybozu::Exception(NAME) << "IO is not alined" << io_blocks;
        }
    }

    std::vector<IndexedDiffRecord> split(uint32_t maxIoBlocks = 0) const;
    std::vector<IndexedDiffRecord> minus(const IndexedDiffRecord& rhs) const;

    void updateRecChecksum();
private:
    bool verifyDetail(bool throwError, bool doChecksum) const;
};


/**
 * sizeof(DiffIndexedSuper) == sizeof(walb_diff_index_super)
 */
struct DiffIndexSuper : walb_diff_index_super
{
    constexpr static const char *NAME = "DiffIndexSuper";
    void init() {
        ::memset(this, 0, sizeof(*this));
    }
    void updateChecksum() {
        checksum = 0;
        checksum = cybozu::util::calcChecksum(this, sizeof(*this), 0);
    }
    void verify() {
        if (cybozu::util::calcChecksum(this, sizeof(*this), 0) != 0) {
            throw cybozu::Exception(NAME) << "invalid checksum";
        }
    }
};


enum class DiffRecType : uint8_t
{
    NORMAL, DISCARD, ALLZERO
};

template <typename Record>
DiffRecType getDiffRecType(const Record& rec)
{
    if (rec.isNormal()) return DiffRecType::NORMAL;
    if (rec.isDiscard()) return DiffRecType::DISCARD;
    if (rec.isAllZero()) return DiffRecType::ALLZERO;
    throw cybozu::Exception(__func__) << "bad record type" << rec;
}

inline const char *toStr(DiffRecType type)
{
    if (type == DiffRecType::NORMAL) {
        return "Normal";
    } else if (type == DiffRecType::DISCARD) {
        return "Discard";
    } else if (type == DiffRecType::ALLZERO) {
        return "Allzero";
    } else {
        return "Unknown";
    }
}

inline std::ostream& operator<<(std::ostream& os, DiffRecType type)
{
    os << toStr(type);
    return os;
}


} //namesapce walb

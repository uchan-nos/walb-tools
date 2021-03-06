#include "walb_diff_mem.hpp"

namespace walb {

bool DiffRecIo::isValid(bool isChecksum) const
{
    if (!rec_.isNormal()) {
        if (!io_.empty()) {
            LOGd("Fro non-normal record, io.ioBlocks must be 0.\n");
            return false;
        }
        return true;
    }
    if (rec_.data_size != io_.size()) {
        LOGd("dataSize invalid %" PRIu32 " %zu\n", rec_.data_size, io_.size());
        return false;
    }
    if (!isChecksum) return true;

    const uint32_t csum = calcDiffIoChecksum(io_);
    if (rec_.checksum != csum) {
        LOGd("checksum invalid %08x %08x\n", rec_.checksum, csum);
        return false;
    }
    return true;
}

std::vector<DiffRecIo> DiffRecIo::splitAll(uint32_t ioBlocks) const
{
    assert(isValid());
    std::vector<DiffRecIo> v;

    std::vector<DiffRecord> recV = rec_.splitAll(ioBlocks);
    std::vector<AlignedArray> ioV;
    if (rec_.isNormal()) {
        ioV = splitIoDataAll(io_, ioBlocks);
    } else {
        ioV.resize(recV.size());
    }
    assert(recV.size() == ioV.size());
    auto it0 = recV.begin();
    auto it1 = ioV.begin();
    while (it0 != recV.end() && it1 != ioV.end()) {
        v.emplace_back(*it0, std::move(*it1));
        ++it0;
        ++it1;
    }
    return v;
}

std::vector<DiffRecIo> DiffRecIo::minus(const DiffRecIo &rhs) const
{
    assert(isValid());
    assert(rhs.isValid());
    if (!rec_.isOverlapped(rhs.rec_)) {
        throw RT_ERR("Non-overlapped.");
    }
    std::vector<DiffRecIo> v;
    /*
     * Pattern 1:
     * __oo__ + xxxxxx = xxxxxx
     */
    if (rec_.isOverwrittenBy(rhs.rec_)) {
        /* Empty */
        return v;
    }
    /*
     * Pattern 2:
     * oooooo + __xx__ = ooxxoo
     */
    if (rhs.rec_.isOverwrittenBy(rec_)) {
        uint32_t blks0 = rhs.rec_.io_address - rec_.io_address;
        uint32_t blks1 = rec_.endIoAddress() - rhs.rec_.endIoAddress();
        uint64_t addr0 = rec_.io_address;
        uint64_t addr1 = rec_.endIoAddress() - blks1;

        DiffRecord rec0 = rec_;
        DiffRecord rec1 = rec_;
        rec0.io_address = addr0;
        rec0.io_blocks = blks0;
        rec1.io_address = addr1;
        rec1.io_blocks = blks1;

        size_t size0 = 0;
        size_t size1 = 0;
        const bool recIsNormal = rec_.isNormal();
        if (recIsNormal) {
            size0 = blks0 * LOGICAL_BLOCK_SIZE;
            size1 = blks1 * LOGICAL_BLOCK_SIZE;
        }
        rec0.data_size = size0;
        rec1.data_size = size1;

        AlignedArray data0, data1;
        if (recIsNormal) {
            size_t off1 = (addr1 - rec_.io_address) * LOGICAL_BLOCK_SIZE;
            assert(size0 + rhs.rec_.io_blocks * LOGICAL_BLOCK_SIZE + size1 == rec_.data_size);
            const char *p = io_.data();
            util::assignAlignedArray(data0, p, size0);
            p += off1;
            util::assignAlignedArray(data1, p, size1);
        }

        if (0 < blks0) {
            v.emplace_back(rec0, std::move(data0));
        }
        if (0 < blks1) {
            v.emplace_back(rec1, std::move(data1));
        }
        return v;
    }
    /*
     * Pattern 3:
     * oooo__ + __xxxx = ooxxxx
     */
    if (rec_.io_address < rhs.rec_.io_address) {
        const uint64_t endIoAddr = rec_.endIoAddress();
        assert(rhs.rec_.io_address < endIoAddr);
        uint32_t rblks = endIoAddr - rhs.rec_.io_address;
        assert(rhs.rec_.io_address + rblks == endIoAddr);

        DiffRecord rec = rec_;
        /* rec.io_address does not change. */
        rec.io_blocks = rec_.io_blocks - rblks;
        assert(rec.endIoAddress() == rhs.rec_.io_address);

        size_t size = 0;
        if (rec_.isNormal()) {
            size = io_.size() - rblks * LOGICAL_BLOCK_SIZE;
        }
        AlignedArray data;
        if (rec_.isNormal()) {
            assert(rec_.data_size == io_.size());
            rec.data_size = size;
            util::assignAlignedArray(data, io_.data(), size);
        }

        v.emplace_back(rec, std::move(data));
        return v;
    }
    /*
     * Pattern 4:
     * __oooo + xxxx__ = xxxxoo
     */
    const uint64_t rhsEndIoAddr = rhs.rec_.endIoAddress();
    assert(rec_.io_address < rhsEndIoAddr);
    uint32_t rblks = rhsEndIoAddr - rec_.io_address;
    assert(rec_.io_address + rblks == rhsEndIoAddr);
    size_t off = rblks * LOGICAL_BLOCK_SIZE;

    DiffRecord rec = rec_;
    rec.io_address = rec_.io_address + rblks;
    rec.io_blocks = rec_.io_blocks - rblks;

    size_t size = 0;
    const bool isNormal = rec_.isNormal();
    if (isNormal) {
        size = io_.size() - off;
    }
    AlignedArray data;
    if (isNormal) {
        assert(rec_.data_size == io_.size());
        rec.data_size = size;
        util::assignAlignedArray(data, io_.data() + off, size);
    }
    assert(rhsEndIoAddr == rec.io_address);
    v.emplace_back(rec, std::move(data));
    return v;
}

void DiffMemory::add(const DiffRecord& rec, AlignedArray &&buf)
{
    /* Decide key range to search overlapped items.
     * We must start from the item which have max address
     * in all the items that address is less than the addr0. */
    const uint64_t addr0 = rec.io_address;
    auto it = map_.lower_bound(addr0);
    if (it == map_.end()) {
        if (!map_.empty()) --it;
    } else {
        if (addr0 < it->first && it != map_.begin()) --it;
    }

    /* Search overlapped items. */
    const uint64_t addr1 = rec.endIoAddress();
    std::queue<DiffRecIo> q;
    while (it != map_.end() && it->first < addr1) {
        DiffRecIo &r = it->second;
        if (r.record().isOverlapped(rec)) {
            nIos_--;
            nBlocks_ -= r.record().io_blocks;
            q.push(std::move(r));
            it = map_.erase(it);
        } else {
            ++it;
        }
    }

    /* Eliminate overlaps. */
    DiffRecIo r0(rec, std::move(buf));
    while (!q.empty()) {
        std::vector<DiffRecIo> v = q.front().minus(r0);
        for (DiffRecIo &r : v) {
            const DiffRecord& dr = r.record();
            nIos_++;
            nBlocks_ += dr.io_blocks;
            map_.emplace(dr.io_address, std::move(r));
        }
        q.pop();
    }
    /* Insert the item. */
    nIos_++;
    nBlocks_ += r0.record().io_blocks;
    std::vector<DiffRecIo> rv;
    if (maxIoBlocks_ > 0 && maxIoBlocks_ < rec.io_blocks) {
        // split a large IO into smaller IOs.
        rv = r0.splitAll(maxIoBlocks_);
    } else {
        rv.push_back(std::move(r0));
    }
    for (DiffRecIo &r : rv) {
        uint64_t addr = r.record().io_address;
        map_.emplace(addr, std::move(r));
    }
}

void DiffMemory::print(::FILE *fp) const
{
    auto it = map_.cbegin();
    while (it != map_.cend()) {
        const DiffRecord &rec = it->second.record();
        rec.printOneline(fp);
        ++it;
    }
}

void DiffMemory::checkStatistics() const
{
    uint64_t nBlocks = 0;
    uint64_t nIos = 0;
    auto it = map_.cbegin();
    while (it != map_.cend()) {
        const DiffRecord &rec = it->second.record();
        nBlocks += rec.io_blocks;
        nIos++;
        ++it;
    }
    if (nBlocks_ != nBlocks) {
        throw cybozu::Exception("DiffMemory:getNIos:bad blocks") << nBlocks_ << nBlocks;
    }
    if (nIos_ != nIos) {
        throw cybozu::Exception("DiffMemory:getNIos:bad ios") << nIos_ << nIos;
    }
}

void DiffMemory::writeTo(int outFd, int cmprType)
{
    SortedDiffWriter writer;
    writer.setFd(outFd);
    writer.writeHeader(fileH_);
    auto it = map_.cbegin();
    while (it != map_.cend()) {
        const DiffRecIo &r = it->second;
        assert(r.isValid());
        if (cmprType != ::WALB_DIFF_CMPR_NONE) {
            writer.compressAndWriteDiff(r.record(), r.io().data(), cmprType);
        } else {
            DiffRecord rec = r.record();
            rec.checksum = calcDiffIoChecksum(r.io());
            writer.writeDiff(rec, r.io().data());
        }
        ++it;
    }
    writer.close();
}

/**
 * Indexed diff is not supported.
 */
void DiffMemory::readFrom(int inFd)
{
    SortedDiffReader reader(inFd);
    reader.readHeader(fileH_);
    DiffRecord rec;
    AlignedArray buf;
    while (reader.readAndUncompressDiff(rec, buf, false)) {
        add(rec, std::move(buf));
    }
}

void DiffMemory::checkNoOverlappedAndSorted() const
{
    auto it = map_.cbegin();
    const DiffRecord *prev = nullptr;
    while (it != map_.cend()) {
        const DiffRecord *curr = &it->second.record();
        if (prev) {
            if (!(prev->io_address < curr->io_address)) {
                throw RT_ERR("Not sorted.");
            }
            if (!(prev->endIoAddress() <= curr->io_address)) {
                throw RT_ERR("Overlapped records exist.");
            }
        }
        prev = curr;
        ++it;
    }
}

void DiffMemory::eraseFromMap(Map::iterator& i)
{
    nIos_--;
    nBlocks_ -= i->second.record().io_blocks;
    i = map_.erase(i);
}

} //namespace walb

#include "walb_diff_merge.hpp"

namespace walb {

void DiffMerger::Wdiff::setFile(cybozu::util::File &&file, IndexedDiffCache *cache)
{
    header_.readFrom(file);
    isIndexed_ = header_.isIndexed();
    if (isIndexed_) {
        if (cache == nullptr) {
            throw cybozu::Exception(NAME) << "indexed diff cache must be specified.";
        }
        iReader_.setFile(std::move(file), *cache);
    } else {
        sReader_.setFile(std::move(file));
        sReader_.dontReadHeader();
        // cache is not used.
    }
}

void DiffMerger::Wdiff::getAndRemoveIo(AlignedArray &buf)
{
    verifyNotEnd(__func__);

    /* for check */
    const uint64_t endIoAddr0 = rec_.endIoAddress();

    verifyFilled(__func__);
    buf = std::move(buf_);
    isFilled_ = false;
    fill();

    /* for check */
    if (!isEnd() && rec_.io_address < endIoAddr0) {
        throw RT_ERR("Invalid wdiff: IOs must be sorted and not overlapped each other.");
    }
}

void DiffMerger::Wdiff::fill() const
{
    if (isEnd_ || isFilled_) return;

    bool success;
    if (isIndexed_) {
        success = readIndexedDiff();
    } else {
        success = sReader_.readAndUncompressDiff(rec_, buf_, false);
    }
    if (success) {
        isFilled_ = true;
    } else {
        isEnd_ = true;
    }
}

bool DiffMerger::Wdiff::readIndexedDiff() const
{
    IndexedDiffRecord irec;
    if (!iReader_.readDiff(irec, buf_)) return false;

    // Convert IndexedDiffRecord to DiffRecord.
    rec_.init();
    rec_.io_address = irec.io_address;
    rec_.io_blocks = irec.io_blocks;
    rec_.flags = irec.flags;
    if (!irec.isNormal()) return true;

    rec_.compression_type = ::WALB_DIFF_CMPR_NONE;
    rec_.data_offset = 0; // updated later.
    rec_.data_size = irec.io_blocks * LOGICAL_BLOCK_SIZE;
    rec_.checksum = irec.io_checksum; // not set.

    assert(buf_.size() == rec_.data_size);

    return true;
}

void DiffMerger::mergeToFd(int outFd)
{
    prepare();
    SortedDiffWriter writer;
    writer.setFd(outFd);
    writer.writeHeader(wdiffH_);

    DiffRecIo d;
    while (getAndRemove(d)) {
        assert(d.isValid());
        writer.compressAndWriteDiff(d.record(), d.io().data());
    }

    writer.close();
    assert(wdiffs_.empty());
    assert(diffMem_.empty());
    statOut_.update(writer.getStat());
}

void DiffMerger::mergeToFdInParallel(int outFd, const CompressOpt& cmpr)
{
    cybozu::util::File file(outFd);
    DiffFileHeader header;
    header.type = ::WALB_DIFF_TYPE_SORTED;
    header.writeTo(file);

    prepare();
    const size_t maxPushedNr = cmpr.numCpu * 2 + 1;
    ConverterQueue conv(maxPushedNr, cmpr.numCpu, true, cmpr.type, cmpr.level);

    DiffRecIo d;
    DiffPacker packer;
    size_t pushedNr = 0;
    while (getAndRemove(d)) {
        assert(d.isValid());
        const DiffRecord& rec = d.record();
        const AlignedArray& buf = d.io();
        if (packer.add(rec, buf.data())) continue;
        conv.push(packer.getPackAsArray());
        pushedNr++;
        packer.clear();
        packer.add(rec, buf.data());
        if (pushedNr < maxPushedNr) continue;
        AlignedArray pack(conv.pop());
        file.write(pack.data(), pack.size());
        pushedNr--;
    }
    if (!packer.empty()) {
        conv.push(packer.getPackAsArray());
    }
    conv.quit();
    for (AlignedArray pack = conv.pop(); !pack.empty(); pack = conv.pop()) {
        file.write(pack.data(), pack.size());
    }

    writeDiffEofPack(file);
}

void DiffMerger::prepare()
{
    if (!isHeaderPrepared_) {
        if (wdiffs_.empty()) {
            throw cybozu::Exception(__func__) << "Wdiffs are not set.";
        }
        const cybozu::Uuid uuid = wdiffs_.back()->header().getUuid();
        if (shouldValidateUuid_) verifyUuid(uuid);

        wdiffH_.init();
        wdiffH_.setUuid(uuid);

        removeEndedWdiffs();
        doneAddr_ = getMinimumAddr();
        isHeaderPrepared_ = true;
    }
}

bool DiffMerger::getAndRemove(DiffRecIo &recIo)
{
    assert(isHeaderPrepared_);
    while (mergedQ_.empty()) {
        moveToDiffMemory();
        if (!moveToMergedQueue()) {
            assert(wdiffs_.empty());
            return false;
        }
    }
    recIo = std::move(mergedQ_.front());
    mergedQ_.pop();
    return true;
}

uint64_t DiffMerger::getMinimumAddr() const
{
    uint64_t addr = UINT64_MAX;
    WdiffPtrList::const_iterator it = wdiffs_.begin();
    while (it != wdiffs_.end()) {
        Wdiff &wdiff = **it;
        DiffRecord rec = wdiff.getFrontRec();
        addr = std::min(addr, rec.io_address);
        ++it;
    }
    return addr;
}

void DiffMerger::moveToDiffMemory()
{
    size_t nr = tryMoveToDiffMemory();
    if (nr == 0 && !wdiffs_.empty()) {
        // Retry with enlarged searchLen_.
        nr = tryMoveToDiffMemory();
    }
    if (!wdiffs_.empty()) {
        // It must progress.
        (void)nr;
        assert(nr > 0);
    }
}

namespace walb_diff_merge_local {

struct Range
{
    uint64_t bgn;
    uint64_t end;

    Range() : bgn(0), end(0) {
    }
    Range(uint64_t bgn, uint64_t end)
        : bgn(bgn), end(end) {
    }
    explicit Range(const DiffRecord &rec) {
        set(rec);
    }
    void set(const DiffRecord &rec) {
        bgn = rec.io_address;
        end = rec.endIoAddress();
    }
    bool isOverlapped(const Range &rhs) const {
        return bgn < rhs.end && rhs.bgn < end;
    }
    bool isLeftRight(const Range &rhs) const {
        return end <= rhs.bgn;
    }
    void merge(const Range &rhs) {
        bgn = std::min(bgn, rhs.bgn);
        end = std::max(end, rhs.end);
    }
    size_t size() const {
        assert(end - bgn <= SIZE_MAX);
        return end - bgn;
    }
    friend inline std::ostream &operator<<(std::ostream &os, const Range &range) {
        os << "(" << range.bgn << ", " << range.end << ")";
        return os;
    }
};

} // namespace walb_diff_merge_local

size_t DiffMerger::tryMoveToDiffMemory()
{
    using Range = walb_diff_merge_local::Range;

    size_t nr = 0;
    uint64_t nextDoneAddr = UINT64_MAX;
    uint64_t minAddr = UINT64_MAX;
    if (wdiffs_.empty()) {
        doneAddr_ = nextDoneAddr;
        return 0;
    }
    WdiffPtrList::iterator it = wdiffs_.begin();
    Range range(wdiffs_.front()->getFrontRec());
    while (it != wdiffs_.end()) {
        bool goNext = true;
        Wdiff &wdiff = **it;
        DiffRecord rec = wdiff.getFrontRec();
        minAddr = std::min(minAddr, rec.io_address);
        Range curRange(rec);
        while (shouldMerge(rec, nextDoneAddr)) {
            nr++;
            curRange.merge(Range(rec));
            AlignedArray buf;
            wdiff.getAndRemoveIo(buf);
            mergeIo(rec, std::move(buf));
            if (wdiff.isEnd()) {
                statIn_.update(wdiff.getStat());
                it = wdiffs_.erase(it);
                goNext = false;
                break;
            }
            rec = wdiff.getFrontRec();
        }
        if (range.isOverlapped(curRange)) {
            range.merge(curRange);
        } else if (curRange.isLeftRight(range)) {
            range = curRange;
        } else {
            assert(range.isLeftRight(curRange));
            // do nothing
        }
        if (goNext) {
            nextDoneAddr = std::min(nextDoneAddr, wdiff.currentAddress());
            ++it;
        }
    }
    if (minAddr != UINT64_MAX) {
        assert(minAddr == range.bgn);
    }
    searchLen_ = std::max(searchLen_, range.size());
#if 0 // debug code
    std::cout << "nr " << nr << " "
              << "doneAddr_ " << doneAddr_ << " "
              << "nextDoneAddr " << (nextDoneAddr == UINT64_MAX ? "-" : cybozu::itoa(nextDoneAddr)) << " "
              << "searchLen_ " << searchLen_ << " "
              << "minAddr " << minAddr << " "
              << "range " << range << std::endl;
#endif
    doneAddr_ = nextDoneAddr;
    return nr;
}

bool DiffMerger::moveToMergedQueue()
{
    if (diffMem_.empty()) return false;
    DiffMemory::Map& map = diffMem_.getMap();
    DiffMemory::Map::iterator i = map.begin();
    while (i != map.end()) {
        DiffRecIo& recIo = i->second;
        if (recIo.record().endIoAddress() > doneAddr_) break;
        mergedQ_.push(std::move(recIo));
        diffMem_.eraseFromMap(i);
    }
    return true;
}

void DiffMerger::removeEndedWdiffs()
{
    WdiffPtrList::iterator it = wdiffs_.begin();
    while (it != wdiffs_.end()) {
        const Wdiff &wdiff = **it;
        if (wdiff.isEnd()) {
            statIn_.update(wdiff.getStat());
            it = wdiffs_.erase(it);
        } else {
            ++it;
        }
    }
}

void DiffMerger::verifyUuid(const cybozu::Uuid &uuid) const
{
    for (const WdiffPtr &wdiffP : wdiffs_) {
        const cybozu::Uuid uuid1 = wdiffP->header().getUuid();
        if (uuid1 != uuid) {
            throw cybozu::Exception(__func__) << "uuid differ" << uuid1 << uuid;
        }
    }
}

} //namespace walb

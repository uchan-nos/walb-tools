#pragma once
/**
 * @file
 * @brief Archive volume information.
 * @author HOSHINO Takashi
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <map>
#include <memory>
#include <sstream>
#include "cybozu/file.hpp"
#include "cybozu/atoi.hpp"
#include "lvm.hpp"
#include "file_path.hpp"
#include "wdiff_data.hpp"
#include "meta.hpp"
#include "walb_diff_merge.hpp"

namespace walb {

const std::string VOLUME_PREFIX = "i_";
const std::string RESTORE_PREFIX = "r_";

const char *const aClear = "Clear";
const char *const aSyncReady = "SyncReady";
const char *const aArchived = "Archived";
const char *const aStopped = "Stopped";

// temporary state
const char *const atInitVol = "InitVol";
const char *const atClearVol = "ClearVol";
const char *const atFullSync = "FullSync";
const char *const atHashSync = "HashSync";
const char *const atWdiffRecv = "WdiffRecv";
const char *const atStop = "Stop";
const char *const atStart = "Start";

/**
 * Data manager for a volume in a server.
 * This is not thread-safe.
 */
class ArchiveVolInfo
{
private:
    const cybozu::FilePath volDir_;
    const std::string vgName_;
    const std::string volId_;
    mutable WalbDiffFiles wdiffs_;

public:
    /**
     * @baseDirStr base directory path string.
     * @volId volume identifier.
     * @vgName volume group name.
     */
    ArchiveVolInfo(const std::string &baseDirStr, const std::string &volId,
                   const std::string &vgName)
        : volDir_(cybozu::FilePath(baseDirStr) + volId)
        , vgName_(vgName)
        , volId_(volId)
        , wdiffs_(volDir_.str(), false) {
        cybozu::FilePath baseDir(baseDirStr);
        if (!baseDir.stat().isDirectory()) {
            throw cybozu::Exception("ArchiveVolInfo:Directory not found: " + baseDirStr);
        }
        if (!cybozu::lvm::vgExists(vgName_)) {
            throw cybozu::Exception("ArchiveVolInfo:Vg does not exist: " + vgName_);
        }
        if (volId.empty()) {
            throw cybozu::Exception("ArchiveVolInfo:volId is empty");
        }
        if (existsVolDir()) {
            wdiffs_.reloadMetadata();
        }
    }
    void init() {
        util::makeDir(volDir_.str(), "ArchiveVolInfo::init", true);
        setUuid(cybozu::Uuid());
        setMetaState(MetaState());
        setState(aSyncReady);
    }
    /**
     * CAUSION:
     *   The volume will be removed if exists.
     *   All data inside the directory will be removed.
     */
    void clear() {
        // Delete all related lvm volumes and snapshots.
        if (lvExists()) {
            getLv().remove();
        }
        if (!volDir_.rmdirRecursive()) {
            throw cybozu::Exception("ArchiveVolInfo::clear:rmdir recursively failed.");
        }
    }
    cybozu::Uuid getUuid() const {
        cybozu::Uuid uuid;
        util::loadFile(volDir_, "uuid", uuid);
        return uuid;
    }
    void setUuid(const cybozu::Uuid &uuid) {
        util::saveFile(volDir_, "uuid", uuid);
    }
    void setMetaState(const MetaState &st) {
        util::saveFile(volDir_, "base", st);
    }
    MetaState getMetaState() const {
        MetaState st;
        util::loadFile(volDir_, "base", st);
        return st;
    }
    void setState(const std::string& newState)
    {
        const char *tbl[] = {
            aSyncReady, aArchived, aStopped,
        };
        for (const char *p : tbl) {
            if (newState == p) {
                util::saveFile(volDir_, "state", newState);
                return;
            }
        }
        throw cybozu::Exception("ArchiveVolInfo::setState:bad state") << newState;
    }
    std::string getState() const {
        std::string st;
        util::loadFile(volDir_, "state", st);
        return st;
    }
    bool existsVolDir() const {
        return volDir_.stat(true).isDirectory();
    }
    bool lvExists() const {
        return cybozu::lvm::exists(vgName_, lvName());
    }
    /**
     * Create a volume.
     * @sizeLb volume size [logical block].
     */
    void createLv(uint64_t sizeLb) {
        if (sizeLb == 0) {
            throw cybozu::Exception("ArchiveVolInfo::createLv:sizeLb is zero");
        }
        if (lvExists()) {
            uint64_t curSizeLb = getLv().sizeLb();
            if (curSizeLb != sizeLb) {
                throw cybozu::Exception("ArchiveVolInfo::createLv:sizeLb is different") << curSizeLb << sizeLb;
            }
            return;
        }
        getVg().create(lvName(), sizeLb);
    }
    /**
     * Get volume data.
     */
    cybozu::lvm::Lv getLv() const {
        cybozu::lvm::Lv lv = cybozu::lvm::locate(vgName_, lvName());
        if (lv.isSnapshot()) {
            throw cybozu::Exception(
                "The target must not be snapshot: " + lv.path().str());
        }
        return lv;
    }
    /**
     * Restore a snapshot.
     * (1) create lvm snapshot of base lv. (with temporal lv name)
     * (2) apply appropriate wdiff files.
     * (3) rename the lvm snapshot.
     */
    bool restore(uint64_t gid) {
        cybozu::lvm::Lv lv = getLv();
        const std::string targetName = restoredSnapshotName(gid);
        const std::string tmpLvName = targetName + "_tmp";
        if (lv.hasSnapshot(tmpLvName)) {
            lv.getSnapshot(tmpLvName).remove();
        }
        if (lv.hasSnapshot(targetName)) {
            // Already restored.
            return false;
        }
        cybozu::lvm::Lv lvSnap = lv.takeSnapshot(tmpLvName, true);
        const MetaDiffManager& mgr = wdiffs_.getMgr();
        const int maxRetryNum = 10;
        int retryNum = 0;
        std::vector<cybozu::util::FileOpener> ops;
    retry:
        {
            const std::vector<MetaDiff> metaDiffList = mgr.getDiffListToRestore(getMetaState(), gid);
            if (metaDiffList.empty()) {
                return false;
            }
            // apply wdiff files indicated by metaDiffList to lvSnap.
            for (const MetaDiff& diff : metaDiffList) {
                cybozu::util::FileOpener op;
                if (!op.open(getDiffPath(diff).str(), O_RDONLY)) {
                    retryNum++;
                    if (retryNum == maxRetryNum) throw cybozu::Exception("ArchiveVolInfo::restore:exceed max retry");
                    wdiffs_.reloadMetadata();
                    ops.clear();
                    goto retry;
                }
                ops.push_back(std::move(op));
            }
        }
        std::vector<int> fds;
        for (cybozu::util::FileOpener& op : ops) {
            fds.push_back(op.fd());
        }
        diff::Merger merger;
        merger.addWdiffs(fds);
        diff::RecIo recIo;
        cybozu::util::BlockDevice bd(lvSnap.path().str(), O_RDWR);
        std::vector<char> zero;
        while (merger.pop(recIo)) {
            diff::RecordRaw& raw = recIo.record();
            assert(!raw.isCompressed());
            const uint64_t ioAddress = raw.ioAddress();
            const uint64_t ioBlocks = raw.ioBlocks();
            LOGd_("ioAddr %" PRIu64 " ioBlks %" PRIu64 "", ioAddress, ioBlocks);
            const uint64_t ioAddrB = ioAddress * LOGICAL_BLOCK_SIZE;
            const uint64_t ioSizeB = ioBlocks * LOGICAL_BLOCK_SIZE;

            // TODO: check the IO is out of range or not.

            const char *data;
            // Curently a discard IO is converted to an all-zero IO.
            if (raw.isAllZero() || raw.isDiscard()) {
                if (zero.size() < ioSizeB) zero.resize(ioSizeB, 0);
                data = &zero[0];
            } else {
                data = recIo.io().rawData();
            }
            bd.write(ioAddrB, ioSizeB, data);
        }
        bd.fdatasync();
        bd.close();
        cybozu::lvm::renameLv(lv.vgName(), tmpLvName, targetName);
        return true;
    }
    /**
     * QQQ
     */
    bool apply(uint64_t timestamp) {
        MetaState st0 = getMetaState();
        const MetaDiffManager &mgr = wdiffs_.getMgr();
        std::vector<MetaDiff> metaDiffList = mgr.getDiffListToApply(st0, timestamp);
        if (metaDiffList.empty()) {
            // There is nothing to apply.
            return false;
        }
        MetaState st1 = applying(st0, metaDiffList);
        setMetaState(st1);

        // QQQ
        // apply wdiff files indicated by metaDiffList to lv.

        MetaState st2 = walb::apply(st0, metaDiffList);
        setMetaState(st2);
        return true;
    }
    std::vector<std::string> getStatusAsStrVec() const {
        std::vector<std::string> v;
        auto &fmt = cybozu::util::formatString;
        wdiffs_.reloadMetadata();

        v.push_back(fmt("volId %s", volId_.c_str()));
        uint64_t sizeLb = 0; // TODO
        v.push_back(fmt("size %" PRIu64 "", sizeLb));
        const cybozu::Uuid uuid = getUuid();
        v.push_back(fmt("uuid %s", uuid.str().c_str()));
        std::string st = getState();
        v.push_back(fmt("state %s", st.c_str()));
        std::vector<std::string> actionV; // TODO
        std::string actions;
        for (const std::string &s : actionV) {
            actions += std::string(" ") + s;
        }
        v.push_back(fmt("actions %s", actions.c_str()));
        const MetaState metaSt = getMetaState();
        v.push_back(fmt("base %s", metaSt.str().c_str()));
        const MetaSnap latest = wdiffs_.getMgr().getLatestSnapshot(metaSt);
        v.push_back(fmt("latest %s", latest.str().c_str()));
        size_t numRestored = 0; // TODO
        v.push_back(fmt("numRestored %zu", numRestored));
        for (size_t i = 0; i < numRestored; i++) {
            uint64_t gid = 0; // TODO
            v.push_back(fmt("restored %" PRIu64 "", gid));
        }
        std::vector<MetaSnap> rv; // TODO
        v.push_back(fmt("numRestoreble %zu", rv.size()));
        for (size_t i = 0; i < rv.size(); i++) {
            MetaSnap snap; // TODO
            v.push_back(fmt("snapshot %" PRIu64 "", snap.gidB));
        }
        std::vector<MetaDiff> dv = wdiffs_.getMgr().getApplicableDiffList(metaSt.snapB);
        v.push_back(fmt("numWdiff %zu", dv.size()));
        for (const MetaDiff &d : dv) {
            size_t size = wdiffs_.getDiffFileSize(d);
            v.push_back(fmt("wdiff %s %d %" PRIu64 " %s"
                            , d.str().c_str(), d.canMerge ? 1 : 0, size
                            , cybozu::unixTimeToStr(d.timestamp).c_str()));
        }
        return v;
    }
private:
    cybozu::lvm::Vg getVg() const {
        return cybozu::lvm::getVg(vgName_);
    }
    std::string lvName() const {
        return VOLUME_PREFIX + volId_;
    }
    std::string restoredSnapshotName(uint64_t gid) const {
        return RESTORE_PREFIX + volId_ + "_" + cybozu::itoa(gid);
    }
    /**
     * Full path of the wdiff file of a corresponding meta diff.
     */
    cybozu::FilePath getDiffPath(const MetaDiff &diff) const {
        return volDir_ + cybozu::FilePath(createDiffFileName(diff));
    }
#if 0 // XXX
    /**
     * Grow the volume.
     * @newSizeLb [logical block].
     */
    void growLv(uint64_t newSizeLb) {
        cybozu::lvm::Lv lv = getLv();
        if (lv.sizeLb() == newSizeLb) {
            /* no need to grow. */
            return;
        }
        if (newSizeLb < lv.sizeLb()) {
            /* Shrink is not supported. */
            throw cybozu::Exception(
                "You tried to shrink the volume: " + lv.path().str());
        }
        lv.resize(newSizeLb);
    }
    bool initialized() const {
        /* now editing */
        return false;
    }
    const WalbDiffFiles &diffs() const {
        assert(wdiffsP_);
        return *wdiffsP_;
    }
    /**
     * Get restored snapshots.
     */
    std::map<uint64_t, cybozu::lvm::Lv> getRestores() const {
        std::map<uint64_t, cybozu::lvm::Lv> map;
        std::string prefix = RESTORE_PREFIX + volId_ + "_";
        for (cybozu::lvm::Lv &lv : getLv().snapshotList()) {
            if (cybozu::util::hasPrefix(lv.snapName(), prefix)) {
                std::string gidStr
                    = cybozu::util::removePrefix(lv.snapName(), prefix);
                uint64_t gid = cybozu::atoi(gidStr);
                map.emplace(gid, lv);
            }
        }
        return map;
    }
    /**
     * Get the latest snapshot which may be dirty.
     * RETURN:
     *   meta snap of the latest snapshot.
     */
    MetaSnap getLatestSnapshot() const {
#if 0
        std::vector<MetaDiff> v = wdiffsP_->listDiff();
        return v.empty() ? baseRecord_ : getSnapFromDiff(v.last());
#else
        return wdiffsP_->latest();
#endif
    }
    /**
     * Get the oldest snapshot which may be dirty.
     * RETURN:
     *   meta snap of the oldest snapshot.
     */
    MetaSnap getOldestSnapshot() const {
        return baseRecord_;
    }
    /**
     * Get the latest clean snapshot.
     *
     * RETURN:
     *   gid that means the latest clean snapshot.
     *   uint64_t(-1) means that there is no clean snapshot.
     */
    uint64_t getLatestCleanSnapshot() const {
        std::vector<MetaDiff> v = wdiffsP_->listDiff();
        auto it = v.crbegin();
        while (it != v.crend()) {
            const MetaDiff &diff = *it;
            if (diff.isClean()) return diff.snap1().gid0();
            ++it;
        }
        if (baseRecord_.isClean()) return baseRecord_.gid0();
        return uint64_t(-1);
    }
    template <typename OutputStream>
    void print(OutputStream &os) const {

        MetaSnap oldest = baseRecord_;
        MetaSnap latest = wdiffsP_->latest();
        std::string oldestState = oldest.isDirty() ? "dirty" : "clean";
        std::string latestState = latest.isDirty() ? "dirty" : "clean";

        os << "vg: " << vgName_ << std::endl;
        os << "name: " << volId_ << std::endl;
        os << "sizeLb: " << getLv().sizeLb() << std::endl;
        os << "oldest: (" << oldest.gid0() << ", " << oldest.gid1() << ") "
           << oldestState << std::endl;
        os << "latest: (" << latest.gid0() << ", " << latest.gid1() << ") "
           << latestState << std::endl;

        os << "----------restored snapshots----------" << std::endl;
        for (auto &pair : getRestores()) {
            cybozu::lvm::Lv &lv = pair.second;
            lv.print(os);
        }

        os << "----------diff files----------" << std::endl;
        std::vector<std::string> v = wdiffsP_->listName();
        for (std::string &fileName : v) {
            os << fileName << std::endl;
        }
        os << "----------end----------" << std::endl;
    }
    void print(::FILE *fp) const {
        std::stringstream ss;
        print(ss);
        std::string s(ss.str());
        if (::fwrite(&s[0], 1, s.size(), fp) < s.size()) {
            throw cybozu::Exception("fwrite failed.");
        }
        ::fflush(fp);
    }
    void print() const { print(::stdout); }
    /**
     * Create a restore volume as a snapshot.
     * You must apply diff(s) by yourself.
     *
     * @gid restored snapshot will indicates the gid.
     */
    bool restore(uint64_t gid = uint64_t(-1)) {
        if (gid == uint64_t(-1)) {
            gid = getLatestCleanSnapshot();
            if (gid == uint64_t(-1)) return false;
        }
        ::printf("target restore gid %" PRIu64 "\n", gid); /* debug */
        if (!canRestore(gid)) return false;
        std::string snapName = restoredSnapshotName(gid);
        if (getLv().hasSnapshot(snapName)) return false;
        cybozu::lvm::Lv snap = getLv().takeSnapshot(snapName);
        return snap.exists();
    }
    /**
     * Whether a specified gid can be restored.
     * That means wdiffs has the clean snaphsot for the gid.
     */
    bool canRestore(uint64_t gid) const {
        ::printf("canRestore begin\n"); /* debug */
        MetaSnap snap = baseRecord_;
        snap.print(); /* debug */
        for (const MetaDiff &diff : wdiffsP_->listDiff()) {
            if (diff.snap0().gid0() < snap.gid0()) {
                /* skip old diffs. */
                continue;
            }
            if (gid < diff.snap1().gid0()) break;
            diff.print(); /* debug */
            assert(canApply(snap, diff));
            snap = walb::apply(snap, diff);
            snap.print(); /* debug */
        }
        ::printf("canRestore end\n"); /* debug */
        return snap.isClean() && snap.gid0() == gid;
    }
    /**
     * Drop a restored snapshot.
     */
    bool drop(uint64_t gid) {
        std::string snapName = restoredSnapshotName(gid);
        if (!getLv().hasSnapshot(snapName)) {
            return false;
        }
        getLv().getSnapshot(snapName).remove();
        return true;
    }
    /**
     * This is what to do for application.
     * DO NOT CALL THIS really.
     *
     * Apply all diffs before gid into the original lv.
     * Applied diff will be deleted after the application completed successfully.
     */
    void apply(uint64_t gid) {
        throw cybozu::Exception("Do not call this.");

        std::vector<MetaDiff> diffV = diffsToApply(gid);
        if (diffV.empty()) return;
        MetaDiff diff = diffV[0];
        for (size_t i = 1; i < diffV.size(); i++) {
            if (!canMerge(diff, diffV[i])) {
                throw cybozu::Exception("could not merge.");
            }
            diff = merge(diff, diffV[i]);
        }
        std::vector<cybozu::FilePath> pathV;
        for (MetaDiff &diff : diffV) {
            pathV.push_back(getDiffPath(diff));
        }

        /*
         * Try to open all files in the pathV.
         */

        startToApply(diff);

        /*
         * Apply the wdiff file indicated by the pathV.
         */

        finishToApply(diff);
    }
    /**
     * @gid threshold gid. That indicates all diffs
     * which gid1 is not greater than a specified gid.
     *
     * RETURN:
     *   a list of meta diff to apply.
     */
    std::vector<MetaDiff> diffsToApply(uint64_t gid) {
        MetaSnap oldest = getOldestSnapshot();
        if (gid <= oldest.gid0()) return {};
        return wdiffsP_->listDiff(oldest.gid0(), gid);
    }
    /**
     * Update the base record to be dirty to start wdiffs application.
     */
    void startToApply(const MetaDiff &diff) {
        baseRecord_ = walb::startToApply(baseRecord_, diff);
        saveBaseRecord();
    }
    /**
     * Update the base record to be clean to finish wdiffs application.
     * And remove old wdiffs.
     */
    void finishToApply(const MetaDiff &diff) {
        baseRecord_ = walb::finishToApply(baseRecord_, diff);
        saveBaseRecord();
    }
    /**
     * Remove old diffs which have been applied to the base lv already.
     */
    void removeOldDiffs() {
        wdiffsP_->removeBeforeGid(baseRecord_.gid0());
    }
    /**
     * You must prepare wdiff file before calling this.
     * Add a wdiff.
     */
    void add(const MetaDiff &diff) {
        if (!wdiffsP_->add(diff)) throw RT_ERR("diff add failed.");
    }
    /**
     * Get wdiff candidates for consolidation.
     * You must consolidate them by yourself.
     * After consolidation completed,
     * you must call finishToConsolidate(diff) where
     * the diff is consolidate(returned diff list).
     *
     * @ts0, @ts1: timestamp range [ts0, ts1).
     *
     * RETURN:
     *   Consolidate candidates whose average file size is the smallest.
     *   canConsolidate(returned) must be true.
     *   {} if there is no diffs to consolidate.
     */
    std::vector<MetaDiff> candidatesToConsolidate(uint64_t ts0, uint64_t ts1) const {
        std::vector<MetaDiff> v;
        std::vector<std::vector<MetaDiff> > vv;
        std::vector<std::pair<uint64_t, uint64_t> > stat;
        uint64_t total = 0;

        auto insert = [&]() {
            assert(!v.empty());
            stat.emplace_back(total, v.size());
            assert(canConsolidate(v));
            vv.push_back(std::move(v));
            v.clear();
            total = 0;
        };

        /*
         * Split wdiff file list where each list can be consolidated.
         */
        for (MetaDiff &diff : wdiffsP_->listDiffInTimeRange(ts0, ts1)) {
            if (!v.empty() && !canMerge(v.back(), diff)) insert();
            v.push_back(diff);
            total += wdiffsP_->getDiffFileSize(diff);
        }
        if (!v.empty()) insert();
        if (vv.empty()) return {};
        assert(vv.size() == stat.size());
        std::vector<double> avgV;
        for (auto &pair : stat) {
            avgV.push_back(double(pair.first) / double(pair.second));
        }
        assert(vv.size() == avgV.size());

        /*
         * Choose the wdiff list whose average size is the smallest.
         */
        size_t idx = 0;
        double min = avgV[0];
        while (vv[idx].size() == 1 && idx < avgV.size()) {
            idx++;
            min = avgV[idx];
        }
        if (idx == avgV.size()) return {};
        for (size_t i = idx + 1; i < avgV.size(); i++) {
            if (1 < vv[i].size() && avgV[i] < min) {
                idx = i;
                min = avgV[i];
            }
        }
        if (1 < vv[idx].size()) {
            return std::move(vv[idx]);
        } else {
            return {};
        }
    }
    /**
     * Finish to consolidate.
     * (1) insert the corresponding meta diff.
     * (2) delete dangling diffs.
     */
    void finishToConsolidate(const MetaDiff &diff) {
        if (!wdiffsP_->consolidate(diff.snap0().gid0(), diff.snap1().gid0())) {
            throw cybozu::Exception("Consolidate failed.");
        }
        wdiffsP_->cleanup();
    }
private:
    cybozu::FilePath baseRecordPath() const {
        return getDir() + cybozu::FilePath("base");
    }
    bool loadBaseRecord() {
        if (!baseRecordPath().stat().exists()) return false;
        cybozu::util::FileReader reader(baseRecordPath().str(), O_RDONLY);
        cybozu::load(baseRecord_, reader);
        checkBaseRecord();
        return true;
    }
    void saveBaseRecord() const {
        checkBaseRecord();
        cybozu::TmpFile tmpFile(getDir().str());
        cybozu::save(tmpFile, baseRecord_);
        tmpFile.save(baseRecordPath().str());
    }
    void checkBaseRecord() const {
        if (!baseRecord_.isValid()) {
            throw cybozu::Exception("baseRecord is not valid.");
        }
    }
#endif // XXX
};

} //namespace walb
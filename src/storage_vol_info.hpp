#pragma once
#include <cassert>
#include <cstring>
#include <time.h>
#include "cybozu/serializer.hpp"
#include "queue_file.hpp"
#include "file_path.hpp"
#include "tmp_file.hpp"
#include "tmp_file_serializer.hpp"
#include "meta.hpp"
#include "uuid.hpp"
#include "walb_util.hpp"
#include "wdev_util.hpp"
#include "wdev_log.hpp"

namespace walb {

const char *const sClear = "Clear";
const char *const sSyncReady = "SyncReady";
const char *const sStopped = "Stopped";
const char *const sMaster = "Master";
const char *const sSlave = "Slave";

// temporary state
const char *const stInitVol = "InitVol";
const char *const stClearVol = "ClearVol";
const char *const stStartSlave = "StartSlave";
const char *const stStopSlave = "StopSlave";
const char *const stFullSync = "FullSync";
const char *const stHashSync = "HashSync";
const char *const stStartMaster = "StartMaster";
const char *const stStopMaster = "StopMaster";
const char *const stReset = "Reset";
const char *const stWlogSend = "WlogSend";
const char *const stWlogRemove = "WlogRemove";


/**
 * Persistent data for a volume managed by a storage daemon.
 *
 * queue file:
 *   must have at least one record.
 */
class StorageVolInfo
{
private:
    cybozu::FilePath volDir_; /* volume directory. */
    std::string volId_; /* volume identifier. */
    cybozu::FilePath wdevPath_; /* wdev path. */

public:
    /**
     * For initialization.
     */
    StorageVolInfo(const std::string &baseDirStr, const std::string &volId, const std::string &wdevPath)
        : volDir_(cybozu::FilePath(baseDirStr) + volId)
        , volId_(volId)
        , wdevPath_(wdevPath) {
        verifyBaseDirExistance(baseDirStr);
        verifyWdevPathExistance();
    }
    /**
     * If volume directory does not exist, only existsVolDir() can be called.
     */
    StorageVolInfo(const std::string &baseDirStr, const std::string &volId)
        : volDir_(cybozu::FilePath(baseDirStr) + volId)
        , volId_(volId)
        , wdevPath_() {
        verifyBaseDirExistance(baseDirStr);
        if (!existsVolDir()) return;
        loadWdevPath();
        verifyWdevPathExistance();
    }
    /**
     * Initialize the volume information directory.
     */
    void init() {
        LOGd("volDir %s volId %s", volDir_.cStr(), volId_.c_str());
        util::makeDir(volDir_.str(), "StorageVolInfo", true);
        {
            cybozu::util::QueueFile qf(queuePath().str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
            qf.sync();
        }
        util::saveFile(volDir_, "path", wdevPath_.str());
        setState(sSyncReady);
        const uint64_t lsid = -1;
        const uint64_t gid = -1;
        MetaLsidGid doneRec(lsid, gid, false, 0);
        setDoneRecord(doneRec);
        setUuid(cybozu::Uuid());
    }
    /**
     * Clear all the volume information.
     * The directory will be deleted completely.
     * The instance will be invalid after calling this.
     */
    void clear() {
        if (!volDir_.rmdirRecursive()) {
            throw cybozu::Exception("StorageVolInfo::clear:rmdir recursively failed.");
        }
    }
    bool existsVolDir() const {
        return volDir_.stat().isDirectory();
    }
    /**
     * get status as a string vector.
     */
    std::vector<std::string> getStatusAsStrVec(bool isVerbose) const {
        std::vector<std::string> v;
        if (!existsVolDir()) return v;

        auto &fmt = cybozu::util::formatString;
        v.push_back(fmt("volId %s", volId_.c_str()));
        v.push_back(fmt("wdevPath %s", wdevPath_.cStr()));
        uint64_t sizeLb = 0; // TODO
        v.push_back(fmt("size %" PRIu64 "", sizeLb));
        const std::string stateStr = getState();
        v.push_back(fmt("state %s", stateStr.c_str()));
        uint64_t logFreeSpacePb = 0; // TODO
        v.push_back(fmt("logFreeSpace %" PRIu64 "", logFreeSpacePb));
        uint64_t logCapacityPb = 0; // TODO
        v.push_back(fmt("logCapacity %" PRIu64 "", logCapacityPb));
        const cybozu::Uuid uuid = getUuid();
        v.push_back(fmt("uuid %s", uuid.str().c_str()));
        uint32_t pbs = 0;
        v.push_back(fmt("pbs %" PRIu32 "", pbs));
        uint32_t salt = 0; // TODO
        v.push_back(fmt("salt %" PRIu32 "", salt));

        // TODO

        // base <lsid> <gidB> <gidE> <canMerge> <timestamp>
        // snapshot <lsid> <gid> <gid> <canMerge> <timestamp>

        if (!isVerbose) return v;
        v.push_back("verbose");

        v.push_back("DoneFile");
        MetaLsidGid doneRec = getDoneRecord();
        v.push_back(doneRec.str());

        v.push_back("QueueFile");
        cybozu::util::QueueFile qf(queuePath().str(), O_RDWR);
        cybozu::util::QueueFile::ConstIterator itr = qf.cbegin();
        while (itr != qf.cend()) {
            MetaLsidGid rec;
            itr.get(rec);
            rec.verify();
            v.push_back(rec.str());
            ++itr;
        }

        return v;
    }
    std::string getState() const {
        std::string ret;
        util::loadFile(volDir_, "state", ret);
        return ret;
    }
    void setState(const std::string& newState)
    {
        const char *tbl[] = {
            sSyncReady,
            sStopped,
            sMaster,
            sSlave,
        };
        for (const char *p : tbl) {
            if (newState == p) {
                util::saveFile(volDir_, "state", newState);
                return;
            }
        }
        throw cybozu::Exception("StorageVolInfo::setState:bad state") << newState;
    }
    void resetWlog(uint64_t gid)
    {
        device::resetWal(wdevPath_.str());
        setDoneRecord(MetaLsidGid(0, gid, false, ::time(0)));
        {
            cybozu::util::QueueFile qf(queuePath().str(), O_RDWR);
            qf.clear();
            qf.sync();
        }
        {
            cybozu::Uuid uuid;
            cybozu::util::BlockDevice bd = device::getWldev(getWdevName());
            device::SuperBlock super(bd);
            uuid.set(super.getUuid());
            setUuid(uuid);
        }
        setState(sSyncReady);
    }
    cybozu::Uuid getUuid() const {
        cybozu::Uuid uuid;
        util::loadFile(volDir_, "uuid", uuid);
        return uuid;
    }
    void setUuid(const cybozu::Uuid &uuid) {
        util::saveFile(volDir_, "uuid", uuid);
    }
    std::string getWdevPath() const { return wdevPath_.str(); }
    std::string getWdevName() const {
        return device::getWdevNameFromWdevPath(wdevPath_.str());
    }
    /**
     * Take a snapshot by pushing a record to the queue file.
     *
     * @maxWlogSendMb
     *   maximum wlog size to send at once [MiB]
     *
     * RETURN:
     *   gid of the snapshot.
     */
    uint64_t takeSnapshot(uint64_t maxWlogSendMb) {
        const char *const FUNC = __func__;
        const uint64_t maxWlogSendPb = getMaxWlogSendPb(maxWlogSendMb, FUNC);
        cybozu::util::QueueFile qf(queuePath().str(), O_RDWR);
        return takeSnapshotDetail(maxWlogSendPb, false, qf);
    }
    /**
     * Calling order:
     *   (0) isRequiredWlogTransfer()
     *   (1) prepareWlogTransfer()
     *   (2) getTransferDiff()
     *   (3) finishWlogTransfer()
     *
     * RETURN:
     *   false if wlogTransfer is not required.
     */
    bool isRequiredWlogTransfer() {
        const char *const FUNC = __func__;
        const std::string wdevPath = getWdevPath();
        const uint64_t lsid0 = device::getOldestLsid(wdevPath);
        const uint64_t lsid1 = device::getPermanentLsid(wdevPath);
        if (lsid0 < lsid1) return true;
        if (lsid0 != lsid1) {
            throw cybozu::Exception(FUNC) << "must be equal" << lsid0 << lsid1;
        }
        cybozu::util::QueueFile qf(queuePath().str(), O_RDWR);
        return !qf.empty();
    }
    /**
     *
     * RETURN:
     *   target lsid/gid range by two MetaLsidGids: recB and recE,
     *   and lsidLimit as uint64_t value.
     *   Do not transfer logpacks which lsid >= lsidLimit.
     */
    std::tuple<MetaLsidGid, MetaLsidGid, uint64_t> prepareWlogTransfer(uint64_t maxWlogSendMb) {
        const char *const FUNC = __func__;
        cybozu::util::QueueFile qf(queuePath().str(), O_RDWR);
        MetaLsidGid recB = getDoneRecord();
        const std::string wdevPath = getWdevPath();
        const uint64_t lsid0 = device::getOldestLsid(wdevPath);
        if (lsid0 < recB.lsid) device::eraseWal(wdevPath, recB.lsid);
        MetaLsidGid recE;
        for (;;) {
            if (qf.empty()) break;
            qf.back(recE);
            recE.verify();
            if ((recE.lsid < recB.lsid) || (recB.lsid == recE.lsid && recE.gid <= recB.gid)) {
                qf.popBack();
                continue;
            }
            break;
        }
        const uint64_t maxWlogSendPb = getMaxWlogSendPb(maxWlogSendMb, FUNC);
        if (qf.empty()) {
            takeSnapshotDetail(maxWlogSendPb, true, qf);
            qf.back(recE);
            recE.verify();
        }
        if (!(recB.lsid <= recE.lsid)) {
            throw cybozu::Exception(FUNC)
                << "invalid MetaLsidGidRecord" << recB << recE;
        }
        assert(recB.gid < recE.gid);

        uint64_t lsidLimit;
        if (recB.gid + 1 == recE.gid) {
            lsidLimit = recE.lsid;
        } else {
            lsidLimit = std::min(recB.lsid + maxWlogSendPb, recE.lsid);
        }
        return std::make_tuple(recB, recE, lsidLimit);
    }
    /**
     * RETURN:
     *   generated diff will be transferred to a proxy daemon.
     */
    MetaDiff getTransferDiff(const MetaLsidGid &recB, const MetaLsidGid &recE, uint64_t lsidE) const {
        MetaDiff diff;
        diff.snapB.set(recB.gid);
        if (lsidE == recE.lsid) {
            diff.snapE.set(recE.gid);
        } else {
            assert(recB.gid + 1 < recE.gid);
            diff.snapE.set(recB.gid + 1);
        }
        diff.timestamp = recE.timestamp;
        diff.isMergeable = recB.isMergeable;
        return diff;
    }
    /**
     * recB and recE must not be changed between calling
     * prepareWlogTransfer() and finishWlogTransfer().
     *
     * RETURN:
     *   true if there is remaining wlogs (that may be empty).
     */
    bool finishWlogTransfer(const MetaLsidGid &recB, const MetaLsidGid &recE, uint64_t lsidE) {
        const char *const FUNC = __func__;
        const MetaLsidGid recBx = getDoneRecord();
        verifyMetaLsidGidEquality(recB, recBx, FUNC);
        cybozu::util::QueueFile qf(queuePath().str(), O_RDWR);
        if (qf.empty()) {
            throw cybozu::Exception(FUNC)
                << "Maybe BUG: queue must have at lease one record.";
        }
        MetaLsidGid recEx;
        qf.back(recEx);
        verifyMetaLsidGidEquality(recE, recEx, FUNC);
        assert(recB.lsid <= lsidE && lsidE <= recE.lsid);

        MetaLsidGid recS;
        recS.lsid = lsidE;
        if (lsidE == recE.lsid) {
            recS.gid = recE.gid;
            recS.isMergeable = recE.isMergeable;
        } else {
            assert(recB.gid + 1 < recB.gid);
            recS.gid = recB.gid + 1;
            recS.isMergeable = true;
        }
        setDoneRecord(recS);
        if (recS.gid == recE.gid) qf.popBack();
        return !qf.empty();
    }
private:
    void loadWdevPath() {
        std::string s;
        util::loadFile(volDir_, "path", s);
        wdevPath_ = cybozu::FilePath(s);
    }
    void verifyWdevPathExistance() {
        if (!wdevPath_.stat().exists()) {
            throw cybozu::Exception("StorageVolInfo:not found") << wdevPath_.str();
        }
    }
    void verifyBaseDirExistance(const std::string &baseDirStr) {
        cybozu::FilePath baseDir(baseDirStr);
        cybozu::FileStat stat = baseDir.stat();
        if (!stat.exists()) {
            throw cybozu::Exception("StorageVolInfo:not exists") << baseDir.str();
        }
        if (!stat.isDirectory()) {
            throw cybozu::Exception("StorageVolInfo:not directory") << baseDir.str();
        }
    }
    void setDoneRecord(const MetaLsidGid &rec) {
        util::saveFile(volDir_, "done", rec);
    }
    MetaLsidGid getDoneRecord() const {
        MetaLsidGid rec;
        util::loadFile(volDir_, "done", rec);
        return rec;
    }
    cybozu::FilePath queuePath() const {
        return volDir_ + "queue";
    }
    uint64_t convertMibToPb(uint64_t mib) const {
        const uint32_t pbs = device::getWldev(getWdevName()).getPhysicalBlockSize();
        return mib * (MEBI / pbs);
    }
    uint64_t getMaxWlogSendPb(uint64_t maxWlogSendMb, const char *msg) const {
        const uint64_t maxWlogSendPb = convertMibToPb(maxWlogSendMb);
        if (maxWlogSendPb == 0) {
            throw cybozu::Exception(msg) << "maxWlogSendPb must be positive";
        }
        return maxWlogSendPb;
    }
    uint64_t takeSnapshotDetail(uint64_t maxWlogSendPb, bool isMergeable, cybozu::util::QueueFile& qf) {
        const char *const FUNC = __func__;
        MetaLsidGid pre;
        if (qf.empty()) {
            pre = getDoneRecord();
        } else {
            qf.front(pre);
            pre.verify();
        }
        const std::string wdevPath = wdevPath_.str();
        const uint64_t lsid = device::getPermanentLsid(wdevPath);
        if (device::isOverflow(wdevPath)) {
            throw cybozu::Exception(FUNC) << "wlog overflow" << wdevPath;
        }
        if (pre.lsid > lsid) {
            throw cybozu::Exception(FUNC) << "invalid lsid" << pre.lsid << lsid;
        }
        const uint64_t gid = pre.gid + 1 + (lsid - pre.lsid) / maxWlogSendPb;
        MetaLsidGid cur(lsid, gid, isMergeable, ::time(0));
        qf.pushFront(cur);
        qf.sync();
        LOGs.debug() << FUNC << cur;
        return gid;
    }
    static void verifyMetaLsidGidEquality(const MetaLsidGid &rec0, const MetaLsidGid &rec1, const char *msg) {
        if (rec0.lsid != rec1.lsid || rec0.gid != rec1.gid) {
            cybozu::Exception(msg) << "not equal lsid or gid" << rec0 << rec1;
        }
    }
};

} //namespace walb

// Copyright (c) 2014-2017 The Dash Core Developers
// Copyright (c) 2016-2017 Duality Blockchain Solutions Ltd
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DYNAMIC_DYNODEMAN_H
#define DYNAMIC_DYNODEMAN_H

#include "dynode.h"
#include "sync.h"

using namespace std;

class CDynodeMan;

extern CDynodeMan dnodeman;

/**
 * Provides a forward and reverse index between DN vin's and integers.
 *
 * This mapping is normally add-only and is expected to be permanent
 * It is only rebuilt if the size of the index exceeds the expected maximum number
 * of DN's and the current number of known DN's.
 *
 * The external interface to this index is provided via delegation by CDynodeMan
 */
class CDynodeIndex
{
public: // Types
    typedef std::map<CTxIn,int> index_m_t;

    typedef index_m_t::iterator index_m_it;

    typedef index_m_t::const_iterator index_m_cit;

    typedef std::map<int,CTxIn> rindex_m_t;

    typedef rindex_m_t::iterator rindex_m_it;

    typedef rindex_m_t::const_iterator rindex_m_cit;

private:
    int                  nSize;

    index_m_t            mapIndex;

    rindex_m_t           mapReverseIndex;

public:
    CDynodeIndex();

    int GetSize() const {
        return nSize;
    }

    /// Retrieve dynode vin by index
    bool Get(int nIndex, CTxIn& vinDynode) const;

    /// Get index of a dynode vin
    int GetDynodeIndex(const CTxIn& vinDynode) const;

    void AddDynodeVIN(const CTxIn& vinDynode);

    void Clear();

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(mapIndex);
        if(ser_action.ForRead()) {
            RebuildIndex();
        }
    }

private:
    void RebuildIndex();

};

class CDynodeMan
{
public:
    typedef std::map<CTxIn,int> index_m_t;

    typedef index_m_t::iterator index_m_it;

    typedef index_m_t::const_iterator index_m_cit;

private:
    static const int MAX_EXPECTED_INDEX_SIZE = 30000;

    /// Only allow 1 index rebuild per hour
    static const int64_t MIN_INDEX_REBUILD_TIME = 3600;

    static const std::string SERIALIZATION_VERSION_STRING;

    static const int PSEG_UPDATE_SECONDS        = 3 * 60 * 60;

    static const int LAST_PAID_SCAN_BLOCKS      = 100;

    static const int MIN_POSE_PROTO_VERSION     = 60800;
    static const int MAX_POSE_RANK              = 10;
    static const int MAX_POSE_BLOCKS            = 10;


    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    // Keep track of current block index
    const CBlockIndex *pCurrentBlockIndex;

    // map to hold all DN's
    std::vector<CDynode> vDynodes;
    // who's asked for the Dynode list and the last time
    std::map<CNetAddr, int64_t> mAskedUsForDynodeList;
    // who we asked for the Dynode list and the last time
    std::map<CNetAddr, int64_t> mWeAskedForDynodeList;
    // which Dynodes we've asked for
    std::map<COutPoint, int64_t> mWeAskedForDynodeListEntry;
    // who we asked for the Dynode verification
    std::map<CNetAddr, CDynodeVerification> mWeAskedForVerification;

    int64_t nLastIndexRebuildTime;

    CDynodeIndex indexDynodes;

    CDynodeIndex indexDynodesOld;

    /// Set when index has been rebuilt, clear when read
    bool fIndexRebuilt;

    /// Set when Dynodes are added, cleared when CGovernanceManager is notified
    bool fDynodesAdded;

    /// Set when Dynodes are removed, cleared when CGovernanceManager is notified
    bool fDynodesRemoved;

    std::vector<uint256> vecDirtyGovernanceObjectHashes;

    int64_t nLastWatchdogVoteTime;

    friend class CDynodeSync;

public:
    // Keep track of all broadcasts I've seen
    std::map<uint256, CDynodeBroadcast> mapSeenDynodeBroadcast;
    // Keep track of all pings I've seen
    std::map<uint256, CDynodePing> mapSeenDynodePing;
    // Keep track of all verifications I've seen
    std::map<uint256, CDynodeVerification> mapSeenDynodeVerification;
    // keep track of psq count to prevent Dynodes from gaming privatesend queue
    int64_t nPsqCount;


    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        LOCK(cs);
        std::string strVersion;
        if(ser_action.ForRead()) {
            READWRITE(strVersion);
        }
        else {
            strVersion = SERIALIZATION_VERSION_STRING; 
            READWRITE(strVersion);
        }

        READWRITE(vDynodes);
        READWRITE(mAskedUsForDynodeList);
        READWRITE(mWeAskedForDynodeList);
        READWRITE(mWeAskedForDynodeListEntry);
        READWRITE(nLastWatchdogVoteTime);
        READWRITE(nPsqCount);

        READWRITE(mapSeenDynodeBroadcast);
        READWRITE(mapSeenDynodePing);
        READWRITE(indexDynodes);
        if(ser_action.ForRead() && (strVersion != SERIALIZATION_VERSION_STRING)) {
            Clear();
        }
    }

    CDynodeMan();

    /// Add an entry
    bool Add(CDynode &dn);

    /// Ask (source) node for dnb
    void AskForDN(CNode *pnode, const CTxIn &vin);

    /// Check all Dynodes
    void Check();

    /// Check all Dynodes and remove inactive
    void CheckAndRemove();

    /// Clear Dynode vector
    void Clear();

    /// Count Dynodes filtered by nProtocolVersion.
    /// Dynode nProtocolVersion should match or be above the one specified in param here.
    int CountDynodes(int nProtocolVersion = -1);
    /// Count enabled Dynodes filtered by nProtocolVersion.
    /// Dynode nProtocolVersion should match or be above the one specified in param here.
    int CountEnabled(int nProtocolVersion = -1);

    /// Count Dynodes by network type - NET_IPV4, NET_IPV6, NET_TOR
    // int CountByIP(int nNetworkType);

    void PSEGUpdate(CNode* pnode);

    /// Find an entry
    CDynode* Find(const CScript &payee);
    CDynode* Find(const CTxIn& vin);
    CDynode* Find(const CPubKey& pubKeyDynode);

    /// Versions of Find that are safe to use from outside the class
    bool Get(const CPubKey& pubKeyDynode, CDynode& dynode);
    bool Get(const CTxIn& vin, CDynode& dynode);

    /// Retrieve dynode vin by index
    bool Get(int nIndex, CTxIn& vinDynode, bool& fIndexRebuiltOut) {
        LOCK(cs);
        fIndexRebuiltOut = fIndexRebuilt;
        return indexDynodes.Get(nIndex, vinDynode);
    }

    bool GetIndexRebuiltFlag() {
        LOCK(cs);
        return fIndexRebuilt;
    }

    /// Get index of a dynode vin
    int GetDynodeIndex(const CTxIn& vinDynode) {
        LOCK(cs);
        return indexDynodes.GetDynodeIndex(vinDynode);
    }

    /// Get old index of a dynode vin
    int GetDynodeIndexOld(const CTxIn& vinDynode) {
        LOCK(cs);
        return indexDynodesOld.GetDynodeIndex(vinDynode);
    }

    /// Get dynode VIN for an old index value
    bool GetDynodeVinForIndexOld(int nDynodeIndex, CTxIn& vinDynodeOut) {
        LOCK(cs);
        return indexDynodesOld.Get(nDynodeIndex, vinDynodeOut);
    }

    /// Get index of a dynode vin, returning rebuild flag
    int GetDynodeIndex(const CTxIn& vinDynode, bool& fIndexRebuiltOut) {
        LOCK(cs);
        fIndexRebuiltOut = fIndexRebuilt;
        return indexDynodes.GetDynodeIndex(vinDynode);
    }

    void ClearOldDynodeIndex() {
        LOCK(cs);
        indexDynodesOld.Clear();
        fIndexRebuilt = false;
    }

    bool Has(const CTxIn& vin);

    dynode_info_t GetDynodeInfo(const CTxIn& vin);

    dynode_info_t GetDynodeInfo(const CPubKey& pubKeyDynode);

    /// Find an entry in the dynode list that is next to be paid
    CDynode* GetNextDynodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCount);

    /// Find a random entry
    CDynode* FindRandomNotInVec(const std::vector<CTxIn> &vecToExclude, int nProtocolVersion = -1);

    std::vector<CDynode> GetFullDynodeVector() { Check(); return vDynodes; }

    std::vector<std::pair<int, CDynode> > GetDynodeRanks(int nBlockHeight = -1, int nMinProtocol=0);
    int GetDynodeRank(const CTxIn &vin, int nBlockHeight, int nMinProtocol=0, bool fOnlyActive=true);
    CDynode* GetDynodeByRank(int nRank, int nBlockHeight, int nMinProtocol=0, bool fOnlyActive=true);

    void ProcessDynodeConnections();

    void ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);

    void DoFullVerificationStep();
    void CheckSameAddr();
    bool SendVerifyRequest(const CAddress& addr, const std::vector<CDynode*>& vSortedByAddr);
    void SendVerifyReply(CNode* pnode, CDynodeVerification& dnv);
    void ProcessVerifyReply(CNode* pnode, CDynodeVerification& dnv);
    void ProcessVerifyBroadcast(CNode* pnode, const CDynodeVerification& dnv);

    /// Return the number of (unique) Dynodes
    int size() { return vDynodes.size(); }

    std::string ToString() const;

    int GetEstimatedDynodes(int nBlock);

    /// Update dynode list and maps using provided CDynodeBroadcast
    void UpdateDynodeList(CDynodeBroadcast dnb);
    /// Perform complete check and only then update list and maps
    bool CheckDnbAndUpdateDynodeList(CDynodeBroadcast dnb, int& nDos);

    void UpdateLastPaid(const CBlockIndex *pindex);

    void CheckAndRebuildDynodeIndex();

    void AddDirtyGovernanceObjectHash(const uint256& nHash)
    {
        LOCK(cs);
        vecDirtyGovernanceObjectHashes.push_back(nHash);
    }

    std::vector<uint256> GetAndClearDirtyGovernanceObjectHashes()
    {
        LOCK(cs);
        std::vector<uint256> vecTmp = vecDirtyGovernanceObjectHashes;
        vecDirtyGovernanceObjectHashes.clear();
        return vecTmp;;
    }

    bool IsWatchdogActive();
    void UpdateWatchdogVoteTime(const CTxIn& vin);
    void AddGovernanceVote(const CTxIn& vin, uint256 nGovernanceObjectHash);
    void RemoveGovernanceObject(uint256 nGovernanceObjectHash);

    void CheckDynode(const CTxIn& vin, bool fForce = false);
    void CheckDynode(const CPubKey& pubKeyDynode, bool fForce = false);

    int GetDynodeState(const CTxIn& vin);
    int GetDynodeState(const CPubKey& pubKeyDynode);

    bool IsDynodePingedWithin(const CTxIn& vin, int nSeconds, int64_t nTimeToCheckAt = -1);
    void SetDynodeLastPing(const CTxIn& vin, const CDynodePing& dnp);

    void UpdatedBlockTip(const CBlockIndex *pindex);

    /**
     * Called to notify CGovernanceManager that the Dynode index has been updated.
     * Must be called while not holding the CDynodeMan::cs mutex
     */
    void NotifyDynodeUpdates();

};

#endif // DYNAMIC_DYNODEMAN_H

// Copyright (c) 2014-2017 The Dash Core Developers
// Copyright (c) 2016-2017 Duality Blockchain Solutions Ltd
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "dynodeman.h"
#include "activedynode.h"
#include "privatesend.h"
#include "governance.h"
#include "dynode.h"
#include "dynode-payments.h"
#include "dynode-sync.h"
#include "netfulfilledman.h"
#include "util.h"
#include "addrman.h"
#include "spork.h"
#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>

/** Dynode manager */
CDynodeMan dnodeman;

const std::string CDynodeMan::SERIALIZATION_VERSION_STRING = "CDynodeMan-Version-1";

struct CompareLastPaidBlock
{
    bool operator()(const std::pair<int, CDynode*>& t1,
                    const std::pair<int, CDynode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->vin < t2.second->vin);
    }
};

struct CompareScoreDN
{
    bool operator()(const std::pair<int64_t, CDynode*>& t1,
                    const std::pair<int64_t, CDynode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->vin < t2.second->vin);
    }
};

CDynodeIndex::CDynodeIndex()
    : nSize(0),
      mapIndex(),
      mapReverseIndex()
{}

bool CDynodeIndex::Get(int nIndex, CTxIn& vinDynode) const
{
    rindex_m_cit it = mapReverseIndex.find(nIndex);
    if(it == mapReverseIndex.end()) {
        return false;
    }
    vinDynode = it->second;
    return true;
}

int CDynodeIndex::GetDynodeIndex(const CTxIn& vinDynode) const
{
    index_m_cit it = mapIndex.find(vinDynode);
    if(it == mapIndex.end()) {
        return -1;
    }
    return it->second;
}

void CDynodeIndex::AddDynodeVIN(const CTxIn& vinDynode)
{
    index_m_it it = mapIndex.find(vinDynode);
    if(it != mapIndex.end()) {
        return;
    }
    int nNextIndex = nSize;
    mapIndex[vinDynode] = nNextIndex;
    mapReverseIndex[nNextIndex] = vinDynode;
    ++nSize;
}

void CDynodeIndex::Clear()
{
    mapIndex.clear();
    mapReverseIndex.clear();
    nSize = 0;
}
struct CompareByAddr

{
    bool operator()(const CDynode* t1,
                    const CDynode* t2) const
    {
        return t1->addr < t2->addr;
    }
};

void CDynodeIndex::RebuildIndex()
{
    nSize = mapIndex.size();
    for(index_m_it it = mapIndex.begin(); it != mapIndex.end(); ++it) {
        mapReverseIndex[it->second] = it->first;
    }
}

CDynodeMan::CDynodeMan()
: cs(),
  vDynodes(),
  mAskedUsForDynodeList(),
  mWeAskedForDynodeList(),
  mWeAskedForDynodeListEntry(),
  nLastIndexRebuildTime(0),
  indexDynodes(),
  indexDynodesOld(),
  fIndexRebuilt(false),
  fDynodesAdded(false),
  fDynodesRemoved(false),
  vecDirtyGovernanceObjectHashes(),
  nLastWatchdogVoteTime(0),
  mapSeenDynodeBroadcast(),
  mapSeenDynodePing(),
  nSsqCount(0)
{}

bool CDynodeMan::Add(CDynode &dn)
{
    LOCK(cs);

    if (!dn.IsEnabled() && !dn.IsPreEnabled())
        return false;

    CDynode *psn = Find(dn.vin);
    if (psn == NULL) {
        LogPrint("dynode", "CDynodeMan::Add -- Adding new Dynode: addr=%s, %i now\n", dn.addr.ToString(), size() + 1);
        dn.nTimeLastWatchdogVote = dn.sigTime;
        vDynodes.push_back(dn);
        indexDynodes.AddDynodeVIN(dn.vin);
        fDynodesAdded = true;
        return true;
    }

    return false;
}

void CDynodeMan::AskForDN(CNode* pnode, const CTxIn &vin)
{
    if(!pnode) return;

    std::map<COutPoint, int64_t>::iterator it = mWeAskedForDynodeListEntry.find(vin.prevout);
    if (it != mWeAskedForDynodeListEntry.end() && GetTime() < (*it).second) {
        // we've asked recently, should not repeat too often or we could get banned
        return;
    }

    // ask for the dnb info once from the node that sent snp

    LogPrintf("CDynodeMan::AskForDN -- Asking node for missing dynode entry: %s\n", vin.prevout.ToStringShort());
    pnode->PushMessage(NetMsgType::DSEG, vin);
    mWeAskedForDynodeListEntry[vin.prevout] = GetTime() + DSEG_UPDATE_SECONDS;;
}

void CDynodeMan::Check()
{
    LOCK(cs);

    LogPrint("dynode", "CDynodeMan::Check nLastWatchdogVoteTime = %d, IsWatchdogActive() = %d\n", nLastWatchdogVoteTime, IsWatchdogActive());

    BOOST_FOREACH(CDynode& dn, vDynodes) {
        dn.Check();
    }
}

void CDynodeMan::CheckAndRemove()
{
    LogPrintf("CDynodeMan::CheckAndRemove\n");

    Check();

    {
        LOCK(cs);

        // Remove inactive and outdated dynodes
        std::vector<CDynode>::iterator it = vDynodes.begin();
        while(it != vDynodes.end()) {
            bool fRemove =  // If it's marked to be removed from the list by CDynode::Check for whatever reason ...
                    (*it).nActiveState == CDynode::DYNODE_REMOVE ||
                    // or collateral was spent ...
                    (*it).nActiveState == CDynode::DYNODE_OUTPOINT_SPENT;

            if (fRemove) {
                LogPrint("dynode", "CDynodeMan::CheckAndRemove -- Removing Dynode: %s  addr=%s  %i now\n", (*it).GetStatus(), (*it).addr.ToString(), size() - 1);

                // erase all of the broadcasts we've seen from this txin, ...
                mapSeenDynodeBroadcast.erase(CDynodeBroadcast(*it).GetHash());
                // allow us to ask for this dynode again if we see another ping ...
                mWeAskedForDynodeListEntry.erase((*it).vin.prevout);

                // and finally remove it from the list
                it = vDynodes.erase(it);
                fDynodesRemoved = true;
            } else {
                ++it;
            }
        }

        // check who's asked for the Dynode list
        std::map<CNetAddr, int64_t>::iterator it1 = mAskedUsForDynodeList.begin();
        while(it1 != mAskedUsForDynodeList.end()){
            if((*it1).second < GetTime()) {
                mAskedUsForDynodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check who we asked for the Dynode list
        it1 = mWeAskedForDynodeList.begin();
        while(it1 != mWeAskedForDynodeList.end()){
            if((*it1).second < GetTime()){
                mWeAskedForDynodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check which Dynodes we've asked for
        std::map<COutPoint, int64_t>::iterator it2 = mWeAskedForDynodeListEntry.begin();
        while(it2 != mWeAskedForDynodeListEntry.end()){
            if((*it2).second < GetTime()){
                mWeAskedForDynodeListEntry.erase(it2++);
            } else {
                ++it2;
            }
        }

        std::map<CNetAddr, CDynodeVerification>::iterator itv1 = mWeAskedForVerification.begin();
        while(itv1 != mWeAskedForVerification.end()){
            if(itv1->second.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS) {
                mWeAskedForVerification.erase(itv1++);
            } else {
                ++itv1;
            }
        }

        // remove expired mapSeenDynodeBroadcast
        std::map<uint256, CDynodeBroadcast>::iterator it3 = mapSeenDynodeBroadcast.begin();
        while(it3 != mapSeenDynodeBroadcast.end()){
            if((*it3).second.lastPing.sigTime < GetTime() - DYNODE_REMOVAL_SECONDS*2){
                LogPrint("dynode", "CDynodeMan::CheckAndRemove -- Removing expired Dynode broadcast: hash=%s\n", (*it3).second.GetHash().ToString());
                mapSeenDynodeBroadcast.erase(it3++);
            } else {
                ++it3;
            }
        }

        // remove expired mapSeenDynodePing
        std::map<uint256, CDynodePing>::iterator it4 = mapSeenDynodePing.begin();
        while(it4 != mapSeenDynodePing.end()){
            if((*it4).second.sigTime < GetTime() - DYNODE_REMOVAL_SECONDS*2){
                LogPrint("dynode", "CDynodeMan::CheckAndRemove -- Removing expired Dynode ping: hash=%s\n", (*it4).second.GetHash().ToString());
                mapSeenDynodePing.erase(it4++);
            } else {
                ++it4;
            }
        }

        // remove expired mapSeenDynodeVerification
        std::map<uint256, CDynodeVerification>::iterator itv2 = mapSeenDynodeVerification.begin();
        while(itv2 != mapSeenDynodeVerification.end()){
            if((*itv2).second.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS){
                LogPrint("dynode", "CDynodeMan::CheckAndRemove -- Removing expired Dynode verification: hash=%s\n", (*itv2).first.ToString());
                mapSeenDynodeVerification.erase(itv2++);
            } else {
                ++itv2;
            }
        }

        LogPrintf("CDynodeMan::CheckAndRemove -- %s\n", ToString());

        if(fDynodesRemoved) {
            CheckAndRebuildDynodeIndex();
        }
    }

    if(fDynodesRemoved) {
        NotifyDynodeUpdates();
    }
}

void CDynodeMan::Clear()
{
    LOCK(cs);
    vDynodes.clear();
    mAskedUsForDynodeList.clear();
    mWeAskedForDynodeList.clear();
    mWeAskedForDynodeListEntry.clear();
    mapSeenDynodeBroadcast.clear();
    mapSeenDynodePing.clear();
    nSsqCount = 0;
    nLastWatchdogVoteTime = 0;
    indexDynodes.Clear();
    indexDynodesOld.Clear();
}

int CDynodeMan::CountDynodes(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? dnpayments.GetMinDynodePaymentsProto() : nProtocolVersion;

    BOOST_FOREACH(CDynode& dn, vDynodes) {
        if(dn.nProtocolVersion < nProtocolVersion) continue;
        nCount++;
    }

    return nCount;
}

int CDynodeMan::CountEnabled(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? dnpayments.GetMinDynodePaymentsProto() : nProtocolVersion;

    BOOST_FOREACH(CDynode& dn, vDynodes) {
        dn.Check();
        if(dn.nProtocolVersion < nProtocolVersion || !dn.IsEnabled()) continue;
        nCount++;
    }

    return nCount;
}

/* Only IPv4 dynodes are allowed in 12.1, saving this for later
int CDynodeMan::CountByIP(int nNetworkType)
{
    LOCK(cs);
    int nNodeCount = 0;

    BOOST_FOREACH(CDynode& dn, vDynodes)
        if ((nNetworkType == NET_IPV4 && dn.addr.IsIPv4()) ||
            (nNetworkType == NET_TOR  && dn.addr.IsTor())  ||
            (nNetworkType == NET_IPV6 && dn.addr.IsIPv6())) {
                nNodeCount++;
        }

    return nNodeCount;
}
*/

void CDynodeMan::SsegUpdate(CNode* pnode)
{
    LOCK(cs);

    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(!(pnode->addr.IsRFC1918() || pnode->addr.IsLocal())) {
            std::map<CNetAddr, int64_t>::iterator it = mWeAskedForDynodeList.find(pnode->addr);
            if(it != mWeAskedForDynodeList.end() && GetTime() < (*it).second) {
                LogPrintf("CDynodeMan::SsegUpdate -- we already asked %s for the list; skipping...\n", pnode->addr.ToString());
                return;
            }
        }
    }
    
    pnode->PushMessage(NetMsgType::DSEG, CTxIn());
    int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
    mWeAskedForDynodeList[pnode->addr] = askAgain;

    LogPrint("dynode", "CDynodeMan::SsegUpdate -- asked %s for the list\n", pnode->addr.ToString());
}

CDynode* CDynodeMan::Find(const CScript &payee)
{
    LOCK(cs);

    BOOST_FOREACH(CDynode& dn, vDynodes)
    {
        if(GetScriptForDestination(dn.pubKeyCollateralAddress.GetID()) == payee)
            return &dn;
    }
    return NULL;
}

CDynode* CDynodeMan::Find(const CTxIn &vin)
{
    LOCK(cs);

    BOOST_FOREACH(CDynode& dn, vDynodes)
    {
        if(dn.vin.prevout == vin.prevout)
            return &dn;
    }
    return NULL;
}

CDynode* CDynodeMan::Find(const CPubKey &pubKeyDynode)
{
    LOCK(cs);

    BOOST_FOREACH(CDynode& dn, vDynodes)
    {
        if(dn.pubKeyDynode == pubKeyDynode)
            return &dn;
    }
    return NULL;
}

bool CDynodeMan::Get(const CPubKey& pubKeyDynode, CDynode& dynode)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    CDynode* pDN = Find(pubKeyDynode);
    if(!pDN)  {
        return false;
    }
    dynode = *pDN;
    return true;
}

bool CDynodeMan::Get(const CTxIn& vin, CDynode& dynode)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    CDynode* pDN = Find(vin);
    if(!pDN)  {
        return false;
    }
    dynode = *pDN;
    return true;
}

dynode_info_t CDynodeMan::GetDynodeInfo(const CTxIn& vin)
{
    dynode_info_t info;
    LOCK(cs);
    CDynode* pDN = Find(vin);
    if(!pDN)  {
        return info;
    }
    info = pDN->GetInfo();
    return info;
}

dynode_info_t CDynodeMan::GetDynodeInfo(const CPubKey& pubKeyDynode)
{
    dynode_info_t info;
    LOCK(cs);
    CDynode* pDN = Find(pubKeyDynode);
    if(!pDN)  {
        return info;
    }
    info = pDN->GetInfo();
    return info;
}

bool CDynodeMan::Has(const CTxIn& vin)
{
    LOCK(cs);
    CDynode* pDN = Find(vin);
    return (pDN != NULL);
}

//
// Deterministically select the oldest/best dynode to pay on the network
//
CDynode* CDynodeMan::GetNextDynodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCount)
{
    // Need LOCK2 here to ensure consistent locking order because the GetBlockHash call below locks cs_main
    LOCK2(cs_main,cs);

    CDynode *pBestDynode = NULL;
    std::vector<std::pair<int, CDynode*> > vecDynodeLastPaid;

    /*
        Make a vector with all of the last paid times
    */

    int nSnCount = CountEnabled();
    BOOST_FOREACH(CDynode &dn, vDynodes)
    {
        dn.Check();
        if(!dn.IsEnabled()) continue;

        // //check protocol version
        if(dn.nProtocolVersion < dnpayments.GetMinDynodePaymentsProto()) continue;

        //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
        if(dnpayments.IsScheduled(dn, nBlockHeight)) continue;

        //it's too new, wait for a cycle
        if(fFilterSigTime && dn.sigTime + (nSnCount*2.6*60) > GetAdjustedTime()) continue;

        //make sure it has at least as many confirmations as there are dynodes
        if(dn.GetCollateralAge() < nSnCount) continue;

        vecDynodeLastPaid.push_back(std::make_pair(dn.GetLastPaidBlock(), &dn));
    }

    nCount = (int)vecDynodeLastPaid.size();

    //when the network is in the process of upgrading, don't penalize nodes that recently restarted
    if(fFilterSigTime && nCount < nSnCount/3) return GetNextDynodeInQueueForPayment(nBlockHeight, false, nCount);

    // Sort them low to high
    sort(vecDynodeLastPaid.begin(), vecDynodeLastPaid.end(), CompareLastPaidBlock());

    uint256 blockHash;
    if(!GetBlockHash(blockHash, nBlockHeight - 101)) {
        LogPrintf("CDynode::GetNextDynodeInQueueForPayment -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight - 101);
        return NULL;
    }
    // Look at 1/10 of the oldest nodes (by last payment), calculate their scores and pay the best one
    //  -- This doesn't look at who is being paid in the +8-10 blocks, allowing for double payments very rarely
    //  -- 1/100 payments should be a double payment on mainnet - (1/(3000/10))*2
    //  -- (chance per block * chances before IsScheduled will fire)
    int nTenthNetwork = CountEnabled()/10;
    int nCountTenth = 0;
    arith_uint256 nHighest = 0;
    BOOST_FOREACH (PAIRTYPE(int, CDynode*)& s, vecDynodeLastPaid){
        arith_uint256 nScore = s.second->CalculateScore(blockHash);
        if(nScore > nHighest){
            nHighest = nScore;
            pBestDynode = s.second;
        }
        nCountTenth++;
        if(nCountTenth >= nTenthNetwork) break;
    }
    return pBestDynode;
}

CDynode* CDynodeMan::FindRandomNotInVec(const std::vector<CTxIn> &vecToExclude, int nProtocolVersion)
{
    LOCK(cs);

    nProtocolVersion = nProtocolVersion == -1 ? dnpayments.GetMinDynodePaymentsProto() : nProtocolVersion;

    int nCountEnabled = CountEnabled(nProtocolVersion);
    int nCountNotExcluded = nCountEnabled - vecToExclude.size();

    LogPrintf("CDynodeMan::FindRandomNotInVec -- %d enabled dynodes, %d dynodes to choose from\n", nCountEnabled, nCountNotExcluded);
    if(nCountNotExcluded < 1) return NULL;

    // fill a vector of pointers
    std::vector<CDynode*> vpDynodesShuffled;
    BOOST_FOREACH(CDynode &dn, vDynodes) {
        vpDynodesShuffled.push_back(&dn);
    }

    InsecureRand insecureRand;

    // shuffle pointers
    std::random_shuffle(vpDynodesShuffled.begin(), vpDynodesShuffled.end(), insecureRand);
    bool fExclude;

    // loop through
    BOOST_FOREACH(CDynode* psn, vpDynodesShuffled) {
        if(psn->nProtocolVersion < nProtocolVersion || !psn->IsEnabled()) continue;
        fExclude = false;
        BOOST_FOREACH(const CTxIn &txinToExclude, vecToExclude) {
            if(psn->vin.prevout == txinToExclude.prevout) {
                fExclude = true;
                break;
            }
        }
        if(fExclude) continue;
        // found the one not in vecToExclude
        LogPrint("dynode", "CDynodeMan::FindRandomNotInVec -- found, dynode=%s\n", psn->vin.prevout.ToStringShort());
        return psn;
    }

    LogPrint("dynode", "CDynodeMan::FindRandomNotInVec -- failed\n");
    return NULL;
}

int CDynodeMan::GetDynodeRank(const CTxIn& vin, int nBlockHeight, int nMinProtocol, bool fOnlyActive)
{
    std::vector<std::pair<int64_t, CDynode*> > vecDynodeScores;

    //make sure we know about this block
    uint256 blockHash = uint256();
    if(!GetBlockHash(blockHash, nBlockHeight)) return -1;

    LOCK(cs);

    // scan for winner
    BOOST_FOREACH(CDynode& dn, vDynodes) {
        if(dn.nProtocolVersion < nMinProtocol) continue;
        if(fOnlyActive) {
            dn.Check();
            if(!dn.IsEnabled()) continue;
        }
        int64_t nScore = dn.CalculateScore(blockHash).GetCompact(false);

        vecDynodeScores.push_back(std::make_pair(nScore, &dn));
    }

    sort(vecDynodeScores.rbegin(), vecDynodeScores.rend(), CompareScoreDN());

    int nRank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CDynode*)& scorePair, vecDynodeScores) {
        nRank++;
        if(scorePair.second->vin.prevout == vin.prevout) return nRank;
    }

    return -1;
}

std::vector<std::pair<int, CDynode> > CDynodeMan::GetDynodeRanks(int nBlockHeight, int nMinProtocol)
{
    std::vector<std::pair<int64_t, CDynode*> > vecDynodeScores;
    std::vector<std::pair<int, CDynode> > vecDynodeRanks;

    //make sure we know about this block
    uint256 blockHash = uint256();
    if(!GetBlockHash(blockHash, nBlockHeight)) return vecDynodeRanks;

    LOCK(cs);

    // scan for winner
    BOOST_FOREACH(CDynode& dn, vDynodes) {

        dn.Check();

        if(dn.nProtocolVersion < nMinProtocol || !dn.IsEnabled()) continue;

        int64_t nScore = dn.CalculateScore(blockHash).GetCompact(false);

        vecDynodeScores.push_back(std::make_pair(nScore, &dn));
    }

    sort(vecDynodeScores.rbegin(), vecDynodeScores.rend(), CompareScoreDN());

    int nRank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CDynode*)& s, vecDynodeScores) {
        nRank++;
        vecDynodeRanks.push_back(std::make_pair(nRank, *s.second));
    }

    return vecDynodeRanks;
}

CDynode* CDynodeMan::GetDynodeByRank(int nRank, int nBlockHeight, int nMinProtocol, bool fOnlyActive)
{
    std::vector<std::pair<int64_t, CDynode*> > vecDynodeScores;

    LOCK(cs);

    uint256 blockHash;
    if(!GetBlockHash(blockHash, nBlockHeight)) {
        LogPrintf("CDynode::GetDynodeByRank -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight);
        return NULL;
    }

    // Fill scores
    BOOST_FOREACH(CDynode& dn, vDynodes) {

        if(dn.nProtocolVersion < nMinProtocol) continue;
        if(fOnlyActive) {
            dn.Check();
            if(!dn.IsEnabled()) continue;
        }

        int64_t nScore = dn.CalculateScore(blockHash).GetCompact(false);

        vecDynodeScores.push_back(std::make_pair(nScore, &dn));
    }

    sort(vecDynodeScores.rbegin(), vecDynodeScores.rend(), CompareScoreDN());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CDynode*)& s, vecDynodeScores){
        rank++;
        if(rank == nRank) {
            return s.second;
        }
    }

    return NULL;
}

void CDynodeMan::ProcessDynodeConnections()
{
    //we don't care about this for regtest
    if(Params().NetworkIDString() == CBaseChainParams::REGTEST) return;

    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes) {
        if(pnode->fDynode) {
            if(privateSendPool.pSubmittedToDynode != NULL && pnode->addr == privateSendPool.pSubmittedToDynode->addr) continue;
            LogPrintf("Closing Dynode connection: peer=%d, addr=%s\n", pnode->id, pnode->addr.ToString());
            pnode->fDisconnect = true;
        }
    }
}

void CDynodeMan::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if(fLiteMode) return; // disable all Dynamic specific functionality
    if(!dynodeSync.IsBlockchainSynced()) return;

    if (strCommand == NetMsgType::DNANNOUNCE) { //Dynode Broadcast

        {
            LOCK(cs);

            CDynodeBroadcast dnb;
            vRecv >> dnb;

            int nDos = 0;

            if (CheckSnbAndUpdateDynodeList(dnb, nDos)) {
                // use announced Dynode as a peer
                addrman.Add(CAddress(dnb.addr), pfrom->addr, 2*60*60);
            } else if(nDos > 0) {
                Misbehaving(pfrom->GetId(), nDos);
            }
        }
        if(fDynodesAdded) {
            NotifyDynodeUpdates();
        }
    } else if (strCommand == NetMsgType::DNPING) { //Dynode Ping
        // ignore dynode pings until dynode list is synced
        if (!dynodeSync.IsDynodeListSynced()) return;

        CDynodePing snp;
        vRecv >> snp;

        LogPrint("dynode", "DNPING -- Dynode ping, dynode=%s\n", snp.vin.prevout.ToStringShort());

        LOCK(cs);

        if(mapSeenDynodePing.count(snp.GetHash())) return; //seen
        mapSeenDynodePing.insert(std::make_pair(snp.GetHash(), snp));

        LogPrint("dynode", "DNPING -- Dynode ping, dynode=%s new\n", snp.vin.prevout.ToStringShort());

        int nDos = 0;
        if(snp.CheckAndUpdate(nDos, false)) return;

        if(nDos > 0) {
            // if anything significant failed, mark that node
            Misbehaving(pfrom->GetId(), nDos);
        } else {
            // if nothing significant failed, search existing Dynode list
            CDynode* psn = Find(snp.vin);
            // if it's known, don't ask for the dnb, just return
            if(psn != NULL) return;
        }

        // something significant is broken or dn is unknown,
        // we might have to ask for a dynode entry once
        AskForDN(pfrom, snp.vin);

    } else if (strCommand == NetMsgType::DSEG) { //Get Dynode list or specific entry
        // Ignore such requests until we are fully synced.
        // We could start processing this after dynode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!dynodeSync.IsSynced()) return;

        CTxIn vin;
        vRecv >> vin;

        LogPrint("dynode", "DSEG -- Dynode list, dynode=%s\n", vin.prevout.ToStringShort());

        LOCK(cs);

        if(vin == CTxIn()) { //only should ask for this once
            //local network
            bool isLocal = (pfrom->addr.IsRFC1918() || pfrom->addr.IsLocal());

            if(!isLocal && Params().NetworkIDString() == CBaseChainParams::MAIN) {
                std::map<CNetAddr, int64_t>::iterator i = mAskedUsForDynodeList.find(pfrom->addr);
                if (i != mAskedUsForDynodeList.end()){
                    int64_t t = (*i).second;
                    if (GetTime() < t) {
                        Misbehaving(pfrom->GetId(), 34);
                        LogPrintf("DSEG -- peer already asked me for the list, peer=%d\n", pfrom->id);
                        return;
                    }
                }
                int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
                mAskedUsForDynodeList[pfrom->addr] = askAgain;
            }
        } //else, asking for a specific node which is ok

        int nInvCount = 0;

        BOOST_FOREACH(CDynode& dn, vDynodes) {
            if (vin != CTxIn() && vin != dn.vin) continue; // asked for specific vin but we are not there yet
            if (dn.addr.IsRFC1918() || dn.addr.IsLocal()) continue; // do not send local network dynode

            LogPrint("dynode", "DSEG -- Sending Dynode entry: dynode=%s  addr=%s\n", dn.vin.prevout.ToStringShort(), dn.addr.ToString());
            CDynodeBroadcast dnb = CDynodeBroadcast(dn);
            uint256 hash = dnb.GetHash();
            pfrom->PushInventory(CInv(MSG_DYNODE_ANNOUNCE, hash));
            nInvCount++;

            if (!mapSeenDynodeBroadcast.count(hash)) {
                mapSeenDynodeBroadcast.insert(std::make_pair(hash, dnb));
            }

            if (vin == dn.vin) {
                LogPrintf("DSEG -- Sent 1 Dynode inv to peer %d\n", pfrom->id);
                return;
            }
        }

        if(vin == CTxIn()) {
            pfrom->PushMessage(NetMsgType::SYNCSTATUSCOUNT, DYNODE_SYNC_LIST, nInvCount);
            LogPrintf("DSEG -- Sent %d Dynode invs to peer %d\n", nInvCount, pfrom->id);
            return;
        }
        // smth weird happen - someone asked us for vin we have no idea about?
        LogPrint("dynode", "DSEG -- No invs sent to peer %d\n", pfrom->id);

    } else if (strCommand == NetMsgType::DNVERIFY) { // Dynode Verify

        LOCK(cs);

        CDynodeVerification snv;
        vRecv >> snv;

        if(snv.vchSig1.empty()) {
            // CASE 1: someone asked me to verify myself /IP we are using/
            SendVerifyReply(pfrom, snv);
        } else if (snv.vchSig2.empty()) {
            // CASE 2: we _probably_ got verification we requested from some dynode
            ProcessVerifyReply(pfrom, snv);
        } else {
            // CASE 3: we _probably_ got verification broadcast signed by some dynode which verified another one
            ProcessVerifyBroadcast(pfrom, snv);
        }
    }
}

// Verification of dynode via unique direct requests.

void CDynodeMan::DoFullVerificationStep()
{
    if(activeDynode.vin == CTxIn()) return;

    std::vector<std::pair<int, CDynode> > vecDynodeRanks = GetDynodeRanks(pCurrentBlockIndex->nHeight - 1, MIN_POSE_PROTO_VERSION);

    LOCK(cs);

    int nCount = 0;
    int nCountMax = std::max(10, (int)vDynodes.size() / 100); // verify at least 10 dynode at once but at most 1% of all known dynodes

    int nMyRank = -1;
    int nRanksTotal = (int)vecDynodeRanks.size();

    // send verify requests only if we are in top MAX_POSE_RANK
    std::vector<std::pair<int, CDynode> >::iterator it = vecDynodeRanks.begin();
    while(it != vecDynodeRanks.end()) {
        if(it->first > MAX_POSE_RANK) {
            LogPrint("dynode", "CDynodeMan::DoFullVerificationStep -- Must be in top %d to send verify request\n",
                        (int)MAX_POSE_RANK);
            return;
        }
        if(it->second.vin == activeDynode.vin) {
            nMyRank = it->first;
            LogPrint("dynode", "CDynodeMan::DoFullVerificationStep -- Found self at rank %d/%d, verifying up to %d dynodes\n",
                        nMyRank, nRanksTotal, nCountMax);
            break;
        }
        ++it;
    }

    // edge case: list is too short and this dynode is not enabled
    if(nMyRank == -1) return;

    // send verify requests to up to nCountMax dynodes starting from
    // (MAX_POSE_RANK + nCountMax * (nMyRank - 1) + 1)
    int nOffset = MAX_POSE_RANK + nCountMax * (nMyRank - 1);
    if(nOffset >= (int)vecDynodeRanks.size()) return;

    std::vector<CDynode*> vSortedByAddr;
    BOOST_FOREACH(CDynode& dn, vDynodes) {
        vSortedByAddr.push_back(&dn);
    }

    sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

    it = vecDynodeRanks.begin() + nOffset;
    while(it != vecDynodeRanks.end()) {
        if(it->second.IsPoSeVerified() || it->second.IsPoSeBanned()) {
            LogPrint("dynode", "CDynodeMan::DoFullVerificationStep -- Already %s%s%s dynode %s address %s, skipping...\n",
                        it->second.IsPoSeVerified() ? "verified" : "",
                        it->second.IsPoSeVerified() && it->second.IsPoSeBanned() ? " and " : "",
                        it->second.IsPoSeBanned() ? "banned" : "",
                        it->second.vin.prevout.ToStringShort(), it->second.addr.ToString());
            ++it;
            continue;
        }
        LogPrint("dynode", "CDynodeMan::DoFullVerificationStep -- Verifying dynode %s rank %d/%d address %s\n",
                    it->second.vin.prevout.ToStringShort(), it->first, nRanksTotal, it->second.addr.ToString());
        if(SendVerifyRequest((CAddress)it->second.addr, vSortedByAddr)) {
            nCount++;
            if(nCount >= nCountMax) break;
        }
        ++it;
    }

    LogPrint("dynode", "CDynodeMan::DoFullVerificationStep -- Sent verification requests to %d dynodes\n", nCount);
}

// This function tries to find dynodes with the same addr,
// find a verified one and ban all the other. If there are many nodes
// with the same addr but none of them is verified yet, then none of them are banned.
// It could take many times to run this before most of the duplicate nodes are banned.

void CDynodeMan::CheckSameAddr()
{
    if(!dynodeSync.IsSynced() || vDynodes.empty()) return;

    std::vector<CDynode*> vBan;
    std::vector<CDynode*> vSortedByAddr;

    {
        LOCK(cs);

        CDynode* pprevDynode = NULL;
        CDynode* pverifiedDynode = NULL;

        BOOST_FOREACH(CDynode& dn, vDynodes) {
            vSortedByAddr.push_back(&dn);
        }

        sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

        BOOST_FOREACH(CDynode* psn, vSortedByAddr) {
            // check only (pre)enabled dynodes
            if(!psn->IsEnabled() && !psn->IsPreEnabled()) continue;
            // initial step
            if(!pprevDynode) {
                pprevDynode = psn;
                pverifiedDynode = psn->IsPoSeVerified() ? psn : NULL;
                continue;
            }
            // second+ step
            if(psn->addr == pprevDynode->addr) {
                if(pverifiedDynode) {
                    // another dynode with the same ip is verified, ban this one
                    vBan.push_back(psn);
                } else if(psn->IsPoSeVerified()) {
                    // this dynode with the same ip is verified, ban previous one
                    vBan.push_back(pprevDynode);
                    // and keep a reference to be able to ban following dynodes with the same ip
                    pverifiedDynode = psn;
                }
            } else {
                pverifiedDynode = psn->IsPoSeVerified() ? psn : NULL;
            }
            pprevDynode = psn;
        }
    }

    // ban duplicates
    BOOST_FOREACH(CDynode* psn, vBan) {
        LogPrintf("CDynodeMan::CheckSameAddr -- increasing PoSe ban score for dynode %s\n", psn->vin.prevout.ToStringShort());
        psn->IncreasePoSeBanScore();
    }
}

bool CDynodeMan::SendVerifyRequest(const CAddress& addr, const std::vector<CDynode*>& vSortedByAddr)
{
    if(netfulfilledman.HasFulfilledRequest(addr, strprintf("%s", NetMsgType::DNVERIFY)+"-request")) {
        // we already asked for verification, not a good idea to do this too often, skip it
        LogPrint("dynode", "CDynodeMan::SendVerifyRequest -- too many requests, skipping... addr=%s\n", addr.ToString());
        return false;
    }

    CNode* pnode = ConnectNode(addr, NULL, true);
    if(pnode != NULL) {
        netfulfilledman.AddFulfilledRequest(addr, strprintf("%s", NetMsgType::DNVERIFY)+"-request");
        // use random nonce, store it and require node to reply with correct one later
        CDynodeVerification snv(addr, GetRandInt(999999), pCurrentBlockIndex->nHeight - 1);
        mWeAskedForVerification[addr] = snv;
        LogPrintf("CDynodeMan::SendVerifyRequest -- verifying using nonce %d addr=%s\n", snv.nonce, addr.ToString());
        pnode->PushMessage(NetMsgType::DNVERIFY, snv);
        return true;
    } else {
        // can't connect, add some PoSe "ban score" to all dynodes with given addr
        bool fFound = false;
        BOOST_FOREACH(CDynode* psn, vSortedByAddr) {
            if(psn->addr != addr) {
                if(fFound) break;
                continue;
            }
            fFound = true;
            psn->IncreasePoSeBanScore();
        }
        return false;
    }
}

void CDynodeMan::SendVerifyReply(CNode* pnode, CDynodeVerification& snv)
{
    // only dynodes can sign this, why would someone ask regular node?
    if(!fDyNode) {
        // do not ban, malicious node might be using my IP
        // and trying to confuse the node which tries to verify it
        return;
    }

    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::DNVERIFY)+"-reply")) {
        // peer should not ask us that often
        LogPrintf("DynodeMan::SendVerifyReply -- ERROR: peer already asked me recently, peer=%d\n", pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, snv.nBlockHeight)) {
        LogPrintf("DynodeMan::SendVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", snv.nBlockHeight, pnode->id);
        return;
    }

    std::string strMessage = strprintf("%s%d%s", activeDynode.service.ToString(false), snv.nonce, blockHash.ToString());

    if(!privateSendSigner.SignMessage(strMessage, snv.vchSig1, activeDynode.keyDynode)) {
        LogPrintf("DynodeMan::SendVerifyReply -- SignMessage() failed\n");
        return;
    }

    std::string strError;

    if(!privateSendSigner.VerifyMessage(activeDynode.pubKeyDynode, snv.vchSig1, strMessage, strError)) {
        LogPrintf("DynodeMan::SendVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
        return;
    }

    pnode->PushMessage(NetMsgType::DNVERIFY, snv);
    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::DNVERIFY)+"-reply");
}

void CDynodeMan::ProcessVerifyReply(CNode* pnode, CDynodeVerification& snv)
{
    std::string strError;

    // did we even ask for it? if that's the case we should have matching fulfilled request
    if(!netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::DNVERIFY)+"-request")) {
        LogPrintf("CDynodeMan::ProcessVerifyReply -- ERROR: we didn't ask for verification of %s, peer=%d\n", pnode->addr.ToString(), pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nonce for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nonce != snv.nonce) {
        LogPrintf("CDynodeMan::ProcessVerifyReply -- ERROR: wrong nounce: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nonce, snv.nonce, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nBlockHeight for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nBlockHeight != snv.nBlockHeight) {
        LogPrintf("CDynodeMan::ProcessVerifyReply -- ERROR: wrong nBlockHeight: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nBlockHeight, snv.nBlockHeight, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, snv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("DynodeMan::ProcessVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", snv.nBlockHeight, pnode->id);
        return;
    }

    // we already verified this address, why node is spamming?
    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::DNVERIFY)+"-done")) {
        LogPrintf("CDynodeMan::ProcessVerifyReply -- ERROR: already verified %s recently\n", pnode->addr.ToString());
        Misbehaving(pnode->id, 20);
        return;
    }

    {
        LOCK(cs);

        CDynode* prealDynode = NULL;
        std::vector<CDynode*> vpDynodesToBan;
        std::vector<CDynode>::iterator it = vDynodes.begin();
        std::string strMessage1 = strprintf("%s%d%s", pnode->addr.ToString(false), snv.nonce, blockHash.ToString());
        while(it != vDynodes.end()) {
            if((CAddress)it->addr == pnode->addr) {
                if(privateSendSigner.VerifyMessage(it->pubKeyDynode, snv.vchSig1, strMessage1, strError)) {
                    // found it!
                    prealDynode = &(*it);
                    if(!it->IsPoSeVerified()) {
                        it->DecreasePoSeBanScore();
                    }
                    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::DNVERIFY)+"-done");

                    // we can only broadcast it if we are an activated dynode
                    if(activeDynode.vin == CTxIn()) continue;
                    // update ...
                    snv.addr = it->addr;
                    snv.vin1 = it->vin;
                    snv.vin2 = activeDynode.vin;
                    std::string strMessage2 = strprintf("%s%d%s%s%s", snv.addr.ToString(false), snv.nonce, blockHash.ToString(),
                                            snv.vin1.prevout.ToStringShort(), snv.vin2.prevout.ToStringShort());
                    // ... and sign it
                    if(!privateSendSigner.SignMessage(strMessage2, snv.vchSig2, activeDynode.keyDynode)) {
                        LogPrintf("DynodeMan::ProcessVerifyReply -- SignMessage() failed\n");
                        return;
                    }

                    std::string strError;

                    if(!privateSendSigner.VerifyMessage(activeDynode.pubKeyDynode, snv.vchSig2, strMessage2, strError)) {
                        LogPrintf("DynodeMan::ProcessVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
                        return;
                    }

                    mWeAskedForVerification[pnode->addr] = snv;
                    snv.Relay();

                } else {
                    vpDynodesToBan.push_back(&(*it));
                }
            }
            ++it;
        }
        // no real dynode found?...
        if(!prealDynode) {
            // this should never be the case normally,
            // only if someone is trying to game the system in some way or smth like that
            LogPrintf("CDynodeMan::ProcessVerifyReply -- ERROR: no real dynode found for addr %s\n", pnode->addr.ToString());
            Misbehaving(pnode->id, 20);
            return;
        }
        LogPrintf("CDynodeMan::ProcessVerifyReply -- verified real dynode %s for addr %s\n",
                    prealDynode->vin.prevout.ToStringShort(), pnode->addr.ToString());
        // increase ban score for everyone else
        BOOST_FOREACH(CDynode* psn, vpDynodesToBan) {
            psn->IncreasePoSeBanScore();
            LogPrint("dynode", "CDynodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                        prealDynode->vin.prevout.ToStringShort(), pnode->addr.ToString(), psn->nPoSeBanScore);
        }
        LogPrintf("CDynodeMan::ProcessVerifyBroadcast -- PoSe score incresed for %d fake Dynodes, addr %s\n",
                    (int)vpDynodesToBan.size(), pnode->addr.ToString());
    }
}

void CDynodeMan::ProcessVerifyBroadcast(CNode* pnode, const CDynodeVerification& snv)
{
    std::string strError;

    if(mapSeenDynodeVerification.find(snv.GetHash()) != mapSeenDynodeVerification.end()) {
        // we already have one
        return;
    }
    mapSeenDynodeVerification[snv.GetHash()] = snv;

    // we don't care about history
    if(snv.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS) {
        LogPrint("dynode", "DynodeMan::ProcessVerifyBroadcast -- Outdated: current block %d, verification block %d, peer=%d\n",
                    pCurrentBlockIndex->nHeight, snv.nBlockHeight, pnode->id);
        return;
    }

    if(snv.vin1.prevout == snv.vin2.prevout) {
        LogPrint("dynode", "DynodeMan::ProcessVerifyBroadcast -- ERROR: same vins %s, peer=%d\n",
                    snv.vin1.prevout.ToStringShort(), pnode->id);
        // that was NOT a good idea to cheat and verify itself,
        // ban the node we received such message from
        Misbehaving(pnode->id, 100);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, snv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("DynodeMan::ProcessVerifyBroadcast -- Can't get block hash for unknown block height %d, peer=%d\n", snv.nBlockHeight, pnode->id);
        return;
    }

    int nRank = GetDynodeRank(snv.vin2, snv.nBlockHeight, MIN_POSE_PROTO_VERSION);
    if(nRank < MAX_POSE_RANK) {
        LogPrint("dynode", "DynodeMan::ProcessVerifyBroadcast -- Dynode is not in top %d, current rank %d, peer=%d\n",
                    (int)MAX_POSE_RANK, nRank, pnode->id);
        return;
    }

    {
        LOCK(cs);

        std::string strMessage1 = strprintf("%s%d%s", snv.addr.ToString(false), snv.nonce, blockHash.ToString());
        std::string strMessage2 = strprintf("%s%d%s%s%s", snv.addr.ToString(false), snv.nonce, blockHash.ToString(),
                                snv.vin1.prevout.ToStringShort(), snv.vin2.prevout.ToStringShort());

        CDynode* psn1 = Find(snv.vin1);
        if(!psn1) {
            LogPrintf("CDynodeMan::ProcessVerifyBroadcast -- can't find dynode1 %s\n", snv.vin1.prevout.ToStringShort());
            return;
        }

        CDynode* psn2 = Find(snv.vin2);
        if(!psn2) {
            LogPrintf("CDynodeMan::ProcessVerifyBroadcast -- can't find dynode %s\n", snv.vin2.prevout.ToStringShort());
            return;
        }

        if(psn1->addr != snv.addr) {
            LogPrintf("CDynodeMan::ProcessVerifyBroadcast -- addr %s do not match %s\n", snv.addr.ToString(), pnode->addr.ToString());
            return;
        }

        if(privateSendSigner.VerifyMessage(psn1->pubKeyDynode, snv.vchSig1, strMessage1, strError)) {
            LogPrintf("DynodeMan::ProcessVerifyBroadcast -- VerifyMessage() for dynode1 failed, error: %s\n", strError);
            return;
        }

        if(privateSendSigner.VerifyMessage(psn2->pubKeyDynode, snv.vchSig2, strMessage2, strError)) {
            LogPrintf("DynodeMan::ProcessVerifyBroadcast -- VerifyMessage() for dynode2 failed, error: %s\n", strError);
            return;
        }

        if(!psn1->IsPoSeVerified()) {
            psn1->DecreasePoSeBanScore();
        }
        snv.Relay();

        LogPrintf("CDynodeMan::ProcessVerifyBroadcast -- verified dynode %s for addr %s\n",
                    psn1->vin.prevout.ToStringShort(), pnode->addr.ToString());

        // increase ban score for everyone else with the same addr
        int nCount = 0;
        BOOST_FOREACH(CDynode& dn, vDynodes) {
            if(dn.addr != snv.addr || dn.vin.prevout == snv.vin1.prevout) continue;
            dn.IncreasePoSeBanScore();
            nCount++;
            LogPrint("dynode", "CDynodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                        dn.vin.prevout.ToStringShort(), dn.addr.ToString(), dn.nPoSeBanScore);
        }
        LogPrintf("CDynodeMan::ProcessVerifyBroadcast -- PoSe score incresed for %d fake dynodes, addr %s\n",
                    nCount, pnode->addr.ToString());
    }
}

std::string CDynodeMan::ToString() const
{
    std::ostringstream info;

    info << "Dynodes: " << (int)vDynodes.size() <<
            ", peers who asked us for Dynode list: " << (int)mAskedUsForDynodeList.size() <<
            ", peers we asked for Dynode list: " << (int)mWeAskedForDynodeList.size() <<
            ", entries in Dynode list we asked for: " << (int)mWeAskedForDynodeListEntry.size() <<
            ", nSsqCount: " << (int)nSsqCount;

    return info.str();
}

int CDynodeMan::GetEstimatedDynodes(int nBlock)
{
    /*
        Dynodes = (Coins/1000)*X on average

        *X = nPercentage, starting at 0.52
        nPercentage goes up 0.01 each period
        Period starts at 35040, which has exponential slowing growth

    */

    int nPercentage = 52; //0.52
    int nPeriod = 35040;
    int nCollateral = 1000;

    for (int i = nPeriod; i <= nBlock; i += nPeriod) {
        nPercentage++;
        nPeriod*=2;
    }
    return (GetTotalCoinEstimate(nBlock)/100*nPercentage/nCollateral);
}

void CDynodeMan::UpdateDynodeList(CDynodeBroadcast dnb)
{
    LOCK(cs);
    mapSeenDynodePing.insert(std::make_pair(dnb.lastPing.GetHash(), dnb.lastPing));
    mapSeenDynodeBroadcast.insert(std::make_pair(dnb.GetHash(), dnb));

    LogPrintf("CDynodeMan::UpdateDynodeList -- dynode=%s  addr=%s\n", dnb.vin.prevout.ToStringShort(), dnb.addr.ToString());

    CDynode* psn = Find(dnb.vin);
    if(psn == NULL) {
        CDynode dn(dnb);
        if(Add(dn)) {
            dynodeSync.AddedDynodeList();
        }
    } else if(psn->UpdateFromNewBroadcast(dnb)) {
        dynodeSync.AddedDynodeList();
    }
}

bool CDynodeMan::CheckSnbAndUpdateDynodeList(CDynodeBroadcast dnb, int& nDos)
{
    LOCK(cs);

    nDos = 0;
    LogPrint("dynode", "CDynodeMan::CheckSnbAndUpdateDynodeList -- dynode=%s\n", dnb.vin.prevout.ToStringShort());

    if(mapSeenDynodeBroadcast.count(dnb.GetHash())) { //seen
        return true;
    }
    mapSeenDynodeBroadcast.insert(std::make_pair(dnb.GetHash(), dnb));

    LogPrint("dynode", "CDynodeMan::CheckSnbAndUpdateDynodeList -- dynode=%s new\n", dnb.vin.prevout.ToStringShort());

    if(!dnb.SimpleCheck(nDos)) {
        LogPrint("dynode", "CDynodeMan::CheckSnbAndUpdateDynodeList -- SimpleCheck() failed, dynode=%s\n", dnb.vin.prevout.ToStringShort());
        return false;
    }

    // search Dynode list
    CDynode* psn = Find(dnb.vin);
    if(psn) {
        if(!dnb.Update(psn, nDos)) {
            LogPrint("dynode", "CDynodeMan::CheckSnbAndUpdateDynodeList -- Update() failed, dynode=%s\n", dnb.vin.prevout.ToStringShort());
            return false;
        }
    } else {
        if(dnb.CheckOutpoint(nDos)) {
            Add(dnb);
            dynodeSync.AddedDynodeList();
            // if it matches our Dynode privkey...
            if(fDyNode && dnb.pubKeyDynode == activeDynode.pubKeyDynode) {
                dnb.nPoSeBanScore = -DYNODE_POSE_BAN_MAX_SCORE;
                if(dnb.nProtocolVersion == PROTOCOL_VERSION) {
                    // ... and PROTOCOL_VERSION, then we've been remotely activated ...
                    LogPrintf("CDynodeMan::CheckSnbAndUpdateDynodeList -- Got NEW Dynode entry: dynode=%s  sigTime=%lld  addr=%s\n",
                                dnb.vin.prevout.ToStringShort(), dnb.sigTime, dnb.addr.ToString());
                    activeDynode.ManageState();
                } else {
                    // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
                    // but also do not ban the node we get this message from
                    LogPrintf("CDynodeMan::CheckSnbAndUpdateDynodeList -- wrong PROTOCOL_VERSION, re-activate your DN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", dnb.nProtocolVersion, PROTOCOL_VERSION);
                    return false;
                }
            }
            dnb.Relay();
        } else {
            LogPrintf("CDynodeMan::CheckSnbAndUpdateDynodeList -- Rejected Dynode entry: %s  addr=%s\n", dnb.vin.prevout.ToStringShort(), dnb.addr.ToString());
            return false;
        }
    }

    return true;
}

void CDynodeMan::UpdateLastPaid(const CBlockIndex *pindex)
{
    LOCK(cs);

    if(fLiteMode) return;

    static bool IsFirstRun = true;
    // Do full scan on first run or if we are not a dynode
    // (MNs should update this info on every block, so limited scan should be enough for them)
    int nMaxBlocksToScanBack = (IsFirstRun || !fDyNode) ? dnpayments.GetStorageLimit() : LAST_PAID_SCAN_BLOCKS;

    // LogPrint("dnpayments", "CDynodeMan::UpdateLastPaid -- nHeight=%d, nMaxBlocksToScanBack=%d, IsFirstRun=%s\n",
    //                         pindex->nHeight, nMaxBlocksToScanBack, IsFirstRun ? "true" : "false");

    BOOST_FOREACH(CDynode& dn, vDynodes) {
        dn.UpdateLastPaid(pindex, nMaxBlocksToScanBack);
    }

    // every time is like the first time if winners list is not synced
    IsFirstRun = !dynodeSync.IsWinnersListSynced();
}

void CDynodeMan::CheckAndRebuildDynodeIndex()
{
    LOCK(cs);

    if(GetTime() - nLastIndexRebuildTime < MIN_INDEX_REBUILD_TIME) {
        return;
    }

    if(indexDynodes.GetSize() <= MAX_EXPECTED_INDEX_SIZE) {
        return;
    }

    if(indexDynodes.GetSize() <= int(vDynodes.size())) {
        return;
    }

    indexDynodesOld = indexDynodes;
    indexDynodes.Clear();
    for(size_t i = 0; i < vDynodes.size(); ++i) {
        indexDynodes.AddDynodeVIN(vDynodes[i].vin);
    }

    fIndexRebuilt = true;
    nLastIndexRebuildTime = GetTime();
}

void CDynodeMan::UpdateWatchdogVoteTime(const CTxIn& vin)
{
    LOCK(cs);
    CDynode* pDN = Find(vin);
    if(!pDN)  {
        return;
    }
    pDN->UpdateWatchdogVoteTime();
    nLastWatchdogVoteTime = GetTime();
}

bool CDynodeMan::IsWatchdogActive()
{
    LOCK(cs);
    // Check if any dynodes have voted recently, otherwise return false
    return (GetTime() - nLastWatchdogVoteTime) <= DYNODE_WATCHDOG_MAX_SECONDS;
}

void CDynodeMan::AddGovernanceVote(const CTxIn& vin, uint256 nGovernanceObjectHash)
{
    LOCK(cs);
    CDynode* pDN = Find(vin);
    if(!pDN)  {
        return;
    }
    pDN->AddGovernanceVote(nGovernanceObjectHash);
}

void CDynodeMan::RemoveGovernanceObject(uint256 nGovernanceObjectHash)
{
    LOCK(cs);
    BOOST_FOREACH(CDynode& dn, vDynodes) {
        dn.RemoveGovernanceObject(nGovernanceObjectHash);
    }
}

void CDynodeMan::CheckDynode(const CTxIn& vin, bool fForce)
{
    LOCK(cs);
    CDynode* pDN = Find(vin);
    if(!pDN)  {
        return;
    }
    pDN->Check(fForce);
}

void CDynodeMan::CheckDynode(const CPubKey& pubKeyDynode, bool fForce)
{
    LOCK(cs);
    CDynode* pDN = Find(pubKeyDynode);
    if(!pDN)  {
        return;
    }
    pDN->Check(fForce);
}

int CDynodeMan::GetDynodeState(const CTxIn& vin)
{
    LOCK(cs);
    CDynode* pDN = Find(vin);
    if(!pDN)  {
        return CDynode::DYNODE_REMOVE;
    }
    return pDN->nActiveState;
}

int CDynodeMan::GetDynodeState(const CPubKey& pubKeyDynode)
{
    LOCK(cs);
    CDynode* pDN = Find(pubKeyDynode);
    if(!pDN)  {
        return CDynode::DYNODE_REMOVE;
    }
    return pDN->nActiveState;
}

bool CDynodeMan::IsDynodePingedWithin(const CTxIn& vin, int nSeconds, int64_t nTimeToCheckAt)
{
    LOCK(cs);
    CDynode* pDN = Find(vin);
    if(!pDN) {
        return false;
    }
    return pDN->IsPingedWithin(nSeconds, nTimeToCheckAt);
}

void CDynodeMan::SetDynodeLastPing(const CTxIn& vin, const CDynodePing& snp)
{
    LOCK(cs);
    CDynode* pDN = Find(vin);
    if(!pDN)  {
        return;
    }
    pDN->lastPing = snp;
    mapSeenDynodePing.insert(std::make_pair(snp.GetHash(), snp));

    CDynodeBroadcast dnb(*pDN);
    uint256 hash = dnb.GetHash();
    if(mapSeenDynodeBroadcast.count(hash)) {
        mapSeenDynodeBroadcast[hash].lastPing = snp;
    }
}

void CDynodeMan::UpdatedBlockTip(const CBlockIndex *pindex)
{
    pCurrentBlockIndex = pindex;
    LogPrint("dynode", "CDynodeMan::UpdatedBlockTip -- pCurrentBlockIndex->nHeight=%d\n", pCurrentBlockIndex->nHeight);

    CheckSameAddr();

    if(fDyNode) {
        DoFullVerificationStep();
        // normal wallet does not need to update this every block, doing update on rpc call should be enough
        UpdateLastPaid(pindex);
    }
}

void CDynodeMan::NotifyDynodeUpdates()
{
    // Avoid double locking
    bool fDynodesAddedLocal = false;
    bool fDynodesRemovedLocal = false;
    {
        LOCK(cs);
        fDynodesAddedLocal = fDynodesAdded;
        fDynodesRemovedLocal = fDynodesRemoved;
    }

    if(fDynodesAddedLocal) {
        governance.CheckDynodeOrphanObjects();
        governance.CheckDynodeOrphanVotes();
    }
    if(fDynodesRemovedLocal) {
        governance.UpdateCachesAndClean();
    }

    LOCK(cs);
    fDynodesAdded = false;
    fDynodesRemoved = false;
}

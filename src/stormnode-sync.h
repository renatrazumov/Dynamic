// Copyright (c) 2014-2017 The Dash Core Developers
// Copyright (c) 2015-2017 Silk Network Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DYNAMIC_DYNODE_SYNC_H
#define DYNAMIC_DYNODE_SYNC_H

#include "chain.h"
#include "net.h"

#include <univalue.h>

class CDynodeSync;

static const int DYNODE_SYNC_FAILED          = -1;
static const int DYNODE_SYNC_INITIAL         = 0;
static const int DYNODE_SYNC_SPORKS          = 1;
static const int DYNODE_SYNC_LIST            = 2;
static const int DYNODE_SYNC_SNW             = 3;
static const int DYNODE_SYNC_GOVERNANCE      = 4;
static const int DYNODE_SYNC_GOVOBJ          = 10;
static const int DYNODE_SYNC_GOVERNANCE_FIN  = 11;
static const int DYNODE_SYNC_FINISHED        = 999;

static const int DYNODE_SYNC_TIMEOUT_SECONDS = 30; // our blocks are 2.5 minutes so 30 seconds should be fine

extern CDynodeSync dynodeSync;

//
// CDynodeSync : Sync dynode assets in stages
//

class CDynodeSync
{
private:
    // Keep track of current asset
    int nRequestedDynodeAssets;
    // Count peers we've requested the asset from
    int nRequestedDynodeAttempt;

    // Time when current dynode asset sync started
    int64_t nTimeAssetSyncStarted;

    // Last time when we received some dynode asset ...
    int64_t nTimeLastDynodeList;
    int64_t nTimeLastPaymentVote;
    int64_t nTimeLastBudgetItem;
    // ... or failed
    int64_t nTimeLastFailure;

    // How many times we failed
    int nCountFailures;

    // Keep track of current block index
    const CBlockIndex *pCurrentBlockIndex;

    void Fail();
    void ClearFulfilledRequests();

public:
    CDynodeSync() { Reset(); }

    void AddedDynodeList() { nTimeLastDynodeList = GetTime(); }
    void AddedPaymentVote() { nTimeLastPaymentVote = GetTime(); }
    void AddedBudgetItem(uint256 hash);

    bool IsFailed() { return nRequestedDynodeAssets == DYNODE_SYNC_FAILED; }
    bool IsBlockchainSynced();
    bool IsDynodeListSynced() { return nRequestedDynodeAssets > DYNODE_SYNC_LIST; }
    bool IsWinnersListSynced() { return nRequestedDynodeAssets > DYNODE_SYNC_SNW; }
    bool IsSynced() { return nRequestedDynodeAssets == DYNODE_SYNC_FINISHED; }

    int GetAssetID() { return nRequestedDynodeAssets; }
    int GetAttempt() { return nRequestedDynodeAttempt; }
    std::string GetAssetName();
    std::string GetSyncStatus();

    void Reset();
    void SwitchToNextAsset();

    void ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);
    void ProcessTick();

    void UpdatedBlockTip(const CBlockIndex *pindex);
};

#endif // DYNAMIC_DYNODE_SYNC_H

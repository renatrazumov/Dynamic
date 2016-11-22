// Copyright (c) 2014-2017 The Dash Core Developers
// Copyright (c) 2016-2017 Duality Blockchain Solutions Ltd
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activedynode.h"
#include "checkpoints.h"
#include "governance.h"
#include "main.h"
#include "dynode.h"
#include "dynode-payments.h"
#include "dynode-sync.h"
#include "dynodeman.h"
#include "netfulfilledman.h"
#include "spork.h"
#include "util.h"

class CDynodeSync;
CDynodeSync dynodeSync;

bool CDynodeSync::IsBlockchainSynced()
{
    static bool fBlockchainSynced = false;
    static int64_t nTimeLastProcess = GetTime();

    // if the last call to this function was more than 60 minutes ago (client was in sleep mode) reset the sync process
    if(GetTime() - nTimeLastProcess > 60*60) {
        Reset();
        fBlockchainSynced = false;
    }
    nTimeLastProcess = GetTime();

    if(fBlockchainSynced) return true;
    if(!pCurrentBlockIndex || !pindexBestHeader || fImporting || fReindex) return false;
    if(fCheckpointsEnabled && pCurrentBlockIndex->nHeight < Checkpoints::GetTotalBlocksEstimate(Params().Checkpoints()))
        return false;

    // same as !IsInitialBlockDownload() but no cs_main needed here
    int nMaxBlockTime = std::max(pCurrentBlockIndex->GetBlockTime(), pindexBestHeader->GetBlockTime());
    fBlockchainSynced = pindexBestHeader->nHeight - pCurrentBlockIndex->nHeight < 24 * 6 &&
                        GetTime() - nMaxBlockTime < Params().MaxTipAge();

    return fBlockchainSynced;
}

void CDynodeSync::Fail()
{
    nTimeLastFailure = GetTime();
    nRequestedDynodeAssets = DYNODE_SYNC_FAILED;
}

void CDynodeSync::Reset()
{
    nRequestedDynodeAssets = DYNODE_SYNC_INITIAL;
    nRequestedDynodeAttempt = 0;
    nTimeAssetSyncStarted = GetTime();
    nTimeLastDynodeList = GetTime();
    nTimeLastPaymentVote = GetTime();
    nTimeLastBudgetItem = GetTime();
    nTimeLastFailure = 0;
    nCountFailures = 0;
}

std::string CDynodeSync::GetAssetName()
{
    switch(nRequestedDynodeAssets)
    {
        case(DYNODE_SYNC_INITIAL):      return "DYNODE_SYNC_INITIAL";
        case(DYNODE_SYNC_SPORKS):       return "DYNODE_SYNC_SPORKS";
        case(DYNODE_SYNC_LIST):         return "DYNODE_SYNC_LIST";
        case(DYNODE_SYNC_DNW):          return "DYNODE_SYNC_DNW";
        case(DYNODE_SYNC_GOVERNANCE):   return "DYNODE_SYNC_GOVERNANCE";
        case(DYNODE_SYNC_FAILED):       return "DYNODE_SYNC_FAILED";
        case DYNODE_SYNC_FINISHED:      return "DYNODE_SYNC_FINISHED";
        default:                           return "UNKNOWN";
    }
}

void CDynodeSync::SwitchToNextAsset()
{
    switch(nRequestedDynodeAssets)
    {
        case(DYNODE_SYNC_FAILED):
            throw std::runtime_error("Can't switch to next asset from failed, should use Reset() first!");
            break;
        case(DYNODE_SYNC_INITIAL):
            ClearFulfilledRequests();
            nRequestedDynodeAssets = DYNODE_SYNC_SPORKS;
            break;
        case(DYNODE_SYNC_SPORKS):
            nTimeLastDynodeList = GetTime();
            nRequestedDynodeAssets = DYNODE_SYNC_LIST;
            break;
        case(DYNODE_SYNC_LIST):
            nTimeLastPaymentVote = GetTime();
            nRequestedDynodeAssets = DYNODE_SYNC_DNW;
            break;
        case(DYNODE_SYNC_DNW):
            nTimeLastBudgetItem = GetTime();
            nRequestedDynodeAssets = DYNODE_SYNC_GOVERNANCE;
            break;
        case(DYNODE_SYNC_GOVERNANCE):
            LogPrintf("CDynodeSync::SwitchToNextAsset -- Sync has finished\n");
            nRequestedDynodeAssets = DYNODE_SYNC_FINISHED;
            uiInterface.NotifyAdditionalDataSyncProgressChanged(1);
            //try to activate our dynode if possible
            activeDynode.ManageState();

            TRY_LOCK(cs_vNodes, lockRecv);
            if(!lockRecv) return;

            BOOST_FOREACH(CNode* pnode, vNodes) {
                netfulfilledman.AddFulfilledRequest(pnode->addr, "full-sync");
            }

            break;
    }
    nRequestedDynodeAttempt = 0;
    nTimeAssetSyncStarted = GetTime();
}

std::string CDynodeSync::GetSyncStatus()
{
    switch (dynodeSync.nRequestedDynodeAssets) {
        case DYNODE_SYNC_INITIAL:       return _("Synchronization pending...");
        case DYNODE_SYNC_SPORKS:        return _("Synchronizing sporks...");
        case DYNODE_SYNC_LIST:          return _("Synchronizing dynodes...");
        case DYNODE_SYNC_DNW:           return _("Synchronizing dynode payments...");
        case DYNODE_SYNC_GOVERNANCE:    return _("Synchronizing governance objects...");
        case DYNODE_SYNC_FAILED:        return _("Synchronization failed");
        case DYNODE_SYNC_FINISHED:      return _("Synchronization finished");
        default:                           return "";
    }
}

void CDynodeSync::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (strCommand == NetMsgType::SYNCSTATUSCOUNT) { //Sync status count

        //do not care about stats if sync process finished or failed
        if(IsSynced() || IsFailed()) return;

        int nItemID;
        int nCount;
        vRecv >> nItemID >> nCount;

        LogPrintf("SYNCSTATUSCOUNT -- got inventory count: nItemID=%d  nCount=%d  peer=%d\n", nItemID, nCount, pfrom->id);
    }
}

void CDynodeSync::ClearFulfilledRequests()
{
    TRY_LOCK(cs_vNodes, lockRecv);
    if(!lockRecv) return;

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "spork-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "dynode-list-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "dynode-payment-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "governance-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "full-sync");
    }
}

void CDynodeSync::ProcessTick()
{
    static int nTick = 0;
    if(nTick++ % 6 != 0) return;
    if(!pCurrentBlockIndex) return;

    //the actual count of dynodes we have currently
    int nSnCount = dnodeman.CountDynodes();

    if(fDebug) LogPrintf("CDynodeSync::ProcessTick -- nTick %d nSnCount %d\n", nTick, nSnCount);

    // RESET SYNCING INCASE OF FAILURE
    {
        if(IsSynced()) {
            /*
                Resync if we lose all dynodes from sleep/wake or failure to sync originally
            */
            if(nSnCount == 0) {
                LogPrintf("CDynodeSync::ProcessTick -- WARNING: not enough data, restarting sync\n");
                Reset();
            } else {
                //if syncing is complete and we have dynodes, return
                return;
            }
        }

        //try syncing again
        if(IsFailed()) {
            if(nTimeLastFailure + (1*60) < GetTime()) { // 1 minute cooldown after failed sync
                Reset();
            }
            return;
        }
    }

    // INITIAL SYNC SETUP / LOG REPORTING
    double nSyncProgress = double(nRequestedDynodeAttempt + (nRequestedDynodeAssets - 1) * 8) / (8*4);
    LogPrintf("CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d nRequestedDynodeAttempt %d nSyncProgress %f\n", nTick, nRequestedDynodeAssets, nRequestedDynodeAttempt, nSyncProgress);
    uiInterface.NotifyAdditionalDataSyncProgressChanged(nSyncProgress);

    // sporks synced but blockchain is not, wait until we're almost at a recent block to continue
    if(Params().NetworkIDString() != CBaseChainParams::REGTEST &&
            !IsBlockchainSynced() && nRequestedDynodeAssets > DYNODE_SYNC_SPORKS)
    {
        LogPrintf("CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d nRequestedDynodeAttempt %d -- blockchain is not synced yet\n", nTick, nRequestedDynodeAssets, nRequestedDynodeAttempt);
        return;
    }

    LOCK2(dnodeman.cs, cs_vNodes);

    if(nRequestedDynodeAssets == DYNODE_SYNC_INITIAL ||
        (nRequestedDynodeAssets == DYNODE_SYNC_SPORKS && IsBlockchainSynced()))
    {
        SwitchToNextAsset();
    }

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        // QUICK MODE (REGTEST ONLY!)
        if(Params().NetworkIDString() == CBaseChainParams::REGTEST)
        {
            if(nRequestedDynodeAttempt <= 2) {
                pnode->PushMessage(NetMsgType::GETSPORKS); //get current network sporks
            } else if(nRequestedDynodeAttempt < 4) {
                dnodeman.SsegUpdate(pnode);
            } else if(nRequestedDynodeAttempt < 6) {
                int nSnCount = dnodeman.CountDynodes();
                pnode->PushMessage(NetMsgType::DYNODEPAYMENTSYNC, nSnCount); //sync payment votes
                uint256 n = uint256();
                pnode->PushMessage(NetMsgType::DNGOVERNANCESYNC, n); //sync dynode votes
            } else {
                nRequestedDynodeAssets = DYNODE_SYNC_FINISHED;
            }
            nRequestedDynodeAttempt++;
            return;
        }

        // NORMAL NETWORK MODE - TESTNET/MAINNET
        {
            if(netfulfilledman.HasFulfilledRequest(pnode->addr, "full-sync")) {
                // we already fully synced from this node recently,
                // disconnect to free this connection slot for a new node
                pnode->fDisconnect = true;
                LogPrintf("CDynodeSync::ProcessTick -- disconnecting from recently synced peer %d\n", pnode->id);
                continue;
            }

            // SPORK : ALWAYS ASK FOR SPORKS AS WE SYNC (we skip this mode now)

            if(!netfulfilledman.HasFulfilledRequest(pnode->addr, "spork-sync")) {
                // only request once from each peer
                netfulfilledman.AddFulfilledRequest(pnode->addr, "spork-sync");
                // get current network sporks
                pnode->PushMessage(NetMsgType::GETSPORKS);
                LogPrintf("CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d -- requesting sporks from peer %d\n", nTick, nRequestedDynodeAssets, pnode->id);
                continue; // always get sporks first, switch to the next node without waiting for the next tick
            }

            // MNLIST : SYNC DYNODE LIST FROM OTHER CONNECTED CLIENTS

            if(nRequestedDynodeAssets == DYNODE_SYNC_LIST) {
                LogPrint("dynode", "CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d nTimeLastDynodeList %lld GetTime() %lld diff %lld\n", nTick, nRequestedDynodeAssets, nTimeLastDynodeList, GetTime(), GetTime() - nTimeLastDynodeList);
                // check for timeout first
                if(nTimeLastDynodeList < GetTime() - DYNODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrintf("CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d -- timeout\n", nTick, nRequestedDynodeAssets);
                    if (nRequestedDynodeAttempt == 0) {
                        LogPrintf("CDynodeSync::ProcessTick -- ERROR: failed to sync %s\n", GetAssetName());
                        // there is no way we can continue without dynode list, fail here and try later
                        Fail();
                        return;
                    }
                    SwitchToNextAsset();
                    return;
                }

                // check for data
                // if we have enough dynodes in our list, switch to the next asset
                /* Note: Is this activing up? It's probably related to int CDynodeMan::GetEstimatedDynodes(int nBlock)
                   Surely doesn't work right for testnet currently */
                // try to fetch data from at least two peers though
                int nSnCountEstimated = dnodeman.GetEstimatedDynodes(pCurrentBlockIndex->nHeight)*0.9;
                LogPrintf("CDynodeSync::ProcessTick -- nTick %d nSnCount %d nSnCountEstimated %d\n",
                          nTick, nSnCount, nSnCountEstimated);
                if(nRequestedDynodeAttempt > 1 && nSnCount > nSnCountEstimated) {
                    LogPrintf("CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d -- found enough data\n", nTick, nRequestedDynodeAssets);
                    SwitchToNextAsset();
                    return;
                }

                // only request once from each peer
                if(netfulfilledman.HasFulfilledRequest(pnode->addr, "dynode-list-sync")) continue;
                netfulfilledman.AddFulfilledRequest(pnode->addr, "dynode-list-sync");

                if (pnode->nVersion < dnpayments.GetMinDynodePaymentsProto()) continue;
                nRequestedDynodeAttempt++;

                dnodeman.SsegUpdate(pnode);

                return; //this will cause each peer to get one request each six seconds for the various assets we need
            }

            // DNW : SYNC DYNODE PAYMENT VOTES FROM OTHER CONNECTED CLIENTS

            if(nRequestedDynodeAssets == DYNODE_SYNC_DNW) {
                LogPrint("dnpayments", "CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d nTimeLastPaymentVote %lld GetTime() %lld diff %lld\n", nTick, nRequestedDynodeAssets, nTimeLastPaymentVote, GetTime(), GetTime() - nTimeLastPaymentVote);
                // check for timeout first
                // This might take a lot longer than DYNODE_SYNC_TIMEOUT_SECONDS minutes due to new blocks,
                // but that should be OK and it should timeout eventually.
                if(nTimeLastPaymentVote < GetTime() - DYNODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrintf("CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d -- timeout\n", nTick, nRequestedDynodeAssets);
                    if (nRequestedDynodeAttempt == 0) {
                        LogPrintf("CDynodeSync::ProcessTick -- ERROR: failed to sync %s\n", GetAssetName());
                        // probably not a good idea to proceed without winner list
                        Fail();
                        return;
                    }
                    SwitchToNextAsset();
                    return;
                }

                // check for data
                // if dnpayments already has enough blocks and votes, switch to the next asset
                // try to fetch data from at least two peers though
                if(nRequestedDynodeAttempt > 1 && dnpayments.IsEnoughData()) {
                    LogPrintf("CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d -- found enough data\n", nTick, nRequestedDynodeAssets);
                    SwitchToNextAsset();
                    return;
                }

                // only request once from each peer
                if(netfulfilledman.HasFulfilledRequest(pnode->addr, "dynode-payment-sync")) continue;
                netfulfilledman.AddFulfilledRequest(pnode->addr, "dynode-payment-sync");

                if(pnode->nVersion < dnpayments.GetMinDynodePaymentsProto()) continue;
                nRequestedDynodeAttempt++;

                // ask node for all payment votes it has (new nodes will only return votes for future payments)
                pnode->PushMessage(NetMsgType::DYNODEPAYMENTSYNC, dnpayments.GetStorageLimit());
                // ask node for missing pieces only (old nodes will not be asked)
                dnpayments.RequestLowDataPaymentBlocks(pnode);

                return; //this will cause each peer to get one request each six seconds for the various assets we need
            }

            // GOVOBJ : SYNC GOVERNANCE ITEMS FROM OUR PEERS

            if(nRequestedDynodeAssets == DYNODE_SYNC_GOVERNANCE) {
                LogPrint("dnpayments", "CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d nTimeLastPaymentVote %lld GetTime() %lld diff %lld\n", nTick, nRequestedDynodeAssets, nTimeLastPaymentVote, GetTime(), GetTime() - nTimeLastPaymentVote);

                // check for timeout first
                if(nTimeLastBudgetItem < GetTime() - DYNODE_SYNC_TIMEOUT_SECONDS){
                    LogPrintf("CDynodeSync::ProcessTick -- nTick %d nRequestedDynodeAssets %d -- timeout\n", nTick, nRequestedDynodeAssets);
                    if(nRequestedDynodeAttempt == 0) {
                        LogPrintf("CDynodeSync::ProcessTick -- WARNING: failed to sync %s\n", GetAssetName());
                        // it's kind of ok to skip this for now, hopefully we'll catch up later?
                    }
                    SwitchToNextAsset();
                    return;
                }

                // check for data
                // if(nCountBudgetItemProp > 0 && nCountBudgetItemFin)
                // {
                //     if(governance.CountProposalInventoryItems() >= (nSumBudgetItemProp / nCountBudgetItemProp)*0.9)
                //     {
                //         if(governance.CountFinalizedInventoryItems() >= (nSumBudgetItemFin / nCountBudgetItemFin)*0.9)
                //         {
                //             SwitchToNextAsset();
                //             return;
                //         }
                //     }
                // }

                // only request once from each peer
                if(netfulfilledman.HasFulfilledRequest(pnode->addr, "governance-sync")) continue;
                netfulfilledman.AddFulfilledRequest(pnode->addr, "governance-sync");

                if (pnode->nVersion < MIN_GOVERNANCE_PEER_PROTO_VERSION) continue;
                nRequestedDynodeAttempt++;

                pnode->PushMessage(NetMsgType::DNGOVERNANCESYNC, uint256()); //sync dynode votes

                return; //this will cause each peer to get one request each six seconds for the various assets we need
            }
        }
    }
}

void CDynodeSync::UpdatedBlockTip(const CBlockIndex *pindex)
{
    pCurrentBlockIndex = pindex;
}


void CDynodeSync::AddedBudgetItem(uint256 hash)
{
    // skip this for now
    return;
}

// Copyright (c) 2014-2017 The Dash Core Developers
// Copyright (c) 2016-2017 Duality Blockchain Solutions Ltd
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activedynode.h"
#include "addrman.h"
#include "privatesend.h"
#include "governance-classes.h"
#include "policy/fees.h"
#include "dynode-payments.h"
#include "dynode-sync.h"
#include "dynodeman.h"
#include "netfulfilledman.h"
#include "spork.h"
#include "util.h"

#include <boost/lexical_cast.hpp>

/** Object for who's going to get paid on which blocks */
CDynodePayments dnpayments;

CCriticalSection cs_vecPayees;
CCriticalSection cs_mapDynodeBlocks;
CCriticalSection cs_mapDynodePaymentVotes;

/**
* IsBlockValueValid
*
*   Determine if coinbase outgoing created money is the correct value
*
*   Why is this needed?
*   - In Dynamic some blocks are superblocks, which output much higher amounts of coins
*   - Otherblocks are 10% lower in outgoing value, so in total, no extra coins are created
*   - When non-superblocks are detected, the normal schedule should be maintained
*/

bool IsBlockValueValid(const CBlock& block, int nBlockHeight, CAmount blockReward)
{
    bool isNormalBlockValueMet = (block.vtx[0].GetValueOut() <= blockReward);
    if(fDebug) LogPrintf("block.vtx[0].GetValueOut() %lld <= blockReward %lld\n", block.vtx[0].GetValueOut(), blockReward);

    // we are still using budgets, but we have no data about them anymore,
    // all we know is predefined budget cycle and window

    const Consensus::Params& consensusParams = Params().GetConsensus();

    if(nBlockHeight < consensusParams.nSuperblockStartBlock) {
        int nOffset = nBlockHeight % consensusParams.nBudgetPaymentsCycleBlocks;
        if(nBlockHeight >= consensusParams.nBudgetPaymentsStartBlock &&
            nOffset < consensusParams.nBudgetPaymentsWindowBlocks) {
            // NOTE: make sure SPORK_13_OLD_SUPERBLOCK_FLAG is disabled when 12.1 starts to go live
            if(dynodeSync.IsSynced() && !sporkManager.IsSporkActive(SPORK_13_OLD_SUPERBLOCK_FLAG)) {
                // no budget blocks should be accepted here, if SPORK_13_OLD_SUPERBLOCK_FLAG is disabled
                LogPrint("gobject", "IsBlockValueValid -- Client synced but budget spork is disabled, checking block value against normal block reward\n");
                return isNormalBlockValueMet;
            }
            LogPrint("gobject", "IsBlockValueValid -- WARNING: Skipping budget block value checks, accepting block\n");
            // TODO: reprocess blocks to make sure they are legit?
            return true;
        }
        // LogPrint("gobject", "IsBlockValueValid -- Block is not in budget cycle window, checking block value against normal block reward\n");
        return isNormalBlockValueMet;
    }

    // superblocks started

    CAmount nSuperblockPaymentsLimit = CSuperblock::GetPaymentsLimit(nBlockHeight);
    bool isSuperblockMaxValueMet = (block.vtx[0].GetValueOut() <= blockReward + nSuperblockPaymentsLimit);

    LogPrint("gobject", "block.vtx[0].GetValueOut() %lld <= nSuperblockPaymentsLimit %lld\n", block.vtx[0].GetValueOut(), nSuperblockPaymentsLimit);

    if(!dynodeSync.IsSynced()) {
        // not enough data but at least it must NOT exceed superblock max value
        if(CSuperblock::IsValidBlockHeight(nBlockHeight)) {
            if(fDebug) LogPrintf("IsBlockPayeeValid -- WARNING: Client not synced, checking superblock max bounds only\n");
            return isSuperblockMaxValueMet;
        }
        // it MUST be a regular block otherwise
        return isNormalBlockValueMet;
    }

    // we are synced, let's try to check as much data as we can

    if(sporkManager.IsSporkActive(SPORK_9_SUPERBLOCKS_ENABLED)) {
        if(CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
            if(CSuperblockManager::IsValid(block.vtx[0], nBlockHeight, blockReward)) {
                LogPrint("gobject", "IsBlockValueValid -- Valid superblock at height %d: %s", nBlockHeight, block.vtx[0].ToString());
                // all checks are done in CSuperblock::IsValid, nothing to do here
                return true;
            }

            // triggered but invalid? that's weird
            LogPrintf("IsBlockValueValid -- ERROR: Invalid superblock detected at height %d: %s", nBlockHeight, block.vtx[0].ToString());
            // should NOT allow invalid superblocks, when superblocks are enabled
            return false;
        }
        LogPrint("gobject", "IsBlockValueValid -- No triggered superblock detected at height %d\n", nBlockHeight);
    } else {
        // should NOT allow superblocks at all, when superblocks are disabled
        LogPrint("gobject", "IsBlockValueValid -- Superblocks are disabled, no superblocks allowed\n");
    }

    // it MUST be a regular block
    return isNormalBlockValueMet;
}

bool IsBlockPayeeValid(const CTransaction& txNew, int nBlockHeight, CAmount blockReward)
{
    if(!dynodeSync.IsSynced()) {
        //there is no budget data to use to check anything, let's just accept the longest chain
        if(fDebug) LogPrintf("IsBlockPayeeValid -- WARNING: Client not synced, skipping block payee checks\n");
        return true;
    }

    // we are still using budgets, but we have no data about them anymore,
    // we can only check dynode payments

    const Consensus::Params& consensusParams = Params().GetConsensus();

    if(nBlockHeight < consensusParams.nSuperblockStartBlock) {
        if(dnpayments.IsTransactionValid(txNew, nBlockHeight)) {
            LogPrint("dnpayments", "IsBlockPayeeValid -- Valid dynode payment at height %d: %s", nBlockHeight, txNew.ToString());
            return true;
        }

        int nOffset = nBlockHeight % consensusParams.nBudgetPaymentsCycleBlocks;
        if(nBlockHeight >= consensusParams.nBudgetPaymentsStartBlock &&
            nOffset < consensusParams.nBudgetPaymentsWindowBlocks) {
            if(!sporkManager.IsSporkActive(SPORK_13_OLD_SUPERBLOCK_FLAG)) {
                // no budget blocks should be accepted here, if SPORK_13_OLD_SUPERBLOCK_FLAG is disabled
                LogPrint("gobject", "IsBlockPayeeValid -- ERROR: Client synced but budget spork is disabled and dynode payment is invalid\n");
                return false;
            }
            // NOTE: this should never happen in real, SPORK_13_OLD_SUPERBLOCK_FLAG MUST be disabled when 12.1 starts to go live
            LogPrint("gobject", "IsBlockPayeeValid -- WARNING: Probably valid budget block, have no data, accepting\n");
            // TODO: reprocess blocks to make sure they are legit?
            return true;
        }

        if(sporkManager.IsSporkActive(SPORK_8_DYNODE_PAYMENT_ENFORCEMENT)) {
            LogPrintf("IsBlockPayeeValid -- ERROR: Invalid dynode payment detected at height %d: %s", nBlockHeight, txNew.ToString());
            return false;
        }

        LogPrintf("IsBlockPayeeValid -- WARNING: Dynode payment enforcement is disabled, accepting any payee\n");
        return true;
    }

    // superblocks started
    // SEE IF THIS IS A VALID SUPERBLOCK

    if(sporkManager.IsSporkActive(SPORK_9_SUPERBLOCKS_ENABLED)) {
        if(CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
            if(CSuperblockManager::IsValid(txNew, nBlockHeight, blockReward)) {
                LogPrint("gobject", "IsBlockPayeeValid -- Valid superblock at height %d: %s", nBlockHeight, txNew.ToString());
                return true;
            }

            LogPrintf("IsBlockPayeeValid -- ERROR: Invalid superblock detected at height %d: %s", nBlockHeight, txNew.ToString());
            // should NOT allow such superblocks, when superblocks are enabled
            return false;
        }
        // continue validation, should pay DN
        LogPrint("gobject", "IsBlockPayeeValid -- No triggered superblock detected at height %d\n", nBlockHeight);
    } else {
        // should NOT allow superblocks at all, when superblocks are disabled
        LogPrint("gobject", "IsBlockPayeeValid -- Superblocks are disabled, no superblocks allowed\n");
    }

    // IF THIS ISN'T A SUPERBLOCK OR SUPERBLOCK IS INVALID, IT SHOULD PAY A DYNODE DIRECTLY
    if(dnpayments.IsTransactionValid(txNew, nBlockHeight)) {
        LogPrint("dnpayments", "IsBlockPayeeValid -- Valid dynode payment at height %d: %s", nBlockHeight, txNew.ToString());
        return true;
    }

    if(sporkManager.IsSporkActive(SPORK_8_DYNODE_PAYMENT_ENFORCEMENT)) {
        LogPrintf("IsBlockPayeeValid -- ERROR: Invalid dynode payment detected at height %d: %s", nBlockHeight, txNew.ToString());
        return false;
    }

    LogPrintf("IsBlockPayeeValid -- WARNING: Dynode payment enforcement is disabled, accepting any payee\n");
    return true;
}

void FillBlockPayments(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward, CTxOut& txoutDynodeRet, std::vector<CTxOut>& voutSuperblockRet)
{
    // only create superblocks if spork is enabled AND if superblock is actually triggered
    // (height should be validated inside)
    if(sporkManager.IsSporkActive(SPORK_9_SUPERBLOCKS_ENABLED) &&
        CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
            LogPrint("gobject", "FillBlockPayments -- triggered superblock creation at height %d\n", nBlockHeight);
            CSuperblockManager::CreateSuperblock(txNew, nBlockHeight, voutSuperblockRet);
            return;
    }

    // FILL BLOCK PAYEE WITH DYNODE PAYMENT OTHERWISE
    dnpayments.FillBlockPayee(txNew);
    LogPrint("dnpayments", "FillBlockPayments -- nBlockHeight %d blockReward %lld txoutDynodeRet %s txNew %s",
                            nBlockHeight, blockReward, txoutDynodeRet.ToString(), txNew.ToString());
}

std::string GetRequiredPaymentsString(int nBlockHeight)
{
    // IF WE HAVE A ACTIVATED TRIGGER FOR THIS HEIGHT - IT IS A SUPERBLOCK, GET THE REQUIRED PAYEES
    if(CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
        return CSuperblockManager::GetRequiredPaymentsString(nBlockHeight);
    }

    // OTHERWISE, PAY DYNODE
    return dnpayments.GetRequiredPaymentsString(nBlockHeight);
}

void CDynodePayments::Clear()
{
    LOCK2(cs_mapDynodeBlocks, cs_mapDynodePaymentVotes);
    mapDynodeBlocks.clear();
    mapDynodePaymentVotes.clear();
}

bool CDynodePayments::CanVote(COutPoint outDynode, int nBlockHeight)
{
    LOCK(cs_mapDynodePaymentVotes);

    if (mapDynodesLastVote.count(outDynode) && mapDynodesLastVote[outDynode] == nBlockHeight) {
        return false;
    }

    //record this dynode voted
    mapDynodesLastVote[outDynode] = nBlockHeight;
    return true;
}

/**
*   FillBlockPayee
*
*   Fill Dynode ONLY payment block
*/

void CDynodePayments::FillBlockPayee(CMutableTransaction& txNew /*CAmount nFees*/)  // TODO GB : Add fees
{
    CBlockIndex* pindexPrev = chainActive.Tip();       
    if(!pindexPrev) return;        

    bool hasPayment = true;
    CScript payee;

    //spork
    if(!dnpayments.GetBlockPayee(pindexPrev->nHeight+1, payee)){       
        //no dynode detected
        CDynode* winningNode = dnodeman.Find(payee);
        if(winningNode){
            payee = GetScriptForDestination(winningNode->pubKeyCollateralAddress.GetID());
        } else {
            if (fDebug)
                LogPrintf("CreateNewBlock: Failed to detect dynode to pay\n");
            hasPayment = false;
        }
    }

    CAmount blockValue;
    CAmount dynodePayment;

    if (chainActive.Height() == 0) { blockValue = 4000000 * COIN; }
    else if (chainActive.Height() >= 1 && chainActive.Height() <= Params().StartDynodePayments()) { blockValue = BLOCKCHAIN_INIT_REWARD; }
    else { blockValue = STATIC_POW_REWARD; }

    if (!hasPayment && chainActive.Height() < Params().StartDynodePayments()) { dynodePayment = BLOCKCHAIN_INIT_REWARD; }
    else { dynodePayment = STATIC_DYNODE_PAYMENT; }

    txNew.vout[0].nValue = blockValue;

    if(hasPayment){
        txNew.vout.resize(2);

        txNew.vout[1].scriptPubKey = payee;
        txNew.vout[1].nValue = dynodePayment;

        txNew.vout[0].nValue = STATIC_POW_REWARD;

        CTxDestination address1;
        ExtractDestination(payee, address1);
        CDynamicAddress address2(address1);

        LogPrintf("CDynodePayments::FillBlockPayee -- Dynode payment %lld to %s\n", dynodePayment, address2.ToString());
    }
}

int CDynodePayments::GetMinDynodePaymentsProto() {
    return sporkManager.IsSporkActive(SPORK_10_DYNODE_PAY_UPDATED_NODES)
            ? MIN_DYNODE_PAYMENT_PROTO_VERSION_2
            : MIN_DYNODE_PAYMENT_PROTO_VERSION_1;
}

void CDynodePayments::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    // Ignore any payments messages until dynode list is synced
    if(!dynodeSync.IsDynodeListSynced()) return;

    if(fLiteMode) return; // disable all Dynamic specific functionality

    if (strCommand == NetMsgType::DYNODEPAYMENTSYNC) { //Dynode Payments Request Sync

        // Ignore such requests until we are fully synced.
        // We could start processing this after dynode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!dynodeSync.IsSynced()) return;

        int nCountNeeded;
        vRecv >> nCountNeeded;

        if(netfulfilledman.HasFulfilledRequest(pfrom->addr, NetMsgType::DYNODEPAYMENTSYNC)) {
            // Asking for the payments list multiple times in a short period of time is no good
            LogPrintf("DYNODEPAYMENTSYNC -- peer already asked me for the list, peer=%d\n", pfrom->id);
            Misbehaving(pfrom->GetId(), 20);
            return;
        }
        netfulfilledman.AddFulfilledRequest(pfrom->addr, NetMsgType::DYNODEPAYMENTSYNC);

        Sync(pfrom, nCountNeeded);
        LogPrintf("DYNODEPAYMENTSYNC -- Sent Dynode payment votes to peer %d\n", pfrom->id);

    } else if (strCommand == NetMsgType::DYNODEPAYMENTVOTE) { // Dynode Payments Vote for the Winner

        CDynodePaymentVote vote;
        vRecv >> vote;

        if(pfrom->nVersion < GetMinDynodePaymentsProto()) return;

        if(!pCurrentBlockIndex) return;

        if(mapDynodePaymentVotes.count(vote.GetHash())) {
            LogPrint("dnpayments", "DYNODEPAYMENTVOTE -- hash=%s, nHeight=%d seen\n", vote.GetHash().ToString(), pCurrentBlockIndex->nHeight);
            return;
        }

        int nFirstBlock = pCurrentBlockIndex->nHeight - GetStorageLimit();
        if(vote.nBlockHeight < nFirstBlock || vote.nBlockHeight > pCurrentBlockIndex->nHeight+20) {
            LogPrint("dnpayments", "DYNODEPAYMENTVOTE -- vote out of range: nFirstBlock=%d, nBlockHeight=%d, nHeight=%d\n", nFirstBlock, vote.nBlockHeight, pCurrentBlockIndex->nHeight);
            return;
        }

        std::string strError = "";
        if(!vote.IsValid(pfrom, pCurrentBlockIndex->nHeight, strError)) {
            LogPrint("dnpayments", "DYNODEPAYMENTVOTE -- invalid message, error: %s\n", strError);
            return;
        }

        if(!CanVote(vote.vinDynode.prevout, vote.nBlockHeight)) {
            LogPrintf("DYNODEPAYMENTVOTE -- dynode already voted, dynode=%s\n", vote.vinDynode.prevout.ToStringShort());
            return;
        }

        if(!vote.CheckSignature()) {
            // do not ban for old dnw, DN simply might be not active anymore
            if(dynodeSync.IsSynced() && vote.nBlockHeight > pCurrentBlockIndex->nHeight) {
                LogPrintf("DYNODEPAYMENTVOTE -- invalid signature\n");
                Misbehaving(pfrom->GetId(), 20);
            }
            // it could just be a non-synced dynode
            dnodeman.AskForDN(pfrom, vote.vinDynode);
            return;
        }

        CTxDestination address1;
        ExtractDestination(vote.payee, address1);
        CDynamicAddress address2(address1);

        LogPrint("dnpayments", "DYNODEPAYMENTVOTE -- vote: address=%s, nBlockHeight=%d, nHeight=%d, prevout=%s\n", address2.ToString(), vote.nBlockHeight, pCurrentBlockIndex->nHeight, vote.vinDynode.prevout.ToStringShort());

        if(AddPaymentVote(vote)){
            vote.Relay();
            dynodeSync.AddedPaymentVote();
        }
    }
}

bool CDynodePaymentVote::Sign()
{
    std::string strError;
    std::string strMessage = vinDynode.prevout.ToStringShort() +
                boost::lexical_cast<std::string>(nBlockHeight) +
                ScriptToAsmStr(payee);

    if(!privateSendSigner.SignMessage(strMessage, vchSig, activeDynode.keyDynode)) {
        LogPrintf("CDynodePaymentVote::Sign -- SignMessage() failed\n");
        return false;
    }

    if(!privateSendSigner.VerifyMessage(activeDynode.pubKeyDynode, vchSig, strMessage, strError)) {
        LogPrintf("CDynodePaymentVote::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CDynodePayments::GetBlockPayee(int nBlockHeight, CScript& payee)
{
    if(mapDynodeBlocks.count(nBlockHeight)){
        return mapDynodeBlocks[nBlockHeight].GetBestPayee(payee);
    }

    return false;
}

// Is this dynode scheduled to get paid soon?
// -- Only look ahead up to 8 blocks to allow for propagation of the latest 2 blocks of votes
bool CDynodePayments::IsScheduled(CDynode& dn, int nNotBlockHeight)
{
    LOCK(cs_mapDynodeBlocks);

    if(!pCurrentBlockIndex) return false;

    CScript dnpayee;
    dnpayee = GetScriptForDestination(dn.pubKeyCollateralAddress.GetID());

    CScript payee;
    for(int64_t h = pCurrentBlockIndex->nHeight; h <= pCurrentBlockIndex->nHeight + 8; h++){
        if(h == nNotBlockHeight) continue;
        if(mapDynodeBlocks.count(h) && mapDynodeBlocks[h].GetBestPayee(payee) && dnpayee == payee) {
            return true;
        }
    }

    return false;
}

bool CDynodePayments::AddPaymentVote(const CDynodePaymentVote& vote)
{
    uint256 blockHash = uint256();
    if(!GetBlockHash(blockHash, vote.nBlockHeight - 101)) return false;

    LOCK2(cs_mapDynodePaymentVotes, cs_mapDynodeBlocks);

    if(mapDynodePaymentVotes.count(vote.GetHash())) return false;

    mapDynodePaymentVotes[vote.GetHash()] = vote;

    if(!mapDynodeBlocks.count(vote.nBlockHeight)) {
       CDynodeBlockPayees blockPayees(vote.nBlockHeight);
       mapDynodeBlocks[vote.nBlockHeight] = blockPayees;
    }

    mapDynodeBlocks[vote.nBlockHeight].AddPayee(vote);

    return true;
}

void CDynodeBlockPayees::AddPayee(const CDynodePaymentVote& vote)
{
    LOCK(cs_vecPayees);

    BOOST_FOREACH(CDynodePayee& payee, vecPayees) {
        if (payee.GetPayee() == vote.payee) {
            payee.AddVoteHash(vote.GetHash());
            return;
        }
    }
    CDynodePayee payeeNew(vote.payee, vote.GetHash());
    vecPayees.push_back(payeeNew);
}

bool CDynodeBlockPayees::GetBestPayee(CScript& payeeRet)
{
    LOCK(cs_vecPayees);

    if(!vecPayees.size()) {
        LogPrint("dnpayments", "CDynodeBlockPayees::GetBestPayee -- ERROR: couldn't find any payee\n");
        return false;
    }

    int nVotes = -1;
    BOOST_FOREACH(CDynodePayee& payee, vecPayees) {
        if (payee.GetVoteCount() > nVotes) {
            payeeRet = payee.GetPayee();
            nVotes = payee.GetVoteCount();
        }
    }

    return (nVotes > -1);
}

bool CDynodeBlockPayees::HasPayeeWithVotes(CScript payeeIn, int nVotesReq)
{
    LOCK(cs_vecPayees);

    BOOST_FOREACH(CDynodePayee& payee, vecPayees) {
        if (payee.GetVoteCount() >= nVotesReq && payee.GetPayee() == payeeIn) {
            return true;
        }
    }

    LogPrint("dnpayments", "CDynodeBlockPayees::HasPayeeWithVotes -- ERROR: couldn't find any payee with %d+ votes\n", nVotesReq);
    return false;
}

bool CDynodeBlockPayees::IsTransactionValid(const CTransaction& txNew)
{
    LOCK(cs_vecPayees);

    int nMaxSignatures = 0;
    std::string strPayeesPossible = "";

    CAmount nDynodePayment = STATIC_DYNODE_PAYMENT;

    //require at least DNPAYMENTS_SIGNATURES_REQUIRED signatures

    BOOST_FOREACH(CDynodePayee& payee, vecPayees) {
        if (payee.GetVoteCount() >= nMaxSignatures) {
            nMaxSignatures = payee.GetVoteCount();
        }
    }

    // if we don't have at least DNPAYMENTS_SIGNATURES_REQUIRED signatures on a payee, approve whichever is the longest chain
    if(nMaxSignatures < DNPAYMENTS_SIGNATURES_REQUIRED) return true;

    BOOST_FOREACH(CDynodePayee& payee, vecPayees) {
        if (payee.GetVoteCount() >= DNPAYMENTS_SIGNATURES_REQUIRED) {
            BOOST_FOREACH(CTxOut txout, txNew.vout) {
                if (payee.GetPayee() == txout.scriptPubKey && nDynodePayment == txout.nValue) {
                    LogPrint("dnpayments", "CDynodeBlockPayees::IsTransactionValid -- Found required payment\n");
                    return true;
                }
            }

            CTxDestination address1;
            ExtractDestination(payee.GetPayee(), address1);
            CDynamicAddress address2(address1);

            if(strPayeesPossible == "") {
                strPayeesPossible = address2.ToString();
            } else {
                strPayeesPossible += "," + address2.ToString();
            }
        }
    }

    LogPrintf("CDynodeBlockPayees::IsTransactionValid -- ERROR: Missing required payment, possible payees: '%s', amount: %f DYN\n", strPayeesPossible, (float)nDynodePayment/COIN);
    return false;
}

std::string CDynodeBlockPayees::GetRequiredPaymentsString()
{
    LOCK(cs_vecPayees);

    std::string strRequiredPayments = "Unknown";

    BOOST_FOREACH(CDynodePayee& payee, vecPayees)
    {
        CTxDestination address1;
        ExtractDestination(payee.GetPayee(), address1);
        CDynamicAddress address2(address1);

        if (strRequiredPayments != "Unknown") {
            strRequiredPayments += ", " + address2.ToString() + ":" + boost::lexical_cast<std::string>(payee.GetVoteCount());
        } else {
            strRequiredPayments = address2.ToString() + ":" + boost::lexical_cast<std::string>(payee.GetVoteCount());
        }
    }

    return strRequiredPayments;
}

std::string CDynodePayments::GetRequiredPaymentsString(int nBlockHeight)
{
    LOCK(cs_mapDynodeBlocks);

    if(mapDynodeBlocks.count(nBlockHeight)){
        return mapDynodeBlocks[nBlockHeight].GetRequiredPaymentsString();
    }

    return "Unknown";
}

bool CDynodePayments::IsTransactionValid(const CTransaction& txNew, int nBlockHeight)
{
    LOCK(cs_mapDynodeBlocks);

    if(mapDynodeBlocks.count(nBlockHeight)){
        return mapDynodeBlocks[nBlockHeight].IsTransactionValid(txNew);
    }

    return true;
}

void CDynodePayments::CheckAndRemove()
{
    if(!pCurrentBlockIndex) return;

    LOCK2(cs_mapDynodePaymentVotes, cs_mapDynodeBlocks);

    int nLimit = GetStorageLimit();

    std::map<uint256, CDynodePaymentVote>::iterator it = mapDynodePaymentVotes.begin();
    while(it != mapDynodePaymentVotes.end()) {
        CDynodePaymentVote vote = (*it).second;

        if(pCurrentBlockIndex->nHeight - vote.nBlockHeight > nLimit) {
            LogPrint("dnpayments", "CDynodePayments::CheckAndRemove -- Removing old Dynode payment: nBlockHeight=%d\n", vote.nBlockHeight);
            mapDynodePaymentVotes.erase(it++);
            mapDynodeBlocks.erase(vote.nBlockHeight);
        } else {
            ++it;
        }
    }
    LogPrintf("CDynodePayments::CheckAndRemove -- %s\n", ToString());
}

bool CDynodePaymentVote::IsValid(CNode* pnode, int nValidationHeight, std::string& strError)
{
    CDynode* pdn = dnodeman.Find(vinDynode);

    if(!pdn) {
        strError = strprintf("Unknown Dynode: prevout=%s", vinDynode.prevout.ToStringShort());
        // Only ask if we are already synced and still have no idea about that Dynode
        if(dynodeSync.IsSynced()) {
            dnodeman.AskForDN(pnode, vinDynode);
        }

        return false;
    }

    int nMinRequiredProtocol;
    if(nBlockHeight > nValidationHeight) {
        // new votes must comply SPORK_10_DYNODE_PAY_UPDATED_NODES rules
        nMinRequiredProtocol = dnpayments.GetMinDynodePaymentsProto();
    } else {
        // allow non-updated dynodes for old blocks
        nMinRequiredProtocol = MIN_DYNODE_PAYMENT_PROTO_VERSION_1;
    }

    if(pdn->nProtocolVersion < nMinRequiredProtocol) {
        strError = strprintf("Dynode protocol is too old: nProtocolVersion=%d, nMinRequiredProtocol=%d", pdn->nProtocolVersion, nMinRequiredProtocol);
        return false;
    }

    int nRank = dnodeman.GetDynodeRank(vinDynode, nBlockHeight - 101, nMinRequiredProtocol);

    if(nRank > DNPAYMENTS_SIGNATURES_TOTAL) {
        // It's common to have dynodes mistakenly think they are in the top 10
        // We don't want to print all of these messages in normal mode, debug mode should print though
        strError = strprintf("Dynode is not in the top %d (%d)", DNPAYMENTS_SIGNATURES_TOTAL, nRank);
        // Only ban for new dnw which is out of bounds, for old dnw DN list itself might be way too much off
        if(nRank > DNPAYMENTS_SIGNATURES_TOTAL*2 && nBlockHeight > nValidationHeight) {
            strError = strprintf("Dynode is not in the top %d (%d)", DNPAYMENTS_SIGNATURES_TOTAL*2, nRank);
            LogPrintf("CDynodePaymentVote::IsValid -- Error: %s\n", strError);
            Misbehaving(pnode->GetId(), 20);
        }
        // Still invalid however
        return false;
    }

    return true;
}

bool CDynodePayments::ProcessBlock(int nBlockHeight)
{
    // DETERMINE IF WE SHOULD BE VOTING FOR THE NEXT PAYEE

    if(fLiteMode || !fDyNode) return false;

    // We have little chances to pick the right winner if winners list is out of sync
    // but we have no choice, so we'll try. However it doesn't make sense to even try to do so
    // if we have not enough data about dynodes.
    if(!dynodeSync.IsDynodeListSynced()) return false;

    int nRank = dnodeman.GetDynodeRank(activeDynode.vin, nBlockHeight - 101, GetMinDynodePaymentsProto());

    if (nRank == -1) {
        LogPrint("dnpayments", "CDynodePayments::ProcessBlock -- Unknown Dynode\n");
        return false;
    }

    if (nRank > DNPAYMENTS_SIGNATURES_TOTAL) {
        LogPrint("dnpayments", "CDynodePayments::ProcessBlock -- Dynode not in the top %d (%d)\n", DNPAYMENTS_SIGNATURES_TOTAL, nRank);
        return false;
    }


    // LOCATE THE NEXT DYNODE WHICH SHOULD BE PAID

    LogPrintf("CDynodePayments::ProcessBlock -- Start: nBlockHeight=%d, dynode=%s\n", nBlockHeight, activeDynode.vin.prevout.ToStringShort());

    // pay to the oldest DN that still had no payment but its input is old enough and it was active long enough
    int nCount = 0;
    CDynode *pdn = dnodeman.GetNextDynodeInQueueForPayment(nBlockHeight, true, nCount);

    if (pdn == NULL) {
        LogPrintf("CDynodePayments::ProcessBlock -- ERROR: Failed to find dynode to pay\n");
        return false;
    }

    LogPrintf("CDynodePayments::ProcessBlock -- Dynode found by GetNextDynodeInQueueForPayment(): %s\n", pdn->vin.prevout.ToStringShort());


    CScript payee = GetScriptForDestination(pdn->pubKeyCollateralAddress.GetID());

    CDynodePaymentVote voteNew(activeDynode.vin, nBlockHeight, payee);

    CTxDestination address1;
    ExtractDestination(payee, address1);
    CDynamicAddress address2(address1);

    LogPrintf("CDynodePayments::ProcessBlock -- vote: payee=%s, nBlockHeight=%d\n", address2.ToString(), nBlockHeight);

    // SIGN MESSAGE TO NETWORK WITH OUR DYNODE KEYS

    LogPrintf("CDynodePayments::ProcessBlock -- Signing vote\n");
    if (voteNew.Sign()) {
        LogPrintf("CDynodePayments::ProcessBlock -- AddPaymentVote()\n");

        if (AddPaymentVote(voteNew)) {
            voteNew.Relay();
            return true;
        }
    }

    return false;
}

void CDynodePaymentVote::Relay()
{
    CInv inv(MSG_DYNODE_PAYMENT_VOTE, GetHash());
    RelayInv(inv);
}

bool CDynodePaymentVote::CheckSignature()
{

    CDynode* pdn = dnodeman.Find(vinDynode);

    if (!pdn) return false;

    std::string strMessage = vinDynode.prevout.ToStringShort() +
                boost::lexical_cast<std::string>(nBlockHeight) +
                ScriptToAsmStr(payee);

    std::string strError = "";
    if (!privateSendSigner.VerifyMessage(pdn->pubKeyDynode, vchSig, strMessage, strError)) {
        return error("CDynodePaymentVote::CheckSignature -- Got bad Dynode payment signature, dynode=%s, error: %s", vinDynode.prevout.ToStringShort().c_str(), strError);
    }

    return true;
}

std::string CDynodePaymentVote::ToString() const
{
    std::ostringstream info;

    info << vinDynode.prevout.ToStringShort() <<
            ", " << nBlockHeight <<
            ", " << ScriptToAsmStr(payee) <<
            ", " << (int)vchSig.size();

    return info.str();
}

// Send all votes up to nCountNeeded blocks (but not more than GetStorageLimit)
void CDynodePayments::Sync(CNode* pnode, int nCountNeeded)
{
    LOCK(cs_mapDynodeBlocks);

    if(!pCurrentBlockIndex) return;

    nCountNeeded = 0;

    int nInvCount = 0;

    for(int h = pCurrentBlockIndex->nHeight - nCountNeeded; h < pCurrentBlockIndex->nHeight + 20; h++) {
        if(mapDynodeBlocks.count(h)) {
            BOOST_FOREACH(CDynodePayee& payee, mapDynodeBlocks[h].vecPayees) {
                std::vector<uint256> vecVoteHashes = payee.GetVoteHashes();
                BOOST_FOREACH(uint256& hash, vecVoteHashes) {
                    pnode->PushInventory(CInv(MSG_DYNODE_PAYMENT_VOTE, hash));
                    nInvCount++;
                }
            }
        }
    }

    LogPrintf("CDynodePayments::Sync -- Sent %d votes to peer %d\n", nInvCount, pnode->id);
    pnode->PushMessage(NetMsgType::SYNCSTATUSCOUNT, DYNODE_SYNC_DNW, nInvCount);
}

// Request low data payment blocks in batches directly from some node instead of/after preliminary Sync.
void CDynodePayments::RequestLowDataPaymentBlocks(CNode* pnode)
{

    LOCK(cs_mapDynodeBlocks);

    std::vector<CInv> vToFetch;
    std::map<int, CDynodeBlockPayees>::iterator it = mapDynodeBlocks.begin();

    while(it != mapDynodeBlocks.end()) {
        int nTotalVotes = 0;
        bool fFound = false;
        BOOST_FOREACH(CDynodePayee& payee, it->second.vecPayees) {
            if(payee.GetVoteCount() >= DNPAYMENTS_SIGNATURES_REQUIRED) {
                fFound = true;
                break;
            }
            nTotalVotes += payee.GetVoteCount();
        }
        // A clear winner (DNPAYMENTS_SIGNATURES_REQUIRED+ votes) was found
        // or no clear winner was found but there are at least avg number of votes
        if(fFound || nTotalVotes >= (DNPAYMENTS_SIGNATURES_TOTAL + DNPAYMENTS_SIGNATURES_REQUIRED)/2) {
            // so just move to the next block
            ++it;
            continue;
        }
        // DEBUG
        DBG (
            // Let's see why this failed
            BOOST_FOREACH(CDynodePayee& payee, it->second.vecPayees) {
                CTxDestination address1;
                ExtractDestination(payee.GetPayee(), address1);
                CDynamicAddress address2(address1);
                printf("payee %s votes %d\n", address2.ToString().c_str(), payee.GetVoteCount());
            }
            printf("block %d votes total %d\n", it->first, nTotalVotes);
        )
        // END DEBUG
        // Low data block found, let's try to sync it
        uint256 hash;
        if(GetBlockHash(hash, it->first)) {
            vToFetch.push_back(CInv(MSG_DYNODE_PAYMENT_BLOCK, hash));
        }
        // We should not violate GETDATA rules
        if(vToFetch.size() == MAX_INV_SZ) {
            LogPrintf("CDynodePayments::SyncLowDataPaymentBlocks -- asking peer %d for %d blocks\n", pnode->id, MAX_INV_SZ);
            pnode->PushMessage(NetMsgType::GETDATA, vToFetch);
            // Start filling new batch
            vToFetch.clear();
        }
        ++it;
    }
    // Ask for the rest of it
    if(!vToFetch.empty()) {
        LogPrintf("CDynodePayments::SyncLowDataPaymentBlocks -- asking peer %d for %d blocks\n", pnode->id, vToFetch.size());
        pnode->PushMessage(NetMsgType::GETDATA, vToFetch);
    }
}

std::string CDynodePayments::ToString() const
{
    std::ostringstream info;

    info << "Votes: " << (int)mapDynodePaymentVotes.size() <<
            ", Blocks: " << (int)mapDynodeBlocks.size();

    return info.str();
}

bool CDynodePayments::IsEnoughData()
{
    float nAverageVotes = (DNPAYMENTS_SIGNATURES_TOTAL + DNPAYMENTS_SIGNATURES_REQUIRED) / 2;
    int nStorageLimit = GetStorageLimit();
    return GetBlockCount() > nStorageLimit && GetVoteCount() > nStorageLimit * nAverageVotes;
}

int CDynodePayments::GetStorageLimit()
{
    return std::max(int(dnodeman.size() * nStorageCoeff), nMinBlocksToStore);
}

void CDynodePayments::UpdatedBlockTip(const CBlockIndex *pindex)
{
    pCurrentBlockIndex = pindex;
    LogPrint("dnpayments", "CDynodePayments::UpdatedBlockTip -- pCurrentBlockIndex->nHeight=%d\n", pCurrentBlockIndex->nHeight);

    ProcessBlock(pindex->nHeight + 10);
}

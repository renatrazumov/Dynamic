// Copyright (c) 2014-2017 The Dash Core Developers
// Copyright (c) 2016-2017 Duality Blockchain Solutions Ltd
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activedynode.h"
#include "consensus/validation.h"
#include "privatesend.h"
#include "init.h"
#include "governance.h"
#include "dynode.h"
#include "dynode-payments.h"
#include "dynode-sync.h"
#include "dynodeman.h"
#include "util.h"

#include <boost/lexical_cast.hpp>


CDynode::CDynode() :
    vin(),
    addr(),
    pubKeyCollateralAddress(),
    pubKeyDynode(),
    lastPing(),
    vchSig(),
    sigTime(GetAdjustedTime()),
    nLastPsq(0),
    nTimeLastChecked(0),
    nTimeLastPaid(0),
    nTimeLastWatchdogVote(0),
    nActiveState(DYNODE_ENABLED),
    nCacheCollateralBlock(0),
    nBlockLastPaid(0),
    nProtocolVersion(PROTOCOL_VERSION),
    nPoSeBanScore(0),
    nPoSeBanHeight(0),
    fAllowMixingTx(true),
    fUnitTest(false)
{}

CDynode::CDynode(CService addrNew, CTxIn vinNew, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeyDynodeNew, int nProtocolVersionIn) :
    vin(vinNew),
    addr(addrNew),
    pubKeyCollateralAddress(pubKeyCollateralAddressNew),
    pubKeyDynode(pubKeyDynodeNew),
    lastPing(),
    vchSig(),
    sigTime(GetAdjustedTime()),
    nLastPsq(0),
    nTimeLastChecked(0),
    nTimeLastPaid(0),
    nTimeLastWatchdogVote(0),
    nActiveState(DYNODE_ENABLED),
    nCacheCollateralBlock(0),
    nBlockLastPaid(0),
    nProtocolVersion(nProtocolVersionIn),
    nPoSeBanScore(0),
    nPoSeBanHeight(0),
    fAllowMixingTx(true),
    fUnitTest(false)
{}

CDynode::CDynode(const CDynode& other) :
    vin(other.vin),
    addr(other.addr),
    pubKeyCollateralAddress(other.pubKeyCollateralAddress),
    pubKeyDynode(other.pubKeyDynode),
    lastPing(other.lastPing),
    vchSig(other.vchSig),
    sigTime(other.sigTime),
    nLastPsq(other.nLastPsq),
    nTimeLastChecked(other.nTimeLastChecked),
    nTimeLastPaid(other.nTimeLastPaid),
    nTimeLastWatchdogVote(other.nTimeLastWatchdogVote),
    nActiveState(other.nActiveState),
    nCacheCollateralBlock(other.nCacheCollateralBlock),
    nBlockLastPaid(other.nBlockLastPaid),
    nProtocolVersion(other.nProtocolVersion),
    nPoSeBanScore(other.nPoSeBanScore),
    nPoSeBanHeight(other.nPoSeBanHeight),
    fAllowMixingTx(other.fAllowMixingTx),
    fUnitTest(other.fUnitTest)
{}

CDynode::CDynode(const CDynodeBroadcast& dnb) :
    vin(dnb.vin),
    addr(dnb.addr),
    pubKeyCollateralAddress(dnb.pubKeyCollateralAddress),
    pubKeyDynode(dnb.pubKeyDynode),
    lastPing(dnb.lastPing),
    vchSig(dnb.vchSig),
    sigTime(dnb.sigTime),
    nLastPsq(dnb.nLastPsq),
    nTimeLastChecked(0),
    nTimeLastPaid(0),
    nTimeLastWatchdogVote(dnb.sigTime),
    nActiveState(DYNODE_ENABLED),
    nCacheCollateralBlock(0),
    nBlockLastPaid(0),
    nProtocolVersion(dnb.nProtocolVersion),
    nPoSeBanScore(0),
    nPoSeBanHeight(0),
    fAllowMixingTx(true),
    fUnitTest(false)
{}

//
// When a new dynode broadcast is sent, update our information
//
bool CDynode::UpdateFromNewBroadcast(CDynodeBroadcast& dnb)
{
    if(dnb.sigTime <= sigTime) return false;

    pubKeyDynode = dnb.pubKeyDynode;
    sigTime = dnb.sigTime;
    vchSig = dnb.vchSig;
    nProtocolVersion = dnb.nProtocolVersion;
    addr = dnb.addr;
    nPoSeBanScore = 0;
    nPoSeBanHeight = 0;
    nTimeLastChecked = 0;
    nTimeLastWatchdogVote = dnb.sigTime;
    int nDos = 0;
    if(dnb.lastPing == CDynodePing() || (dnb.lastPing != CDynodePing() && dnb.lastPing.CheckAndUpdate(nDos, false))) {
        lastPing = dnb.lastPing;
        dnodeman.mapSeenDynodePing.insert(std::make_pair(lastPing.GetHash(), lastPing));
    }
    // if it matches our Dynode privkey...
    if(fDyNode && pubKeyDynode == activeDynode.pubKeyDynode) {
        nPoSeBanScore = -DYNODE_POSE_BAN_MAX_SCORE;
        if(nProtocolVersion == PROTOCOL_VERSION) {
            // ... and PROTOCOL_VERSION, then we've been remotely activated ...
            activeDynode.ManageState();
        } else {
            // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
            // but also do not ban the node we get this message from
            LogPrintf("CDynode::UpdateFromNewBroadcast -- wrong PROTOCOL_VERSION, re-activate your DN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", nProtocolVersion, PROTOCOL_VERSION);
            return false;
        }
    }
    return true;
}

//
// Deterministically calculate a given "score" for a Dynode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block
//
arith_uint256 CDynode::CalculateScore(const uint256& blockHash)
{
    uint256 aux = ArithToUint256(UintToArith256(vin.prevout.hash) + vin.prevout.n);

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << blockHash;
    arith_uint256 hash2 = UintToArith256(ss.GetHash());

    CHashWriter ss2(SER_GETHASH, PROTOCOL_VERSION);
    ss2 << blockHash;
    ss2 << aux;
    arith_uint256 hash3 = UintToArith256(ss2.GetHash());

    return (hash3 > hash2 ? hash3 - hash2 : hash2 - hash3);
}

void CDynode::Check(bool fForce)
{
    LOCK(cs);

    static int64_t nTimeStart = GetTime();

    LogPrint("dynode", "CDynode::Check start -- vin %s\n", vin.prevout.ToStringShort());

    //once spent, stop doing the checks
    if(nActiveState == DYNODE_OUTPOINT_SPENT) return;

    if(ShutdownRequested()) return;

    if(!fForce && (GetTime() - nTimeLastChecked < DYNODE_CHECK_SECONDS)) return;
    nTimeLastChecked = GetTime();

    int nHeight = 0;
    if(!fUnitTest) {
        TRY_LOCK(cs_main, lockMain);
        if(!lockMain) return;

        CCoins coins;
        if(!pcoinsTip->GetCoins(vin.prevout.hash, coins) ||
           (unsigned int)vin.prevout.n>=coins.vout.size() ||
           coins.vout[vin.prevout.n].IsNull()) {
            nActiveState = DYNODE_OUTPOINT_SPENT;
            LogPrint("dynode", "CDynode::Check -- Failed to find Dynode UTXO, dynode=%s\n", vin.prevout.ToStringShort());
            return;
        }

        nHeight = chainActive.Height();
    }

    // keep old dynodes on start, give them a chance to receive an updated ping without removal/expiry
    if(!dynodeSync.IsDynodeListSynced()) nTimeStart = GetTime();
    bool fWaitForPing = (GetTime() - nTimeStart < DYNODE_MIN_DNP_SECONDS);

    if(nActiveState == DYNODE_POSE_BAN) {
        if(nHeight < nPoSeBanHeight) return; // too early?
        // Otherwise give it a chance to proceed further to do all the usual checks and to change its state.
        // Dynode still will be on the edge and can be banned back easily if it keeps ignoring dnverify
        // or connect attempts. Will require few dnverify messages to strengthen its position in dn list.
        LogPrintf("CDynode::Check -- Dynode %s is unbanned and back in list now\n", vin.prevout.ToStringShort());
        DecreasePoSeBanScore();
    } else if(nPoSeBanScore >= DYNODE_POSE_BAN_MAX_SCORE) {
        nActiveState = DYNODE_POSE_BAN;
        // ban for the whole payment cycle
        nPoSeBanHeight = nHeight + dnodeman.size();
        LogPrintf("CDynode::Check -- Dynode %s is banned till block %d now\n", vin.prevout.ToStringShort(), nPoSeBanHeight);
        return;
    }

                   // dynode doesn't meet payment protocol requirements ...
    bool fRemove = nProtocolVersion < dnpayments.GetMinDynodePaymentsProto() ||
                   // or it's our own node and we just updated it to the new protocol but we are still waiting for activation ...
                   (pubKeyDynode == activeDynode.pubKeyDynode && nProtocolVersion < PROTOCOL_VERSION);

    if(fRemove) {
        // it should be removed from the list
        nActiveState = DYNODE_REMOVE;

        // RESCAN AFFECTED VOTES
        FlagGovernanceItemsAsDirty();
        return;
    }

    bool fWatchdogActive = dnodeman.IsWatchdogActive();
    bool fWatchdogExpired = (fWatchdogActive && ((GetTime() - nTimeLastWatchdogVote) > DYNODE_WATCHDOG_MAX_SECONDS));

    LogPrint("dynode", "CDynode::Check -- vin %s, nTimeLastWatchdogVote %d, GetTime() %d, fWatchdogExpired %d\n",
            vin.prevout.ToStringShort(), nTimeLastWatchdogVote, GetTime(), fWatchdogExpired);

    if(fWatchdogExpired) {
        nActiveState = DYNODE_WATCHDOG_EXPIRED;
        return;
    }

    if(!fWaitForPing && !IsPingedWithin(DYNODE_EXPIRATION_SECONDS)) {
        nActiveState = DYNODE_EXPIRED;
        // RESCAN AFFECTED VOTES
        FlagGovernanceItemsAsDirty();
        return;
    }

    if(lastPing.sigTime - sigTime < DYNODE_MIN_DNP_SECONDS) {
        nActiveState = DYNODE_PRE_ENABLED;
        return;
    }

    nActiveState = DYNODE_ENABLED; // OK
}

bool CDynode::IsValidNetAddr()
{
    // TODO: regtest is fine with any addresses for now,
    // should probably be a bit smarter if one day we start to implement tests for this
    return Params().NetworkIDString() == CBaseChainParams::REGTEST ||
            (addr.IsIPv4() && IsReachable(addr) && addr.IsRoutable());
}

dynode_info_t CDynode::GetInfo()
{
    dynode_info_t info;
    info.vin = vin;
    info.addr = addr;
    info.pubKeyCollateralAddress = pubKeyCollateralAddress;
    info.pubKeyDynode = pubKeyDynode;
    info.sigTime = sigTime;
    info.nLastPsq = nLastPsq;
    info.nTimeLastChecked = nTimeLastChecked;
    info.nTimeLastPaid = nTimeLastPaid;
    info.nTimeLastWatchdogVote = nTimeLastWatchdogVote;
    info.nActiveState = nActiveState;
    info.nProtocolVersion = nProtocolVersion;
    info.fInfoValid = true;
    return info;
}

std::string CDynode::StateToString(int nStateIn)
{
    switch(nStateIn) {
        case CDynode::DYNODE_PRE_ENABLED:       return "PRE_ENABLED";
        case CDynode::DYNODE_ENABLED:           return "ENABLED";
        case CDynode::DYNODE_EXPIRED:           return "EXPIRED";
        case CDynode::DYNODE_OUTPOINT_SPENT:    return "OUTPOINT_SPENT";
        case CDynode::DYNODE_REMOVE:            return "REMOVE";
        case CDynode::DYNODE_WATCHDOG_EXPIRED:  return "WATCHDOG_EXPIRED";
        case CDynode::DYNODE_POSE_BAN:          return "POSE_BAN";
        default:                                        return "UNKNOWN";
    }
}

std::string CDynode::GetStateString() const
{
    return StateToString(nActiveState);
}

std::string CDynode::GetStatus() const
{
    // TODO: return smth a bit more human readable here
    return GetStateString();
}

int CDynode::GetCollateralAge()
{
    int nHeight;
    {
        TRY_LOCK(cs_main, lockMain);
        if(!lockMain || !chainActive.Tip()) return -1;
        nHeight = chainActive.Height();
    }

    if (nCacheCollateralBlock == 0) {
        int nInputAge = GetInputAge(vin);
        if(nInputAge > 0) {
            nCacheCollateralBlock = nHeight - nInputAge;
        } else {
            return nInputAge;
        }
    }

    return nHeight - nCacheCollateralBlock;
}

void CDynode::UpdateLastPaid(const CBlockIndex *pindex, int nMaxBlocksToScanBack)
{
    if(!pindex) return;

    const CBlockIndex *BlockReading = pindex;

    CScript dnpayee = GetScriptForDestination(pubKeyCollateralAddress.GetID());
    // LogPrint("dynode", "CDynode::UpdateLastPaidBlock -- searching for block with payment to %s\n", vin.prevout.ToStringShort());

    LOCK(cs_mapDynodeBlocks);

    for (int i = 0; BlockReading && BlockReading->nHeight > nBlockLastPaid && i < nMaxBlocksToScanBack; i++) {
        if(dnpayments.mapDynodeBlocks.count(BlockReading->nHeight) &&
            dnpayments.mapDynodeBlocks[BlockReading->nHeight].HasPayeeWithVotes(dnpayee, 2))
        {
            CBlock block;
            if(!ReadBlockFromDisk(block, BlockReading, Params().GetConsensus())) // shouldn't really happen
                continue;

            CAmount nDynodePayment = STATIC_DYNODE_PAYMENT;

            BOOST_FOREACH(CTxOut txout, block.vtx[0].vout)
                if(dnpayee == txout.scriptPubKey && nDynodePayment == txout.nValue) {
                    nBlockLastPaid = BlockReading->nHeight;
                    nTimeLastPaid = BlockReading->nTime;
                    LogPrint("dynode", "CDynode::UpdateLastPaidBlock -- searching for block with payment to %s -- found new %d\n", vin.prevout.ToStringShort(), nBlockLastPaid);
                    return;
                }
        }

        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
        BlockReading = BlockReading->pprev;
    }

    // Last payment for this dynode wasn't found in latest dnpayments blocks
    // or it was found in dnpayments blocks but wasn't found in the blockchain.
    // LogPrint("dynode", "CDynode::UpdateLastPaidBlock -- searching for block with payment to %s -- keeping old %d\n", vin.prevout.ToStringShort(), nBlockLastPaid);
}

bool CDynodeBroadcast::Create(std::string strService, std::string strKeyDynode, std::string strTxHash, std::string strOutputIndex, std::string& strErrorRet, CDynodeBroadcast &dnbRet, bool fOffline)
{
    CTxIn txin;
    CPubKey pubKeyCollateralAddressNew;
    CKey keyCollateralAddressNew;
    CPubKey pubKeyDynodeNew;
    CKey keyDynodeNew;

    //need correct blocks to send ping
    if(!fOffline && !dynodeSync.IsBlockchainSynced()) {
        strErrorRet = "Sync in progress. Must wait until sync is complete to start Dynode";
        LogPrintf("CDynodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    if(!privateSendSigner.GetKeysFromSecret(strKeyDynode, keyDynodeNew, pubKeyDynodeNew)) {
        strErrorRet = strprintf("Invalid dynode key %s", strKeyDynode);
        LogPrintf("CDynodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    if(!pwalletMain->GetDynodeVinAndKeys(txin, pubKeyCollateralAddressNew, keyCollateralAddressNew, strTxHash, strOutputIndex)) {
        strErrorRet = strprintf("Could not allocate txin %s:%s for dynode %s", strTxHash, strOutputIndex, strService);
        LogPrintf("CDynodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    CService service = CService(strService);
    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(service.GetPort() != mainnetDefaultPort) {
            strErrorRet = strprintf("Invalid port %u for dynode %s, only %d is supported on mainnet.", service.GetPort(), strService, mainnetDefaultPort);
            LogPrintf("CDynodeBroadcast::Create -- %s\n", strErrorRet);
            return false;
        }
    } else if (service.GetPort() == mainnetDefaultPort) {
        strErrorRet = strprintf("Invalid port %u for dynode %s, %d is the only supported on mainnet.", service.GetPort(), strService, mainnetDefaultPort);
        LogPrintf("CDynodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    return Create(txin, CService(strService), keyCollateralAddressNew, pubKeyCollateralAddressNew, keyDynodeNew, pubKeyDynodeNew, strErrorRet, dnbRet);
}

bool CDynodeBroadcast::Create(CTxIn txin, CService service, CKey keyCollateralAddressNew, CPubKey pubKeyCollateralAddressNew, CKey keyDynodeNew, CPubKey pubKeyDynodeNew, std::string &strErrorRet, CDynodeBroadcast &dnbRet)
{
    // wait for reindex and/or import to finish
    if (fImporting || fReindex) return false;

    LogPrint("dynode", "CDynodeBroadcast::Create -- pubKeyCollateralAddressNew = %s, pubKeyDynodeNew.GetID() = %s\n",
             CDynamicAddress(pubKeyCollateralAddressNew.GetID()).ToString(),
             pubKeyDynodeNew.GetID().ToString());


    CDynodePing snp(txin);
    if(!snp.Sign(keyDynodeNew, pubKeyDynodeNew)) {
        strErrorRet = strprintf("Failed to sign ping, dynode=%s", txin.prevout.ToStringShort());
        LogPrintf("CDynodeBroadcast::Create -- %s\n", strErrorRet);
        dnbRet = CDynodeBroadcast();
        return false;
    }

    dnbRet = CDynodeBroadcast(service, txin, pubKeyCollateralAddressNew, pubKeyDynodeNew, PROTOCOL_VERSION);

    if(!dnbRet.IsValidNetAddr()) {
        strErrorRet = strprintf("Invalid IP address, dynode=%s", txin.prevout.ToStringShort());
        LogPrintf("CDynodeBroadcast::Create -- %s\n", strErrorRet);
        dnbRet = CDynodeBroadcast();
        return false;
    }

    dnbRet.lastPing = snp;
    if(!dnbRet.Sign(keyCollateralAddressNew)) {
        strErrorRet = strprintf("Failed to sign broadcast, dynode=%s", txin.prevout.ToStringShort());
        LogPrintf("CDynodeBroadcast::Create -- %s\n", strErrorRet);
        dnbRet = CDynodeBroadcast();
        return false;
    }

    return true;
}

bool CDynodeBroadcast::SimpleCheck(int& nDos)
{
    nDos = 0;

    // make sure addr is valid
    if(!IsValidNetAddr()) {
        LogPrintf("CDynodeBroadcast::SimpleCheck -- Invalid addr, rejected: dynode=%s  addr=%s\n",
                    vin.prevout.ToStringShort(), addr.ToString());
        return false;
    }

    // make sure signature isn't in the future (past is OK)
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CDynodeBroadcast::SimpleCheck -- Signature rejected, too far into the future: dynode=%s\n", vin.prevout.ToStringShort());
        nDos = 1;
        return false;
    }

    // empty ping or incorrect sigTime/blockhash
    if(lastPing == CDynodePing() || !lastPing.CheckAndUpdate(nDos, false, true)) {
        return false;
    }

    if(nProtocolVersion < dnpayments.GetMinDynodePaymentsProto()) {
        LogPrintf("CDynodeBroadcast::SimpleCheck -- ignoring outdated Dynode: dynode=%s  nProtocolVersion=%d\n", vin.prevout.ToStringShort(), nProtocolVersion);
        return false;
    }

    CScript pubkeyScript;
    pubkeyScript = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    if(pubkeyScript.size() != 25) {
        LogPrintf("CDynodeBroadcast::SimpleCheck -- pubKeyCollateralAddress has the wrong size\n");
        nDos = 100;
        return false;
    }

    CScript pubkeyScript2;
    pubkeyScript2 = GetScriptForDestination(pubKeyDynode.GetID());

    if(pubkeyScript2.size() != 25) {
        LogPrintf("CDynodeBroadcast::SimpleCheck -- pubKeyDynode has the wrong size\n");
        nDos = 100;
        return false;
    }

    if(!vin.scriptSig.empty()) {
        LogPrintf("CDynodeBroadcast::SimpleCheck -- Ignore Not Empty ScriptSig %s\n",vin.ToString());
        return false;
    }

    if (!CheckSignature(nDos)) {
        LogPrintf("CDynodeBroadcast::SimpleCheck -- CheckSignature() failed, dynode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(addr.GetPort() != mainnetDefaultPort) return false;
    } else if(addr.GetPort() == mainnetDefaultPort) return false;

    return true;
}

bool CDynodeBroadcast::Update(CDynode* pdn, int& nDos)
{
    if(pdn->sigTime == sigTime) {
        // mapSeenDynodeBroadcast in CDynodeMan::CheckDnbAndUpdateDynodeList should filter legit duplicates
        // but this still can happen if we just started, which is ok, just do nothing here.
        return true;
    }

    // this broadcast is older than the one that we already have - it's bad and should never happen
    // unless someone is doing something fishy
    if(pdn->sigTime > sigTime) {
        LogPrintf("CDynodeBroadcast::Update -- Bad sigTime %d (existing broadcast is at %d) for Dynode %s %s\n",
                      sigTime, pdn->sigTime, vin.prevout.ToStringShort(), addr.ToString());
        return false;
    }

    pdn->Check();

    // dynode is banned by PoSe
    if(pdn->IsPoSeBanned()) {
        LogPrintf("CDynodeBroadcast::Update -- Banned by PoSe, dynode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    // IsVnAssociatedWithPubkey is validated once in CheckOutpoint, after that they just need to match
    if(pdn->pubKeyCollateralAddress != pubKeyCollateralAddress) {
        LogPrintf("CDynodeMan::Update -- Got mismatched pubKeyCollateralAddress and vin\n");
        nDos = 33;
        return false;
    }

    // if ther was no dynode broadcast recently or if it matches our Dynode privkey...
    if(!pdn->IsBroadcastedWithin(DYNODE_MIN_DNB_SECONDS) || (fDyNode && pubKeyDynode == activeDynode.pubKeyDynode)) {
        // take the newest entry
        LogPrintf("CDynodeBroadcast::Update -- Got UPDATED Dynode entry: addr=%s\n", addr.ToString());
        if(pdn->UpdateFromNewBroadcast((*this))) {
            pdn->Check();
            Relay();
        }
        dynodeSync.AddedDynodeList();
    }

    return true;
}

bool CDynodeBroadcast::CheckOutpoint(int& nDos)
{
    // we are a dynode with the same vin (i.e. already activated) and this dnb is ours (matches our Dynodes privkey)
    // so nothing to do here for us
    if(fDyNode && vin.prevout == activeDynode.vin.prevout && pubKeyDynode == activeDynode.pubKeyDynode) {
        return false;
    }

    {
        TRY_LOCK(cs_main, lockMain);
        if(!lockMain) {
            // not dnb fault, let it to be checked again later
            LogPrint("dynode", "CDynodeBroadcast::CheckOutpoint -- Failed to aquire lock, addr=%s", addr.ToString());
            dnodeman.mapSeenDynodeBroadcast.erase(GetHash());
            return false;
        }

        CCoins coins;
        if(!pcoinsTip->GetCoins(vin.prevout.hash, coins) ||
           (unsigned int)vin.prevout.n>=coins.vout.size() ||
           coins.vout[vin.prevout.n].IsNull()) {
            LogPrint("dynode", "CDynodeBroadcast::CheckOutpoint -- Failed to find Dynode UTXO, dynode=%s\n", vin.prevout.ToStringShort());
            return false;
        }
        if(coins.vout[vin.prevout.n].nValue != 1000 * COIN) {
            LogPrint("dynode", "CDynodeBroadcast::CheckOutpoint -- Dynode UTXO should have 1000 DYN, dynode=%s\n", vin.prevout.ToStringShort());
            return false;
        }
        if(chainActive.Height() - coins.nHeight + 1 < Params().GetConsensus().nDynodeMinimumConfirmations) {
            LogPrintf("CDynodeBroadcast::CheckOutpoint -- Dynode UTXO must have at least %d confirmations, dynode=%s\n",
                    Params().GetConsensus().nDynodeMinimumConfirmations, vin.prevout.ToStringShort());
            // maybe we miss few blocks, let this dnb to be checked again later
            dnodeman.mapSeenDynodeBroadcast.erase(GetHash());
            return false;
        }
    }

    LogPrint("dynode", "CDynodeBroadcast::CheckOutpoint -- Dynode UTXO verified\n");

    // make sure the vout that was signed is related to the transaction that spawned the Dynode
    //  - this is expensive, so it's only done once per Dynode
    if(!privateSendSigner.IsVinAssociatedWithPubkey(vin, pubKeyCollateralAddress)) {
        LogPrintf("CDynodeMan::CheckOutpoint -- Got mismatched pubKeyCollateralAddress and vin\n");
        nDos = 33;
        return false;
    }

    // verify that sig time is legit in past
    // should be at least not earlier than block when 1000 DYN tx got nDynodeMinimumConfirmations
    uint256 hashBlock = uint256();
    CTransaction tx2;
    GetTransaction(vin.prevout.hash, tx2, Params().GetConsensus(), hashBlock, true);
    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi != mapBlockIndex.end() && (*mi).second) {
            CBlockIndex* pDNIndex = (*mi).second; // block for 1000 DYN tx -> 1 confirmation
            CBlockIndex* pConfIndex = chainActive[pDNIndex->nHeight + Params().GetConsensus().nDynodeMinimumConfirmations - 1]; // block where tx got nDynodeMinimumConfirmations
            if(pConfIndex->GetBlockTime() > sigTime) {
                LogPrintf("CDynodeBroadcast::CheckOutpoint -- Bad sigTime %d (%d conf block is at %d) for Dynode %s %s\n",
                          sigTime, Params().GetConsensus().nDynodeMinimumConfirmations, pConfIndex->GetBlockTime(), vin.prevout.ToStringShort(), addr.ToString());
                return false;
            }
        }
    }

    return true;
}

bool CDynodeBroadcast::Sign(CKey& keyCollateralAddress)
{
    std::string strError;
    std::string strMessage;

    sigTime = GetAdjustedTime();

    strMessage = addr.ToString(false) + boost::lexical_cast<std::string>(sigTime) +
                    pubKeyCollateralAddress.GetID().ToString() + pubKeyDynode.GetID().ToString() +
                    boost::lexical_cast<std::string>(nProtocolVersion);

    if(!privateSendSigner.SignMessage(strMessage, vchSig, keyCollateralAddress)) {
        LogPrintf("CDynodeBroadcast::Sign -- SignMessage() failed\n");
        return false;
    }

    if(!privateSendSigner.VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, strError)) {
        LogPrintf("CDynodeBroadcast::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CDynodeBroadcast::CheckSignature(int& nDos)
{
    std::string strMessage;
    std::string strError = "";
    nDos = 0;

    strMessage = addr.ToString(false) + boost::lexical_cast<std::string>(sigTime) +
                    pubKeyCollateralAddress.GetID().ToString() + pubKeyDynode.GetID().ToString() +
                    boost::lexical_cast<std::string>(nProtocolVersion);

    LogPrint("dynode", "CDynodeBroadcast::CheckSignature -- strMessage: %s  pubKeyCollateralAddress address: %s  sig: %s\n", strMessage, CDynamicAddress(pubKeyCollateralAddress.GetID()).ToString(), EncodeBase64(&vchSig[0], vchSig.size()));

    if(!privateSendSigner.VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, strError)){
        LogPrintf("CDynodeBroadcast::CheckSignature -- Got bad Dynode announce signature, error: %s\n", strError);
        nDos = 100;
        return false;
    }

    return true;
}

void CDynodeBroadcast::Relay()
{
    CInv inv(MSG_DYNODE_ANNOUNCE, GetHash());
    RelayInv(inv);
}

CDynodePing::CDynodePing(CTxIn& vinNew)
{
    LOCK(cs_main);
    if (!chainActive.Tip() || chainActive.Height() < 12) return;

    vin = vinNew;
    blockHash = chainActive[chainActive.Height() - 12]->GetBlockHash();
    sigTime = GetAdjustedTime();
    vchSig = std::vector<unsigned char>();
}

bool CDynodePing::Sign(CKey& keyDynode, CPubKey& pubKeyDynode)
{
    std::string strError;
    std::string strDyNodeSignMessage;

    sigTime = GetAdjustedTime();
    std::string strMessage = vin.ToString() + blockHash.ToString() + boost::lexical_cast<std::string>(sigTime);

    if(!privateSendSigner.SignMessage(strMessage, vchSig, keyDynode)) {
        LogPrintf("CDynodePing::Sign -- SignMessage() failed\n");
        return false;
    }

    if(!privateSendSigner.VerifyMessage(pubKeyDynode, vchSig, strMessage, strError)) {
        LogPrintf("CDynodePing::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CDynodePing::CheckSignature(CPubKey& pubKeyDynode, int &nDos)
{
    std::string strMessage = vin.ToString() + blockHash.ToString() + boost::lexical_cast<std::string>(sigTime);
    std::string strError = "";
    nDos = 0;

    if(!privateSendSigner.VerifyMessage(pubKeyDynode, vchSig, strMessage, strError)) {
        LogPrintf("CDynodePing::CheckSignature -- Got bad Dynode ping signature, dynode=%s, error: %s\n", vin.prevout.ToStringShort(), strError);
        nDos = 33;
        return false;
    }
    return true;
}

bool CDynodePing::CheckAndUpdate(int& nDos, bool fRequireEnabled, bool fSimpleCheck)
{
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CDynodePing::CheckAndUpdate -- Signature rejected, too far into the future, dynode=%s\n", vin.prevout.ToStringShort());
        nDos = 1;
        return false;
    }

    if (sigTime <= GetAdjustedTime() - 60 * 60) {
        LogPrintf("CDynodePing::CheckAndUpdate -- Signature rejected, too far into the past: dynode=%s  sigTime=%d  GetAdjustedTime()=%d\n", vin.prevout.ToStringShort(), sigTime, GetAdjustedTime());
        nDos = 1;
        return false;
    }

    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if (mi == mapBlockIndex.end()) {
            LogPrint("dynode", "CDynodePing::CheckAndUpdate -- Dynode ping is invalid, unknown block hash: dynode=%s blockHash=%s\n", vin.prevout.ToStringShort(), blockHash.ToString());
            // maybe we stuck or forked so we shouldn't ban this node, just fail to accept this ping
            // TODO: or should we also request this block?
            return false;
        }
        if ((*mi).second && (*mi).second->nHeight < chainActive.Height() - 24) {
            LogPrintf("CDynodePing::CheckAndUpdate -- Dynode ping is invalid, block hash is too old: dynode=%s  blockHash=%s\n", vin.prevout.ToStringShort(), blockHash.ToString());
            // Do nothing here (no Dynode update, no snping relay)
            // Let this node to be visible but fail to accept snping
            return false;
        }
    }

    if (fSimpleCheck) {
        LogPrint("dynode", "CDynodePing::CheckAndUpdate -- ping verified in fSimpleCheck mode: dynode=%s  blockHash=%s  sigTime=%d\n", vin.prevout.ToStringShort(), blockHash.ToString(), sigTime);
        return true;
    }

    LogPrint("dynode", "CDynodePing::CheckAndUpdate -- New ping: dynode=%s  blockHash=%s  sigTime=%d\n", vin.prevout.ToStringShort(), blockHash.ToString(), sigTime);

    // see if we have this Dynode
    CDynode* pdn = dnodeman.Find(vin);

    if (pdn == NULL || pdn->nProtocolVersion < dnpayments.GetMinDynodePaymentsProto()) {
        LogPrint("dynode", "CDynodePing::CheckAndUpdate -- Couldn't find compatible Dynode entry, dynode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    if (fRequireEnabled && !pdn->IsEnabled() && !pdn->IsPreEnabled() && !pdn->IsWatchdogExpired()) return false;

    // LogPrintf("snping - Found corresponding dn for vin: %s\n", vin.prevout.ToStringShort());
    // update only if there is no known ping for this dynode or
    // last ping was more then DYNODE_MIN_DNP_SECONDS-60 ago comparing to this one
    if (pdn->IsPingedWithin(DYNODE_MIN_DNP_SECONDS - 60, sigTime)) {
        LogPrint("dynode", "CDynodePing::CheckAndUpdate -- Dynode ping arrived too early, dynode=%s\n", vin.prevout.ToStringShort());
        //nDos = 1; //disable, this is happening frequently and causing banned peers
        return false;
    }

    if (!CheckSignature(pdn->pubKeyDynode, nDos)) return false;

    // so, ping seems to be ok, let's store it
    LogPrint("dynode", "CDynodePing::CheckAndUpdate -- Dynode ping accepted, dynode=%s\n", vin.prevout.ToStringShort());
    pdn->lastPing = *this;

    // and update dnodeman.mapSeenDynodeBroadcast.lastPing which is probably outdated
    CDynodeBroadcast dnb(*pdn);
    uint256 hash = dnb.GetHash();
    if (dnodeman.mapSeenDynodeBroadcast.count(hash)) {
        dnodeman.mapSeenDynodeBroadcast[hash].lastPing = *this;
    }

    pdn->Check(true); // force update, ignoring cache
    if (!pdn->IsEnabled()) return false;

    LogPrint("dynode", "CDynodePing::CheckAndUpdate -- Dynode ping acceepted and relayed, dynode=%s\n", vin.prevout.ToStringShort());
    Relay();

    return true;
}

void CDynodePing::Relay()
{
    CInv inv(MSG_DYNODE_PING, GetHash());
    RelayInv(inv);
}

void CDynode::AddGovernanceVote(uint256 nGovernanceObjectHash)
{
    if(mapGovernanceObjectsVotedOn.count(nGovernanceObjectHash)) {
        mapGovernanceObjectsVotedOn[nGovernanceObjectHash]++;
    } else {
        mapGovernanceObjectsVotedOn.insert(std::make_pair(nGovernanceObjectHash, 1));
    }
}

void CDynode::RemoveGovernanceObject(uint256 nGovernanceObjectHash)
{
    std::map<uint256, int>::iterator it = mapGovernanceObjectsVotedOn.find(nGovernanceObjectHash);
    if(it == mapGovernanceObjectsVotedOn.end()) {
        return;
    }
    mapGovernanceObjectsVotedOn.erase(it);
}

void CDynode::UpdateWatchdogVoteTime()
{
    LOCK(cs);
    nTimeLastWatchdogVote = GetTime();
}

/**
*   FLAG GOVERNANCE ITEMS AS DIRTY
*
*   - When dynode come and go on the network, we must flag the items they voted on to recalc it's cached flags
*
*/
void CDynode::FlagGovernanceItemsAsDirty()
{
    std::map<uint256, int>::iterator it = mapGovernanceObjectsVotedOn.begin();
    while(it != mapGovernanceObjectsVotedOn.end()){
        CGovernanceObject *pObj = governance.FindGovernanceObject((*it).first);

        if(pObj) pObj->InvalidateVoteCache();
        ++it;
    }

    std::vector<uint256> vecDirty;
    {
        std::map<uint256, int>::iterator it = mapGovernanceObjectsVotedOn.begin();
        while(it != mapGovernanceObjectsVotedOn.end()) {
            vecDirty.push_back(it->first);
            ++it;
        }
    }
    for(size_t i = 0; i < vecDirty.size(); ++i) {
        dnodeman.AddDirtyGovernanceObjectHash(vecDirty[i]);
    }
}

// Copyright (c) 2014-2017 The Dash Core Developers
// Copyright (c) 2016-2017 Duality Blockchain Solutions Ltd
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"
#include "db.h"
#include "init.h"
#include "activedynode.h"
#include "privatesend.h"
#include "governance.h"
#include "dynode-payments.h"
#include "dynode-sync.h"
#include "dynodeconfig.h"
#include "dynodeman.h"
#include "rpcserver.h"
#include "utilmoneystr.h"

#include <fstream>
#include <iomanip>
#include <univalue.h>

void EnsureWalletIsUnlocked();

UniValue privatesend(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
            "privatesend \"command\"\n"
            "\nArguments:\n"
            "1. \"command\"        (string or set of strings, required) The command to execute\n"
            "\nAvailable commands:\n"
            "  start       - Start mixing\n"
            "  stop        - Stop mixing\n"
            "  reset       - Reset mixing\n"
            + HelpRequiringPassphrase());

    if(params[0].get_str() == "start") {
        if (pwalletMain->IsLocked(true))
            throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");

        if(fDyNode)
            return "Mixing is not supported from dynodes";

        fEnablePrivateSend = true;
        bool result = privateSendPool.DoAutomaticDenominating();
        return "Mixing " + (result ? "started successfully" : ("start failed: " + privateSendPool.GetStatus() + ", will retry"));
    }

    if(params[0].get_str() == "stop") {
        fEnablePrivateSend = false;
        return "Mixing was stopped";
    }

    if(params[0].get_str() == "reset") {
        privateSendPool.ResetPool();
        return "Mixing was reset";
    }

    return "Unknown command, please see \"help privatesend\"";
}

UniValue getpoolinfo(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw std::runtime_error(
            "getpoolinfo\n"
            "Returns an object containing mixing pool related information.\n");

    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("state",             privateSendPool.GetStateString()));
    obj.push_back(Pair("mixing_mode",       fPrivateSendMultiSession ? "multi-session" : "normal"));
    obj.push_back(Pair("queue",             privateSendPool.GetQueueSize()));
    obj.push_back(Pair("entries",           privateSendPool.GetEntriesCount()));
    obj.push_back(Pair("status",            privateSendPool.GetStatus()));

    if (privateSendPool.pSubmittedToDynode) {
        obj.push_back(Pair("outpoint",      privateSendPool.pSubmittedToDynode->vin.prevout.ToStringShort()));
        obj.push_back(Pair("addr",          privateSendPool.pSubmittedToDynode->addr.ToString()));
    }

    if (pwalletMain) {
        obj.push_back(Pair("keys_left",     pwalletMain->nKeysLeftSinceAutoBackup));
        obj.push_back(Pair("warnings",      pwalletMain->nKeysLeftSinceAutoBackup < PRIVATESEND_KEYS_THRESHOLD_WARNING
                                                ? "WARNING: keypool is almost depleted!" : ""));
    }

    return obj;
}


UniValue dynode(const UniValue& params, bool fHelp)
{
    std::string strCommand;
    if (params.size() >= 1) {
        strCommand = params[0].get_str();
    }

    if (strCommand == "start-many")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "DEPRECATED, please use start-all instead");

    if (fHelp  ||
        (strCommand != "start" && strCommand != "start-alias" && strCommand != "start-all" && strCommand != "start-missing" &&
         strCommand != "start-disabled" && strCommand != "list" && strCommand != "list-conf" && strCommand != "count" &&
         strCommand != "debug" && strCommand != "current" && strCommand != "winner" && strCommand != "winners" && strCommand != "genkey" &&
         strCommand != "connect" && strCommand != "outputs" && strCommand != "status"))
            throw std::runtime_error(
                "dynode \"command\"... ( \"passphrase\" )\n"
                "Set of commands to execute dynode-sync related actions\n"
                "\nArguments:\n"
                "1. \"command\"        (string or set of strings, required) The command to execute\n"
                "2. \"passphrase\"     (string, optional) The wallet passphrase\n"
                "\nAvailable commands:\n"
                "  count        - Print number of all known dynodes (optional: 'ps', 'enabled', 'all', 'qualify')\n"
                "  current      - Print info on current dynode winner to be paid the next block (calculated locally)\n"
                "  debug        - Print dynode status\n"
                "  genkey       - Generate new dynodeprivkey\n"
                "  outputs      - Print dynode compatible outputs\n"
                "  start        - Start local Hot dynode configured in dynamic.conf\n"
                "  start-alias  - Start single remote dynode by assigned alias configured in dynode.conf\n"
                "  start-<mode> - Start remote dynodes configured in dynode.conf (<mode>: 'all', 'missing', 'disabled')\n"
                "  status       - Print dynode status information\n"
                "  list         - Print list of all known dynodes (see dynodelist for more info)\n"
                "  list-conf    - Print dynode.conf in JSON format\n"
                "  winner       - Print info on next dynode winner to vote for\n"
                "  winners      - Print list of dynode winners\n"
                );

    if (strCommand == "list")
    {
        UniValue newParams(UniValue::VARR);
        // forward params but skip "list"
        for (unsigned int i = 1; i < params.size(); i++) {
            newParams.push_back(params[i]);
        }
        return dynodelist(newParams, fHelp);
    }

    if(strCommand == "connect")
    {
        if (params.size() < 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Dynode address required");

        std::string strAddress = params[1].get_str();

        CService addr = CService(strAddress);

        CNode *pnode = ConnectNode((CAddress)addr, NULL);
        if(!pnode)
            throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Couldn't connect to dynode %s", strAddress));

        return "successfully connected";
    }

    if (strCommand == "count")
    {
        if (params.size() > 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Too many parameters");

        if (params.size() == 1)
            return dnodeman.size();

        std::string strMode = params[1].get_str();

        if (strMode == "ps")
            return dnodeman.CountEnabled(MIN_PRIVATESEND_PEER_PROTO_VERSION);

        if (strMode == "enabled")
            return dnodeman.CountEnabled();

        LOCK(cs_main);
        int nCount;
        dnodeman.GetNextDynodeInQueueForPayment(chainActive.Height(), true, nCount);

        if (strMode == "qualify")
            return nCount;

        if (strMode == "all")
            return strprintf("Total: %d (PS Compatible: %d / Enabled: %d / Qualify: %d)",
                dnodeman.size(), dnodeman.CountEnabled(MIN_PRIVATESEND_PEER_PROTO_VERSION),
                dnodeman.CountEnabled(), nCount);
    }

    if (strCommand == "current" || strCommand == "winner")
    {
        int nCount;
        int nHeight;
        CBlockIndex* pindex;
        CDynode* winner = NULL;
        {
            LOCK(cs_main);
            nHeight = chainActive.Height() + (strCommand == "current" ? 1 : 10);
            pindex = chainActive.Tip();
        }
        dnodeman.UpdateLastPaid(pindex);
        winner = dnodeman.GetNextDynodeInQueueForPayment(nHeight, true, nCount);
        if(!winner) return "unknown";

        UniValue obj(UniValue::VOBJ);

        obj.push_back(Pair("height",        nHeight));
        obj.push_back(Pair("IP:port",       winner->addr.ToString()));
        obj.push_back(Pair("protocol",      (int64_t)winner->nProtocolVersion));
        obj.push_back(Pair("vin",           winner->vin.prevout.ToStringShort()));
        obj.push_back(Pair("payee",         CDynamicAddress(winner->pubKeyCollateralAddress.GetID()).ToString()));
        obj.push_back(Pair("lastseen",      (winner->lastPing == CDynodePing()) ? winner->sigTime :
                                                    winner->lastPing.sigTime));
        obj.push_back(Pair("activeseconds", (winner->lastPing == CDynodePing()) ? 0 :
                                                    (winner->lastPing.sigTime - winner->sigTime)));
        return obj;
    }

    if (strCommand == "debug")
    {
        if(activeDynode.nState != ACTIVE_DYNODE_INITIAL || !dynodeSync.IsBlockchainSynced())
            return activeDynode.GetStatus();

        CTxIn vin;
        CPubKey pubkey;
        CKey key;

        if(!pwalletMain || !pwalletMain->GetDynodeVinAndKeys(vin, pubkey, key))
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing dynode input, please look at the documentation for instructions on dynode creation");

        return activeDynode.GetStatus();
    }

    if (strCommand == "start")
    {
        if(!fDyNode)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "You must set dynode=1 in the configuration");

        {
            LOCK(pwalletMain->cs_wallet);
            EnsureWalletIsUnlocked();
        }

        if(activeDynode.nState != ACTIVE_DYNODE_STARTED){
            activeDynode.nState = ACTIVE_DYNODE_INITIAL; // TODO: consider better way
            activeDynode.ManageState();
        }

        return activeDynode.GetStatus();
    }

    if (strCommand == "start-alias")
    {
        if (params.size() < 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Please specify an alias");

        {
            LOCK(pwalletMain->cs_wallet);
            EnsureWalletIsUnlocked();
        }

        std::string strAlias = params[1].get_str();

        bool fFound = false;

        UniValue statusObj(UniValue::VOBJ);
        statusObj.push_back(Pair("alias", strAlias));

        BOOST_FOREACH(CDynodeConfig::CDynodeEntry dne, dynodeConfig.getEntries()) {
            if(dne.getAlias() == strAlias) {
                fFound = true;
                std::string strError;
                CDynodeBroadcast dnb;

                bool fResult = CDynodeBroadcast::Create(dne.getIp(), dne.getPrivKey(), dne.getTxHash(), dne.getOutputIndex(), strError, dnb);

                statusObj.push_back(Pair("result", fResult ? "successful" : "failed"));
                if(fResult) {
                    dnodeman.UpdateDynodeList(dnb);
                    dnb.Relay();
                } else {
                    statusObj.push_back(Pair("errorMessage", strError));
                }
                dnodeman.NotifyDynodeUpdates();
                break;
            }
        }

        if(!fFound) {
            statusObj.push_back(Pair("result", "failed"));
            statusObj.push_back(Pair("errorMessage", "Could not find alias in config. Verify with list-conf."));
        }

        return statusObj;

    }

    if (strCommand == "start-all" || strCommand == "start-missing" || strCommand == "start-disabled")
    {
        {
            LOCK(pwalletMain->cs_wallet);
            EnsureWalletIsUnlocked();
        }

        if((strCommand == "start-missing" || strCommand == "start-disabled") && !dynodeSync.IsDynodeListSynced()) {
            throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "You can't use this command until dynode list is synced");
        }

        int nSuccessful = 0;
        int nFailed = 0;

        UniValue resultsObj(UniValue::VOBJ);

        BOOST_FOREACH(CDynodeConfig::CDynodeEntry dne, dynodeConfig.getEntries()) {
            std::string strError;

            CTxIn vin = CTxIn(uint256S(dne.getTxHash()), uint32_t(atoi(dne.getOutputIndex().c_str())));
            CDynode *pdn = dnodeman.Find(vin);
            CDynodeBroadcast dnb;

            if(strCommand == "start-missing" && pdn) continue;
            if(strCommand == "start-disabled" && pdn && pdn->IsEnabled()) continue;

            bool fResult = CDynodeBroadcast::Create(dne.getIp(), dne.getPrivKey(), dne.getTxHash(), dne.getOutputIndex(), strError, dnb);

            UniValue statusObj(UniValue::VOBJ);
            statusObj.push_back(Pair("alias", dne.getAlias()));
            statusObj.push_back(Pair("result", fResult ? "successful" : "failed"));

            if (fResult) {
                nSuccessful++;
                dnodeman.UpdateDynodeList(dnb);
                dnb.Relay();
            } else {
                nFailed++;
                statusObj.push_back(Pair("errorMessage", strError));
            }

            resultsObj.push_back(Pair("status", statusObj));
        }
        dnodeman.NotifyDynodeUpdates();

        UniValue returnObj(UniValue::VOBJ);
        returnObj.push_back(Pair("overall", strprintf("Successfully started %d dynodes, failed to start %d, total %d", nSuccessful, nFailed, nSuccessful + nFailed)));
        returnObj.push_back(Pair("detail", resultsObj));

        return returnObj;
    }

    if (strCommand == "genkey")
    {
        CKey secret;
        secret.MakeNewKey(false);

        return CDynamicSecret(secret).ToString();
    }

    if (strCommand == "list-conf")
    {
        UniValue resultObj(UniValue::VOBJ);

        BOOST_FOREACH(CDynodeConfig::CDynodeEntry dne, dynodeConfig.getEntries()) {
            CTxIn vin = CTxIn(uint256S(dne.getTxHash()), uint32_t(atoi(dne.getOutputIndex().c_str())));
            CDynode *pdn = dnodeman.Find(vin);

            std::string strStatus = pdn ? pdn->GetStatus() : "MISSING";

            UniValue dnObj(UniValue::VOBJ);
            dnObj.push_back(Pair("alias", dne.getAlias()));
            dnObj.push_back(Pair("address", dne.getIp()));
            dnObj.push_back(Pair("privateKey", dne.getPrivKey()));
            dnObj.push_back(Pair("txHash", dne.getTxHash()));
            dnObj.push_back(Pair("outputIndex", dne.getOutputIndex()));
            dnObj.push_back(Pair("status", strStatus));
            resultObj.push_back(Pair("dynode", dnObj));
        }

        return resultObj;
    }

    if (strCommand == "outputs") {
        // Find possible candidates
        std::vector<COutput> vPossibleCoins;
        pwalletMain->AvailableCoins(vPossibleCoins, true, NULL, false, ONLY_1000);

        UniValue obj(UniValue::VOBJ);
        BOOST_FOREACH(COutput& out, vPossibleCoins) {
            obj.push_back(Pair(out.tx->GetHash().ToString(), strprintf("%d", out.i)));
        }

        return obj;

    }

    if (strCommand == "status")
    {
        if (!fDyNode)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "This is not a dynode");

        UniValue dnObj(UniValue::VOBJ);

        dnObj.push_back(Pair("vin", activeDynode.vin.ToString()));
        dnObj.push_back(Pair("service", activeDynode.service.ToString()));

        CDynode dn;
        if(dnodeman.Get(activeDynode.vin, dn)) {
            dnObj.push_back(Pair("payee", CDynamicAddress(dn.pubKeyCollateralAddress.GetID()).ToString()));
        }

        dnObj.push_back(Pair("status", activeDynode.GetStatus()));
        return dnObj;
    }

    if (strCommand == "winners")
    {
        int nHeight;
        {
            LOCK(cs_main);
            CBlockIndex* pindex = chainActive.Tip();
            if(!pindex) return NullUniValue;

            nHeight = pindex->nHeight;
        }

        int nLast = 10;
        std::string strFilter = "";

        if (params.size() >= 2) {
            nLast = atoi(params[1].get_str());
        }

        if (params.size() == 3) {
            strFilter = params[2].get_str();
        }

        if (params.size() > 3)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'dynode winners ( \"count\" \"filter\" )'");

        UniValue obj(UniValue::VOBJ);

        for(int i = nHeight - nLast; i < nHeight + 20; i++) {
            std::string strPayment = GetRequiredPaymentsString(i);
            if (strFilter !="" && strPayment.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strprintf("%d", i), strPayment));
        }

        return obj;
    }

    return NullUniValue;
}

UniValue dynodelist(const UniValue& params, bool fHelp)
{
    std::string strMode = "status";
    std::string strFilter = "";

    if (params.size() >= 1) strMode = params[0].get_str();
    if (params.size() == 2) strFilter = params[1].get_str();

    if (fHelp || (
                strMode != "activeseconds" && strMode != "addr" && strMode != "full" &&
                strMode != "lastseen" && strMode != "lastpaidtime" && strMode != "lastpaidblock" &&
                strMode != "protocol" && strMode != "payee" && strMode != "rank" && strMode != "status"))
    {
        throw std::runtime_error(
                "dynodelist ( \"mode\" \"filter\" )\n"
                "Get a list of dynodes in different modes\n"
                "\nArguments:\n"
                "1. \"mode\"      (string, optional/required to use filter, defaults = status) The mode to run list in\n"
                "2. \"filter\"    (string, optional) Filter results. Partial match by outpoint by default in all modes,\n"
                "                                    additional matches in some modes are also available\n"
                "\nAvailable modes:\n"
                "  activeseconds  - Print number of seconds dynode recognized by the network as enabled\n"
                "                   (since latest issued \"dynode start/start-many/start-alias\")\n"
                "  addr           - Print ip address associated with a dynode (can be additionally filtered, partial match)\n"
                "  full           - Print info in format 'status protocol payee lastseen activeseconds lastpaidtime lastpaidblock IP'\n"
                "                   (can be additionally filtered, partial match)\n"
                "  lastpaidblock  - Print the last block height a node was paid on the network\n"
                "  lastpaidtime   - Print the last time a node was paid on the network\n"
                "  lastseen       - Print timestamp of when a dynode was last seen on the network\n"
                "  payee          - Print Dynamic address associated with a dynode (can be additionally filtered,\n"
                "                   partial match)\n"
                "  protocol       - Print protocol of a dynode (can be additionally filtered, exact match))\n"
                "  rank           - Print rank of a dynode based on current block\n"
                "  status         - Print dynode status: PRE_ENABLED / ENABLED / EXPIRED / OUTPOINT_SPENT / REMOVE\n"
                "                   (can be additionally filtered, partial match)\n"
                );
    }

    if (strMode == "full" || strMode == "lastpaidtime" || strMode == "lastpaidblock") {
        CBlockIndex* pindex;
        {
            LOCK(cs_main);
            pindex = chainActive.Tip();
        }
        dnodeman.UpdateLastPaid(pindex);
    }

    UniValue obj(UniValue::VOBJ);
    if (strMode == "rank") {
        int nHeight;
        {
            LOCK(cs_main);
            nHeight = chainActive.Height();
        }
        std::vector<std::pair<int, CDynode> > vDynodeRanks = dnodeman.GetDynodeRanks(nHeight);
        BOOST_FOREACH(PAIRTYPE(int, CDynode)& s, vDynodeRanks) {
            std::string strOutpoint = s.second.vin.prevout.ToStringShort();
            if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
            obj.push_back(Pair(strOutpoint, s.first));
        }
    } else {
        std::vector<CDynode> vDynodes = dnodeman.GetFullDynodeVector();
        BOOST_FOREACH(CDynode& dn, vDynodes) {
            std::string strOutpoint = dn.vin.prevout.ToStringShort();
            if (strMode == "activeseconds") {
                if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, (int64_t)(dn.lastPing.sigTime - dn.sigTime)));
            } else if (strMode == "addr") {
                std::string strAddress = dn.addr.ToString();
                if (strFilter !="" && strAddress.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, strAddress));
            } else if (strMode == "full") {
                std::ostringstream streamFull;
                streamFull << std::setw(15) <<
                               dn.GetStatus() << " " <<
                               dn.nProtocolVersion << " " <<
                               CDynamicAddress(dn.pubKeyCollateralAddress.GetID()).ToString() << " " <<
                               (int64_t)dn.lastPing.sigTime << " " << std::setw(8) <<
                               (int64_t)(dn.lastPing.sigTime - dn.sigTime) << " " << std::setw(10) <<
                               dn.GetLastPaidTime() << " "  << std::setw(6) <<
                               dn.GetLastPaidBlock() << " " <<
                               dn.addr.ToString();
                std::string strFull = streamFull.str();
                if (strFilter !="" && strFull.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, strFull));
            } else if (strMode == "lastpaidblock") {
                if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, dn.GetLastPaidBlock()));
            } else if (strMode == "lastpaidtime") {
                if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, dn.GetLastPaidTime()));
            } else if (strMode == "lastseen") {
                if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, (int64_t)dn.lastPing.sigTime));
            } else if (strMode == "payee") {
                CDynamicAddress address(dn.pubKeyCollateralAddress.GetID());
                std::string strPayee = address.ToString();
                if (strFilter !="" && strPayee.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, strPayee));
            } else if (strMode == "protocol") {
                if (strFilter !="" && strFilter != strprintf("%d", dn.nProtocolVersion) &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, (int64_t)dn.nProtocolVersion));
            } else if (strMode == "status") {
                std::string strStatus = dn.GetStatus();
                if (strFilter !="" && strStatus.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos) continue;
                obj.push_back(Pair(strOutpoint, strStatus));
            }
        }
    }
    return obj;
}

bool DecodeHexVecDnb(std::vector<CDynodeBroadcast>& vecDnb, std::string strHexDnb) {

    if (!IsHex(strHexDnb))
        return false;

    std::vector<unsigned char> dnbData(ParseHex(strHexDnb));
    CDataStream ssData(dnbData, SER_NETWORK, PROTOCOL_VERSION);
    try {
        ssData >> vecDnb;
    }
    catch (const std::exception&) {
        return false;
    }

    return true;
}

UniValue dynodebroadcast(const UniValue& params, bool fHelp)
{
    std::string strCommand;
    if (params.size() >= 1)
        strCommand = params[0].get_str();

    if (fHelp  ||
        (strCommand != "create-alias" && strCommand != "create-all" && strCommand != "decode" && strCommand != "relay"))
        throw std::runtime_error(
                "dynodebroadcast \"command\"... ( \"passphrase\" )\n"
                "Set of commands to create and relay dynode broadcast messages\n"
                "\nArguments:\n"
                "1. \"command\"        (string or set of strings, required) The command to execute\n"
                "2. \"passphrase\"     (string, optional) The wallet passphrase\n"
                "\nAvailable commands:\n"
                "  create-alias  - Create single remote dynode broadcast message by assigned alias configured in dynode.conf\n"
                "  create-all    - Create remote dynode broadcast messages for all dynodes configured in dynode.conf\n"
                "  decode        - Decode dynode broadcast message\n"
                "  relay         - Relay dynode broadcast message to the network\n"
                + HelpRequiringPassphrase());

    if (strCommand == "create-alias")
    {
        // wait for reindex and/or import to finish
        if (fImporting || fReindex)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Wait for reindex and/or import to finish");

        if (params.size() < 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Please specify an alias");

        {
            LOCK(pwalletMain->cs_wallet);
            EnsureWalletIsUnlocked();
        }

        bool fFound = false;
        std::string strAlias = params[1].get_str();

        UniValue statusObj(UniValue::VOBJ);
        std::vector<CDynodeBroadcast> vecDnb;

        statusObj.push_back(Pair("alias", strAlias));

        BOOST_FOREACH(CDynodeConfig::CDynodeEntry dne, dynodeConfig.getEntries()) {
            if(dne.getAlias() == strAlias) {
                fFound = true;
                std::string strError;
                CDynodeBroadcast dnb;

                bool fResult = CDynodeBroadcast::Create(dne.getIp(), dne.getPrivKey(), dne.getTxHash(), dne.getOutputIndex(), strError, dnb, true);

                statusObj.push_back(Pair("result", fResult ? "successful" : "failed"));
                if(fResult) {
                    vecDnb.push_back(dnb);
                    CDataStream ssVecDnb(SER_NETWORK, PROTOCOL_VERSION);
                    ssVecDnb << vecDnb;
                    statusObj.push_back(Pair("hex", HexStr(ssVecDnb.begin(), ssVecDnb.end())));
                } else {
                    statusObj.push_back(Pair("errorMessage", strError));
                }
                break;
            }
        }

        if(!fFound) {
            statusObj.push_back(Pair("result", "not found"));
            statusObj.push_back(Pair("errorMessage", "Could not find alias in config. Verify with list-conf."));
        }

        return statusObj;

    }

    if (strCommand == "create-all")
    {
        // wait for reindex and/or import to finish
        if (fImporting || fReindex)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Wait for reindex and/or import to finish");

        {
            LOCK(pwalletMain->cs_wallet);
            EnsureWalletIsUnlocked();
        }

        std::vector<CDynodeConfig::CDynodeEntry> dnEntries;
        dnEntries = dynodeConfig.getEntries();

        int nSuccessful = 0;
        int nFailed = 0;

        UniValue resultsObj(UniValue::VOBJ);
        std::vector<CDynodeBroadcast> vecDnb;

        BOOST_FOREACH(CDynodeConfig::CDynodeEntry dne, dynodeConfig.getEntries()) {
            std::string strError;
            CDynodeBroadcast dnb;

            bool fResult = CDynodeBroadcast::Create(dne.getIp(), dne.getPrivKey(), dne.getTxHash(), dne.getOutputIndex(), strError, dnb, true);

            UniValue statusObj(UniValue::VOBJ);
            statusObj.push_back(Pair("alias", dne.getAlias()));
            statusObj.push_back(Pair("result", fResult ? "successful" : "failed"));

            if(fResult) {
                nSuccessful++;
                vecDnb.push_back(dnb);
            } else {
                nFailed++;
                statusObj.push_back(Pair("errorMessage", strError));
            }

            resultsObj.push_back(Pair("status", statusObj));
        }

        CDataStream ssVecDnb(SER_NETWORK, PROTOCOL_VERSION);
        ssVecDnb << vecDnb;
        UniValue returnObj(UniValue::VOBJ);
        returnObj.push_back(Pair("overall", strprintf("Successfully created broadcast messages for %d dynodes, failed to create %d, total %d", nSuccessful, nFailed, nSuccessful + nFailed)));
        returnObj.push_back(Pair("detail", resultsObj));
        returnObj.push_back(Pair("hex", HexStr(ssVecDnb.begin(), ssVecDnb.end())));

        return returnObj;
    }

    if (strCommand == "decode")
    {
        if (params.size() != 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'dynodebroadcast decode \"hexstring\"'");

        std::vector<CDynodeBroadcast> vecDnb;

        if (!DecodeHexVecDnb(vecDnb, params[1].get_str()))
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Dynode broadcast message decode failed");

        int nSuccessful = 0;
        int nFailed = 0;
        int nDos = 0;
        UniValue returnObj(UniValue::VOBJ);

        BOOST_FOREACH(CDynodeBroadcast& dnb, vecDnb) {
            UniValue resultObj(UniValue::VOBJ);

            if(dnb.CheckSignature(nDos)) {
                nSuccessful++;
                resultObj.push_back(Pair("vin", dnb.vin.ToString()));
                resultObj.push_back(Pair("addr", dnb.addr.ToString()));
                resultObj.push_back(Pair("pubKeyCollateralAddress", CDynamicAddress(dnb.pubKeyCollateralAddress.GetID()).ToString()));
                resultObj.push_back(Pair("pubKeyDynode", CDynamicAddress(dnb.pubKeyDynode.GetID()).ToString()));
                resultObj.push_back(Pair("vchSig", EncodeBase64(&dnb.vchSig[0], dnb.vchSig.size())));
                resultObj.push_back(Pair("sigTime", dnb.sigTime));
                resultObj.push_back(Pair("protocolVersion", dnb.nProtocolVersion));
                resultObj.push_back(Pair("nLastPsq", dnb.nLastPsq));

                UniValue lastPingObj(UniValue::VOBJ);
                lastPingObj.push_back(Pair("vin", dnb.lastPing.vin.ToString()));
                lastPingObj.push_back(Pair("blockHash", dnb.lastPing.blockHash.ToString()));
                lastPingObj.push_back(Pair("sigTime", dnb.lastPing.sigTime));
                lastPingObj.push_back(Pair("vchSig", EncodeBase64(&dnb.lastPing.vchSig[0], dnb.lastPing.vchSig.size())));

                resultObj.push_back(Pair("lastPing", lastPingObj));
            } else {
                nFailed++;
                resultObj.push_back(Pair("errorMessage", "Dynode broadcast signature verification failed"));
            }

            returnObj.push_back(Pair(dnb.GetHash().ToString(), resultObj));
        }

        returnObj.push_back(Pair("overall", strprintf("Successfully decoded broadcast messages for %d dynodes, failed to decode %d, total %d", nSuccessful, nFailed, nSuccessful + nFailed)));

        return returnObj;
    }

    if (strCommand == "relay")
    {
        if (params.size() < 2 || params.size() > 3)
            throw JSONRPCError(RPC_INVALID_PARAMETER,   "dynodebroadcast relay \"hexstring\" ( fast )\n"
                                                        "\nArguments:\n"
                                                        "1. \"hex\"      (string, required) Broadcast messages hex string\n"
                                                        "2. fast       (string, optional) If none, using safe method\n");

        std::vector<CDynodeBroadcast> vecDnb;

        if (!DecodeHexVecDnb(vecDnb, params[1].get_str()))
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Dynode broadcast message decode failed");

        int nSuccessful = 0;
        int nFailed = 0;
        bool fSafe = params.size() == 2;
        UniValue returnObj(UniValue::VOBJ);

        // verify all signatures first, bailout if any of them broken
        BOOST_FOREACH(CDynodeBroadcast& dnb, vecDnb) {
            UniValue resultObj(UniValue::VOBJ);

            resultObj.push_back(Pair("vin", dnb.vin.ToString()));
            resultObj.push_back(Pair("addr", dnb.addr.ToString()));

            int nDos = 0;
            bool fResult;
            if (dnb.CheckSignature(nDos)) {
                if (fSafe) {
                    fResult = dnodeman.CheckDnbAndUpdateDynodeList(dnb, nDos);
                } else {
                    dnodeman.UpdateDynodeList(dnb);
                    dnb.Relay();
                    fResult = true;
                }
                dnodeman.NotifyDynodeUpdates();
            } else fResult = false;

            if(fResult) {
                nSuccessful++;
                resultObj.push_back(Pair(dnb.GetHash().ToString(), "successful"));
            } else {
                nFailed++;
                resultObj.push_back(Pair("errorMessage", "Dynode broadcast signature verification failed"));
            }

            returnObj.push_back(Pair(dnb.GetHash().ToString(), resultObj));
        }

        returnObj.push_back(Pair("overall", strprintf("Successfully relayed broadcast messages for %d dynodes, failed to relay %d, total %d", nSuccessful, nFailed, nSuccessful + nFailed)));

        return returnObj;
    }

    return NullUniValue;
}

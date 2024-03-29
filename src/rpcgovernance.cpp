// Copyright (c) 2009-2017 Satoshi Nakamoto
// Copyright (c) 2009-2017 The Bitcoin Developers
// Copyright (c) 2014-2017 The Dash Core Developers
// Copyright (c) 2016-2017 Duality Blockchain Solutions Ltd
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//#define ENABLE_DYNAMIC_DEBUG

#include "util.h"
#include "main.h"
#include "db.h"
#include "init.h"
#include "activedynode.h"
#include "privatesend.h"
#include "governance.h"
#include "dynode.h"
#include "dynode-payments.h"
#include "dynode-sync.h"
#include "dynodeconfig.h"
#include "dynodeman.h"
#include "rpcserver.h"
#include "utilmoneystr.h"
#include "governance-vote.h"
#include "governance-classes.h"
#include <boost/lexical_cast.hpp>

#include <fstream>
#include <iostream>
#include <sstream>

using namespace std;

UniValue gobject(const UniValue& params, bool fHelp)
{
    std::string strCommand;
    if (params.size() >= 1)
        strCommand = params[0].get_str();

    if (fHelp  ||
        (strCommand != "vote-many" && strCommand != "vote-conf" && strCommand != "vote-alias" && strCommand != "prepare" && strCommand != "submit" &&
         strCommand != "vote" && strCommand != "get" && strCommand != "getvotes" && strCommand != "getcurrentvotes" && strCommand != "list" && strCommand != "diff" && strCommand != "deserialize"))
        throw std::runtime_error(
                "gobject \"command\"...\n"
                "Manage governance objects\n"
                "\nAvailable commands:\n"
                "  prepare            - Prepare governance object by signing and creating tx\n"
                "  submit             - Submit governance object to network\n"
                "  get                - Get governance object by hash\n"
                "  getvotes           - Get all votes for a governance object hash (including old votes)\n"
                "  getcurrentvotes    - Get only current (tallying) votes for a governance object hash (does not include old votes)\n"
                "  list               - List all governance objects\n"
                "  diff               - List differences since last diff\n"
                "  vote-alias         - Vote on a governance object by dynode alias (using dynode.conf setup)\n"
                "  vote-conf          - Vote on a governance object by dynode configured in dynamic.conf\n"
                "  vote-many          - Vote on a governance object by all dynodes (using dynode.conf setup)\n"
                );


    /*
        ------ Example Governance Item ------

        gobject submit 6e622bb41bad1fb18e7f23ae96770aeb33129e18bd9efe790522488e580a0a03 0 1 1464292854 "beer-reimbursement" 5b5b22636f6e7472616374222c207b2270726f6a6563745f6e616d65223a20225c22626565722d7265696d62757273656d656e745c22222c20227061796d656e745f61646472657373223a20225c225879324c4b4a4a64655178657948726e34744744514238626a6876464564615576375c22222c2022656e645f64617465223a202231343936333030343030222c20226465736372697074696f6e5f75726c223a20225c227777772e646173687768616c652e6f72672f702f626565722d7265696d62757273656d656e745c22222c2022636f6e74726163745f75726c223a20225c22626565722d7265696d62757273656d656e742e636f6d2f3030312e7064665c22222c20227061796d656e745f616d6f756e74223a20223233342e323334323232222c2022676f7665726e616e63655f6f626a6563745f6964223a2037342c202273746172745f64617465223a202231343833323534303030227d5d5d1
    */

    // DEBUG : TEST DESERIALIZATION OF GOVERNANCE META DATA
    if(strCommand == "deserialize")
    {
        std::string strHex = params[1].get_str();

        std::vector<unsigned char> v = ParseHex(strHex);
        std::string s(v.begin(), v.end());

        UniValue u(UniValue::VOBJ);
        u.read(s);

        return u.write().c_str();
    }

    // PREPARE THE GOVERNANCE OBJECT BY CREATING A COLLATERAL TRANSACTION
    if(strCommand == "prepare")
    {
        if (params.size() != 5) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'gobject prepare <parent-hash> <revision> <time> <data-hex>'");
        }

        // ASSEMBLE NEW GOVERNANCE OBJECT FROM USER PARAMETERS

        CBlockIndex* pindex = NULL;
        {
            LOCK(cs_main);
            pindex = chainActive.Tip();
        }

        std::vector<CDynodeConfig::CDynodeEntry> dnEntries;
        dnEntries = dynodeConfig.getEntries();

        uint256 hashParent;

        // -- attach to root node (root node doesn't really exist, but has a hash of zero)
        if(params[1].get_str() == "0") {
            hashParent = uint256();
        } else {
            hashParent = ParseHashV(params[1], "fee-txid, parameter 1");
        }

        std::string strRevision = params[2].get_str();
        std::string strTime = params[3].get_str();
        int nRevision = boost::lexical_cast<int>(strRevision);
        int nTime = boost::lexical_cast<int>(strTime);
        std::string strData = params[4].get_str();

        // CREATE A NEW COLLATERAL TRANSACTION FOR THIS SPECIFIC OBJECT

        CGovernanceObject govobj(hashParent, nRevision, nTime, uint256(), strData);

        if((govobj.GetObjectType() == GOVERNANCE_OBJECT_TRIGGER) ||
           (govobj.GetObjectType() == GOVERNANCE_OBJECT_WATCHDOG)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Trigger and watchdog objects need not be prepared (however only dynodes can create them)");
        }

        std::string strError = "";
        if(!govobj.IsValidLocally(pindex, strError, false))
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Governance object is not valid - " + govobj.GetHash().ToString() + " - " + strError);

        CWalletTx wtx;
        if(!pwalletMain->GetBudgetSystemCollateralTX(wtx, govobj.GetHash(), govobj.GetMinCollateralFee(), false)) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Error making collateral transaction for governance object. Please check your wallet balance and make sure your wallet is unlocked.");
        }

        // -- make our change address
        CReserveKey reservekey(pwalletMain);
        // -- send the tx to the network
        pwalletMain->CommitTransaction(wtx, reservekey, NetMsgType::TX);

        DBG( cout << "gobject: prepare "
             << " strData = " << govobj.GetDataAsString()
             << ", hash = " << govobj.GetHash().GetHex()
             << ", txidFee = " << wtx.GetHash().GetHex()
             << endl; );

        return wtx.GetHash().ToString();
    }

    // AFTER COLLATERAL TRANSACTION HAS MATURED USER CAN SUBMIT GOVERNANCE OBJECT TO PROPAGATE NETWORK
    if(strCommand == "submit")
    {
        if ((params.size() < 5) || (params.size() > 6))  {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'gobject submit <parent-hash> <revision> <time> <data-hex> <fee-txid>'");
        }

        if(!dynodeSync.IsBlockchainSynced()) {
            throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Must wait for client to sync with dynode network. Try again in a minute or so.");
        }

        CDynode dn;
        bool dnFound = dnodeman.Get(activeDynode.vin, dn);

        DBG( cout << "gobject: submit activeDynode.pubKeyDynode = " << activeDynode.pubKeyDynode.GetHash().ToString()
             << ", vin = " << activeDynode.vin.prevout.ToStringShort()
             << ", params.size() = " << params.size()
             << ", dnFound = " << dnFound << endl; );

        // ASSEMBLE NEW GOVERNANCE OBJECT FROM USER PARAMETERS

        CBlockIndex* pindex = NULL;
        {
            LOCK(cs_main);
            pindex = chainActive.Tip();
        }

        uint256 txidFee;

        if(params.size() == 6) {
            txidFee = ParseHashV(params[5], "fee-txid, parameter 6");
        }
        uint256 hashParent;
        if(params[1].get_str() == "0") { // attach to root node (root node doesn't really exist, but has a hash of zero)
            hashParent = uint256();
        } else {
            hashParent = ParseHashV(params[1], "parent object hash, parameter 2");
        }

        // GET THE PARAMETERS FROM USER

        std::string strRevision = params[2].get_str();
        std::string strTime = params[3].get_str();
        int nRevision = boost::lexical_cast<int>(strRevision);
        int nTime = boost::lexical_cast<int>(strTime);
        std::string strData = params[4].get_str();

        CGovernanceObject govobj(hashParent, nRevision, nTime, txidFee, strData);

        DBG( cout << "gobject: submit "
             << " strData = " << govobj.GetDataAsString()
             << ", hash = " << govobj.GetHash().GetHex()
             << ", txidFee = " << txidFee.GetHex()
             << endl; );

        // Attempt to sign triggers if we are a DN
        if((govobj.GetObjectType() == GOVERNANCE_OBJECT_TRIGGER) ||
           (govobj.GetObjectType() == GOVERNANCE_OBJECT_WATCHDOG)) {
            if(dnFound) {
                govobj.SetDynodeInfo(dn.vin);
                govobj.Sign(activeDynode.keyDynode, activeDynode.pubKeyDynode);
            }
            else {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Only valid dynodes can submit this type of object");
            }
        }
        else {
            if(params.size() != 6) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "The fee-txid parameter must be included to submit this type of object");
            }
        }

        std::string strError = "";
        if(!govobj.IsValidLocally(pindex, strError, true)) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Governance object is not valid - " + govobj.GetHash().ToString() + " - " + strError);
        }

        // RELAY THIS OBJECT
        governance.AddSeenGovernanceObject(govobj.GetHash(), SEEN_OBJECT_IS_VALID);
        govobj.Relay();
        governance.AddGovernanceObject(govobj);

        return govobj.GetHash().ToString();
    }

    if(strCommand == "vote-conf")
    {
        if(params.size() != 4)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'gobject vote-conf <governance-hash> [funding|valid|delete] [yes|no|abstain]'");

        uint256 hash;
        std::string strVote;

        hash = ParseHashV(params[1], "Object hash");
        std::string strVoteSignal = params[2].get_str();
        std::string strVoteOutcome = params[3].get_str();

        vote_signal_enum_t eVoteSignal = CGovernanceVoting::ConvertVoteSignal(strVoteSignal);
        if(eVoteSignal == VOTE_SIGNAL_NONE) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "Invalid vote signal. Please using one of the following: "
                               "(funding|valid|delete|endorsed) OR `custom sentinel code` ");
        }

        vote_outcome_enum_t eVoteOutcome = CGovernanceVoting::ConvertVoteOutcome(strVoteOutcome);
        if(eVoteOutcome == VOTE_OUTCOME_NONE) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid vote outcome. Please use one of the following: 'yes', 'no' or 'abstain'");
        }

        int success = 0;
        int failed = 0;

        UniValue resultsObj(UniValue::VOBJ);

        std::vector<unsigned char> vchDyNodeSignature;
        std::string strDyNodeSignMessage;

        UniValue statusObj(UniValue::VOBJ);
        UniValue returnObj(UniValue::VOBJ);

        CDynode dn;
        bool dnFound = dnodeman.Get(activeDynode.vin, dn);

        if(!dnFound) {
            failed++;
            statusObj.push_back(Pair("result", "failed"));
            statusObj.push_back(Pair("errorMessage", "Can't find dynode by collateral output"));
            resultsObj.push_back(Pair("dynamic.conf", statusObj));
            returnObj.push_back(Pair("overall", strprintf("Voted successfully %d time(s) and failed %d time(s).", success, failed)));
            returnObj.push_back(Pair("detail", resultsObj));
            return returnObj;
        }

        CGovernanceVote vote(dn.vin, hash, eVoteSignal, eVoteOutcome);
        if(!vote.Sign(activeDynode.keyDynode, activeDynode.pubKeyDynode)) {
            failed++;
            statusObj.push_back(Pair("result", "failed"));
            statusObj.push_back(Pair("errorMessage", "Failure to sign."));
            resultsObj.push_back(Pair("dynamic.conf", statusObj));
            returnObj.push_back(Pair("overall", strprintf("Voted successfully %d time(s) and failed %d time(s).", success, failed)));
            returnObj.push_back(Pair("detail", resultsObj));
            return returnObj;
        }

        CGovernanceException exception;
        if(governance.ProcessVote(vote, exception)) {
            success++;
            statusObj.push_back(Pair("result", "success"));
        }
        else {
            failed++;
            statusObj.push_back(Pair("result", "failed"));
            statusObj.push_back(Pair("errorMessage", exception.GetMessage()));
        }

        resultsObj.push_back(Pair("dynamic.conf", statusObj));

        returnObj.push_back(Pair("overall", strprintf("Voted successfully %d time(s) and failed %d time(s).", success, failed)));
        returnObj.push_back(Pair("detail", resultsObj));

        return returnObj;
    }

    if(strCommand == "vote-many")
    {
        if(params.size() != 4)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'gobject vote-many <governance-hash> [funding|valid|delete] [yes|no|abstain]'");

        uint256 hash;
        std::string strVote;

        hash = ParseHashV(params[1], "Object hash");
        std::string strVoteSignal = params[2].get_str();
        std::string strVoteOutcome = params[3].get_str();


        vote_signal_enum_t eVoteSignal = CGovernanceVoting::ConvertVoteSignal(strVoteSignal);
        if(eVoteSignal == VOTE_SIGNAL_NONE) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "Invalid vote signal. Please using one of the following: "
                               "(funding|valid|delete|endorsed) OR `custom sentinel code` ");
        }

        vote_outcome_enum_t eVoteOutcome = CGovernanceVoting::ConvertVoteOutcome(strVoteOutcome);
        if(eVoteOutcome == VOTE_OUTCOME_NONE) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid vote outcome. Please use one of the following: 'yes', 'no' or 'abstain'");
        }

        int success = 0;
        int failed = 0;

        std::vector<CDynodeConfig::CDynodeEntry> dnEntries;
        dnEntries = dynodeConfig.getEntries();

        UniValue resultsObj(UniValue::VOBJ);

        BOOST_FOREACH(CDynodeConfig::CDynodeEntry dne, dynodeConfig.getEntries()) {
            std::string strError;
            std::vector<unsigned char> vchDyNodeSignature;
            std::string strDyNodeSignMessage;

            CPubKey pubKeyCollateralAddress;
            CKey keyCollateralAddress;
            CPubKey pubKeyDynode;
            CKey keyDynode;

            UniValue statusObj(UniValue::VOBJ);

            if(!privateSendSigner.GetKeysFromSecret(dne.getPrivKey(), keyDynode, pubKeyDynode)){
                failed++;
                statusObj.push_back(Pair("result", "failed"));
                statusObj.push_back(Pair("errorMessage", "Dynode signing error, could not set key correctly"));
                resultsObj.push_back(Pair(dne.getAlias(), statusObj));
                continue;
            }

            uint256 nTxHash;
            nTxHash.SetHex(dne.getTxHash());

            int nOutputIndex = 0;
            if(!ParseInt32(dne.getOutputIndex(), &nOutputIndex)) {
                continue;
            }

            CTxIn vin(COutPoint(nTxHash, nOutputIndex));

            CDynode dn;
            bool dnFound = dnodeman.Get(vin, dn);

            if(!dnFound) {
                failed++;
                statusObj.push_back(Pair("result", "failed"));
                statusObj.push_back(Pair("errorMessage", "Can't find dynode by collateral output"));
                resultsObj.push_back(Pair(dne.getAlias(), statusObj));
                continue;
            }

            CGovernanceVote vote(dn.vin, hash, eVoteSignal, eVoteOutcome);
            if(!vote.Sign(keyDynode, pubKeyDynode)){
                failed++;
                statusObj.push_back(Pair("result", "failed"));
                statusObj.push_back(Pair("errorMessage", "Failure to sign."));
                resultsObj.push_back(Pair(dne.getAlias(), statusObj));
                continue;
            }

            CGovernanceException exception;
            if(governance.ProcessVote(vote, exception)) {
                success++;
                statusObj.push_back(Pair("result", "success"));
            }
            else {
                failed++;
                statusObj.push_back(Pair("result", "failed"));
                statusObj.push_back(Pair("errorMessage", exception.GetMessage()));
            }

            resultsObj.push_back(Pair(dne.getAlias(), statusObj));
        }

        UniValue returnObj(UniValue::VOBJ);
        returnObj.push_back(Pair("overall", strprintf("Voted successfully %d time(s) and failed %d time(s).", success, failed)));
        returnObj.push_back(Pair("detail", resultsObj));

        return returnObj;
    }


    // DYNODES CAN VOTE ON GOVERNANCE OBJECTS ON THE NETWORK FOR VARIOUS SIGNALS AND OUTCOMES
    if(strCommand == "vote-alias")
    {
        if(params.size() != 5)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'gobject vote-alias <governance-hash> [funding|valid|delete] [yes|no|abstain] <alias-name>'");

        uint256 hash;
        std::string strVote;

        // COLLECT NEEDED PARAMETRS FROM USER

        hash = ParseHashV(params[1], "Object hash");
        std::string strVoteSignal = params[2].get_str();
        std::string strVoteOutcome = params[3].get_str();
        std::string strAlias = params[4].get_str();

        // CONVERT NAMED SIGNAL/ACTION AND CONVERT

        vote_signal_enum_t eVoteSignal = CGovernanceVoting::ConvertVoteSignal(strVoteSignal);
        if(eVoteSignal == VOTE_SIGNAL_NONE) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "Invalid vote signal. Please using one of the following: "
                               "(funding|valid|delete|endorsed) OR `custom sentinel code` ");
        }

        vote_outcome_enum_t eVoteOutcome = CGovernanceVoting::ConvertVoteOutcome(strVoteOutcome);
        if(eVoteOutcome == VOTE_OUTCOME_NONE) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid vote outcome. Please use one of the following: 'yes', 'no' or 'abstain'");
        }

        // EXECUTE VOTE FOR EACH DYNODE, COUNT SUCCESSES VS FAILURES

        int success = 0;
        int failed = 0;

        std::vector<CDynodeConfig::CDynodeEntry> dnEntries;
        dnEntries = dynodeConfig.getEntries();

        UniValue resultsObj(UniValue::VOBJ);

        BOOST_FOREACH(CDynodeConfig::CDynodeEntry dne, dynodeConfig.getEntries())
        {
            // IF WE HAVE A SPECIFIC NODE REQUESTED TO VOTE, DO THAT
            if(strAlias != dne.getAlias()) continue;

            // INIT OUR NEEDED VARIABLES TO EXECUTE THE VOTE
            std::string strError;
            std::vector<unsigned char> vchDyNodeSignature;
            std::string strDyNodeSignMessage;

            CPubKey pubKeyCollateralAddress;
            CKey keyCollateralAddress;
            CPubKey pubKeyDynode;
            CKey keyDynode;

            // SETUP THE SIGNING KEY FROM DYNODE.CONF ENTRY

            UniValue statusObj(UniValue::VOBJ);

            if(!privateSendSigner.GetKeysFromSecret(dne.getPrivKey(), keyDynode, pubKeyDynode)) {
                failed++;
                statusObj.push_back(Pair("result", "failed"));
                statusObj.push_back(Pair("errorMessage", strprintf("Invalid dynode key %s.", dne.getPrivKey())));
                resultsObj.push_back(Pair(dne.getAlias(), statusObj));
                continue;
            }

            // SEARCH FOR THIS DYNODE ON THE NETWORK, THE NODE MUST BE ACTIVE TO VOTE

            uint256 nTxHash;
            nTxHash.SetHex(dne.getTxHash());

            int nOutputIndex = 0;
            if(!ParseInt32(dne.getOutputIndex(), &nOutputIndex)) {
                continue;
            }

            CTxIn vin(COutPoint(nTxHash, nOutputIndex));

            CDynode dn;
            bool dnFound = dnodeman.Get(vin, dn);

            if(!dnFound) {
                failed++;
                statusObj.push_back(Pair("result", "failed"));
                statusObj.push_back(Pair("errorMessage", "Dynode must be publically available on network to vote. Dynode not found."));
                resultsObj.push_back(Pair(dne.getAlias(), statusObj));
                continue;
            }

            // CREATE NEW GOVERNANCE OBJECT VOTE WITH OUTCOME/SIGNAL

            CGovernanceVote vote(vin, hash, eVoteSignal, eVoteOutcome);
            if(!vote.Sign(keyDynode, pubKeyDynode)) {
                failed++;
                statusObj.push_back(Pair("result", "failed"));
                statusObj.push_back(Pair("errorMessage", "Failure to sign."));
                resultsObj.push_back(Pair(dne.getAlias(), statusObj));
                continue;
            }

            // UPDATE LOCAL DATABASE WITH NEW OBJECT SETTINGS

            CGovernanceException exception;
            if(governance.ProcessVote(vote, exception)) {
                success++;
                statusObj.push_back(Pair("result", "success"));
            }
            else {
                failed++;
                statusObj.push_back(Pair("result", "failed"));
                statusObj.push_back(Pair("errorMessage", exception.GetMessage()));
            }

            resultsObj.push_back(Pair(dne.getAlias(), statusObj));
        }

        // REPORT STATS TO THE USER

        UniValue returnObj(UniValue::VOBJ);
        returnObj.push_back(Pair("overall", strprintf("Voted successfully %d time(s) and failed %d time(s).", success, failed)));
        returnObj.push_back(Pair("detail", resultsObj));

        return returnObj;
    }

    // USERS CAN QUERY THE SYSTEM FOR A LIST OF VARIOUS GOVERNANCE ITEMS
    if(strCommand == "list" || strCommand == "diff")
    {
        if (params.size() > 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'gobject [list|diff] [valid]'");

        // GET MAIN PARAMETER FOR THIS MODE, VALID OR ALL?

        std::string strShow = "valid";
        if (params.size() == 2) strShow = params[1].get_str();
        if (strShow != "valid" && strShow != "all") return "Invalid mode, should be valid or all";

        // GET STARTING TIME TO QUERY SYSTEM WITH

        int nStartTime = 0; //list
        if(strCommand == "diff") nStartTime = governance.GetLastDiffTime();

        // SETUP BLOCK INDEX VARIABLE / RESULTS VARIABLE

        CBlockIndex* pindex = NULL;
        {
            LOCK(cs_main);
            pindex = chainActive.Tip();
        }

        UniValue objResult(UniValue::VOBJ);

        // GET MATCHING GOVERNANCE OBJECTS

        LOCK(governance.cs);

        std::vector<CGovernanceObject*> objs = governance.GetAllNewerThan(nStartTime);
        governance.UpdateLastDiffTime(GetTime());

        // CREATE RESULTS FOR USER

        BOOST_FOREACH(CGovernanceObject* pGovObj, objs)
        {
            // IF WE HAVE A SPECIFIC NODE REQUESTED TO VOTE, DO THAT
            if(strShow == "valid" && !pGovObj->IsSetCachedValid()) continue;

            UniValue bObj(UniValue::VOBJ);
            bObj.push_back(Pair("DataHex",  pGovObj->GetDataAsHex()));
            bObj.push_back(Pair("DataString",  pGovObj->GetDataAsString()));
            bObj.push_back(Pair("Hash",  pGovObj->GetHash().ToString()));
            bObj.push_back(Pair("CollateralHash",  pGovObj->GetCollateralHash().ToString()));

            // REPORT STATUS FOR FUNDING VOTES SPECIFICALLY
            bObj.push_back(Pair("AbsoluteYesCount",  pGovObj->GetAbsoluteYesCount(VOTE_SIGNAL_FUNDING)));
            bObj.push_back(Pair("YesCount",  pGovObj->GetYesCount(VOTE_SIGNAL_FUNDING)));
            bObj.push_back(Pair("NoCount",  pGovObj->GetNoCount(VOTE_SIGNAL_FUNDING)));
            bObj.push_back(Pair("AbstainCount",  pGovObj->GetAbstainCount(VOTE_SIGNAL_FUNDING)));

            // REPORT VALIDITY AND CACHING FLAGS FOR VARIOUS SETTINGS
            std::string strError = "";
            bObj.push_back(Pair("fBlockchainValidity",  pGovObj->IsValidLocally(pindex , strError, false)));
            bObj.push_back(Pair("IsValidReason",  strError.c_str()));
            bObj.push_back(Pair("fCachedValid",  pGovObj->IsSetCachedValid()));
            bObj.push_back(Pair("fCachedFunding",  pGovObj->IsSetCachedFunding()));
            bObj.push_back(Pair("fCachedDelete",  pGovObj->IsSetCachedDelete()));
            bObj.push_back(Pair("fCachedEndorsed",  pGovObj->IsSetCachedEndorsed()));

            objResult.push_back(Pair(pGovObj->GetHash().ToString(), bObj));
        }

        return objResult;
    }

    // GET SPECIFIC GOVERNANCE ENTRY
    if(strCommand == "get")
    {
        if (params.size() != 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'gobject get <governance-hash>'");

        // COLLECT VARIABLES FROM OUR USER
        uint256 hash = ParseHashV(params[1], "GovObj hash");

        LOCK2(cs_main, governance.cs);

        // FIND THE GOVERNANCE OBJECT THE USER IS LOOKING FOR
        CGovernanceObject* pGovObj = governance.FindGovernanceObject(hash);

        if(pGovObj == NULL)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown governance object");

        // REPORT BASIC OBJECT STATS

        UniValue objResult(UniValue::VOBJ);
        objResult.push_back(Pair("DataHex",  pGovObj->GetDataAsHex()));
        objResult.push_back(Pair("DataString",  pGovObj->GetDataAsString()));
        objResult.push_back(Pair("Hash",  pGovObj->GetHash().ToString()));
        objResult.push_back(Pair("CollateralHash",  pGovObj->GetCollateralHash().ToString()));

        // SHOW (MUCH MORE) INFORMATION ABOUT VOTES FOR GOVERNANCE OBJECT (THAN LIST/DIFF ABOVE)
        // -- FUNDING VOTING RESULTS

        UniValue objFundingResult(UniValue::VOBJ);
        objFundingResult.push_back(Pair("AbsoluteYesCount",  pGovObj->GetAbsoluteYesCount(VOTE_SIGNAL_FUNDING)));
        objFundingResult.push_back(Pair("YesCount",  pGovObj->GetYesCount(VOTE_SIGNAL_FUNDING)));
        objFundingResult.push_back(Pair("NoCount",  pGovObj->GetNoCount(VOTE_SIGNAL_FUNDING)));
        objFundingResult.push_back(Pair("AbstainCount",  pGovObj->GetAbstainCount(VOTE_SIGNAL_FUNDING)));
        objResult.push_back(Pair("FundingResult", objFundingResult));

        // -- VALIDITY VOTING RESULTS
        UniValue objValid(UniValue::VOBJ);
        objValid.push_back(Pair("AbsoluteYesCount",  pGovObj->GetAbsoluteYesCount(VOTE_SIGNAL_VALID)));
        objValid.push_back(Pair("YesCount",  pGovObj->GetYesCount(VOTE_SIGNAL_VALID)));
        objValid.push_back(Pair("NoCount",  pGovObj->GetNoCount(VOTE_SIGNAL_VALID)));
        objValid.push_back(Pair("AbstainCount",  pGovObj->GetAbstainCount(VOTE_SIGNAL_VALID)));
        objResult.push_back(Pair("ValidResult", objValid));

        // -- DELETION CRITERION VOTING RESULTS
        UniValue objDelete(UniValue::VOBJ);
        objDelete.push_back(Pair("AbsoluteYesCount",  pGovObj->GetAbsoluteYesCount(VOTE_SIGNAL_DELETE)));
        objDelete.push_back(Pair("YesCount",  pGovObj->GetYesCount(VOTE_SIGNAL_DELETE)));
        objDelete.push_back(Pair("NoCount",  pGovObj->GetNoCount(VOTE_SIGNAL_DELETE)));
        objDelete.push_back(Pair("AbstainCount",  pGovObj->GetAbstainCount(VOTE_SIGNAL_DELETE)));
        objResult.push_back(Pair("DeleteResult", objDelete));

        // -- ENDORSED VIA DYNODE-ELECTED BOARD
        UniValue objEndorsed(UniValue::VOBJ);
        objEndorsed.push_back(Pair("AbsoluteYesCount",  pGovObj->GetAbsoluteYesCount(VOTE_SIGNAL_ENDORSED)));
        objEndorsed.push_back(Pair("YesCount",  pGovObj->GetYesCount(VOTE_SIGNAL_ENDORSED)));
        objEndorsed.push_back(Pair("NoCount",  pGovObj->GetNoCount(VOTE_SIGNAL_ENDORSED)));
        objEndorsed.push_back(Pair("AbstainCount",  pGovObj->GetAbstainCount(VOTE_SIGNAL_ENDORSED)));
        objResult.push_back(Pair("EndorsedResult", objEndorsed));

        // --
        std::string strError = "";
        objResult.push_back(Pair("fLocalValidity",  pGovObj->IsValidLocally(chainActive.Tip(), strError, false)));
        objResult.push_back(Pair("IsValidReason",  strError.c_str()));
        objResult.push_back(Pair("fCachedValid",  pGovObj->IsSetCachedValid()));
        objResult.push_back(Pair("fCachedFunding",  pGovObj->IsSetCachedFunding()));
        objResult.push_back(Pair("fCachedDelete",  pGovObj->IsSetCachedDelete()));
        objResult.push_back(Pair("fCachedEndorsed",  pGovObj->IsSetCachedEndorsed()));
        return objResult;
    }

    // GETVOTES FOR SPECIFIC GOVERNANCE OBJECT
    if(strCommand == "getvotes")
    {
        if (params.size() != 2)
            throw std::runtime_error(
                "Correct usage is 'gobject getvotes <governance-hash>'"
                );

        // COLLECT PARAMETERS FROM USER

        uint256 hash = ParseHashV(params[1], "Governance hash");

        // FIND OBJECT USER IS LOOKING FOR

        LOCK(governance.cs);

        CGovernanceObject* pGovObj = governance.FindGovernanceObject(hash);

        if(pGovObj == NULL) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown governance-hash");
        }

        // REPORT RESULTS TO USER

        UniValue bResult(UniValue::VOBJ);

        // GET MATCHING VOTES BY HASH, THEN SHOW USERS VOTE INFORMATION

        std::vector<CGovernanceVote> vecVotes = governance.GetMatchingVotes(hash);
        BOOST_FOREACH(CGovernanceVote vote, vecVotes) {
            bResult.push_back(Pair(vote.GetHash().ToString(),  vote.ToString()));
        }

        return bResult;
    }

    // GETVOTES FOR SPECIFIC GOVERNANCE OBJECT
    if(strCommand == "getcurrentvotes")
    {
        if (params.size() < 2 || params.size() == 3 || params.size() > 4)
            throw std::runtime_error(
                "Correct usage is 'gobject getcurrentvotes <governance-hash> [txid vout_index]'"
                );

        // COLLECT PARAMETERS FROM USER

        uint256 hash = ParseHashV(params[1], "Governance hash");

        CTxIn dnCollateralOutpoint;
        if (params.size() == 4) {
            uint256 txid = ParseHashV(params[2], "Dynode Collateral hash");
            std::string strVout = params[3].get_str();
            uint32_t vout = boost::lexical_cast<uint32_t>(strVout);
            dnCollateralOutpoint = CTxIn(txid, vout);
        }

        // FIND OBJECT USER IS LOOKING FOR

        LOCK(governance.cs);

        CGovernanceObject* pGovObj = governance.FindGovernanceObject(hash);

        if(pGovObj == NULL) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown governance-hash");
        }

        // REPORT RESULTS TO USER

        UniValue bResult(UniValue::VOBJ);

        // GET MATCHING VOTES BY HASH, THEN SHOW USERS VOTE INFORMATION

        std::vector<CGovernanceVote> vecVotes = governance.GetCurrentVotes(hash, dnCollateralOutpoint);
        BOOST_FOREACH(CGovernanceVote vote, vecVotes) {
            bResult.push_back(Pair(vote.GetHash().ToString(),  vote.ToString()));
        }

        return bResult;
    }

    return NullUniValue;
}

UniValue voteraw(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 6)
        throw std::runtime_error(
                "voteraw <dynode-tx-hash> <dynode-tx-index> <governance-hash> <vote-signal> [yes|no|abstain] <time> <vote-sig>\n"
                "Compile and relay a governance vote with provided external signature instead of signing vote internally\n"
                );

    uint256 hashDnTx = ParseHashV(params[0], "dn tx hash");
    int nDnTxIndex = params[1].get_int();
    CTxIn vin = CTxIn(hashDnTx, nDnTxIndex);

    uint256 hashGovObj = ParseHashV(params[2], "Governance hash");
    std::string strVoteSignal = params[3].get_str();
    std::string strVoteOutcome = params[4].get_str();

    vote_signal_enum_t eVoteSignal = CGovernanceVoting::ConvertVoteSignal(strVoteSignal);
    if(eVoteSignal == VOTE_SIGNAL_NONE)  {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "Invalid vote signal. Please using one of the following: "
                           "(funding|valid|delete|endorsed) OR `custom sentinel code` ");
    }

    vote_outcome_enum_t eVoteOutcome = CGovernanceVoting::ConvertVoteOutcome(strVoteOutcome);
    if(eVoteOutcome == VOTE_OUTCOME_NONE) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid vote outcome. Please use one of the following: 'yes', 'no' or 'abstain'");
    }

    int64_t nTime = params[5].get_int64();
    std::string strSig = params[6].get_str();
    bool fInvalid = false;
    std::vector<unsigned char> vchSig = DecodeBase64(strSig.c_str(), &fInvalid);

    if (fInvalid) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Malformed base64 encoding");
    }

    CDynode dn;
    bool dnFound = dnodeman.Get(vin, dn);

    if(!dnFound) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Failure to find dynode in list : " + vin.ToString());
    }

    CGovernanceVote vote(vin, hashGovObj, eVoteSignal, eVoteOutcome);
    vote.SetTime(nTime);
    vote.SetSignature(vchSig);

    if(!vote.IsValid(true)) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Failure to verify vote.");
    }

    CGovernanceException exception;
    if(governance.ProcessVote(vote, exception)) {
        return "Voted successfully";
    }
    else {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Error voting : " + exception.GetMessage());
    }
}

UniValue getgovernanceinfo(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0) {
        throw std::runtime_error(
            "getgovernanceinfo\n"
            "Returns an object containing governance parameters.\n"
            "\nResult:\n"
            "{\n"
            "  \"governanceminquorum\": xxxxx,           (numeric) the absolute minimum number of votes needed to trigger a governance action\n"
            "  \"dynodewatchdogmaxseconds\": xxxxx,  (numeric) sentinel watchdog expiration time in seconds\n"
            "  \"proposalfee\": xxx.xx,                  (numeric) the collateral transaction fee which must be paid to create a proposal in " + CURRENCY_UNIT + "\n"
            "  \"superblockcycle\": xxxxx,               (numeric) the number of blocks between superblocks\n"
            "  \"lastsuperblock\": xxxxx,                (numeric) the block number of the last superblock\n"
            "  \"nextsuperblock\": xxxxx,                (numeric) the block number of the next superblock\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getgovernanceinfo", "")
            + HelpExampleRpc("getgovernanceinfo", "")
            );
    }

    // Compute last/next superblock
    int nLastSuperblock, nNextSuperblock;

    // Get current block height
    int nBlockHeight = 0;
    {
        LOCK(cs_main);
        nBlockHeight = (int)chainActive.Height();
    }

    // Get chain parameters
    int nSuperblockStartBlock = Params().GetConsensus().nSuperblockStartBlock;
    int nSuperblockCycle = Params().GetConsensus().nSuperblockCycle;

    // Get first superblock
    int nFirstSuperblockOffset = (nSuperblockCycle - nSuperblockStartBlock % nSuperblockCycle) % nSuperblockCycle;
    int nFirstSuperblock = nSuperblockStartBlock + nFirstSuperblockOffset;

    if(nBlockHeight < nFirstSuperblock){
        nLastSuperblock = 0;
        nNextSuperblock = nFirstSuperblock;
    } else {
        nLastSuperblock = nBlockHeight - nBlockHeight % nSuperblockCycle;
        nNextSuperblock = nLastSuperblock + nSuperblockCycle;
    }

    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("governanceminquorum", Params().GetConsensus().nGovernanceMinQuorum));
    obj.push_back(Pair("dynodewatchdogmaxseconds", DYNODE_WATCHDOG_MAX_SECONDS));
    obj.push_back(Pair("proposalfee", ValueFromAmount(GOVERNANCE_PROPOSAL_FEE_TX)));
    obj.push_back(Pair("superblockcycle", Params().GetConsensus().nSuperblockCycle));
    obj.push_back(Pair("lastsuperblock", nLastSuperblock));
    obj.push_back(Pair("nextsuperblock", nNextSuperblock));

    return obj;
}

UniValue getsuperblockbudget(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1) {
        throw std::runtime_error(
            "getsuperblockbudget index\n"
            "\nReturns the absolute minimum number of votes needed to trigger a governance action.\n"
            "\nArguments:\n"
            "1. index         (numeric, required) The block index\n"
            "\nResult:\n"
            "n    (numeric) The current minimum governance quorum\n"
            "\nExamples:\n"
            + HelpExampleCli("getsuperblockbudget", "1000")
            + HelpExampleRpc("getsuperblockbudget", "1000")
        );
    }

    int nBlockHeight = params[0].get_int();
    if (nBlockHeight < 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Block height out of range");
    }

    CAmount nBudget = CSuperblock::GetPaymentsLimit(nBlockHeight);
    std::string strBudget = FormatMoney(nBudget);

    return strBudget;
}

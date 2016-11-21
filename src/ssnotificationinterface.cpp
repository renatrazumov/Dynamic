// Copyright (c) 2014-2017 The Dash Core Developers
// Copyright (c) 2015-2017 Silk Network Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ssnotificationinterface.h"
#include "privatesend.h"
#include "governance.h"
#include "dynodeman.h"
#include "dynode-payments.h"
#include "dynode-sync.h"

CSSNotificationInterface::CSSNotificationInterface()
{
}

CSSNotificationInterface::~CSSNotificationInterface()
{
}

void CSSNotificationInterface::UpdatedBlockTip(const CBlockIndex *pindex)
{
    snodeman.UpdatedBlockTip(pindex);
    privateSendPool.UpdatedBlockTip(pindex);
    snpayments.UpdatedBlockTip(pindex);
    governance.UpdatedBlockTip(pindex);
    dynodeSync.UpdatedBlockTip(pindex);
}

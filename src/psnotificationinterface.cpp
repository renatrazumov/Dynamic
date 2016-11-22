// Copyright (c) 2014-2017 The Dash Core Developers
// Copyright (c) 2016-2017 Duality Blockchain Solutions Ltd
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "psnotificationinterface.h"
#include "privatesend.h"
#include "governance.h"
#include "dynodeman.h"
#include "dynode-payments.h"
#include "dynode-sync.h"

CPSNotificationInterface::CPSNotificationInterface()
{
}

CPSNotificationInterface::~CPSNotificationInterface()
{
}

void CPSNotificationInterface::UpdatedBlockTip(const CBlockIndex *pindex)
{
    dnodeman.UpdatedBlockTip(pindex);
    privateSendPool.UpdatedBlockTip(pindex);
    dnpayments.UpdatedBlockTip(pindex);
    governance.UpdatedBlockTip(pindex);
    dynodeSync.UpdatedBlockTip(pindex);
}

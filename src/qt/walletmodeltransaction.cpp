// Copyright (c) 2009-2017 Satoshi Nakamoto
// Copyright (c) 2009-2017 The Bitcoin Developers
// Copyright (c) 2014-2017 The Dash Core Developers
// Copyright (c) 2016-2017 Duality Blockchain Solutions Ltd
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "walletmodeltransaction.h"

#include "wallet/wallet.h"

WalletModelTransaction::WalletModelTransaction(const QList<SendCoinsRecipient> &recipients) :
    recipients(recipients),
    walletTransaction(0),
    keyChange(0),
    fee(0)
{
    walletTransaction = new CWalletTx();
}

WalletModelTransaction::~WalletModelTransaction()
{
    delete keyChange;
    delete walletTransaction;
}

QList<SendCoinsRecipient> WalletModelTransaction::getRecipients()
{
    return recipients;
}

CWalletTx *WalletModelTransaction::getTransaction()
{
    return walletTransaction;
}

unsigned int WalletModelTransaction::getTransactionSize()
{
    return (!walletTransaction ? 0 : (::GetSerializeSize(*(CTransaction*)walletTransaction, SER_NETWORK, PROTOCOL_VERSION)));
}

CAmount WalletModelTransaction::getTransactionFee()
{
    return fee;
}

void WalletModelTransaction::setTransactionFee(const CAmount& newFee)
{
    fee = newFee;
}

void WalletModelTransaction::reassignAmounts(int nChangePosRet)
{
    int i = 0;
    for (QList<SendCoinsRecipient>::iterator it = recipients.begin(); it != recipients.end(); ++it)
    {
        SendCoinsRecipient& rcp = (*it);

        if (rcp.paymentRequest.IsInitialized())
        {
            CAmount subtotal = 0;
            const payments::PaymentDetails& details = rcp.paymentRequest.getDetails();
            for (int j = 0; j < details.outputs_size(); j++)
            {
                const payments::Output& out = details.outputs(j);
                if (out.amount() <= 0) continue;
                if (i == nChangePosRet)
                    i++;
                subtotal += walletTransaction->vout[i].nValue;
                i++;
            }
            rcp.amount = subtotal;
        }
        else // normal recipient (no payment request)
        {
            if (i == nChangePosRet)
                i++;
            rcp.amount = walletTransaction->vout[i].nValue;
            i++;
        }
    }
}

CAmount WalletModelTransaction::getTotalTransactionAmount()
{
    CAmount totalTransactionAmount = 0;
    Q_FOREACH(const SendCoinsRecipient &rcp, recipients)
    {
        totalTransactionAmount += rcp.amount;
    }
    return totalTransactionAmount;
}

void WalletModelTransaction::newPossibleKeyChange(CWallet *wallet)
{
    keyChange = new CReserveKey(wallet);
}

CReserveKey *WalletModelTransaction::getPossibleKeyChange()
{
    return keyChange;
}

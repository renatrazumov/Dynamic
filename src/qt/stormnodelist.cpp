// Copyright (c) 2009-2017 Satoshi Nakamoto
// Copyright (c) 2009-2017 The Bitcoin Developers
// Copyright (c) 2014-2017 The Dash Core Developers
// Copyright (c) 2016-2017 Duality Blockchain Solutions Ltd
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "dynodelist.h"
#include "ui_dynodelist.h"

#include "sync.h"
#include "clientmodel.h"
#include "walletmodel.h"
#include "activedynode.h"
#include "dynode-sync.h"
#include "dynodeconfig.h"
#include "dynodeman.h"
#include "wallet/wallet.h"
#include "init.h"
#include "guiutil.h"

#include <QTimer>
#include <QMessageBox>

CCriticalSection cs_dynodes;

DynodeList::DynodeList(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::DynodeList),
    clientModel(0),
    walletModel(0)
{
    ui->setupUi(this);

    ui->startButton->setEnabled(true);

    int columnAliasWidth = 100;
    int columnAddressWidth = 200;
    int columnProtocolWidth = 60;
    int columnStatusWidth = 80;
    int columnActiveWidth = 130;
    int columnLastSeenWidth = 130;

    ui->tableWidgetMyDynodes->setColumnWidth(0, columnAliasWidth);
    ui->tableWidgetMyDynodes->setColumnWidth(1, columnAddressWidth);
    ui->tableWidgetMyDynodes->setColumnWidth(2, columnProtocolWidth);
    ui->tableWidgetMyDynodes->setColumnWidth(3, columnStatusWidth);
    ui->tableWidgetMyDynodes->setColumnWidth(4, columnActiveWidth);
    ui->tableWidgetMyDynodes->setColumnWidth(5, columnLastSeenWidth);

    ui->tableWidgetDynodes->setColumnWidth(0, columnAddressWidth);
    ui->tableWidgetDynodes->setColumnWidth(1, columnProtocolWidth);
    ui->tableWidgetDynodes->setColumnWidth(2, columnStatusWidth);
    ui->tableWidgetDynodes->setColumnWidth(3, columnActiveWidth);
    ui->tableWidgetDynodes->setColumnWidth(4, columnLastSeenWidth);

    ui->tableWidgetMyDynodes->setContextMenuPolicy(Qt::CustomContextMenu);

    QAction *startAliasAction = new QAction(tr("Start alias"), this);
    contextMenu = new QMenu();
    contextMenu->addAction(startAliasAction);
    connect(ui->tableWidgetMyDynodes, SIGNAL(customContextMenuRequested(const QPoint&)), this, SLOT(showContextMenu(const QPoint&)));
    connect(startAliasAction, SIGNAL(triggered()), this, SLOT(on_startButton_clicked()));

    timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(updateNodeList()));
    connect(timer, SIGNAL(timeout()), this, SLOT(updateMyNodeList()));
    timer->start(1000);

    fFilterUpdated = false;
    nTimeFilterUpdated = GetTime();
    updateNodeList();
}

DynodeList::~DynodeList()
{
    delete ui;
}

void DynodeList::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if(model) {
        // try to update list when dynode count changes
        connect(clientModel, SIGNAL(strDynodesChanged(QString)), this, SLOT(updateNodeList()));
    }
}

void DynodeList::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
}

void DynodeList::showContextMenu(const QPoint &point)
{
    QTableWidgetItem *item = ui->tableWidgetMyDynodes->itemAt(point);
    if(item) contextMenu->exec(QCursor::pos());
}

void DynodeList::StartAlias(std::string strAlias)
{
    std::string strStatusHtml;
    strStatusHtml += "<center>Alias: " + strAlias;

    BOOST_FOREACH(CDynodeConfig::CDynodeEntry sne, dynodeConfig.getEntries()) {
        if(sne.getAlias() == strAlias) {
            std::string strError;
            CDynodeBroadcast snb;

            bool fSuccess = CDynodeBroadcast::Create(sne.getIp(), sne.getPrivKey(), sne.getTxHash(), sne.getOutputIndex(), strError, snb);

            if(fSuccess) {
                strStatusHtml += "<br>Successfully started dynode.";
                snodeman.UpdateDynodeList(snb);
                snb.Relay();
                snodeman.NotifyDynodeUpdates();
            } else {
                strStatusHtml += "<br>Failed to start dynode.<br>Error: " + strError;
            }
            break;
        }
    }
    strStatusHtml += "</center>";

    QMessageBox msg;
    msg.setText(QString::fromStdString(strStatusHtml));
    msg.exec();

    updateMyNodeList(true);
}

void DynodeList::StartAll(std::string strCommand)
{
    int nCountSuccessful = 0;
    int nCountFailed = 0;
    std::string strFailedHtml;

    BOOST_FOREACH(CDynodeConfig::CDynodeEntry sne, dynodeConfig.getEntries()) {
        std::string strError;
        CDynodeBroadcast snb;

        CTxIn txin = CTxIn(uint256S(sne.getTxHash()), uint32_t(atoi(sne.getOutputIndex().c_str())));
        CDynode *psn = snodeman.Find(txin);

        if(strCommand == "start-missing" && psn) continue;

        bool fSuccess = CDynodeBroadcast::Create(sne.getIp(), sne.getPrivKey(), sne.getTxHash(), sne.getOutputIndex(), strError, snb);

        if(fSuccess) {
            nCountSuccessful++;
            snodeman.UpdateDynodeList(snb);
            snb.Relay();
            snodeman.NotifyDynodeUpdates();
        } else {
            nCountFailed++;
            strFailedHtml += "\nFailed to start " + sne.getAlias() + ". Error: " + strError;
        }
    }
    pwalletMain->Lock();

    std::string returnObj;
    returnObj = strprintf("Successfully started %d dynodes, failed to start %d, total %d", nCountSuccessful, nCountFailed, nCountFailed + nCountSuccessful);
    if (nCountFailed > 0) {
        returnObj += strFailedHtml;
    }

    QMessageBox msg;
    msg.setText(QString::fromStdString(returnObj));
    msg.exec();

    updateMyNodeList(true);
}

void DynodeList::updateMyDynodeInfo(QString strAlias, QString strAddr, CDynode *psn)
{
    LOCK(cs_snlistupdate);
    bool fOldRowFound = false;
    int nNewRow = 0;

    for(int i = 0; i < ui->tableWidgetMyDynodes->rowCount(); i++) {
        if(ui->tableWidgetMyDynodes->item(i, 0)->text() == strAlias) {
            fOldRowFound = true;
            nNewRow = i;
            break;
        }
    }

    if(nNewRow == 0 && !fOldRowFound) {
        nNewRow = ui->tableWidgetMyDynodes->rowCount();
        ui->tableWidgetMyDynodes->insertRow(nNewRow);
    }

    QTableWidgetItem *aliasItem = new QTableWidgetItem(strAlias);
    QTableWidgetItem *addrItem = new QTableWidgetItem(psn ? QString::fromStdString(psn->addr.ToString()) : strAddr);
    QTableWidgetItem *protocolItem = new QTableWidgetItem(QString::number(psn ? psn->nProtocolVersion : -1));
    QTableWidgetItem *statusItem = new QTableWidgetItem(QString::fromStdString(psn ? psn->GetStatus() : "MISSING"));
    QTableWidgetItem *activeSecondsItem = new QTableWidgetItem(QString::fromStdString(DurationToDHMS(psn ? (psn->lastPing.sigTime - psn->sigTime) : 0)));
    QTableWidgetItem *lastSeenItem = new QTableWidgetItem(QString::fromStdString(DateTimeStrFormat("%Y-%m-%d %H:%M", psn ? psn->lastPing.sigTime : 0)));
    QTableWidgetItem *pubkeyItem = new QTableWidgetItem(QString::fromStdString(psn ? CDynamicAddress(psn->pubKeyCollateralAddress.GetID()).ToString() : ""));

    ui->tableWidgetMyDynodes->setItem(nNewRow, 0, aliasItem);
    ui->tableWidgetMyDynodes->setItem(nNewRow, 1, addrItem);
    ui->tableWidgetMyDynodes->setItem(nNewRow, 2, protocolItem);
    ui->tableWidgetMyDynodes->setItem(nNewRow, 3, statusItem);
    ui->tableWidgetMyDynodes->setItem(nNewRow, 4, activeSecondsItem);
    ui->tableWidgetMyDynodes->setItem(nNewRow, 5, lastSeenItem);
    ui->tableWidgetMyDynodes->setItem(nNewRow, 6, pubkeyItem);
}

void DynodeList::updateMyNodeList(bool fForce)
{
    static int64_t nTimeMyListUpdated = 0;

    // automatically update my dynode list only once in MY_DYNODELIST_UPDATE_SECONDS seconds,
    // this update still can be triggered manually at any time via button click
    int64_t nSecondsTillUpdate = nTimeMyListUpdated + MY_DYNODELIST_UPDATE_SECONDS - GetTime();
    ui->secondsLabel->setText(QString::number(nSecondsTillUpdate));

    if(nSecondsTillUpdate > 0 && !fForce) return;
    nTimeMyListUpdated = GetTime();

    ui->tableWidgetDynodes->setSortingEnabled(false);
    BOOST_FOREACH(CDynodeConfig::CDynodeEntry sne, dynodeConfig.getEntries()) {
        CTxIn txin = CTxIn(uint256S(sne.getTxHash()), uint32_t(atoi(sne.getOutputIndex().c_str())));
        CDynode *psn = snodeman.Find(txin);

        updateMyDynodeInfo(QString::fromStdString(sne.getAlias()), QString::fromStdString(sne.getIp()), psn);
    }
    ui->tableWidgetDynodes->setSortingEnabled(true);

    // reset "timer"
    ui->secondsLabel->setText("0");
}

void DynodeList::updateNodeList()
{
    static int64_t nTimeListUpdated = GetTime();

    // to prevent high cpu usage update only once in DYNODELIST_UPDATE_SECONDS seconds
    // or DYNODELIST_FILTER_COOLDOWN_SECONDS seconds after filter was last changed
    int64_t nSecondsToWait = fFilterUpdated
                            ? nTimeFilterUpdated - GetTime() + DYNODELIST_FILTER_COOLDOWN_SECONDS
                            : nTimeListUpdated - GetTime() + DYNODELIST_UPDATE_SECONDS;

    if(fFilterUpdated) ui->countLabel->setText(QString::fromStdString(strprintf("Please wait... %d", nSecondsToWait)));
    if(nSecondsToWait > 0) return;

    nTimeListUpdated = GetTime();
    fFilterUpdated = false;

    TRY_LOCK(cs_dynodes, lockDynodes);
    if(!lockDynodes) return;

    QString strToFilter;
    ui->countLabel->setText("Updating...");
    ui->tableWidgetDynodes->setSortingEnabled(false);
    ui->tableWidgetDynodes->clearContents();
    ui->tableWidgetDynodes->setRowCount(0);
    std::vector<CDynode> vDynodes = snodeman.GetFullDynodeVector();

    BOOST_FOREACH(CDynode& sn, vDynodes)
    {
        // populate list
        // Address, Protocol, Status, Active Seconds, Last Seen, Pub Key
        QTableWidgetItem *addressItem = new QTableWidgetItem(QString::fromStdString(sn.addr.ToString()));
        QTableWidgetItem *protocolItem = new QTableWidgetItem(QString::number(sn.nProtocolVersion));
        QTableWidgetItem *statusItem = new QTableWidgetItem(QString::fromStdString(sn.GetStatus()));
        QTableWidgetItem *activeSecondsItem = new QTableWidgetItem(QString::fromStdString(DurationToDHMS(sn.lastPing.sigTime - sn.sigTime)));
        QTableWidgetItem *lastSeenItem = new QTableWidgetItem(QString::fromStdString(DateTimeStrFormat("%Y-%m-%d %H:%M", sn.lastPing.sigTime)));
        QTableWidgetItem *pubkeyItem = new QTableWidgetItem(QString::fromStdString(CDynamicAddress(sn.pubKeyCollateralAddress.GetID()).ToString()));

        if (strCurrentFilter != "")
        {
            strToFilter =   addressItem->text() + " " +
                            protocolItem->text() + " " +
                            statusItem->text() + " " +
                            activeSecondsItem->text() + " " +
                            lastSeenItem->text() + " " +
                            pubkeyItem->text();
            if (!strToFilter.contains(strCurrentFilter)) continue;
        }

        ui->tableWidgetDynodes->insertRow(0);
        ui->tableWidgetDynodes->setItem(0, 0, addressItem);
        ui->tableWidgetDynodes->setItem(0, 1, protocolItem);
        ui->tableWidgetDynodes->setItem(0, 2, statusItem);
        ui->tableWidgetDynodes->setItem(0, 3, activeSecondsItem);
        ui->tableWidgetDynodes->setItem(0, 4, lastSeenItem);
        ui->tableWidgetDynodes->setItem(0, 5, pubkeyItem);
    }

    ui->countLabel->setText(QString::number(ui->tableWidgetDynodes->rowCount()));
    ui->tableWidgetDynodes->setSortingEnabled(true);
}

void DynodeList::on_filterLineEdit_textChanged(const QString &strFilterIn)
{
    strCurrentFilter = strFilterIn;
    nTimeFilterUpdated = GetTime();
    fFilterUpdated = true;
    ui->countLabel->setText(QString::fromStdString(strprintf("Please wait... %d", DYNODELIST_FILTER_COOLDOWN_SECONDS)));
}

void DynodeList::on_startButton_clicked()
{
    // Find selected node alias
    QItemSelectionModel* selectionModel = ui->tableWidgetMyDynodes->selectionModel();
    QModelIndexList selected = selectionModel->selectedRows();

    if(selected.count() == 0) return;

    QModelIndex index = selected.at(0);
    int nSelectedRow = index.row();
    std::string strAlias = ui->tableWidgetMyDynodes->item(nSelectedRow, 0)->text().toStdString();

    // Display message box
    QMessageBox::StandardButton retval = QMessageBox::question(this, tr("Confirm dynode start"),
        tr("Are you sure you want to start dynode %1?").arg(QString::fromStdString(strAlias)),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);

    if(retval != QMessageBox::Yes) return;

    WalletModel::EncryptionStatus encStatus = walletModel->getEncryptionStatus();

    if(encStatus == walletModel->Locked || encStatus == walletModel->UnlockedForMixingOnly) {
        WalletModel::UnlockContext ctx(walletModel->requestUnlock());

        if(!ctx.isValid()) return; // Unlock wallet was cancelled

        StartAlias(strAlias);
        return;
    }

    StartAlias(strAlias);
}

void DynodeList::on_startAllButton_clicked()
{
    // Display message box
    QMessageBox::StandardButton retval = QMessageBox::question(this, tr("Confirm all dynodes start"),
        tr("Are you sure you want to start ALL dynodes?"),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);

    if(retval != QMessageBox::Yes) return;

    WalletModel::EncryptionStatus encStatus = walletModel->getEncryptionStatus();

    if(encStatus == walletModel->Locked || encStatus == walletModel->UnlockedForMixingOnly) {
        WalletModel::UnlockContext ctx(walletModel->requestUnlock());

        if(!ctx.isValid()) return; // Unlock wallet was cancelled

        StartAll();
        return;
    }

    StartAll();
}

void DynodeList::on_startMissingButton_clicked()
{

    if(!dynodeSync.IsDynodeListSynced()) {
        QMessageBox::critical(this, tr("Command is not available right now"),
            tr("You can't use this command until dynode list is synced"));
        return;
    }

    // Display message box
    QMessageBox::StandardButton retval = QMessageBox::question(this,
        tr("Confirm missing dynodes start"),
        tr("Are you sure you want to start MISSING dynodes?"),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);

    if(retval != QMessageBox::Yes) return;

    WalletModel::EncryptionStatus encStatus = walletModel->getEncryptionStatus();

    if(encStatus == walletModel->Locked || encStatus == walletModel->UnlockedForMixingOnly) {
        WalletModel::UnlockContext ctx(walletModel->requestUnlock());

        if(!ctx.isValid()) return; // Unlock wallet was cancelled

        StartAll("start-missing");
        return;
    }

    StartAll("start-missing");
}

void DynodeList::on_tableWidgetMyDynodes_itemSelectionChanged()
{
    if(ui->tableWidgetMyDynodes->selectedItems().count() > 0) {
        ui->startButton->setEnabled(true);
    }
}

void DynodeList::on_UpdateButton_clicked()
{
    updateMyNodeList(true);
}

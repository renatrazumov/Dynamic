// Copyright (c) 2009-2017 Satoshi Nakamoto
// Copyright (c) 2009-2017 The Bitcoin Developers
// Copyright (c) 2014-2017 The Dash Core Developers
// Copyright (c) 2016-2017 Duality Blockchain Solutions Ltd
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DYNAMIC_QT_INTRO_H
#define DYNAMIC_QT_INTRO_H

#include <QDialog>
#include <QMutex>
#include <QThread>

static const bool DEFAULT_CHOOSE_DATADIR = false;

class FreespaceChecker;

namespace Ui {
    class Intro;
}

/** Introduction screen (pre-GUI startup).
  Allows the user to choose a data directory,
  in which the wallet and block chain will be stored.
 */
class Intro : public QDialog
{
    Q_OBJECT

public:
    explicit Intro(QWidget *parent = 0);
    ~Intro();

    QString getDataDirectory();
    void setDataDirectory(const QString &dataDir);

    /**
     * Determine data directory. Let the user choose if the current one doesn't exist.
     *
     * @note do NOT call global GetDataDir() before calling this function, this
     * will cause the wrong path to be cached.
     */
    static void pickDataDirectory();

    /**
     * Determine default data directory for operating system.
     */
    static QString getDefaultDataDirectory();

Q_SIGNALS:
    void requestCheck();
    void stopThread();

public Q_SLOTS:
    void setStatus(int status, const QString &message, quint64 bytesAvailable);

private Q_SLOTS:
    void on_dataDirectory_textChanged(const QString &arg1);
    void on_ellipsisButton_clicked();
    void on_dataDirDefault_clicked();
    void on_dataDirCustom_clicked();

private:
    Ui::Intro *ui;
    QThread *thread;
    QMutex mutex;
    bool signalled;
    QString pathToCheck;

    void startThread();
    void checkPath(const QString &dataDir);
    QString getPathToCheck();

    friend class FreespaceChecker;
};

#endif // DYNAMIC_QT_INTRO_H

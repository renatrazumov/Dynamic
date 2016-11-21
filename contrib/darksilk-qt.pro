TEMPLATE = app
TARGET = dynamic
VERSION = 1.0.0.0
INCLUDEPATH += src \
               src/crypto \
               src/crypto/heavyhash \
               src/qt \
               src/secp256k1/include \
               src/univalue/include \
               src/leveldb/helpers/memenv \
               src/script \

INCLUDEPATH += $$PWD/../../../../usr/lib/x86_64-linux-gnu
DEPENDPATH += $$PWD/../../../../usr/lib/x86_64-linux-gnu
LIBS += -lrt

QT += core gui network widgets printsupport

DEFINES += ENABLE_WALLET
DEFINES += BOOST_THREAD_USE_LIB BOOST_SPIRIT_THREADSAFE
CONFIG += no_include_pwd
CONFIG += thread


lessThan(QT_MAJOR_VERSION, 5) {
    # stop build

}
DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0
DEFINES += HAVE_WORKING_BOOST_SLEEP_FOR=1
QT += widgets
# LIBSEC256K1 SUPPORT
# QMAKE_CXXFLAGS *= -DUSE_SECP256K1

# for boost 1.37, add -mt to the boost libraries
# use: qmake BOOST_LIB_SUFFIX=-mt
# for boost thread win32 with _win32 sufix
# use: BOOST_THREAD_LIB_SUFFIX=_win32-...
# or when linking against a specific BerkelyDB version: BDB_LIB_SUFFIX=-4.8

# Dependency library locations can be customized with:
#    BOOST_INCLUDE_PATH, BOOST_LIB_PATH, BDB_INCLUDE_PATH,
#    BDB_LIB_PATH, OPENSSL_INCLUDE_PATH and OPENSSL_LIB_PATH respectively

OBJECTS_DIR = build
MOC_DIR = build
UI_DIR = build

# use: qmake "RELEASE=1"
contains(RELEASE, 1) {
    # Mac: compile for maximum compatibility (10.5, 32-bit)
    macx:QMAKE_CXXFLAGS += -mmacosx-version-min=10.5 -arch x86_64 -isysroot /Developer/SDKs/MacOSX10.5.sdk

    !windows:!macx {
        # Linux: static link
        LIBS += -Wl,-Bstatic
    }
}

!win32 {
# for extra security against potential buffer overflows: enable GCCs Stack Smashing Protection
QMAKE_CXXFLAGS *= -fstack-protector-all --param ssp-buffer-size=1
QMAKE_LFLAGS *= -fstack-protector-all --param ssp-buffer-size=1
# We need to exclude this for Windows cross compile with MinGW 4.2.x, as it will result in a non-working executable!
# This can be enabled for Windows, when we switch to MinGW >= 4.4.x.
}
# for extra security on Windows: enable ASLR and DEP via GCC linker flags
win32:QMAKE_LFLAGS *= -Wl,--dynamicbase -Wl,--nxcompat -static
win32:QMAKE_LFLAGS += -static-libgcc -static-libstdc++

# use: qmake "USE_QRCODE=1"
# libqrencode (http://fukuchi.org/works/qrencode/index.en.html) must be installed for support
contains(USE_QRCODE, 1) {
    message(Building with QRCode support)
    DEFINES += USE_QRCODE
    LIBS += -lqrencode
}

# use: qmake "USE_UPNP=1" ( enabled by default; default)
#  or: qmake "USE_UPNP=0" (disabled by default)
#  or: qmake "USE_UPNP=-" (not supported)
# miniupnpc (http://miniupnp.free.fr/files/) must be installed for support
contains(USE_UPNP, -) {
    message(Building without UPNP support)
} else {
    message(Building with UPNP support)
    count(USE_UPNP, 0) {
        USE_UPNP=1
    }
    DEFINES += USE_UPNP=$$USE_UPNP STATICLIB
    INCLUDEPATH += $$MINIUPNPC_INCLUDE_PATH
    LIBS += $$join(MINIUPNPC_LIB_PATH,,-L,) -lminiupnpc
    win32:LIBS += -liphlpapi
}

# use: qmake "USE_DBUS=1" or qmake "USE_DBUS=0"
linux:count(USE_DBUS, 0) {
    USE_DBUS=1
}
contains(USE_DBUS, 1) {
    message(Building with DBUS (Freedesktop notifications) support)
    DEFINES += USE_DBUS
    QT += dbus
}

contains(DYNAMIC_NEED_QT_PLUGINS, 1) {
    DEFINES += DYNAMIC_NEED_QT_PLUGINS
    QTPLUGIN += qcncodecs qjpcodecs qtwcodecs qkrcodecs qtaccessiblewidgets
}

#Build Secp256k1
INCLUDEPATH += src/secp256k1/include
LIBS += $$PWD/src/secp256k1/.libs/libsecp256k1.a
!win32 {
    # we use QMAKE_CXXFLAGS_RELEASE even without RELEASE=1 because we use RELEASE to indicate linking preferences not -O preferences
     gensecp256k1.commands = if [ -f $$PWD/src/secp256k1/.libs/libsecp256k1.a ]; then echo "Secp256k1 already built"; else cd $$PWD/src/secp256k1 && ./autogen.sh && ./configure --disable-shared --with-pic && CC=$$QMAKE_CC CXX=$$QMAKE_CXX $(MAKE) OPT=\"$$QMAKE_CXXFLAGS $$QMAKE_CXXFLAGS_RELEASE\"; fi
} else {
    #Windows ???
}
gensecp256k1.target = $$PWD/src/secp256k1/.libs/libsecp256k1.a
gensecp256k1.depends = FORCE
PRE_TARGETDEPS += $$PWD/src/secp256k1/.libs/libsecp256k1.a
QMAKE_EXTRA_TARGETS += gensecp256k1
QMAKE_CLEAN += $$PWD/src/secp256k1/.libs/libsecp256k1.a; cd $$PWD/src/secp256k1 ; $(MAKE) clean


#Build LevelDB
INCLUDEPATH += src/leveldb/include src/leveldb/helpers src/leveldb/helpers/memenv
LIBS += $$PWD/src/leveldb/libleveldb.a $$PWD/src/leveldb/libmemenv.a
!win32 {
    # we use QMAKE_CXXFLAGS_RELEASE even without RELEASE=1 because we use RELEASE to indicate linking preferences not -O preferences
    genleveldb.commands = cd $$PWD/src/leveldb && CC=$$QMAKE_CC CXX=$$QMAKE_CXX $(MAKE) OPT=\"$$QMAKE_CXXFLAGS $$QMAKE_CXXFLAGS_RELEASE\" libleveldb.a libmemenv.a
} else {
    # make an educated guess about what the ranlib command is called
    isEmpty(QMAKE_RANLIB) {
        QMAKE_RANLIB = $$replace(QMAKE_STRIP, strip, ranlib)
    }
    LIBS += -lshlwapi
    genleveldb.commands = cd $$PWD/src/leveldb && CC=$$QMAKE_CC CXX=$$QMAKE_CXX TARGET_OS=OS_WINDOWS_CROSSCOMPILE $(MAKE) OPT=\"$$QMAKE_CXXFLAGS $$QMAKE_CXXFLAGS_RELEASE\" libleveldb.a libmemenv.a && $$QMAKE_RANLIB $$PWD/src/leveldb/libleveldb.a && $$QMAKE_RANLIB $$PWD/src/leveldb/libmemenv.a
}
genleveldb.target = $$PWD/src/leveldb/libleveldb.a
genleveldb.depends = FORCE
PRE_TARGETDEPS += $$PWD/src/leveldb/libleveldb.a
QMAKE_EXTRA_TARGETS += genleveldb
# Gross ugly hack that depends on qmake internals, unfortunately there is no other way to do it.
QMAKE_CLEAN += $$PWD/src/leveldb/libleveldb.a; cd $$PWD/src/leveldb ; $(MAKE) clean

#Build Univalue
INCLUDEPATH += src/univalue/include
LIBS += $$PWD/src/univalue/lib/libunivalue_la-univalue.o
LIBS += $$PWD/src/univalue/lib/libunivalue_la-univalue_read.o
LIBS += $$PWD/src/univalue/lib/libunivalue_la-univalue_write.o
!win32 {
    # we use QMAKE_CXXFLAGS_RELEASE even without RELEASE=1 because we use RELEASE to indicate linking preferences not -O preferences
    genUnivalue.commands =if [ -f $$PWD/src/univalue/lib/libunivalue_la-univalue.o ]; then echo "Univalue already built"; else cd $$PWD/src/univalue && ./autogen.sh && ./configure && CC=$$QMAKE_CC CXX=$$QMAKE_CXX $(MAKE) OPT=\"$$QMAKE_CXXFLAGS $$QMAKE_CXXFLAGS_RELEASE\"; fi
} else {
    #Windows ???
}
genUnivalue.target = $$PWD/src/univalue/lib/libunivalue_la-univalue.o
genUnivalue.depends = FORCE
PRE_TARGETDEPS += $$PWD/src/univalue/lib/libunivalue_la-univalue.o
QMAKE_EXTRA_TARGETS += genUnivalue
QMAKE_CLEAN += $$PWD/src/univalue/lib/libunivalue_la-univalue.o; cd $$PWD/src/univalue ; $(MAKE) clean

# Build Protobuf Payment Request cpp code file
LIBS += -L/usr/local/lib -lprotobuf
!win32 {
    genprotobuff.commands = cd $$PWD/src/qt && protoc -I=. --cpp_out=. ./paymentrequest.proto
} else {
    genprotobuff.commands = cd $$PWD/src/qt && protoc -I=. --cpp_out=. ./paymentrequest.proto
}
genprotobuff.depends = FORCE
QMAKE_EXTRA_TARGETS += genprotobuff

# regenerate src/build.h
!windows|contains(USE_BUILD_INFO, 1) {
    genbuild.depends = FORCE
    genbuild.commands = cd $$PWD; /bin/sh share/genbuild.sh $$OUT_PWD/build/build.h
    genbuild.target = $$OUT_PWD/build/build.h
    PRE_TARGETDEPS += $$OUT_PWD/build/build.h
    QMAKE_EXTRA_TARGETS += genbuild
    DEFINES += HAVE_BUILD_INFO
}

contains(USE_O3, 1) {
    message(Building O3 optimization flag)
    QMAKE_CXXFLAGS_RELEASE -= -O2
    QMAKE_CFLAGS_RELEASE -= -O2
    QMAKE_CXXFLAGS += -O3
    QMAKE_CFLAGS += -O3
}

*-g++-32 {
    message("32 platform, adding -msse2 flag")

    QMAKE_CXXFLAGS += -msse2
    QMAKE_CFLAGS += -msse2
}

QMAKE_CXXFLAGS_WARN_ON = -fdiagnostics-show-option -Wall -Wextra -Wno-ignored-qualifiers -Wformat -Wformat-security -Wno-unused-parameter -Wstack-protector

# Input
DEPENDPATH += src src/json src/qt

contains(USE_QRCODE, 1) {
HEADERS += src/qt/qrcodedialog.h
SOURCES += src/qt/qrcodedialog.cpp
FORMS += src/qt/forms/qrcodedialog.ui
}

CODECFORTR = UTF-8

# for lrelease/lupdate
# also add new translations to src/qt/dynamic.qrc under translations/
TRANSLATIONS = $$files(src/qt/locale/dynamic_*.ts)

isEmpty(QMAKE_LRELEASE) {
    win32:QMAKE_LRELEASE = $$[QT_INSTALL_BINS]\\lrelease.exe
    else:QMAKE_LRELEASE = $$[QT_INSTALL_BINS]/lrelease
}
isEmpty(QM_DIR):QM_DIR = $$PWD/src/qt/locale
# automatically build translations, so they can be included in resource file
TSQM.name = lrelease ${QMAKE_FILE_IN}
TSQM.input = TRANSLATIONS
TSQM.output = $$QM_DIR/${QMAKE_FILE_BASE}.qm
TSQM.commands = $$QMAKE_LRELEASE ${QMAKE_FILE_IN} -qm ${QMAKE_FILE_OUT}
TSQM.CONFIG = no_link
QMAKE_EXTRA_COMPILERS += TSQM


# platform specific defaults, if not overridden on command line
isEmpty(BOOST_LIB_SUFFIX) {
    macx:BOOST_LIB_SUFFIX = -mt
    windows:BOOST_LIB_SUFFIX = -mt
}

isEmpty(BOOST_THREAD_LIB_SUFFIX) {
    win32:BOOST_THREAD_LIB_SUFFIX = _win32$$BOOST_LIB_SUFFIX
    else:BOOST_THREAD_LIB_SUFFIX = $$BOOST_LIB_SUFFIX
}

isEmpty(BDB_LIB_PATH) {
    macx:BDB_LIB_PATH = /opt/local/lib/db48
}

isEmpty(BDB_LIB_SUFFIX) {
    macx:BDB_LIB_SUFFIX = -4.8
}

isEmpty(BDB_INCLUDE_PATH) {
    macx:BDB_INCLUDE_PATH = /opt/local/include/db48
}

isEmpty(BOOST_LIB_PATH) {
    macx:BOOST_LIB_PATH = /opt/local/lib
}

isEmpty(BOOST_INCLUDE_PATH) {
    macx:BOOST_INCLUDE_PATH = /opt/local/include
}

windows:DEFINES += WIN32
windows:RC_FILE = src/qt/res/dynamic-qt.rc

windows:!contains(MINGW_THREAD_BUGFIX, 0) {
    # At least qmake's win32-g++-cross profile is missing the -lmingwthrd
    # thread-safety flag. GCC has -mthreads to enable this, but it doesn't
    # work with static linking. -lmingwthrd must come BEFORE -lmingw, so
    # it is prepended to QMAKE_LIBS_QT_ENTRY.
    # It can be turned off with MINGW_THREAD_BUGFIX=0, just in case it causes
    # any problems on some untested qmake profile now or in the future.
    DEFINES += _MT BOOST_THREAD_PROVIDES_GENERIC_SHARED_MUTEX_ON_WIN
    QMAKE_LIBS_QT_ENTRY = -lmingwthrd $$QMAKE_LIBS_QT_ENTRY
}

macx:HEADERS += src/qt/macdockiconhandler.h
macx:OBJECTIVE_SOURCES += src/qt/macdockiconhandler.mm
macx:LIBS += -framework Foundation -framework ApplicationServices -framework AppKit
macx:DEFINES += MAC_OSX MSG_NOSIGNAL=0
macx:ICON = src/qt/res/icons/dynamic.icns
macx:TARGET = "Crave-Qt"
macx:QMAKE_CFLAGS_THREAD += -pthread
macx:QMAKE_LFLAGS_THREAD += -pthread
macx:QMAKE_CXXFLAGS_THREAD += -pthread
macx:QMAKE_INFO_PLIST = share/qt/Info.plist

# Set libraries and includes at end, to use platform-defined defaults if not overridden
INCLUDEPATH += $$BOOST_INCLUDE_PATH $$BDB_INCLUDE_PATH $$OPENSSL_INCLUDE_PATH $$QRENCODE_INCLUDE_PATH
INCLUDEPATH += $$SECP256K1_INCLUDE_PATH
LIBS +=  $$join(BOOST_LIB_PATH,,-L,) $$join(BDB_LIB_PATH,,-L,) $$join(OPENSSL_LIB_PATH,,-L,) $$join(QRENCODE_LIB_PATH,,-L,)
LIBS += -lssl -lcrypto -ldb_cxx$$BDB_LIB_SUFFIX
LIBS += $$join(SECP256K1_LIB_PATH,,-L,)
# -lgdi32 has to happen after -lcrypto (see  #681)
windows:LIBS += -lws2_32 -lshlwapi -lmswsock -lole32 -loleaut32 -luuid -lgdi32

LIBS += -lboost_system$$BOOST_LIB_SUFFIX -lboost_filesystem$$BOOST_LIB_SUFFIX -lboost_program_options$$BOOST_LIB_SUFFIX -lboost_thread$$BOOST_THREAD_LIB_SUFFIX
windows:LIBS += -lboost_chrono$$BOOST_LIB_SUFFIX

contains(RELEASE, 1) {
    !windows:!macx {
        # Linux: turn dynamic linking back on for c/c++ runtime libraries
        LIBS += -Wl,-Bdynamic
    }
}

!windows:!macx {
    DEFINES += LINUX
    LIBS += -lrt -ldl
}

system($$QMAKE_LRELEASE -silent $$_PRO_FILE_)

HEADERS += \
    src/qt/paymentrequest.pb.h \ 
    src/qt/addeditdynode.h \
    src/qt/addressbookpage.h \
    src/qt/addresstablemodel.h \
    src/qt/askpassphrasedialog.h \
    src/qt/clientmodel.h \
    src/qt/coincontroldialog.h \
    src/qt/coincontroltreewidget.h \
    src/qt/csvmodelwriter.h \
    src/qt/dynamicaddressvalidator.h \
    src/qt/dynamicamountfield.h \
    src/qt/dynamicgui.h \
    src/qt/dynamicunits.h \
    src/qt/dnspage.h \
    src/qt/editaddressdialog.h \
    src/qt/guiconstants.h \
    src/qt/guiutil.h \
    src/qt/intro.h \
    src/qt/macnotificationhandler.h \
    src/qt/monitoreddatamapper.h \
    src/qt/multisigaddressentry.h \
    src/qt/multisigdialog.h \
    src/qt/multisiginputentry.h \
    src/qt/nametablemodel.h \
    src/qt/networkstyle.h \
    src/qt/notificator.h \
    src/qt/openuridialog.h \
    src/qt/optionsdialog.h \
    src/qt/optionsmodel.h \
    src/qt/overviewpage.h \
    src/qt/paymentrequestplus.h \
    src/qt/paymentserver.h \
    src/qt/peertablemodel.h \
    src/qt/qvalidatedlineedit.h \
    src/qt/qvaluecombobox.h \
    src/qt/receivecoinsdialog.h \
    src/qt/receiverequestdialog.h \
    src/qt/recentrequeststablemodel.h \
    src/qt/rpcconsole.h \
    src/qt/privatesendconfig.h \
    src/qt/sendcoinsdialog.h \
    src/qt/sendcoinsentry.h \
    src/qt/signverifymessagedialog.h \
    src/qt/splashscreen.h \
    src/qt/dynodeconfigdialog.h \
    src/qt/dynodemanager.h \
    src/qt/trafficgraphwidget.h \
    src/qt/transactiondesc.h \
    src/qt/transactiondescdialog.h \
    src/qt/transactionfilterproxy.h \
    src/qt/transactionrecord.h \
    src/qt/transactiontablemodel.h \
    src/qt/transactionview.h \
    src/qt/utilitydialog.h \
    src/qt/walletframe.h \
    src/qt/walletmodel.h \
    src/qt/walletmodeltransaction.h \
    src/qt/walletview.h \
    src/qt/winshutdownmonitor.h \
    src/compat/sanity.h \
    src/consensus/consensus.h \
    src/consensus/merkle.h \
    src/consensus/params.h \
    src/consensus/validation.h \
    src/crypto/aes.h \
    src/crypto/common.h \
    src/crypto/hmac_sha256.h \
    src/crypto/hmac_sha512.h \
    src/crypto/rfc6979_hmac_sha256.h \
    src/crypto/ripemd160.h \
    src/crypto/sha1.h \
    src/crypto/sha256.h \
    src/crypto/sha512.h \
    src/crypto/argon2d/thread.h \
    src/crypto/argon2d/ref.h \
    src/crypto/argon2d/opt.h \
    src/crypto/argon2d/encoding.h \
    src/crypto/argon2d/core.h \
    src/crypto/argon2d/argon2.h \
    src/crypto/blake2/blamka-round-ref.h \
    src/crypto/blake2/blamka-round-opt.h \
    src/crypto/blake2/blake2.h \
    src/crypto/blake2/blake2-impl.h \
    src/incognito/instantx.h \
    src/incognito/privatesend/privatesend-relay.h \
    src/incognito/privatesend/privatesend.h \
    src/incognito/dns/dyndns.h \
    src/incognito/dns/dyndns.h \
    src/incognito/dns/hooks.h \
    src/primitives/block.h \
    src/primitives/transaction.h \
    src/rpc/rpcclient.h \
    src/rpc/rpcprotocol.h \
    src/rpc/rpcregister.h \
    src/rpc/rpcserver.h \
    src/script/dynamicconsensus.h \
    src/script/interpreter.h \
    src/script/script_error.h \
    src/script/script.h \
    src/script/sigcache.h \
    src/script/sign.h \
    src/script/standard.h \
    src/dynode/dynodeman.h \
    src/dynode/dynodeconfig.h \
    src/dynode/dynode.h \
    src/dynode/dynode-sync.h \
    src/dynode/dynode-payments.h \
    src/dynode/dynode-budget.h \
    src/dynode/spork.h \
    src/dynode/activedynode.h \
    src/wallet/crypter.h \
    src/wallet/db.h \
    src/wallet/wallet_ismine.h \
    src/wallet/wallet.h \
    src/wallet/walletdb.h \
    src/addrman.h \
    src/alert.h \
    src/allocators.h \
    src/amount.h \
    src/base58.h \
    src/bloom.h \
    src/chain.h \
    src/chainparams.h \
    src/chainparamsbase.h \
    src/chainparamsseeds.h \
    src/checkpoints.h \
    src/checkqueue.h \
    src/clientversion.h \
    src/coincontrol.h \
    src/coins.h \
    src/compat.h \
    src/compressor.h \
    src/core_io.h \
    src/eccryptoverify.h \
    src/ecwrapper.h \
    src/hash.h \
    src/init.h \
    src/keepass.h \
    src/key.h \
    src/keystore.h \
    src/leveldbwrapper.h \
    src/limitedmap.h \
    src/main.h \
    src/merkleblock.h \
    src/miner.h \
    src/mruset.h \
    src/net.h \
    src/netbase.h \
    src/noui.h \
    src/pbkdf2.h \
    src/pow.h \
    src/protocol.h \
    src/pubkey.h \
    src/random.h \
    src/serialize.h \
    src/streams.h \
    src/sync.h \
    src/threadsafety.h \
    src/timedata.h \
    src/tinyformat.h \
    src/txdb.h \
    src/txmempool.h \
    src/ui_interface.h \
    src/uint256.h \
    src/undo.h \
    src/util.h \
    src/utilmoneystr.h \
    src/utilstrencodings.h \
    src/utiltime.h \
    src/version.h


SOURCES += \
    src/qt/addeditdynode.cpp \
    src/qt/addressbookpage.cpp \
    src/qt/addresstablemodel.cpp \
    src/qt/askpassphrasedialog.cpp \
    src/qt/clientmodel.cpp \
    src/qt/coincontroldialog.cpp \
    src/qt/coincontroltreewidget.cpp \
    src/qt/csvmodelwriter.cpp \
    src/qt/dynamic.cpp \
    src/qt/dynamicaddressvalidator.cpp \
    src/qt/dynamicamountfield.cpp \
    src/qt/dynamicgui.cpp \
    src/qt/dynamicstrings.cpp \
    src/qt/dynamicunits.cpp \
    src/qt/dnspage.cpp \
    src/qt/editaddressdialog.cpp \
    src/qt/guiutil.cpp \
    src/qt/intro.cpp \
    src/qt/monitoreddatamapper.cpp \
    src/qt/multisigaddressentry.cpp \
    src/qt/multisigdialog.cpp \
    src/qt/multisiginputentry.cpp \
    src/qt/nametablemodel.cpp \
    src/qt/networkstyle.cpp \
    src/qt/notificator.cpp \
    src/qt/openuridialog.cpp \
    src/qt/optionsdialog.cpp \
    src/qt/optionsmodel.cpp \
    src/qt/overviewpage.cpp \
    src/qt/paymentrequestplus.cpp \
    src/qt/paymentserver.cpp \
    src/qt/peertablemodel.cpp \
    src/qt/qvalidatedlineedit.cpp \
    src/qt/qvaluecombobox.cpp \
    src/qt/receivecoinsdialog.cpp \
    src/qt/receiverequestdialog.cpp \
    src/qt/recentrequeststablemodel.cpp \
    src/qt/rpcconsole.cpp \
    src/qt/privatesendconfig.cpp \
    src/qt/sendcoinsdialog.cpp \
    src/qt/sendcoinsentry.cpp \
    src/qt/signverifymessagedialog.cpp \
    src/qt/splashscreen.cpp \
    src/qt/dynodeconfigdialog.cpp \
    src/qt/dynodemanager.cpp \
    src/qt/trafficgraphwidget.cpp \
    src/qt/transactiondesc.cpp \
    src/qt/transactiondescdialog.cpp \
    src/qt/transactionfilterproxy.cpp \
    src/qt/transactionrecord.cpp \
    src/qt/transactiontablemodel.cpp \
    src/qt/transactionview.cpp \
    src/qt/utilitydialog.cpp \
    src/qt/walletframe.cpp \
    src/qt/walletmodel.cpp \
    src/qt/walletmodeltransaction.cpp \
    src/qt/walletview.cpp \
    src/qt/winshutdownmonitor.cpp \
    src/compat/strnlen.cpp \
    src/consensus/merkle.cpp \
    src/crypto/aes.cpp \
    src/crypto/hmac_sha256.cpp \
    src/crypto/hmac_sha512.cpp \
    src/crypto/rfc6979_hmac_sha256.cpp \
    src/crypto/ripemd160.cpp \
    src/crypto/sha1.cpp \
    src/crypto/sha256.cpp \
    src/crypto/sha512.cpp \
    src/crypto/argon2d/thread.c \
    src/crypto/argon2d/ref.c \
    src/crypto/argon2d/encoding.c \
    src/crypto/argon2d/core.c \
    src/crypto/argon2d/argon2.c \
    src/crypto/blake2/blake2b.c \
    src/incognito/instantx.cpp \
    src/incognito/privatesend/privatesend-relay.cpp \
    src/incognito/privatesend/privatesend.cpp \
    src/incognito/dns/dns.cpp \
    src/incognito/dns/dyndns.cpp \
    src/primitives/block.cpp \
    src/primitives/transaction.cpp \
    src/rpc/rpcblockchain.cpp \
    src/rpc/rpcclient.cpp \
    src/rpc/rpcdump.cpp \
    src/rpc/rpcmining.cpp \
    src/rpc/rpcmisc.cpp \
    src/rpc/rpcnet.cpp \
    src/rpc/rpcprotocol.cpp \
    src/rpc/rpcrawtransaction.cpp \
    src/rpc/rpcserver.cpp \
    src/rpc/rpcdynode-budget.cpp \
    src/rpc/rpcdynode.cpp \
    src/rpc/rpcwallet.cpp \
    src/script/dynamicconsensus.cpp \
    src/script/interpreter.cpp \
    src/script/script_error.cpp \
    src/script/script.cpp \
    src/script/sigcache.cpp \
    src/script/sign.cpp \
    src/script/standard.cpp \
    src/dynode/dynodeman.cpp \
    src/dynode/dynodeconfig.cpp \
    src/dynode/dynode.cpp \
    src/dynode/dynode-sync.cpp \
    src/dynode/dynode-payments.cpp \
    src/dynode/dynode-budget.cpp \
    src/dynode/spork.cpp \
    src/dynode/activedynode.cpp \
    src/wallet/crypter.cpp \
    src/wallet/db.cpp \
    src/wallet/wallet_ismine.cpp \
    src/wallet/wallet.cpp \
    src/wallet/walletdb.cpp \
    src/addrman.cpp \
    src/alert.cpp \
    src/allocators.cpp \
    src/amount.cpp \
    src/base58.cpp \
    src/bloom.cpp \
    src/chain.cpp \
    src/chainparams.cpp \
    src/chainparamsbase.cpp \
    src/checkpoints.cpp \
    src/clientversion.cpp \
    src/coins.cpp \
    src/compressor.cpp \
    src/core_read.cpp \
    src/core_write.cpp \
    src/eccryptoverify.cpp \
    src/ecwrapper.cpp \
    src/hash.cpp \
    src/init.cpp \
    src/keepass.cpp \
    src/key.cpp \
    src/keystore.cpp \
    src/leveldbwrapper.cpp \
    src/main.cpp \
    src/merkleblock.cpp \
    src/miner.cpp \
    src/net.cpp \
    src/netbase.cpp \
    src/noui.cpp \
    src/pbkdf2.cpp \
    src/pow.cpp \
    src/protocol.cpp \
    src/pubkey.cpp \
    src/random.cpp \
    src/rest.cpp \
    src/sync.cpp \
    src/timedata.cpp \
    src/txdb.cpp \
    src/txmempool.cpp \
    src/uint256.cpp \
    src/util.cpp \
    src/utilmoneystr.cpp \
    src/utilstrencodings.cpp \
    src/utiltime.cpp \
    src/compat/glibcxx_sanity.cpp \
    src/compat/glibc_sanity.cpp \
    src/qt/paymentrequest.pb.cc


OTHER_FILES += \
    Makefile.am \
    configure.ac \
    src/Makefile.am

FORMS += \
    src/qt/forms/addeditdynode.ui \
    src/qt/forms/addressbookpage.ui \
    src/qt/forms/askpassphrasedialog.ui \
    src/qt/forms/coincontroldialog.ui \
    src/qt/forms/editaddressdialog.ui \
    src/qt/forms/helpmessagedialog.ui \
    src/qt/forms/intro.ui \
    src/qt/forms/openuridialog.ui \
    src/qt/forms/optionsdialog.ui \
    src/qt/forms/overviewpage.ui \
    src/qt/forms/receivecoinsdialog.ui \
    src/qt/forms/receiverequestdialog.ui \
    src/qt/forms/rpcconsole.ui \
    src/qt/forms/privatesendconfig.ui \
    src/qt/forms/sendcoinsdialog.ui \
    src/qt/forms/sendcoinsentry.ui \
    src/qt/forms/signverifymessagedialog.ui \
    src/qt/forms/dynodeconfigdialog.ui \
    src/qt/forms/dynodemanager.ui \
    src/qt/forms/transactiondescdialog.ui \ 
    src/qt/forms/dnspage.ui \
    src/qt/forms/multisigaddressentry.ui \
    src/qt/forms/multisigdialog.ui \
    src/qt/forms/multisiginputentry.ui

RESOURCES += \
    src/qt/dynamic.qrc \
    src/qt/dynamic_locale.qrc


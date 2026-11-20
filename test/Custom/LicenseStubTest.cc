#include "LicenseStubTest.h"

#include "LicenseManager.h"

#include <QtCore/QSettings>
#include <QtCore/QUrl>
#include <QtTest/QTest>

namespace {

const QString kTestKey = QStringLiteral("GC-TEST-0001");
constexpr int kReplyMs = 15000;

bool recordStored()
{
    return QSettings().contains(QStringLiteral("svc/sr"));
}

} // namespace

/// Waits for the request in flight to finish (fails the test on timeout).
#define SETTLE(lm) QTRY_VERIFY_WITH_TIMEOUT(!(lm).busy(), kReplyMs)

void LicenseStubTest::_clearSettings()
{
    QSettings s;
    for (const char *k : {"svc/sr", "svc/lk", "svc/rk", "svc/hw", "svc/mg", "svc/sv"}) {
        s.remove(QLatin1String(k));
    }
    s.sync();
}

void LicenseStubTest::initTestCase()
{
#ifndef QGC_DAILY_BUILD
    QSKIP("A stable build has no test-key hook: LicenseStubTest runs in Daily builds only.");
#else
    const QByteArray base = qgetenv("ROTORLAB_BASE_URL");
    const QByteArray keys = qgetenv("ROTORLAB_LICENSE_PUBKEYS");
    if (base.isEmpty() || keys.isEmpty()) {
        QSKIP("Needs a local signing test stub, with ROTORLAB_BASE_URL and "
              "ROTORLAB_LICENSE_PUBKEYS set to it.");
    }
    // Loopback only: this test never sends a license key anywhere else.
    const QUrl url(QString::fromUtf8(base));
    QVERIFY2(url.host() == QLatin1String("127.0.0.1"),
             "ROTORLAB_BASE_URL must be the signing stub on 127.0.0.1");
#endif
}

void LicenseStubTest::init()
{
    UnitTest::init();
    _clearSettings();
}

void LicenseStubTest::cleanup()
{
    _clearSettings();
    UnitTest::cleanup();
}

void LicenseStubTest::_activateThroughGcFlow()
{
    LicenseManager lm;
    QVERIFY(!lm.activated());
    lm.activate(kTestKey);
    SETTLE(lm);
    QVERIFY2(lm.activated(), qPrintable(lm.statusText()));
    QVERIFY2(lm.statusText().contains(QStringLiteral("can open without a connection until")),
             qPrintable(lm.statusText()));
    QVERIFY(recordStored());
}

void LicenseStubTest::_nextStartOpensOfflineFromSignedRecord()
{
    {
        LicenseManager lm;
        lm.activate(kTestKey);
        SETTLE(lm);
        QVERIFY2(lm.activated(), qPrintable(lm.statusText()));
    }
    // A new start reads the stored record and checks its signature against
    // the stub's public key, with no request sent.
    LicenseManager next;
    QVERIFY(!next.busy());
    QVERIFY2(next.activated(), qPrintable(next.statusText()));
    QVERIFY(next.statusText().startsWith(QStringLiteral("Activated.")));
}

void LicenseStubTest::_recheckRefreshesRecord()
{
    LicenseManager lm;
    lm.activate(kTestKey);
    SETTLE(lm);
    QVERIFY(lm.activated());
    const QByteArray first = QSettings().value(QStringLiteral("svc/sr")).toByteArray();

    lm.recheck();
    SETTLE(lm);
    QVERIFY2(lm.activated(), qPrintable(lm.statusText()));
    QVERIFY2(lm.statusText().startsWith(QStringLiteral("License valid")), qPrintable(lm.statusText()));
    const QByteArray second = QSettings().value(QStringLiteral("svc/sr")).toByteArray();
    QVERIFY(!second.isEmpty());
    QVERIFY(second != first);   // a fresh record: new nonce
}

void LicenseStubTest::_unknownKeyRefused()
{
    LicenseManager lm;
    lm.activate(QStringLiteral("RLGC-ABCDE-23456-FGHJK-789AB"));
    SETTLE(lm);
    QVERIFY(!lm.activated());
    QVERIFY(!recordStored());
    QCOMPARE(lm.statusText(), QStringLiteral("That is not a license key RotorLab issued."));
}

void LicenseStubTest::_deactivateClearsRecord()
{
    LicenseManager lm;
    lm.activate(kTestKey);
    SETTLE(lm);
    QVERIFY(lm.activated());
    lm.deactivate();
    SETTLE(lm);
    QVERIFY(!lm.activated());
    QVERIFY(!recordStored());

    LicenseManager next;
    QVERIFY(!next.activated());
}

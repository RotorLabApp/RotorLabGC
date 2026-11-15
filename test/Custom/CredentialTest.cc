#include "CredentialTest.h"

#include "Credential.h"

#include <QtCore/QSettings>
#include <QtCore/QUuid>
#include <QtTest/QTest>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif

namespace {

QString testAccount()
{
    return QStringLiteral("unittest-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
}

const QString kSecret = QStringLiteral("rl_unittest_0123456789abcdef");

bool armed()    { return true; }
bool disarmed() { return false; }

/// Put a key straight into the platform store, behind Credential's back, as
/// an earlier run (or another build) would have left it.
void writeBehind(const QString &account, const QString &secret)
{
#ifdef Q_OS_WIN
    const QString target = QStringLiteral("RotorLab GC \u2014 %1").arg(account);
    QByteArray blob = secret.toUtf8();
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(target.utf16()));
    cred.CredentialBlobSize = static_cast<DWORD>(blob.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    QVERIFY(::CredWriteW(&cred, 0));
#else
    QSettings s;
    s.setValue(QStringLiteral("RotorLabCredential/") + account, secret.toUtf8().toBase64());
    s.sync();
#endif
}

bool presentBehind(const QString &account)
{
#ifdef Q_OS_WIN
    const QString target = QStringLiteral("RotorLab GC \u2014 %1").arg(account);
    PCREDENTIALW cred = nullptr;
    if (!::CredReadW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC, 0, &cred)) {
        return false;
    }
    ::CredFree(cred);
    return true;
#else
    Credential::dropCache();
    return !Credential::load(account).isEmpty();
#endif
}

} // namespace

void CredentialTest::_roundTrip()
{
    const QString account = testAccount();
    QVERIFY(Credential::load(account).isEmpty());
    QVERIFY(Credential::store(account, kSecret));
    QCOMPARE(Credential::load(account), kSecret);
    QVERIFY(Credential::store(account, kSecret + QStringLiteral("2")));
    QCOMPARE(Credential::load(account), kSecret + QStringLiteral("2"));
    Credential::erase(account);
    QVERIFY(Credential::load(account).isEmpty());
    Credential::erase(account);   // twice is fine

    // In a real store the key is not in the settings file at all.
    if (Credential::inOsStore()) {
        QVERIFY(Credential::store(account, kSecret));
        QSettings s;
        QVERIFY(!s.contains(QStringLiteral("RotorLabCredential/") + account));
        Credential::erase(account);
    }
}

void CredentialTest::_storeNameIsPlain()
{
    const QString name = Credential::storeName();
    QVERIFY(!name.isEmpty());
#if defined(Q_OS_WIN)
    QCOMPARE(name, QStringLiteral("Windows Credential Manager"));
    QVERIFY(Credential::inOsStore());
#elif defined(Q_OS_ANDROID)
    QVERIFY(name.contains(QStringLiteral("Android Keystore")));
#else
    // Either a real store, or a name that says the key is not encrypted.
    if (Credential::inOsStore()) {
        QVERIFY(name.contains(QStringLiteral("Secret Service")));
    } else {
        QVERIFY(name.contains(QStringLiteral("not encrypted")));
    }
#endif
}

void CredentialTest::_windowsTargetNameKept()
{
#ifdef Q_OS_WIN
    // Keys already saved on Windows installs live under this exact name.
    const QString account = testAccount();
    QVERIFY(Credential::store(account, kSecret));
    const QString target = QStringLiteral("RotorLab GC \u2014 %1").arg(account);
    PCREDENTIALW cred = nullptr;
    QVERIFY(::CredReadW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC, 0, &cred));
    ::CredFree(cred);
    Credential::erase(account);
#else
    QSKIP("Windows only");
#endif
}

void CredentialTest::_legacyValueMovedOnce()
{
#ifdef Q_OS_WIN
    QSKIP("Windows never used the settings value");
#else
    const QString account = testAccount();
    const QString legacyKey = QStringLiteral("RotorLabCredential/") + account;
    {
        QSettings s;
        s.setValue(legacyKey, kSecret.toUtf8().toBase64());
        s.sync();
    }
    QCOMPARE(Credential::load(account), kSecret);
    QSettings s;
    s.sync();
    if (Credential::inOsStore()) {
        QVERIFY(!s.contains(legacyKey));                // plain copy gone
        Credential::dropCache();
        QCOMPARE(Credential::load(account), kSecret);   // the store has it
    } else {
        QVERIFY(s.contains(legacyKey));                 // still the store
    }
    Credential::erase(account);
    s.sync();
    QVERIFY(!s.contains(legacyKey));
    QVERIFY(Credential::load(account).isEmpty());
#endif
}

void CredentialTest::_noStoreReadWhileArmed()
{
    // F1: a key not yet in memory is not read from the store while a vehicle
    // is armed; after landing it is.
    const QString account = testAccount();
    writeBehind(account, kSecret);
    Credential::dropCache();

    Credential::setArmedProbe(&armed);
    QVERIFY(Credential::load(account).isEmpty());
    QVERIFY(!Credential::store(account, kSecret + QStringLiteral("x")));   // no save either
    Credential::setArmedProbe(&disarmed);
    QCOMPARE(Credential::load(account), kSecret);

    // Once read, it comes from memory, armed or not.
    Credential::setArmedProbe(&armed);
    QCOMPARE(Credential::load(account), kSecret);
    Credential::setArmedProbe(nullptr);
    Credential::erase(account);
}

void CredentialTest::_eraseWhileArmedHeldBack()
{
    const QString account = testAccount();
    QVERIFY(Credential::store(account, kSecret));

    Credential::setArmedProbe(&armed);
    Credential::erase(account);
    QVERIFY(Credential::load(account).isEmpty());   // gone for this run at once
#ifdef Q_OS_WIN
    QVERIFY(presentBehind(account));                // store not touched yet
#endif
    Credential::setArmedProbe(&disarmed);
    QVERIFY(Credential::load(account).isEmpty());   // first unarmed call cleans up
    QVERIFY(!presentBehind(account));
    Credential::setArmedProbe(nullptr);
}

void CredentialTest::_fileValueWinsAfterCrash()
{
#ifdef Q_OS_WIN
    QSKIP("Windows has no settings-file copy");
#else
    // F2: a run stopped between writing the store and erasing the file leaves
    // both; a key saved to the file while no Secret Service ran is newer than
    // the keyring copy. Either way the file value wins and is moved.
    const QString account = testAccount();
    const QString legacyKey = QStringLiteral("RotorLabCredential/") + account;
    QVERIFY(Credential::store(account, kSecret));                 // old copy
    writeBehind(account, kSecret + QStringLiteral("new"));       // newer, in the file
    Credential::dropCache();

    QCOMPARE(Credential::load(account), kSecret + QStringLiteral("new"));
    QSettings s;
    s.sync();
    if (Credential::inOsStore()) {
        QVERIFY(!s.contains(legacyKey));
        Credential::dropCache();
        QCOMPARE(Credential::load(account), kSecret + QStringLiteral("new"));
    }

    // Same value in both (the crash case): still one copy afterwards.
    if (Credential::inOsStore()) {
        writeBehind(account, kSecret + QStringLiteral("new"));
        Credential::dropCache();
        QCOMPARE(Credential::load(account), kSecret + QStringLiteral("new"));
        s.sync();
        QVERIFY(!s.contains(legacyKey));
    }
    Credential::erase(account);
#endif
}

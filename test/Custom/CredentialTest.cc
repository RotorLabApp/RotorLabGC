#include "CredentialTest.h"

#include "Credential.h"
#include "RotorLabConnector.h"

#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QUuid>
#include <QtTest/QTest>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif

namespace {

/// G105: every account a test makes, so cleanup() removes what it left in
/// the OS store or the settings file even when a check failed half way.
QStringList &createdAccounts()
{
    static QStringList list;
    return list;
}

QString track(const QString &account)
{
    createdAccounts().append(account);
    return account;
}

/// The test flag as the run started, read once before any test changes it.
const QByteArray &testFlagAtStart()
{
    static const QByteArray v = qEnvironmentVariableIsSet("RL_TEST_NO_CREDENTIAL_STORE")
                                    ? qgetenv("RL_TEST_NO_CREDENTIAL_STORE")
                                    : QByteArray();
    return v;
}

QString testAccount()
{
    (void)testFlagAtStart();   // read before any test sets or clears it
    return track(QStringLiteral("unittest-%1").arg(QUuid::createUuid().toString(QUuid::Id128)));
}

const QString kSecret = QStringLiteral("rl_unittest_0123456789abcdef");

bool armed()    { return true; }
bool disarmed() { return false; }

#ifdef Q_OS_WIN
/// The Credential Manager target name GC uses now (plain hyphen).
QString newTarget(const QString &account)
{
    return QStringLiteral("RotorLab GC - %1").arg(account);
}

/// The name GC 5.1.x and earlier used (em dash, U+2014).
QString oldTarget(const QString &account)
{
    return QStringLiteral("RotorLab GC ") + QChar(0x2014) + QStringLiteral(" ") + account;
}

void writeTarget(const QString &target, const QString &secret)
{
    QByteArray blob = secret.toUtf8();
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(target.utf16()));
    cred.CredentialBlobSize = static_cast<DWORD>(blob.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    QVERIFY(::CredWriteW(&cred, 0));
}

/// What `target` holds; `found` says whether it exists.
QString readTarget(const QString &target, bool *found)
{
    PCREDENTIALW cred = nullptr;
    *found = ::CredReadW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC, 0, &cred);
    if (!*found) {
        return {};
    }
    const QString out = QString::fromUtf8(reinterpret_cast<const char *>(cred->CredentialBlob),
                                          static_cast<int>(cred->CredentialBlobSize));
    ::CredFree(cred);
    return out;
}

void deleteTarget(const QString &target)
{
    (void)::CredDeleteW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC, 0);
}
#endif

/// Put a key straight into the platform store, behind Credential's back, as
/// an earlier run (or another build) would have left it.
void writeBehind(const QString &account, const QString &secret)
{
#ifdef Q_OS_WIN
    writeTarget(newTarget(account), secret);
#else
    QSettings s;
    s.setValue(QStringLiteral("RotorLabCredential/") + account, secret.toUtf8().toBase64());
    s.sync();
#endif
}

bool presentBehind(const QString &account)
{
#ifdef Q_OS_WIN
    bool found = false;
    readTarget(newTarget(account), &found);
    return found;
#else
    Credential::dropCache();
    return !Credential::load(account).isEmpty();
#endif
}

} // namespace

void CredentialTest::_roundTrip()
{
    const QString account = testAccount();
    if (!Credential::inOsStore()) {
        // G82: no store, nothing saved (see _noStoreNoFileWrite).
        QVERIFY(!Credential::store(account, kSecret));
        QVERIFY(Credential::load(account).isEmpty());
        return;
    }
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
    // Either a real store, or a name that says nothing is saved (G82).
    if (Credential::inOsStore()) {
        QVERIFY(name.contains(QStringLiteral("Secret Service")));
    } else {
        QVERIFY(name.startsWith(QStringLiteral("this session only")));
    }
#endif
}

void CredentialTest::_windowsTargetName()
{
#ifdef Q_OS_WIN
    // G81: saved under a plain hyphen, never under the old em-dash name.
    const QString account = testAccount();
    QVERIFY(Credential::store(account, kSecret));
    bool found = false;
    QCOMPARE(readTarget(newTarget(account), &found), kSecret);
    QVERIFY(found);
    readTarget(oldTarget(account), &found);
    QVERIFY(!found);
    QVERIFY(!newTarget(account).contains(QChar(0x2014)));
    Credential::erase(account);
    readTarget(newTarget(account), &found);
    QVERIFY(!found);
#else
    QSKIP("Windows only");
#endif
}

void CredentialTest::_windowsOldTargetMigrated()
{
#ifdef Q_OS_WIN
    // G81: a key saved by GC 5.1.x under the em-dash name is read, written
    // under the new name, and the old entry deleted after that write.
    const QString account = testAccount();
    writeTarget(oldTarget(account), kSecret);
    Credential::dropCache();
    QCOMPARE(Credential::load(account), kSecret);
    bool found = false;
    QCOMPARE(readTarget(newTarget(account), &found), kSecret);
    QVERIFY(found);
    readTarget(oldTarget(account), &found);
    QVERIFY(!found);
    // The next start reads the new entry.
    Credential::dropCache();
    QCOMPARE(Credential::load(account), kSecret);

    // Both present (a run stopped between the write and the delete): the new
    // entry wins and the old one is removed.
    writeTarget(newTarget(account), kSecret + QStringLiteral("new"));
    writeTarget(oldTarget(account), kSecret);
    Credential::dropCache();
    QCOMPARE(Credential::load(account), kSecret + QStringLiteral("new"));
    readTarget(oldTarget(account), &found);
    QVERIFY(!found);

    // A save also clears an old entry, and an erase clears both names.
    writeTarget(oldTarget(account), kSecret);
    QVERIFY(Credential::store(account, kSecret + QStringLiteral("2")));
    readTarget(oldTarget(account), &found);
    QVERIFY(!found);
    writeTarget(oldTarget(account), kSecret);
    Credential::erase(account);
    readTarget(newTarget(account), &found);
    QVERIFY(!found);
    readTarget(oldTarget(account), &found);
    QVERIFY(!found);
    // Nothing this test made is left behind, even after a failed check above.
    deleteTarget(newTarget(account));
    deleteTarget(oldTarget(account));
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
    QCOMPARE(Credential::load(account), Credential::inOsStore() ? kSecret : QString());
    QSettings s;
    s.sync();
    if (Credential::inOsStore()) {
        QVERIFY(!s.contains(legacyKey));                // plain copy gone
        Credential::dropCache();
        QCOMPARE(Credential::load(account), kSecret);   // the store has it
    } else {
        QVERIFY(!s.contains(legacyKey));                // G82: removed, not used
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
    if (!Credential::inOsStore()) {
        QSKIP("needs a system key store");
    }
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
    if (!Credential::inOsStore()) {
        QSKIP("needs a system key store");
    }
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
    if (!Credential::inOsStore()) {
        QSKIP("needs a system key store");
    }
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

void CredentialTest::_entryLabels()
{
    // The TAK password entry (and its unit-test account) reads as such.
    QCOMPARE(Credential::entryLabel(QStringLiteral("TAK password")), QStringLiteral("RotorLab GC TAK password"));
    QCOMPARE(Credential::entryLabel(QStringLiteral("Unit test TAK password 4242")),
             QStringLiteral("RotorLab GC TAK password"));
    // Device Key entries keep their label.
    QCOMPARE(Credential::entryLabel(QStringLiteral("Device Key")),
             QStringLiteral("RotorLab GC Device Key (Device Key)"));
    QCOMPARE(Credential::entryLabel(QStringLiteral("Unit test Device Key 4242")),
             QStringLiteral("RotorLab GC Device Key (Unit test Device Key 4242)"));
    // G81: no label the keyring shows has an em dash.
    for (const QString &a : {QStringLiteral("TAK password"), QStringLiteral("Device Key"),
                             QStringLiteral("Unit test Device Key 4242")}) {
        QVERIFY(!Credential::entryLabel(a).contains(QChar(0x2014)));
    }
    // Lookup is unchanged: a TAK password stored and read back by account.
    if (!Credential::inOsStore()) {
        return;
    }
    const QString account = track(QStringLiteral("Unit test TAK password %1").arg(QUuid::createUuid().toString(QUuid::Id128)));
    QVERIFY(Credential::store(account, kSecret));
    Credential::dropCache();
    QCOMPARE(Credential::load(account), kSecret);
    Credential::erase(account);
    Credential::dropCache();
    QVERIFY(Credential::load(account).isEmpty());
}

namespace {

/// Behaves as Linux with no Secret Service for the life of the object.
struct NoOsStore {
    NoOsStore()  { Credential::setNoOsStoreForTests(true); }
    ~NoOsStore() { Credential::setNoOsStoreForTests(false); }
};

bool inSettingsFile(const QString &account)
{
    QSettings s;
    s.sync();
    return s.contains(QStringLiteral("RotorLabCredential/") + account);
}

} // namespace

void CredentialTest::_noStoreNoFileWrite()
{
    // G82: with no system key store the Device Key is never written to the
    // settings file. store() refuses; keepForSession() keeps it in memory.
    const QString account = testAccount();
    {
        NoOsStore none;
        QVERIFY(!Credential::inOsStore());
        QVERIFY(Credential::storeName().startsWith(QStringLiteral("this session only")));
        QVERIFY(!Credential::store(account, kSecret));
        QVERIFY(!inSettingsFile(account));
        QVERIFY(Credential::load(account).isEmpty());

        QVERIFY(Credential::keepForSession(account, kSecret));
        QCOMPARE(Credential::load(account), kSecret);   // usable this session
        QVERIFY(!inSettingsFile(account));
        QCOMPARE(Credential::fileCopy(account), Credential::FileCopy::None);

        Credential::dropCache();                         // a restart
        QVERIFY(Credential::load(account).isEmpty());   // nothing was saved
        QVERIFY(!inSettingsFile(account));

        QVERIFY(Credential::keepForSession(account, kSecret));
        Credential::erase(account);
        QVERIFY(Credential::load(account).isEmpty());
    }
#ifdef Q_OS_WIN
    // Nothing reached Credential Manager either.
    bool found = false;
    readTarget(newTarget(account), &found);
    QVERIFY(!found);
#endif
}

void CredentialTest::_noStoreOldFileCopyRemoved()
{
    // G82: a copy an older build left in the settings file is not used when
    // there is no store to move it to. It is removed from the file and the
    // user is asked to enter the key again.
    const QString account = testAccount();
    NoOsStore none;
    {
        QSettings s;
        s.setValue(QStringLiteral("RotorLabCredential/") + account, kSecret.toUtf8().toBase64());
        s.sync();
    }
    QVERIFY(inSettingsFile(account));
    Credential::dropCache();
    QVERIFY(Credential::load(account).isEmpty());        // not used
    QVERIFY(!inSettingsFile(account));                   // and gone
    QCOMPARE(Credential::fileCopy(account), Credential::FileCopy::Removed);
    Credential::dropCache();
    QVERIFY(Credential::load(account).isEmpty());        // not back after a restart

    // Entering it again clears the notice; still nothing in the file.
    QVERIFY(Credential::keepForSession(account, kSecret));
    QCOMPARE(Credential::fileCopy(account), Credential::FileCopy::None);
    QCOMPARE(Credential::load(account), kSecret);
    QVERIFY(!inSettingsFile(account));
    Credential::erase(account);
}

void CredentialTest::_fileCopyRule()
{
    // G82 (Android too): the platform-free rule Linux and Android
    // apply to a key an older build left in the settings file.
    const QString account = testAccount();
    auto plant = [&account] {
        QSettings s;
        s.setValue(QStringLiteral("RotorLabCredential/") + account, kSecret.toUtf8().toBase64());
        s.sync();
    };

    // The store takes it: moved, used, gone from the file.
    plant();
    bool dropped = true;
    QString stored;
    QCOMPARE(Credential::resolveFileCopyForTests(account, true, &dropped, &stored), kSecret);
    QCOMPARE(stored, kSecret);
    QVERIFY(!dropped);
    QVERIFY(!inSettingsFile(account));
    QCOMPARE(Credential::fileCopy(account), Credential::FileCopy::Moved);

    // The store refuses (Android: Keystore encryption fails): removed from the
    // file, not used, the older store copy cleared, the user asked again.
    plant();
    stored.clear();
    QVERIFY(Credential::resolveFileCopyForTests(account, false, &dropped, &stored).isEmpty());
    QVERIFY(stored.isEmpty());
    QVERIFY(dropped);
    QVERIFY(!inSettingsFile(account));
    QCOMPARE(Credential::fileCopy(account), Credential::FileCopy::Removed);
    QCOMPARE(RotorLabConnector::keyStorageTextFor(Credential::fileCopy(account), true, false,
                                                  QStringLiteral("encrypted app storage (Android Keystore)")),
             QStringLiteral("The readable copy of your Device Key that an earlier version kept in the settings file "
                            "has been deleted. It was not used, because encrypted app storage (Android Keystore) did "
                            "not accept it. Enter the key again to save it in encrypted app storage (Android "
                            "Keystore)."));

    // No file copy: nothing happens.
    dropped = false;
    QVERIFY(Credential::resolveFileCopyForTests(account, true, &dropped, &stored).isEmpty());
    QVERIFY(!dropped);
    Credential::erase(account);
    QCOMPARE(Credential::fileCopy(account), Credential::FileCopy::None);
}

void CredentialTest::_refusalText()
{
    // G82: the settings page text, in the TAK password wording style.
    const QString store = QStringLiteral("Secret Service (desktop keyring)");
    const QString sessionOnly = RotorLabConnector::keyStorageTextFor(Credential::FileCopy::None, false, true,
                                                                     QStringLiteral("this session only"));
    QCOMPARE(sessionOnly,
             QStringLiteral("Device Key kept for this session only. GC found no system key store on this device, "
                            "so the key is not saved. Enter it again after a restart to keep sending flight records to RotorLab."));
    const QString removedNoStore = RotorLabConnector::keyStorageTextFor(Credential::FileCopy::Removed, false, false,
                                                                        QStringLiteral("this session only"));
    QCOMPARE(removedNoStore,
             QStringLiteral("The readable copy of your Device Key that an earlier version kept in the settings file "
                            "has been deleted. It was not used. GC found no system key store on this device, so "
                            "enter the key again: GC keeps it for this session only."));
    const QString removedRefused = RotorLabConnector::keyStorageTextFor(Credential::FileCopy::Removed, true, false,
                                                                        store);
    QCOMPARE(removedRefused,
             QStringLiteral("The readable copy of your Device Key that an earlier version kept in the settings file "
                            "has been deleted. It was not used, because Secret Service (desktop keyring) did not "
                            "accept it. Enter the key again to save it in Secret Service (desktop keyring)."));
    const QString moved = RotorLabConnector::keyStorageTextFor(Credential::FileCopy::Moved, true, true, store);
    QCOMPARE(moved,
             QStringLiteral("Device Key saved in Secret Service (desktop keyring). The readable copy that an earlier "
                            "version kept in the settings file has been deleted."));
    // Saved normally, or no key at all: nothing to add to "Stored in".
    QVERIFY(RotorLabConnector::keyStorageTextFor(Credential::FileCopy::None, true, true, store).isEmpty());
    QVERIFY(RotorLabConnector::keyStorageTextFor(Credential::FileCopy::None, false, false, store).isEmpty());
    for (const QString &t : {sessionOnly, removedNoStore, removedRefused, moved}) {
        QVERIFY(!t.contains(QChar(0x2014)));
        QVERIFY(t.contains(QStringLiteral("Device Key")));
    }
    {
        NoOsStore none;
        QVERIFY(!Credential::storeName().contains(QChar(0x2014)));
    }
    QVERIFY(!Credential::storeName().contains(QChar(0x2014)));
}

void CredentialTest::cleanup()
{
    // G105: nothing a test made stays behind, whatever the test did. The test
    // flag is put back as the run started (a run started with it set keeps it
    // for the later test classes).
    if (testFlagAtStart().isNull()) {
        qunsetenv("RL_TEST_NO_CREDENTIAL_STORE");
    } else {
        qputenv("RL_TEST_NO_CREDENTIAL_STORE", testFlagAtStart());
    }
    Credential::setNoOsStoreForTests(false);
    Credential::setArmedProbe(nullptr);
    QSettings s;
    for (const QString &account : std::as_const(createdAccounts())) {
        Credential::erase(account);
#ifdef Q_OS_WIN
        deleteTarget(newTarget(account));
        deleteTarget(oldTarget(account));
#endif
        s.remove(QStringLiteral("RotorLabCredential/") + account);
    }
    s.sync();
    createdAccounts().clear();
    Credential::dropCache();
    UnitTest::cleanup();
}

void CredentialTest::_testFlagBlocksStoreWrites()
{
#ifndef QGC_DAILY_BUILD
    QSKIP("RL_TEST_NO_CREDENTIAL_STORE exists only in Daily builds");
#else
    // G105: a Daily test run with RL_TEST_NO_CREDENTIAL_STORE=1 never reads,
    // writes or erases the OS store, so a test value never lands there under
    // the name a real entry uses.
    const QString account = testAccount();
    qputenv("RL_TEST_NO_CREDENTIAL_STORE", "1");
    Credential::dropCache();
    QVERIFY(!Credential::inOsStore());
    QVERIFY(Credential::storeName().contains(QStringLiteral("test run")));
    QVERIFY(!Credential::store(account, kSecret));          // not saved
    QVERIFY(Credential::keepForSession(account, kSecret));  // what the callers do instead
    QCOMPARE(Credential::load(account), kSecret);           // from memory
    QVERIFY(!inSettingsFile(account));                      // never the settings file
#ifdef Q_OS_WIN
    bool found = true;
    readTarget(newTarget(account), &found);
    QVERIFY(!found);                                        // nothing written

    // An entry already in Credential Manager (as a real one would be) is
    // neither read nor deleted by the test run.
    writeTarget(newTarget(account), kSecret + QStringLiteral("real"));
    Credential::dropCache();
    QVERIFY(Credential::load(account).isEmpty());
    Credential::erase(account);
    QCOMPARE(readTarget(newTarget(account), &found), kSecret + QStringLiteral("real"));
    QVERIFY(found);

    // Without the flag the same entry is read: the check above saw a real entry.
    qunsetenv("RL_TEST_NO_CREDENTIAL_STORE");
    Credential::dropCache();
    QCOMPARE(Credential::load(account), kSecret + QStringLiteral("real"));
#endif
#endif
}

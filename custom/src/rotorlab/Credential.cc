/****************************************************************************
 *
 * RotorLab GC: where the RotorLab Device Key is kept, per platform.
 * See Credential.h for what each platform uses.
 *
 ****************************************************************************/

#include "Credential.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QHash>
#include <QtCore/QMutex>
#include <QtCore/QSet>

#include <functional>

#ifndef CREDENTIAL_NO_VEHICLES
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"
#endif

#if defined(Q_OS_WIN)
// wincred.h does not include what it needs; windows.h has to come first.
#include <windows.h>
#include <wincred.h>
#endif

#include <QtCore/QSettings>

#if defined(Q_OS_ANDROID)
#include <QtCore/QJniEnvironment>
#include <QtCore/QJniObject>
#include <QtCore/QStringList>
#elif defined(Q_OS_LINUX)
#include <QtCore/QLibrary>
#endif

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("Credential", text);
}

/// What the stored secret is, in messages: the TAK password account (and
/// its unit-test variant) is "TAK password"; every other account is the
/// RotorLab Device Key.
QString entryName(const QString &account)
{
    return account.contains(QLatin1String("TAK password")) ? QStringLiteral("TAK password")
                                                           : QStringLiteral("Device Key");
}

#ifdef Q_OS_WIN

/// The name the secret is filed under in the Windows Credential Manager. An
/// operator who goes looking for it should be able to recognize it.
QString targetName(const QString &account)
{
    return QStringLiteral("RotorLab GC - %1").arg(account);
}

/// The name GC 5.1.x and earlier used (with an em dash, U+2014). Read once to
/// move an entry to targetName(), then deleted. Never written.
QString oldTargetName(const QString &account)
{
    return QStringLiteral("RotorLab GC ") + QChar(0x2014) + QStringLiteral(" ") + account;
}

/// True when an entry exists under `target`; its secret goes to `out`.
bool winRead(const QString &target, QString *out)
{
    PCREDENTIALW cred = nullptr;
    if (!::CredReadW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC, 0, &cred)) {
        return false;
    }
    out->clear();
    if (cred->CredentialBlob && cred->CredentialBlobSize > 0) {
        *out = QString::fromUtf8(reinterpret_cast<const char *>(cred->CredentialBlob),
                                 static_cast<int>(cred->CredentialBlobSize));
        ::SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
    }
    ::CredFree(cred);
    return true;
}

bool winWrite(const QString &target, const QString &secret)
{
    QByteArray blob = secret.toUtf8();
    CREDENTIALW cred{};
    cred.Type               = CRED_TYPE_GENERIC;
    cred.TargetName         = const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(target.utf16()));
    cred.CredentialBlobSize = static_cast<DWORD>(blob.size());
    cred.CredentialBlob     = reinterpret_cast<LPBYTE>(blob.data());
    // Bound to this machine and this Windows account. Deliberately not
    // CRED_PERSIST_ENTERPRISE: a ground station's key should not roam to every
    // machine the operator signs in to.
    cred.Persist            = CRED_PERSIST_LOCAL_MACHINE;
    const bool ok = ::CredWriteW(&cred, 0);
    // The key existed in this process's memory; do not leave a copy behind.
    ::SecureZeroMemory(blob.data(), static_cast<size_t>(blob.size()));
    return ok;
}

void winDelete(const QString &target)
{
    // Nothing to delete is not an error worth reporting.
    (void)::CredDeleteW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC, 0);
}

/// Read `account`, moving an entry saved under the old name to the new one.
/// The old entry is deleted only after the new one is written and reads back
/// the same; otherwise it stays and the move is tried again at the next start.
QString winLoad(const QString &account)
{
    QString key;
    if (winRead(targetName(account), &key)) {
        // A run stopped between the write and the delete leaves both. The new
        // entry was written, so the old one can go.
        winDelete(oldTargetName(account));
        return key;
    }
    QString old;
    if (!winRead(oldTargetName(account), &old) || old.isEmpty()) {
        return {};
    }
    QString check;
    if (winWrite(targetName(account), old) && winRead(targetName(account), &check) && check == old) {
        winDelete(oldTargetName(account));
        qInfo().noquote() << "Credential: the" << entryName(account)
                          << "entry in Windows Credential Manager was renamed to" << targetName(account);
    } else {
        qWarning().noquote() << "Credential: the" << entryName(account)
                             << "entry in Windows Credential Manager could not be renamed;"
                             << "GC keeps using the old entry and tries again at the next start";
    }
    return old;
}

#endif // Q_OS_WIN

void wipe(QByteArray &bytes)
{
    volatile char *p = bytes.data();
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        p[i] = 0;
    }
}

// ------------------------------------------------------------ settings file
// Older builds kept the key base64-encoded in the application's settings
// file on every platform except Windows, and on Linux with no Secret Service
// until GC 5.2.0. Base64 is an encoding, not encryption. GC no longer writes
// a key there on any platform (G82); it only reads an old copy, to move it
// into a store or remove it, and clears it.

constexpr auto kFileGroup = "RotorLabCredential";

QString fileLoad(const QString &account)
{
    QSettings s;
    s.beginGroup(QLatin1String(kFileGroup));
    QByteArray raw = s.value(account).toByteArray();
    if (raw.isEmpty()) {
        return {};
    }
    QByteArray plain = QByteArray::fromBase64(raw);
    const QString out = QString::fromUtf8(plain);
    wipe(plain);
    wipe(raw);
    return out;
}

/// False when a value was there and the settings file could not be written
/// without it.
bool fileErase(const QString &account)
{
    QSettings s;
    s.beginGroup(QLatin1String(kFileGroup));
    if (!s.contains(account)) {
        return true;
    }
    s.remove(account);
    s.endGroup();
    s.sync();
    return s.status() == QSettings::NoError;
}

/// After a store write: the file copy must be gone, or it would win over the
/// newer stored key at the next start (platformLoad reads the file first).
[[maybe_unused]] void dropFileCopyAfterStore(const QString &account)
{
    if (!fileErase(account)) {
        qWarning().noquote() << "Credential: the old settings-file copy of the" << entryName(account) << "could not be removed;"
                   << "it may replace the newly stored key at the next start";
    }
}

// What the last read did with an old settings-file copy, per account. Read
// and written only with the key cache mutex held (platformLoad runs under it).
QHash<QString, Credential::FileCopy> &fileCopies()
{
    static QHash<QString, Credential::FileCopy> h;
    return h;
}

/// The old settings-file copy is not used: it is removed from the file and the
/// user is asked to enter the key again. Returns the empty key.
QString dropFileCopyUnused(const QString &account, const char *why)
{
    if (!fileErase(account)) {
        qWarning().noquote() << "Credential: the old settings-file copy of the" << entryName(account)
                             << "could not be removed from the settings file";
    }
    qWarning().noquote() << "Credential: removed the" << entryName(account)
                         << "from the settings file, where an earlier version saved it, and did not use it:" << why;
    fileCopies().insert(account, Credential::FileCopy::Removed);
    return {};
}

/// G82, one rule for every platform with a store (Linux Secret Service,
/// Android Keystore): a key an older build left in the settings file is
/// moved into the store when the store takes it and reads it back, and is
/// then removed from the file. Otherwise it is removed from the file and NOT
/// used: dropStoreCopy() clears any older store copy it replaced, and the
/// user is asked to enter the key again. Returns false when there is no file
/// copy (and leaves *key alone). Platform-free, so tests drive it with fakes.
/// Caller holds the key cache mutex.
bool resolveFileCopyLocked(const QString &account, const std::function<bool(const QString &)> &moveToStore,
                           const std::function<void()> &dropStoreCopy, QString *key)
{
    const QString plain = fileLoad(account);
    if (plain.isEmpty()) {
        return false;
    }
    if (moveToStore(plain)) {
        if (fileErase(account)) {
            fileCopies().insert(account, Credential::FileCopy::Moved);
            qInfo().noquote() << "Credential: moved the" << entryName(account)
                              << "from the settings file to" << Credential::storeName();
        } else {
            dropFileCopyAfterStore(account);   // warns
        }
        *key = plain;
        return true;
    }
    dropStoreCopy();
    *key = dropFileCopyUnused(account, "the system key store did not accept it");
    return true;
}

bool g_noOsStoreForTests = false;

/// G105: a Daily build started with RL_TEST_NO_CREDENTIAL_STORE=1 never
/// reads, writes or erases the OS credential store. A test GUI on a developer
/// machine keeps every key for the session only, so a test value never lands
/// in the OS store under the same name as a real entry. Compiled out of stable
/// builds.
bool testStoreBlocked()
{
#ifdef QGC_DAILY_BUILD
    return qEnvironmentVariableIntValue("RL_TEST_NO_CREDENTIAL_STORE") == 1;
#else
    return false;
#endif
}

#if defined(Q_OS_ANDROID)

// ------------------------------------------------------- Android Keystore
// AES-256-GCM with a key generated inside the Android Keystore. The key
// material never leaves the Keystore; only the IV and the ciphertext are kept,
// in app-private settings.

constexpr auto kKeystoreAlias  = "RotorLabGC.DeviceKey";
constexpr auto kKeystoreGroup  = "RotorLabCredentialKeystore";
constexpr auto kKeystoreFormat = "v1";

bool jniFailed(QJniEnvironment &env)
{
    return env.checkAndClearExceptions(QJniEnvironment::OutputMode::Silent);
}

QJniObject jstr(const char *s)
{
    return QJniObject::fromString(QString::fromLatin1(s));
}

jbyteArray toJava(QJniEnvironment &env, const QByteArray &bytes)
{
    jbyteArray arr = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (arr) {
        env->SetByteArrayRegion(arr, 0, static_cast<jsize>(bytes.size()),
                                reinterpret_cast<const jbyte *>(bytes.constData()));
    }
    return arr;
}

QByteArray fromJava(QJniEnvironment &env, const QJniObject &obj)
{
    if (!obj.isValid()) {
        return {};
    }
    const jbyteArray arr = obj.object<jbyteArray>();
    const jsize n = env->GetArrayLength(arr);
    QByteArray out(n, Qt::Uninitialized);
    env->GetByteArrayRegion(arr, 0, n, reinterpret_cast<jbyte *>(out.data()));
    return out;
}

QJniObject oneStringArray(QJniEnvironment &env, const char *value)
{
    const QJniObject s = jstr(value);
    jobjectArray arr = env->NewObjectArray(1, env.findClass("java/lang/String"), s.object<jstring>());
    return QJniObject::fromLocalRef(arr);
}

/// The Keystore key, created on first use.
QJniObject keystoreKey(QJniEnvironment &env)
{
    QJniObject ks = QJniObject::callStaticObjectMethod(
        "java/security/KeyStore", "getInstance",
        "(Ljava/lang/String;)Ljava/security/KeyStore;",
        jstr("AndroidKeyStore").object<jstring>());
    if (jniFailed(env) || !ks.isValid()) {
        return {};
    }
    ks.callMethod<void>("load", "(Ljava/security/KeyStore$LoadStoreParameter;)V",
                        static_cast<jobject>(nullptr));
    if (jniFailed(env)) {
        return {};
    }
    QJniObject key = ks.callObjectMethod("getKey", "(Ljava/lang/String;[C)Ljava/security/Key;",
                                         jstr(kKeystoreAlias).object<jstring>(),
                                         static_cast<jcharArray>(nullptr));
    if (jniFailed(env)) {
        return {};
    }
    if (key.isValid()) {
        return key;
    }

    const char *builderSig = "Landroid/security/keystore/KeyGenParameterSpec$Builder;";
    QJniObject builder("android/security/keystore/KeyGenParameterSpec$Builder",
                       "(Ljava/lang/String;I)V",
                       jstr(kKeystoreAlias).object<jstring>(),
                       jint(1 | 2));   // PURPOSE_ENCRYPT | PURPOSE_DECRYPT
    if (jniFailed(env) || !builder.isValid()) {
        return {};
    }
    builder = builder.callObjectMethod("setBlockModes",
                                       QByteArray("([Ljava/lang/String;)").append(builderSig).constData(),
                                       oneStringArray(env, "GCM").object<jobjectArray>());
    if (jniFailed(env) || !builder.isValid()) {
        return {};
    }
    builder = builder.callObjectMethod("setEncryptionPaddings",
                                       QByteArray("([Ljava/lang/String;)").append(builderSig).constData(),
                                       oneStringArray(env, "NoPadding").object<jobjectArray>());
    if (jniFailed(env) || !builder.isValid()) {
        return {};
    }
    builder = builder.callObjectMethod("setKeySize", QByteArray("(I)").append(builderSig).constData(),
                                       jint(256));
    if (jniFailed(env) || !builder.isValid()) {
        return {};
    }
    const QJniObject spec = builder.callObjectMethod(
        "build", "()Landroid/security/keystore/KeyGenParameterSpec;");
    if (jniFailed(env) || !spec.isValid()) {
        return {};
    }

    QJniObject gen = QJniObject::callStaticObjectMethod(
        "javax/crypto/KeyGenerator", "getInstance",
        "(Ljava/lang/String;Ljava/lang/String;)Ljavax/crypto/KeyGenerator;",
        jstr("AES").object<jstring>(), jstr("AndroidKeyStore").object<jstring>());
    if (jniFailed(env) || !gen.isValid()) {
        return {};
    }
    gen.callMethod<void>("init", "(Ljava/security/spec/AlgorithmParameterSpec;)V", spec.object());
    if (jniFailed(env)) {
        return {};
    }
    key = gen.callObjectMethod("generateKey", "()Ljavax/crypto/SecretKey;");
    if (jniFailed(env)) {
        return {};
    }
    return key;
}

QJniObject gcmCipher(QJniEnvironment &env)
{
    QJniObject c = QJniObject::callStaticObjectMethod(
        "javax/crypto/Cipher", "getInstance", "(Ljava/lang/String;)Ljavax/crypto/Cipher;",
        jstr("AES/GCM/NoPadding").object<jstring>());
    if (jniFailed(env)) {
        return {};
    }
    return c;
}

bool keystoreEncrypt(const QByteArray &plain, QByteArray *iv, QByteArray *sealed)
{
    QJniEnvironment env;
    const QJniObject key = keystoreKey(env);
    QJniObject cipher = gcmCipher(env);
    if (!key.isValid() || !cipher.isValid()) {
        return false;
    }
    cipher.callMethod<void>("init", "(ILjava/security/Key;)V", jint(1) /* ENCRYPT_MODE */, key.object());
    if (jniFailed(env)) {
        return false;
    }
    *iv = fromJava(env, cipher.callObjectMethod("getIV", "()[B"));
    if (jniFailed(env) || iv->isEmpty()) {
        return false;
    }
    jbyteArray in = toJava(env, plain);
    if (!in) {
        jniFailed(env);
        return false;
    }
    const QJniObject out = cipher.callObjectMethod("doFinal", "([B)[B", in);
    // Check (and clear) any exception before the next JNI call. Qt may have
    // cleared it already, so a missing result counts as a failure too.
    const bool failed = jniFailed(env) || !out.isValid();
    // Clear the Java copy of the plaintext before letting it go.
    QByteArray zeros(plain.size(), '\0');
    env->SetByteArrayRegion(in, 0, static_cast<jsize>(zeros.size()),
                            reinterpret_cast<const jbyte *>(zeros.constData()));
    env->DeleteLocalRef(in);
    if (failed) {
        return false;
    }
    *sealed = fromJava(env, out);
    return !sealed->isEmpty();
}

bool keystoreDecrypt(const QByteArray &iv, const QByteArray &sealed, QByteArray *plain)
{
    QJniEnvironment env;
    const QJniObject key = keystoreKey(env);
    QJniObject cipher = gcmCipher(env);
    if (!key.isValid() || !cipher.isValid()) {
        return false;
    }
    jbyteArray ivArr = toJava(env, iv);
    if (!ivArr) {
        jniFailed(env);
        return false;
    }
    const QJniObject params("javax/crypto/spec/GCMParameterSpec", "(I[B)V", jint(128), ivArr);
    const bool paramsFailed = jniFailed(env) || !params.isValid();
    env->DeleteLocalRef(ivArr);
    if (paramsFailed) {
        return false;
    }
    cipher.callMethod<void>("init", "(ILjava/security/Key;Ljava/security/spec/AlgorithmParameterSpec;)V",
                            jint(2) /* DECRYPT_MODE */, key.object(), params.object());
    if (jniFailed(env)) {
        return false;
    }
    jbyteArray in = toJava(env, sealed);
    if (!in) {
        jniFailed(env);
        return false;
    }
    const QJniObject out = cipher.callObjectMethod("doFinal", "([B)[B", in);
    // AEADBadTagException (tampered, or another key) lands here. Qt may have
    // cleared the exception already, so a missing result is a failure too.
    const bool failed = jniFailed(env) || !out.isValid();
    env->DeleteLocalRef(in);
    if (failed) {
        return false;
    }
    *plain = fromJava(env, out);
    const jbyteArray arr = out.object<jbyteArray>();
    QByteArray zeros(plain->size(), '\0');
    env->SetByteArrayRegion(arr, 0, static_cast<jsize>(zeros.size()),
                            reinterpret_cast<const jbyte *>(zeros.constData()));
    return !plain->isEmpty();
}

bool sealedStore(const QString &account, const QString &secret)
{
    QByteArray plain = secret.toUtf8();
    QByteArray iv;
    QByteArray sealed;
    const bool ok = keystoreEncrypt(plain, &iv, &sealed);
    wipe(plain);
    if (!ok) {
        qWarning().noquote() << "Credential: the Android Keystore refused to encrypt the" << entryName(account);
        return false;
    }
    QSettings s;
    s.beginGroup(QLatin1String(kKeystoreGroup));
    s.setValue(account, QStringLiteral("%1:%2:%3").arg(QLatin1String(kKeystoreFormat),
                                                      QString::fromLatin1(iv.toBase64()),
                                                      QString::fromLatin1(sealed.toBase64())));
    s.endGroup();
    s.sync();
    return true;
}

/// found: a sealed value exists (even if it could not be opened).
QString sealedLoad(const QString &account, bool *found)
{
    QSettings s;
    s.beginGroup(QLatin1String(kKeystoreGroup));
    const QString value = s.value(account).toString();
    *found = !value.isEmpty();
    const QStringList parts = value.split(QLatin1Char(':'));
    if (parts.size() != 3 || parts.at(0) != QLatin1String(kKeystoreFormat)) {
        return {};
    }
    QByteArray plain;
    if (!keystoreDecrypt(QByteArray::fromBase64(parts.at(1).toLatin1()),
                         QByteArray::fromBase64(parts.at(2).toLatin1()), &plain)) {
        qWarning().noquote() << "Credential: the Android Keystore could not open the stored" << entryName(account);
        return {};
    }
    const QString out = QString::fromUtf8(plain);
    wipe(plain);
    return out;
}

void sealedErase(const QString &account)
{
    QSettings s;
    s.beginGroup(QLatin1String(kKeystoreGroup));
    s.remove(account);
    s.endGroup();
    s.sync();
}

#elif defined(Q_OS_LINUX)

// ------------------------------------------------------ Secret Service
// libsecret is loaded at run time, so the build needs no libsecret headers and
// the program still starts where it is not installed. The declarations below
// mirror libsecret-1's public, stable C ABI (secret/secret-schema.h,
// secret/secret-password.h).

using gboolean = int;
struct GError { quint32 domain; int code; char *message; };
struct SecretSchemaAttribute { const char *name; int type; };   // 0 = STRING
struct SecretSchema {
    const char *name;
    int flags;                                                   // 0 = NONE
    SecretSchemaAttribute attributes[32];
    int reserved;
    void *reserved1, *reserved2, *reserved3, *reserved4, *reserved5, *reserved6, *reserved7;
};

using StoreFn   = gboolean (*)(const SecretSchema *, const char *collection, const char *label,
                               const char *password, void *cancellable, GError **, ...);
using LookupFn  = char *(*)(const SecretSchema *, void *cancellable, GError **, ...);
using ClearFn   = gboolean (*)(const SecretSchema *, void *cancellable, GError **, ...);
using PwFreeFn  = void (*)(char *);
using ErrFreeFn = void (*)(GError *);

struct SecretApi {
    StoreFn   store   = nullptr;
    LookupFn  lookup  = nullptr;
    ClearFn   clear   = nullptr;
    PwFreeFn  pwFree  = nullptr;
    ErrFreeFn errFree = nullptr;
    bool      usable  = false;   ///< loaded, and a Secret Service answered
};

const SecretSchema &deviceKeySchema()
{
    static const SecretSchema schema = [] {
        SecretSchema s{};
        s.name = "app.rotorlab.gc.DeviceKey";
        s.attributes[0] = {"account", 0};
        return s;
    }();
    return schema;
}

void takeError(const SecretApi &api, GError *err, const char *what)
{
    if (!err) {
        return;
    }
    // libsecret's message names the service problem, never the secret.
    qWarning() << "Credential: Secret Service" << what << "failed:"
               << (err->message ? err->message : "");
    if (api.errFree) {
        api.errFree(err);
    }
}

const SecretApi &secretApi()
{
    static const SecretApi api = [] {
        SecretApi a;
        QLibrary secret(QStringLiteral("secret-1"), 0);
        QLibrary glib(QStringLiteral("glib-2.0"), 0);
        if (!secret.load() || !glib.load()) {
            return a;
        }
        a.store   = reinterpret_cast<StoreFn>(secret.resolve("secret_password_store_sync"));
        a.lookup  = reinterpret_cast<LookupFn>(secret.resolve("secret_password_lookup_sync"));
        a.clear   = reinterpret_cast<ClearFn>(secret.resolve("secret_password_clear_sync"));
        a.pwFree  = reinterpret_cast<PwFreeFn>(secret.resolve("secret_password_free"));
        a.errFree = reinterpret_cast<ErrFreeFn>(glib.resolve("g_error_free"));
        if (!a.store || !a.lookup || !a.clear || !a.pwFree || !a.errFree) {
            return a;
        }
        // Probe: a lookup that finds nothing succeeds only when a Secret
        // Service is running on the session bus.
        GError *err = nullptr;
        char *pw = a.lookup(&deviceKeySchema(), nullptr, &err,
                            "account", "rotorlab-gc-probe", static_cast<const char *>(nullptr));
        if (pw) {
            a.pwFree(pw);
        }
        if (err) {
            takeError(a, err, "probe");
            return a;
        }
        a.usable = true;
        return a;
    }();
    return api;
}

bool secretStore(const QString &account, const QString &secret)
{
    const SecretApi &api = secretApi();
    const QByteArray acc = account.toUtf8();
    // Display only: lookups match the schema and the "account" attribute.
    const QByteArray label = Credential::entryLabel(account).toUtf8();
    QByteArray pw = secret.toUtf8();
    GError *err = nullptr;
    const gboolean ok = api.store(&deviceKeySchema(), "default", label.constData(), pw.constData(),
                                  nullptr, &err,
                                  "account", acc.constData(), static_cast<const char *>(nullptr));
    wipe(pw);
    if (err) {
        takeError(api, err, "store");
        return false;
    }
    return ok != 0;
}

QString secretLoad(const QString &account)
{
    const SecretApi &api = secretApi();
    const QByteArray acc = account.toUtf8();
    GError *err = nullptr;
    char *pw = api.lookup(&deviceKeySchema(), nullptr, &err,
                          "account", acc.constData(), static_cast<const char *>(nullptr));
    if (err) {
        takeError(api, err, "lookup");
    }
    if (!pw) {
        return {};
    }
    const QString out = QString::fromUtf8(pw);
    api.pwFree(pw);   // libsecret clears the buffer before freeing it
    return out;
}

void secretErase(const QString &account)
{
    const SecretApi &api = secretApi();
    const QByteArray acc = account.toUtf8();
    GError *err = nullptr;
    api.clear(&deviceKeySchema(), nullptr, &err,
              "account", acc.constData(), static_cast<const char *>(nullptr));
    takeError(api, err, "clear");
}

#endif

} // namespace

namespace {

/// A store GC may save a key in: Credential Manager, the Android Keystore, or
/// a Secret Service that answered. Never the settings file.
bool osStoreUsable()
{
    if (g_noOsStoreForTests || testStoreBlocked()) {
        return false;
    }
#if defined(Q_OS_WIN) || defined(Q_OS_ANDROID)
    return true;
#elif defined(Q_OS_LINUX)
    return secretApi().usable;
#else
    return false;
#endif
}

bool platformStore(const QString &account, const QString &secret)
{
    if (!osStoreUsable()) {
        // G82: never the settings file. The caller keeps it for this session
        // only (keepForSession) and says so.
        qWarning().noquote() << "Credential: the" << entryName(account)
                             << "is not saved: there is no system key store, and GC does not write it to the settings file";
        return false;
    }
#if defined(Q_OS_WIN)
    if (!winWrite(targetName(account), secret)) {
        return false;
    }
    // The new entry replaces anything under the old name.
    winDelete(oldTargetName(account));
    return true;
#elif defined(Q_OS_ANDROID)
    if (!sealedStore(account, secret)) {
        return false;   // fail closed: no plain copy instead
    }
    dropFileCopyAfterStore(account);
    return true;
#elif defined(Q_OS_LINUX)
    if (!secretStore(account, secret)) {
        return false;   // fail closed: no plain copy instead
    }
    dropFileCopyAfterStore(account);
    return true;
#else
    Q_UNUSED(secret);
    return false;   // osStoreUsable() is false here
#endif
}

QString platformLoad(const QString &account)
{
    // Test runs only (G64, rule 14; G105): a Daily build started with
    // RL_TEST_NO_CREDENTIAL_STORE=1 never reads the OS credential store, so a
    // test GUI on a developer machine cannot pick up the installed GC's real
    // Device Key. It does not touch the settings file either. Compiled out of
    // stable builds (testStoreBlocked() is false there).
    if (testStoreBlocked()) {
        Q_UNUSED(account);
        return {};
    }
    if (!osStoreUsable()) {
        // G82: an old settings-file copy is not used silently. With no store
        // to move it to, it is removed and the user enters the key again.
        if (!fileLoad(account).isEmpty()) {
            return dropFileCopyUnused(account, "there is no system key store to move it to");
        }
        return {};
    }
#if defined(Q_OS_WIN)
    return winLoad(account);
#elif defined(Q_OS_ANDROID)
    // A plain base64 value is never older than the sealed one: every store
    // erases it. So it wins, and is moved: sealed, read back, then erased.
    // G82: if the Keystore does not take it (encryption fails, or it does not
    // read back), it is removed from the file and not used; the user enters
    // the key again.
    QString key;
    if (resolveFileCopyLocked(account,
                              [&account](const QString &plain) {
                                  bool found = false;
                                  return sealedStore(account, plain) && sealedLoad(account, &found) == plain;
                              },
                              [&account] { sealedErase(account); }, &key)) {
        return key;
    }
    bool found = false;
    return sealedLoad(account, &found);
#elif defined(Q_OS_LINUX)
    // A file value is never older than the keyring copy (every keyring store
    // erases it), and it may have been saved while no Secret Service ran. So
    // it wins, and is moved: stored, read back, then erased from the file.
    // G82: if the keyring does not take it, it is not used and not left in
    // the file either; the user enters the key again. The older keyring
    // copy it replaced is cleared too, so it does not come back later.
    QString key;
    if (resolveFileCopyLocked(account,
                              [&account](const QString &plain) {
                                  return secretStore(account, plain) && secretLoad(account) == plain;
                              },
                              [&account] { secretErase(account); }, &key)) {
        return key;
    }
    return secretLoad(account);
#else
    return {};   // osStoreUsable() is false here
#endif
}

void platformErase(const QString &account)
{
    if (testStoreBlocked()) {
        // G105: a test run never deletes an OS store entry: under the fixed
        // names it could be the installed GC's real Device Key or TAK
        // password. Only a plain value in this run's own settings file goes.
        fileErase(account);
        return;
    }
#if defined(Q_OS_WIN)
    winDelete(targetName(account));
    winDelete(oldTargetName(account));
#else
#if defined(Q_OS_ANDROID)
    sealedErase(account);
#elif defined(Q_OS_LINUX)
    if (osStoreUsable()) {
        secretErase(account);
    }
#endif
#endif
    // Also any plain value an older build left behind.
    fileErase(account);
}

// ------------------------------------------- in-memory copy, armed rule
// The key is read from the platform store once and then served from memory.
// The platform store is never touched while any vehicle is armed: a Secret
// Service lookup can raise a keyring unlock prompt, and nothing on a flight
// path may wait on one.

struct KeyCache {
    QMutex                  mutex;
    QHash<QString, QString> keys;           ///< account -> key ("" = none)
    QSet<QString>           pendingErase;   ///< erased while armed, store not yet
    bool                    quitHooked = false;
    QSet<QString>           unitTestAccounts;   ///< G105: erased when the unit test run ends
};

KeyCache &keyCache()
{
    static KeyCache c;
    return c;
}

Credential::ArmedProbe g_armedProbe = nullptr;

bool anyVehicleArmed()
{
    if (g_armedProbe) {
        return g_armedProbe();
    }
#ifdef CREDENTIAL_NO_VEHICLES
    return false;
#else
    if (!QCoreApplication::instance()) {
        return false;
    }
    const QmlObjectListModel *list = MultiVehicleManager::instance()->vehicles();
    for (int i = 0; list && i < list->count(); ++i) {
        const Vehicle *v = qobject_cast<const Vehicle *>((*list)[i]);
        if (v && v->armed()) {
            return true;
        }
    }
    return false;
#endif
}

/// Carry out erases that were held back while a vehicle was armed. Caller
/// holds the mutex and has checked that no vehicle is armed.
void flushPendingErases(KeyCache &c)
{
    for (const QString &account : std::as_const(c.pendingErase)) {
        platformErase(account);
    }
    c.pendingErase.clear();
}

void flushAtQuit()
{
    KeyCache &c = keyCache();
    QMutexLocker lock(&c.mutex);
    if (!c.pendingErase.isEmpty() && !anyVehicleArmed()) {
        flushPendingErases(c);
    }
}

/// G105: the unit test accounts ("Unit test Device Key <pid>", "Unit test TAK
/// password <pid>", CredentialTest's "unittest-<uuid>") get a new name every
/// run, so an entry a test left behind would stay in the OS store for good.
/// Every one a unit test run saves is remembered here and erased by
/// Credential::eraseUnitTestAccounts() when the run ends. Caller holds the
/// mutex.
void trackUnitTestAccount(KeyCache &c, const QString &account)
{
#ifdef QGC_UNITTEST_BUILD
    if (account.startsWith(QLatin1String("Unit test ")) || account.startsWith(QLatin1String("unittest-"))) {
        c.unitTestAccounts.insert(account);
    }
#else
    Q_UNUSED(c);
    Q_UNUSED(account);
#endif
}
} // namespace

namespace Credential {

bool inOsStore()
{
    return osStoreUsable();
}

QString storeName()
{
    if (!osStoreUsable()) {
        // G82: nothing is saved anywhere when there is no store.
        if (testStoreBlocked()) {
            return tr("this session only (test run: the system key store is not used)");
        }
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
        return tr("this session only (no Secret Service found)");
#else
        return tr("this session only (no system key store found)");
#endif
    }
#if defined(Q_OS_WIN)
    return tr("Windows Credential Manager");
#elif defined(Q_OS_ANDROID)
    return tr("encrypted app storage (Android Keystore)");
#elif defined(Q_OS_LINUX)
    return tr("Secret Service (desktop keyring)");
#else
    return {};   // osStoreUsable() is false here
#endif
}

bool store(const QString &account, const QString &secret)
{
    if (account.isEmpty()) {
        return false;
    }
    if (secret.isEmpty()) {
        erase(account);
        return true;
    }

    KeyCache &c = keyCache();
    QMutexLocker lock(&c.mutex);
    if (anyVehicleArmed()) {
        qWarning().noquote() << "Credential: the" << entryName(account)
                             << "is not saved while a vehicle is armed; save it again after landing";
        return false;
    }
    flushPendingErases(c);
    if (!platformStore(account, secret)) {
        return false;
    }
    trackUnitTestAccount(c, account);
    c.keys.insert(account, secret);
    fileCopies().remove(account);
    return true;
}

bool keepForSession(const QString &account, const QString &secret)
{
    if (account.isEmpty() || secret.isEmpty()) {
        return false;
    }
    KeyCache &c = keyCache();
    QMutexLocker lock(&c.mutex);
    c.keys.insert(account, secret);   // memory only: no store, no file
    fileCopies().remove(account);
    return true;
}

FileCopy fileCopy(const QString &account)
{
    KeyCache &c = keyCache();
    QMutexLocker lock(&c.mutex);
    return fileCopies().value(account, FileCopy::None);
}

void setNoOsStoreForTests(bool on)
{
    g_noOsStoreForTests = on;
}

QString resolveFileCopyForTests(const QString &account, bool storeAccepts, bool *storeCopyDropped,
                                QString *storedCopy)
{
    KeyCache &c = keyCache();
    QMutexLocker lock(&c.mutex);
    *storeCopyDropped = false;
    QString key;
    resolveFileCopyLocked(account,
                          [&](const QString &plain) {
                              if (storeAccepts) {
                                  *storedCopy = plain;
                              }
                              return storeAccepts;
                          },
                          [&] { *storeCopyDropped = true; }, &key);
    return key;
}

QString load(const QString &account)
{
    if (account.isEmpty()) {
        return {};
    }

    KeyCache &c = keyCache();
    QMutexLocker lock(&c.mutex);
    // Erases held back during a flight are carried out at the first call with
    // no vehicle armed, even when the answer itself comes from memory.
    const bool armedNow = anyVehicleArmed();
    if (!armedNow && !c.pendingErase.isEmpty()) {
        flushPendingErases(c);
    }
    const auto it = c.keys.constFind(account);
    if (it != c.keys.constEnd()) {
        return it.value();
    }
    if (armedNow) {
        // Not read yet, and the store is off limits now. Nothing is cached,
        // so the next call after landing reads it.
        return {};
    }
    const QString key = platformLoad(account);
    c.keys.insert(account, key);
    return key;
}

void erase(const QString &account)
{
    if (account.isEmpty()) {
        return;
    }

    KeyCache &c = keyCache();
    QMutexLocker lock(&c.mutex);
    c.keys.insert(account, QString());   // gone for this run, at once
    fileCopies().remove(account);
    if (anyVehicleArmed()) {
        // The store is cleaned at the next call with no vehicle armed, or at
        // exit if none is armed then.
        c.pendingErase.insert(account);
        if (!c.quitHooked && QCoreApplication::instance()) {
            QCoreApplication *app = QCoreApplication::instance();
            QObject::connect(app, &QCoreApplication::aboutToQuit, app, [] { flushAtQuit(); });
            c.quitHooked = true;
        }
        return;
    }
    c.pendingErase.remove(account);
    platformErase(account);
}

void setArmedProbe(ArmedProbe probe)
{
    g_armedProbe = probe;
}

QString entryLabel(const QString &account)
{
    if (entryName(account) == QLatin1String("TAK password")) {
        return QStringLiteral("RotorLab GC TAK password");
    }
    return QStringLiteral("RotorLab GC Device Key (%1)").arg(account);
}

bool heldWhileArmed()
{
    return anyVehicleArmed();
}

void dropCache()
{
    KeyCache &c = keyCache();
    QMutexLocker lock(&c.mutex);
    c.keys.clear();
}

void eraseUnitTestAccounts()
{
#ifdef QGC_UNITTEST_BUILD
    KeyCache &c = keyCache();
    QMutexLocker lock(&c.mutex);
    for (const QString &a : std::as_const(c.unitTestAccounts)) {
        platformErase(a);
        c.keys.remove(a);
    }
    c.unitTestAccounts.clear();
#endif
}

} // namespace Credential

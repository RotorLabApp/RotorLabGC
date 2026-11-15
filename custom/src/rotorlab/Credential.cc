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

#ifndef CREDENTIAL_NO_VEHICLES
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"
#endif

#if defined(Q_OS_WIN)
// wincred.h does not include what it needs; windows.h has to come first.
#include <windows.h>
#include <wincred.h>
#else
#include <QtCore/QSettings>
#endif

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

#ifdef Q_OS_WIN

/// The name the secret is filed under in the Windows Credential Manager. An
/// operator who goes looking for it should be able to recognise it.
/// Deliberate exception to the no-em-dash rule: this exact string is the key
/// already saved on every Windows install, and renaming it would orphan them.
QString targetName(const QString &account)
{
    return QStringLiteral("RotorLab GC — %1").arg(account);
}

#else

void wipe(QByteArray &bytes)
{
    volatile char *p = bytes.data();
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        p[i] = 0;
    }
}

// ------------------------------------------------------------ settings file
// The key base64-encoded in the application's settings file. This is the
// store on platforms without one, and where older builds kept the key on
// every platform except Windows. Base64 is an encoding, not encryption.

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

[[maybe_unused]] void fileStore(const QString &account, const QString &secret)
{
    QByteArray plain = secret.toUtf8();
    QSettings s;
    s.beginGroup(QLatin1String(kFileGroup));
    s.setValue(account, plain.toBase64());
    s.endGroup();
    s.sync();
    wipe(plain);
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
        qWarning() << "Credential: the old settings-file copy of the Device Key could not be removed;"
                   << "it may replace the newly stored key at the next start";
    }
}

#endif // !Q_OS_WIN

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
        qWarning() << "Credential: the Android Keystore refused to encrypt the Device Key";
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
        qWarning() << "Credential: the Android Keystore could not open the stored Device Key";
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
    const QByteArray label = QStringLiteral("RotorLab GC Device Key (%1)").arg(account).toUtf8();
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

bool platformStore(const QString &account, const QString &secret)
{
#if defined(Q_OS_WIN)
    const QString target = targetName(account);
    QByteArray blob = secret.toUtf8();

    CREDENTIALW cred{};
    cred.Type            = CRED_TYPE_GENERIC;
    cred.TargetName      = const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(target.utf16()));
    cred.CredentialBlobSize = static_cast<DWORD>(blob.size());
    cred.CredentialBlob  = reinterpret_cast<LPBYTE>(blob.data());
    // Bound to this machine and this Windows account. Deliberately not
    // CRED_PERSIST_ENTERPRISE: a ground station's key should not roam to every
    // machine the operator signs in to.
    cred.Persist         = CRED_PERSIST_LOCAL_MACHINE;

    const bool ok = ::CredWriteW(&cred, 0);
    // The key existed in this process's memory; do not leave a copy behind.
    ::SecureZeroMemory(blob.data(), static_cast<size_t>(blob.size()));
    return ok;
#elif defined(Q_OS_ANDROID)
    if (!sealedStore(account, secret)) {
        return false;   // fail closed: no plain copy instead
    }
    dropFileCopyAfterStore(account);
    return true;
#elif defined(Q_OS_LINUX)
    if (secretApi().usable) {
        if (!secretStore(account, secret)) {
            return false;   // fail closed: no plain copy instead
        }
        dropFileCopyAfterStore(account);
        return true;
    }
    fileStore(account, secret);
    return true;
#else
    fileStore(account, secret);
    return true;
#endif
}

QString platformLoad(const QString &account)
{
#if defined(Q_OS_WIN)
    const QString target = targetName(account);
    PCREDENTIALW cred = nullptr;
    if (!::CredReadW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC, 0, &cred)) {
        return {};
    }
    QString out;
    if (cred->CredentialBlob && cred->CredentialBlobSize > 0) {
        out = QString::fromUtf8(reinterpret_cast<const char *>(cred->CredentialBlob),
                                static_cast<int>(cred->CredentialBlobSize));
        ::SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
    }
    ::CredFree(cred);
    return out;
#elif defined(Q_OS_ANDROID)
    // A plain base64 value is never older than the sealed one: every store
    // erases it. So it wins, and is moved: sealed, read back, then erased.
    // If anything fails the plain value stays and the move is tried again at
    // the next start.
    const QString plain = fileLoad(account);
    if (!plain.isEmpty()) {
        if (sealedStore(account, plain)) {
            bool found = false;
            if (sealedLoad(account, &found) == plain) {
                fileErase(account);
            }
        }
        return plain;
    }
    bool found = false;
    return sealedLoad(account, &found);
#elif defined(Q_OS_LINUX)
    if (!secretApi().usable) {
        return fileLoad(account);
    }
    // A file value is never older than the keyring copy (every keyring store
    // erases it), and it may have been saved while no Secret Service ran. So
    // it wins, and is moved: stored, read back, then erased from the file.
    const QString plain = fileLoad(account);
    if (!plain.isEmpty()) {
        if (secretStore(account, plain) && secretLoad(account) == plain) {
            fileErase(account);
        }
        return plain;
    }
    return secretLoad(account);
#else
    return fileLoad(account);
#endif
}

void platformErase(const QString &account)
{
#if defined(Q_OS_WIN)
    const QString target = targetName(account);
    // Nothing to delete is not an error worth reporting.
    (void)::CredDeleteW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC, 0);
#else
#if defined(Q_OS_ANDROID)
    sealedErase(account);
#elif defined(Q_OS_LINUX)
    if (secretApi().usable) {
        secretErase(account);
    }
#endif
    // Also any plain value an older build left behind.
    fileErase(account);
#endif
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
} // namespace

namespace Credential {

bool inOsStore()
{
#if defined(Q_OS_WIN) || defined(Q_OS_ANDROID)
    return true;
#elif defined(Q_OS_LINUX)
    return secretApi().usable;
#else
    return false;
#endif
}

QString storeName()
{
#if defined(Q_OS_WIN)
    return tr("Windows Credential Manager");
#elif defined(Q_OS_ANDROID)
    return tr("app-private settings, encrypted with an Android Keystore key");
#elif defined(Q_OS_LINUX)
    return secretApi().usable
        ? tr("Secret Service (desktop keyring)")
        : tr("settings file, base64-encoded, not encrypted (no Secret Service found)");
#else
    return tr("settings file, base64-encoded, not encrypted");
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
        qWarning() << "Credential: the Device Key is not saved while a vehicle is armed; save it again after landing";
        return false;
    }
    flushPendingErases(c);
    if (!platformStore(account, secret)) {
        return false;
    }
    c.keys.insert(account, secret);
    return true;
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

void dropCache()
{
    KeyCache &c = keyCache();
    QMutexLocker lock(&c.mutex);
    c.keys.clear();
}

} // namespace Credential

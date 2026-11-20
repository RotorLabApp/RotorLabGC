/****************************************************************************
 *
 * RotorLab GC - the signed offline activation record (G89). See
 * LicenseRecord.h.
 *
 ****************************************************************************/

#include "LicenseRecord.h"
#include "Ed25519.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QJsonValue>

// A stable build must never carry the test-key hook below.
#if defined(RLGC_STABLE_BUILD) && defined(QGC_DAILY_BUILD)
#error "QGC_DAILY_BUILD in a stable build would let ROTORLAB_LICENSE_PUBKEYS add a trusted key"
#endif

#ifndef RLGC_LICENSE_PUBKEYS
#define RLGC_LICENSE_PUBKEYS ""
#endif

namespace LicenseRecord {

const char kDomain[] = "rotorlab-gc-offline-record-v1";
const char kKidTag[] = "rotorlab-license-signing-key-v1";
const char kKeyTag[] = "rotorlab-gc-license-key-v1";

namespace {

bool isHex(const QString &s, int len)
{
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]+$"));
    return s.size() == len && re.match(s).hasMatch();
}

bool b64url(const QJsonValue &v, QByteArray *out)
{
    if (!v.isString()) {
        return false;
    }
    const QString s = v.toString();
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]+$"));   // no padding
    if (s.isEmpty() || !re.match(s).hasMatch()) {
        return false;
    }
    const auto r = QByteArray::fromBase64Encoding(
        s.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!r) {
        return false;
    }
    *out = r.decoded;
    return true;
}

/// "YYYY-MM-DDTHH:MM:SSZ" to UTC seconds, or -1.
qint64 utcSecs(const QJsonValue &v)
{
    if (!v.isString()) {
        return -1;
    }
    const QString s = v.toString();
    static const QRegularExpression re(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z$"));
    if (!re.match(s).hasMatch()) {
        return -1;
    }
    const QDateTime t = QDateTime::fromString(s, Qt::ISODate);
    if (!t.isValid()) {
        return -1;
    }
    return t.toSecsSinceEpoch();
}

} // namespace

QString kidOf(const QByteArray &publicKey)
{
    QByteArray in(kKidTag);
    in.append('\0');
    in.append(publicKey);
    return QString::fromLatin1(QCryptographicHash::hash(in, QCryptographicHash::Sha256).toHex().left(16));
}

QList<PinnedKey> keysFromHex(const QString &list)
{
    static const QRegularExpression sep(QStringLiteral("[,;\\s]+"));
    QList<PinnedKey> keys;
    QSet<QString> seen;
    for (const QString &entry : list.split(sep, Qt::SkipEmptyParts)) {
        const QString hex = entry.toLower();
        if (!isHex(hex, 2 * Ed25519::kPublicKeySize)) {
            continue;
        }
        PinnedKey k;
        k.publicKey = QByteArray::fromHex(hex.toLatin1());
        k.kid = kidOf(k.publicKey);
        if (seen.contains(k.kid)) {
            continue;
        }
        seen.insert(k.kid);
        keys.append(k);
    }
    return keys;
}

QList<PinnedKey> pinnedKeys()
{
    QString list = QStringLiteral(RLGC_LICENSE_PUBKEYS);
#ifdef QGC_DAILY_BUILD
    // Development builds only, beside ROTORLAB_BASE_URL, so GC can be run
    // against a local RotorLab and its own signing key. Compiled out of a
    // stable build: a released GC trusts exactly the keys it was built with.
    //
    // Test-key hook: this block and the one in
    // rotorlab/RotorLabStore.cc::baseUrl() are the only places GC reads a
    // license key or a RotorLab address from the environment. Both must stay
    // inside #ifdef QGC_DAILY_BUILD, which the top-level CMakeLists.txt defines
    // only when QGC_STABLE_BUILD is OFF. The release scripts check it on every
    // stable binary (deploy/rotorlab/release.ps1 and
    // deploy/rotorlab/linux/build-appimage.sh): a stable binary must not contain
    // either variable name.
    const QByteArray extra = qgetenv("ROTORLAB_LICENSE_PUBKEYS");
    if (!extra.isEmpty()) {
        list += QLatin1Char(',') + QString::fromLatin1(extra);
    }
#endif
    return keysFromHex(list);
}

QString keyHash(const QString &key)
{
    QByteArray norm;
    for (const QChar c : key) {
        const QChar u = c.toUpper();
        if ((u >= QLatin1Char('A') && u <= QLatin1Char('Z')) || (u >= QLatin1Char('0') && u <= QLatin1Char('9'))) {
            norm.append(static_cast<char>(u.unicode()));
        }
    }
    QByteArray in(kKeyTag);
    in.append('\0');
    in.append(norm);
    return QString::fromLatin1(QCryptographicHash::hash(in, QCryptographicHash::Sha256).toHex());
}

QByteArray signingInput(const QByteArray &recordBytes)
{
    QByteArray m(kDomain);
    m.append('\0');
    m.append(recordBytes);
    return m;
}

Check check(const QByteArray &envelope, const QList<PinnedKey> &keys,
            const QString &deviceId, const QString &key,
            qint64 now, qint64 highWater,
            Record *out, bool *signatureOk)
{
    if (signatureOk) {
        *signatureOk = false;
    }
    if (envelope.isEmpty()) {
        return Check::Missing;
    }

    // The envelope: which key signed, the record bytes, the signature.
    QJsonParseError err{};
    const QJsonDocument envDoc = QJsonDocument::fromJson(envelope, &err);
    if (err.error != QJsonParseError::NoError || !envDoc.isObject()) {
        return Check::Malformed;
    }
    const QJsonObject env = envDoc.object();
    const QString kid = env.value(QStringLiteral("kid")).toString();
    QByteArray recordBytes, signature;
    if (!isHex(kid, 16)
        || !b64url(env.value(QStringLiteral("record")), &recordBytes)
        || !b64url(env.value(QStringLiteral("signature")), &signature)
        || recordBytes.isEmpty() || recordBytes.size() > kMaxRecordBytes
        || signature.size() != Ed25519::kSignatureSize) {
        return Check::Malformed;
    }

    const PinnedKey *pinned = nullptr;
    for (const PinnedKey &k : keys) {
        if (k.kid == kid) {
            pinned = &k;
            break;
        }
    }
    if (!pinned) {
        return Check::UnknownKey;
    }
    if (!Ed25519::verify(signature, signingInput(recordBytes), pinned->publicKey)) {
        return Check::BadSignature;
    }

    // Only now is the record read. A signed record that is not the contract's
    // shape is still refused: GC never guesses at a field.
    const QJsonDocument recDoc = QJsonDocument::fromJson(recordBytes, &err);
    if (err.error != QJsonParseError::NoError || !recDoc.isObject()) {
        return Check::Malformed;
    }
    const QJsonObject o = recDoc.object();
    static const QStringList kFields = {
        QStringLiteral("device_id"), QStringLiteral("issued"), QStringLiteral("key_sha256"),
        QStringLiteral("kid"), QStringLiteral("nonce"), QStringLiteral("offline_until"),
        QStringLiteral("product"), QStringLiteral("v")};
    QStringList fields = o.keys();
    fields.sort();
    if (fields != kFields) {   // exactly the eight, no more, no fewer
        return Check::Malformed;
    }
    Record r;
    const QJsonValue v = o.value(QStringLiteral("v"));
    r.v = v.isDouble() && v.toDouble() == kVersion ? kVersion : 0;
    r.kid = o.value(QStringLiteral("kid")).toString();
    r.product = o.value(QStringLiteral("product")).toString();
    r.keySha256 = o.value(QStringLiteral("key_sha256")).toString();
    r.deviceId = o.value(QStringLiteral("device_id")).toString();
    r.nonce = o.value(QStringLiteral("nonce")).toString();
    r.issued = utcSecs(o.value(QStringLiteral("issued")));
    r.offlineUntil = utcSecs(o.value(QStringLiteral("offline_until")));
    QByteArray nonceBytes;
    if (r.v != kVersion || r.kid != kid || !isHex(r.keySha256, 64)
        || r.deviceId.isEmpty() || r.issued < 0 || r.offlineUntil <= r.issued
        || !b64url(o.value(QStringLiteral("nonce")), &nonceBytes) || nonceBytes.size() < 16) {
        return Check::Malformed;
    }

    if (signatureOk) {
        *signatureOk = true;
    }
    if (out) {
        *out = r;
    }

    if (r.product != QLatin1String("gc")) {
        return Check::WrongProduct;
    }
    if (r.deviceId != deviceId) {
        return Check::WrongDevice;
    }
    if (!key.isEmpty() && r.keySha256 != keyHash(key)) {
        return Check::WrongKey;
    }
    if (now < r.issued - kFutureSkewSecs) {
        return Check::IssuedInFuture;
    }
    if (highWater > 0 && now < highWater - kRollbackSecs) {
        return Check::ClockRolledBack;
    }
    if (now >= r.offlineUntil) {
        return Check::Expired;
    }
    return Check::Valid;
}

bool isTimeProblem(Check c)
{
    return c == Check::Expired || c == Check::IssuedInFuture || c == Check::ClockRolledBack;
}

const char *name(Check c)
{
    switch (c) {
    case Check::Valid:           return "valid";
    case Check::Missing:         return "missing";
    case Check::Malformed:       return "malformed";
    case Check::UnknownKey:      return "unknown_key";
    case Check::BadSignature:    return "bad_signature";
    case Check::WrongProduct:    return "wrong_product";
    case Check::WrongDevice:     return "wrong_device";
    case Check::WrongKey:        return "wrong_key";
    case Check::IssuedInFuture:  return "issued_in_future";
    case Check::ClockRolledBack: return "clock_rolled_back";
    case Check::Expired:         return "expired";
    }
    return "unknown";
}

} // namespace LicenseRecord

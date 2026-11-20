#pragma once

/// Signed offline activation records for the license tests (G89).
///
/// Every key pair here is generated inside the test run from random bytes:
/// its private half exists only in the test process and is never written
/// anywhere. The one fixed key is RotorLab's published test fixture (its seed
/// is SHA-256 of a public string), used only to prove GC's Ed25519 and kid
/// derivation agree byte for byte with the RotorLab server's.

#include "Ed25519.h"
#include "LicenseRecord.h"

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>
#include <QtCore/QString>

namespace LicenseTestRecords {

struct Signer {
    QByteArray seed;    ///< private, test process only
    QByteArray pub;
    QString    kid;

    LicenseRecord::PinnedKey pinned() const { return {kid, pub}; }
};

inline Signer makeSigner()
{
    quint32 words[Ed25519::kSeedSize / sizeof(quint32)];
    QRandomGenerator::system()->fillRange(words);
    const QByteArray seed(reinterpret_cast<const char *>(words), Ed25519::kSeedSize);
    Signer s;
    s.seed = seed;
    s.pub = Ed25519::publicKeyFromSeed(seed);
    s.kid = LicenseRecord::kidOf(s.pub);
    return s;
}

inline QString iso(qint64 utcSecs)
{
    return QDateTime::fromSecsSinceEpoch(utcSecs).toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'"));
}

inline QString b64url(const QByteArray &b)
{
    return QString::fromLatin1(b.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

/// The record's eight fields. Tests change single fields on the result.
inline QJsonObject recordObject(const Signer &s, const QString &deviceId, const QString &key,
                                qint64 issued, qint64 offlineUntil)
{
    quint32 words[4];
    QRandomGenerator::system()->fillRange(words);
    const QByteArray nonce(reinterpret_cast<const char *>(words), sizeof(words));
    return QJsonObject{
        {QStringLiteral("v"), 1},
        {QStringLiteral("kid"), s.kid},
        {QStringLiteral("product"), QStringLiteral("gc")},
        {QStringLiteral("key_sha256"), LicenseRecord::keyHash(key)},
        {QStringLiteral("device_id"), deviceId},
        {QStringLiteral("issued"), iso(issued)},
        {QStringLiteral("offline_until"), iso(offlineUntil)},
        {QStringLiteral("nonce"), b64url(nonce)},
    };
}

/// Sorted keys, no white space: what the server's canonical form looks like
/// for these ASCII values.
inline QByteArray bytesOf(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

inline QByteArray signRecord(const Signer &s, const QByteArray &recordBytes)
{
    return Ed25519::sign(s.seed, LicenseRecord::signingInput(recordBytes));
}

inline QJsonObject envelope(const Signer &s, const QByteArray &recordBytes, const QByteArray &signature)
{
    return QJsonObject{
        {QStringLiteral("kid"), s.kid},
        {QStringLiteral("record"), b64url(recordBytes)},
        {QStringLiteral("signature"), b64url(signature)},
    };
}

inline QJsonObject envelope(const Signer &s, const QJsonObject &record)
{
    const QByteArray bytes = bytesOf(record);
    return envelope(s, bytes, signRecord(s, bytes));
}

inline QByteArray envelopeBytes(const QJsonObject &env)
{
    return QJsonDocument(env).toJson(QJsonDocument::Compact);
}

/// A license API reply carrying the envelope.
inline QByteArray reply(const QString &state, const QJsonObject &env)
{
    QJsonObject o{
        {QStringLiteral("ok"), true},
        {QStringLiteral("state"), state},
        {QStringLiteral("product"), QStringLiteral("gc")},
    };
    if (!env.isEmpty()) {
        o.insert(QStringLiteral("offline_record"), env);
    }
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

} // namespace LicenseTestRecords

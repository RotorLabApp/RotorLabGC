/****************************************************************************
 *
 * RotorLab GC - the signed offline activation record (G89).
 *
 * RotorLab signs a short record on every successful activate and verify: this
 * license key (by hash), this device id, product "gc", when it was issued and
 * until when GC may open without a connection. GC checks the Ed25519
 * signature against the RotorLab public keys compiled into the binary, then
 * the fields. Nothing in the settings file can make an unmodified GC binary
 * open: the record has to carry RotorLab's signature.
 *
 * The format RotorLab's server sends, in short:
 *
 *   reply field "offline_record": {"kid": "<16 hex>",
 *                                   "record": "<base64url, no padding>",
 *                                   "signature": "<base64url, no padding>"}
 *   signed message: "rotorlab-gc-offline-record-v1", one zero byte, then the
 *                   record bytes exactly as decoded from "record"
 *   record: canonical JSON (sorted keys, no spaces, ASCII) with exactly
 *           v, kid, product, key_sha256, device_id, issued, offline_until,
 *           nonce
 *
 * This file is pure logic over its inputs (no settings, no clock, no
 * network) so the unit tests can drive every case.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QString>

namespace LicenseRecord {

constexpr int    kVersion         = 1;
constexpr qint64 kFutureSkewSecs  = 10 * 60;        ///< issued may be this far ahead of the local clock
constexpr qint64 kRollbackSecs    = 24 * 60 * 60;   ///< the clock may go back this far before GC refuses
constexpr int    kMaxRecordBytes  = 2048;

/// The signature's domain tag; a zero byte follows it in the signed message.
extern const char kDomain[];
/// The kid's domain tag, the same one the RotorLab license signing keys use.
extern const char kKidTag[];
/// The key hash's domain tag.
extern const char kKeyTag[];

struct PinnedKey {
    QString    kid;         ///< 16 lowercase hex, derived from the key
    QByteArray publicKey;   ///< 32 raw bytes
};

/// The first 16 hex characters of SHA-256 over kKidTag, a zero byte and the
/// raw 32-byte public key (the RotorLab license API's kid).
QString kidOf(const QByteArray &publicKey);

/// Public keys from a list of 64-hex-character entries separated by commas,
/// semicolons or white space. A malformed entry is skipped, never guessed at.
QList<PinnedKey> keysFromHex(const QString &list);

/// The keys this binary trusts: the ones compiled in (RLGC_LICENSE_PUBKEYS,
/// from custom/license-pubkeys.txt) and, in a development build only, the
/// ROTORLAB_LICENSE_PUBKEYS environment variable for a local RotorLab.
QList<PinnedKey> pinnedKeys();

/// The key as the record names it: lowercase hex SHA-256 over kKeyTag, a zero
/// byte, then the key with everything but A-Z and 0-9 removed, upper-cased.
QString keyHash(const QString &key);

struct Record {
    int     v = 0;
    QString kid;
    QString product;
    QString keySha256;
    QString deviceId;
    QString nonce;
    qint64  issued = 0;         ///< UTC seconds since the epoch
    qint64  offlineUntil = 0;   ///< UTC seconds since the epoch
};

enum class Check {
    Valid,
    Missing,            ///< no record at all
    Malformed,          ///< not the contract's shape
    UnknownKey,         ///< kid is not one this binary pins
    BadSignature,
    WrongProduct,
    WrongDevice,
    WrongKey,           ///< key_sha256 is not the stored key's
    IssuedInFuture,     ///< issued more than kFutureSkewSecs ahead of the clock
    ClockRolledBack,    ///< clock more than kRollbackSecs behind the highest time seen
    Expired,            ///< at or past offline_until
};

/// The bytes the signature covers: kDomain, a zero byte, then the record.
QByteArray signingInput(const QByteArray &recordBytes);

/// Checks a stored or received envelope ({"kid","record","signature"} as
/// compact JSON). `key` may be empty when GC does not know it; then the key
/// hash is not compared. `highWater` is the highest time GC has seen (0 for
/// none). `out` gets the record when its signature verified, whatever the
/// later checks say; `signatureOk` says whether it did. A record whose
/// signature does not verify is never parsed.
Check check(const QByteArray &envelope, const QList<PinnedKey> &keys,
            const QString &deviceId, const QString &key,
            qint64 now, qint64 highWater,
            Record *out = nullptr, bool *signatureOk = nullptr);

/// True for the checks that a correct clock and an online re-check cure:
/// the device held a genuine activation and only time ran out or went wrong.
bool isTimeProblem(Check c);

/// For tests and logs.
const char *name(Check c);

} // namespace LicenseRecord

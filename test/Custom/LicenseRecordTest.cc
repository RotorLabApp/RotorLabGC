#include "LicenseRecordTest.h"
#include "LicenseTestRecords.h"

#include "Ed25519.h"
#include "LicenseManager.h"
#include "LicenseRecord.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSettings>
#include <QtCore/QTimeZone>
#include <QtCore/QScopeGuard>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtTest/QTest>

using namespace LicenseTestRecords;
using LicenseRecord::Check;

namespace {

const QString kKey    = QStringLiteral("TEST-0000-0000-0001");
const QString kDevice = QStringLiteral("0123456789abcdef0123456789abcdef");
constexpr qint64 kDay = 24 * 60 * 60;

bool noVehicle()  { return false; }
bool oneVehicle() { return true; }

qint64 realNow()
{
    return QDateTime::currentSecsSinceEpoch();
}

// RFC 8032 section 7.1, TEST 1 (empty message).
const char kRfc1Seed[] = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
const char kRfc1Pub[]  = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
const char kRfc1Sig[]  = "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";
/// The group order L, little-endian.
const char kL[] = "edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010";

bool stored(const char *key)
{
    return QSettings().contains(QLatin1String(key));
}

Check checkRecord(const QJsonObject &env, const Signer &s, qint64 now, qint64 highWater = 0,
          const QString &device = kDevice, const QString &key = kKey,
          LicenseRecord::Record *out = nullptr, bool *sigOk = nullptr)
{
    return LicenseRecord::check(envelopeBytes(env), {s.pinned()}, device, key, now, highWater, out, sigOk);
}

/// A 32-byte little-endian add, for S + L.
QByteArray addLE(const QByteArray &a, const QByteArray &b)
{
    QByteArray r(32, '\0');
    int carry = 0;
    for (int i = 0; i < 32; ++i) {
        const int v = quint8(a[i]) + quint8(b[i]) + carry;
        r[i] = char(v & 0xff);
        carry = v >> 8;
    }
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void LicenseRecordTest::_clearSettings()
{
    QSettings s;
    for (const char *k : {"svc/sr", "svc/lk", "svc/rk", "svc/hw", "svc/mg", "svc/sv"}) {
        s.remove(QLatin1String(k));
    }
    s.sync();
}

void LicenseRecordTest::init()
{
    UnitTest::init();
    _clearSettings();
}

void LicenseRecordTest::cleanup()
{
    _clearSettings();
    UnitTest::cleanup();
}

void LicenseRecordTest::_storeEnvelope(const QByteArray &envelope)
{
    QSettings s;
    s.setValue(QStringLiteral("svc/sr"), envelope);
    s.sync();
}

/// The 5.2.0 record exactly as 5.2.0 wrote it.
void LicenseRecordTest::_storeLegacy(const QString &key, const QString &deviceId)
{
    const QByteArray plain = QJsonDocument(QJsonObject{
        {QStringLiteral("k"), key}, {QStringLiteral("d"), deviceId}}).toJson(QJsonDocument::Compact);
    const QByteArray ks = QCryptographicHash::hash(deviceId.toUtf8(), QCryptographicHash::Sha256);
    QByteArray blob(plain.size(), '\0');
    for (int i = 0; i < plain.size(); ++i) {
        blob[i] = plain[i] ^ ks[i % ks.size()];
    }
    QSettings s;
    s.setValue(QStringLiteral("svc/rk"), blob.toBase64());
    s.sync();
}

QString LicenseRecordTest::_deviceId()
{
    LicenseManager lm;   // creates and persists the id on first use
    return lm._deviceId;
}

// ---------------------------------------------------------------------------
// Ed25519
// ---------------------------------------------------------------------------

void LicenseRecordTest::_ed25519Rfc8032Vector1()
{
    const QByteArray seed = QByteArray::fromHex(kRfc1Seed);
    const QByteArray pub  = QByteArray::fromHex(kRfc1Pub);
    const QByteArray sig  = QByteArray::fromHex(kRfc1Sig);

    QCOMPARE(Ed25519::publicKeyFromSeed(seed).toHex(), pub.toHex());
    QCOMPARE(Ed25519::sign(seed, QByteArray()).toHex(), sig.toHex());
    QVERIFY(Ed25519::verify(sig, QByteArray(), pub));
    QVERIFY(!Ed25519::verify(sig, QByteArrayLiteral("x"), pub));
}

void LicenseRecordTest::_ed25519Rfc8032Vector1FlippedBits()
{
    // TEST 1 with one signature bit flipped, in R and in S: both refused.
    const QByteArray pub = QByteArray::fromHex(kRfc1Pub);
    const QByteArray sig = QByteArray::fromHex(kRfc1Sig);
    QVERIFY(Ed25519::verify(sig, QByteArray(), pub));
    for (const int byte : {0, 31, 32, 40, 63}) {
        for (const int bit : {0, 7}) {
            QByteArray bad = sig;
            bad[byte] = char(bad[byte] ^ (1 << bit));
            QVERIFY2(!Ed25519::verify(bad, QByteArray(), pub),
                     qPrintable(QStringLiteral("byte %1 bit %2").arg(byte).arg(bit)));
        }
    }
    // A public key bit flipped.
    QByteArray badPub = pub;
    badPub[5] = char(badPub[5] ^ 0x10);
    QVERIFY(!Ed25519::verify(sig, QByteArray(), badPub));
}

void LicenseRecordTest::_ed25519Rfc8032Vector1NonCanonicalS()
{
    // TEST 1 with L added to S. [S+L]B is the same point, so the equation
    // still holds; RFC 8032 section 5.1.7 requires S < L, so it is refused.
    const QByteArray pub = QByteArray::fromHex(kRfc1Pub);
    const QByteArray sig = QByteArray::fromHex(kRfc1Sig);
    const QByteArray sPlusL = addLE(sig.mid(32), QByteArray::fromHex(kL));
    QVERIFY(quint8(sPlusL[31]) < 0x20);   // still a 32-byte scalar
    QVERIFY(!Ed25519::verify(sig.left(32) + sPlusL, QByteArray(), pub));
    // S = L exactly is refused too.
    QVERIFY(!Ed25519::verify(sig.left(32) + QByteArray::fromHex(kL), QByteArray(), pub));
}

void LicenseRecordTest::_ed25519MatchesRotorLabFixture()
{
    // The RotorLab server's published test fixture (tests/fixtures/license in
    // the RotorLab repository): a seed anyone can derive, its public key, kid,
    // a signed entitlement statement and its signature, made by the server's
    // Python cryptography signer. GC's Ed25519 and kid must agree byte for byte.
    const QByteArray seed = QCryptographicHash::hash(QByteArrayLiteral("rotorlab-license-signing-test-key-v1"),
                                                     QCryptographicHash::Sha256);
    const QByteArray pub = QByteArray::fromHex("bf3e3dd99f4e69b8f60f204da016d4e496b3fe312047871a154f061fcc112f54");
    const QByteArray statement = QByteArrayLiteral(
        R"({"device_id":"fixture-device-0001","entitlements":["custody","field"],"expires":"2027-06-30",)"
        R"("issued":"2026-11-05T00:00:00Z","key":"RLRS-7FFAP-ZS638-M6Y8K-F6279","kid":"73b5467a18b55e25",)"
        R"("product":"rs","v":1})");
    const QByteArray sig = QByteArray::fromBase64(
        "+i9poewe5qFP/wmXzSO80spVodZaRdgbNyrR7UPvIYKCiTFKhI0hMdo6x1WZyO1PtKQUPL6fFRKHz7gLcCEHDw==");
    QByteArray message("rotorlab-license-entitlement-v1");
    message.append('\0');
    message.append(statement);

    QCOMPARE(Ed25519::publicKeyFromSeed(seed).toHex(), pub.toHex());
    QCOMPARE(LicenseRecord::kidOf(pub), QStringLiteral("73b5467a18b55e25"));
    QVERIFY(Ed25519::verify(sig, message, pub));
    QCOMPARE(Ed25519::sign(seed, message).toHex(), sig.toHex());   // deterministic

    QByteArray badMsg = message;
    badMsg[badMsg.size() - 2] = '2';
    QVERIFY(!Ed25519::verify(sig, badMsg, pub));
    QByteArray badSig = sig;
    badSig[0] = char(badSig[0] ^ 1);
    QVERIFY(!Ed25519::verify(badSig, message, pub));
    // Without the domain tag and its zero byte it is not the same message.
    QVERIFY(!Ed25519::verify(sig, statement, pub));
}

void LicenseRecordTest::_ed25519RefusesNonCanonicalS()
{
    const Signer s = makeSigner();
    const QByteArray msg = QByteArrayLiteral("record");
    const QByteArray sig = Ed25519::sign(s.seed, msg);
    QVERIFY(Ed25519::verify(sig, msg, s.pub));

    // S + L names the same point; RFC 8032 requires S < L, so it is refused.
    const QByteArray malleated = sig.left(32) + addLE(sig.mid(32), QByteArray::fromHex(kL));
    QVERIFY(!Ed25519::verify(malleated, msg, s.pub));

    QVERIFY(!Ed25519::verify(sig.left(63), msg, s.pub));
    QVERIFY(!Ed25519::verify(sig, msg, s.pub.left(31)));
}

// ---------------------------------------------------------------------------
// The record
// ---------------------------------------------------------------------------

void LicenseRecordTest::_validRecord()
{
    const Signer s = makeSigner();
    const qint64 now = realNow();
    LicenseRecord::Record r;
    bool sigOk = false;
    const QJsonObject env = envelope(s, recordObject(s, kDevice, kKey, now - 60, now + 14 * kDay));
    QCOMPARE(checkRecord(env, s, now, now - kDay, kDevice, kKey, &r, &sigOk), Check::Valid);
    QVERIFY(sigOk);
    QCOMPARE(r.deviceId, kDevice);
    QCOMPARE(r.product, QStringLiteral("gc"));
    QCOMPARE(r.kid, s.kid);
    QCOMPARE(r.offlineUntil, now + 14 * kDay);
    // Without a key (GC has not stored one) the key hash is not compared.
    QCOMPARE(checkRecord(env, s, now, 0, kDevice, QString()), Check::Valid);
}

void LicenseRecordTest::_badSignature()
{
    const Signer s = makeSigner();
    const qint64 now = realNow();
    const QByteArray bytes = bytesOf(recordObject(s, kDevice, kKey, now - 60, now + 14 * kDay));
    const QByteArray sig = signRecord(s, bytes);

    // The record edited after signing: a later offline_until.
    QByteArray edited = bytes;
    edited.replace(iso(now + 14 * kDay).toLatin1(), iso(now + 400 * kDay).toLatin1());
    QVERIFY(edited != bytes);
    LicenseRecord::Record r;
    bool sigOk = true;
    QCOMPARE(checkRecord(envelope(s, edited, sig), s, now, 0, kDevice, kKey, &r, &sigOk), Check::BadSignature);
    QVERIFY(!sigOk);
    QVERIFY(r.deviceId.isEmpty());   // never parsed

    // One bit of the signature changed.
    QByteArray badSig = sig;
    badSig[10] = char(badSig[10] ^ 0x01);
    QCOMPARE(checkRecord(envelope(s, bytes, badSig), s, now), Check::BadSignature);

    // Signed by someone else under the pinned key's kid.
    const Signer other = makeSigner();
    QCOMPARE(checkRecord(envelope(s, bytes, signRecord(other, bytes)), s, now), Check::BadSignature);
}

void LicenseRecordTest::_unknownSigningKey()
{
    const Signer pinned = makeSigner();
    const Signer attacker = makeSigner();
    const qint64 now = realNow();
    const QJsonObject env = envelope(attacker, recordObject(attacker, kDevice, kKey, now, now + 14 * kDay));
    QCOMPARE(checkRecord(env, pinned, now), Check::UnknownKey);
    QCOMPARE(LicenseRecord::check(envelopeBytes(env), {}, kDevice, kKey, now, 0), Check::UnknownKey);
}

void LicenseRecordTest::_wrongDevice()
{
    const Signer s = makeSigner();
    const qint64 now = realNow();
    const QJsonObject env = envelope(s, recordObject(s, QStringLiteral("ffffffffffffffffffffffffffffffff"),
                                                     kKey, now, now + 14 * kDay));
    bool sigOk = false;
    QCOMPARE(checkRecord(env, s, now, 0, kDevice, kKey, nullptr, &sigOk), Check::WrongDevice);
    QVERIFY(sigOk);
}

void LicenseRecordTest::_wrongProduct()
{
    const Signer s = makeSigner();
    const qint64 now = realNow();
    QJsonObject rec = recordObject(s, kDevice, kKey, now, now + 14 * kDay);
    rec[QStringLiteral("product")] = QStringLiteral("fs");
    QCOMPARE(checkRecord(envelope(s, rec), s, now), Check::WrongProduct);
}

void LicenseRecordTest::_wrongKey()
{
    const Signer s = makeSigner();
    const qint64 now = realNow();
    const QJsonObject env = envelope(s, recordObject(s, kDevice, QStringLiteral("TEST-0000-0000-0002"),
                                                     now, now + 14 * kDay));
    QCOMPARE(checkRecord(env, s, now), Check::WrongKey);
}

void LicenseRecordTest::_expired()
{
    const Signer s = makeSigner();
    const qint64 issued = realNow() - 14 * kDay;
    const qint64 until = issued + 14 * kDay;
    const QJsonObject env = envelope(s, recordObject(s, kDevice, kKey, issued, until));
    QCOMPARE(checkRecord(env, s, until - 1), Check::Valid);
    QCOMPARE(checkRecord(env, s, until), Check::Expired);
    QCOMPARE(checkRecord(env, s, until + 30 * kDay), Check::Expired);
    QVERIFY(LicenseRecord::isTimeProblem(Check::Expired));
}

void LicenseRecordTest::_futureIssued()
{
    const Signer s = makeSigner();
    const qint64 now = realNow();
    const QJsonObject ahead9 = envelope(s, recordObject(s, kDevice, kKey, now + 9 * 60, now + 14 * kDay));
    const QJsonObject ahead11 = envelope(s, recordObject(s, kDevice, kKey, now + 11 * 60, now + 14 * kDay));
    QCOMPARE(checkRecord(ahead9, s, now), Check::Valid);             // inside the 10-minute skew
    QCOMPARE(checkRecord(ahead11, s, now), Check::IssuedInFuture);
    // A clock set back to before the record was issued.
    const QJsonObject today = envelope(s, recordObject(s, kDevice, kKey, now, now + 14 * kDay));
    QCOMPARE(checkRecord(today, s, now - 3 * kDay), Check::IssuedInFuture);
}

void LicenseRecordTest::_clockRollback()
{
    const Signer s = makeSigner();
    const qint64 now = realNow();
    const QJsonObject env = envelope(s, recordObject(s, kDevice, kKey, now - 5 * kDay, now + 9 * kDay));
    // GC saw a later time before: back by up to a day is tolerated, more is not.
    QCOMPARE(checkRecord(env, s, now, now + 23 * 3600), Check::Valid);
    QCOMPARE(checkRecord(env, s, now, now + kDay + 60), Check::ClockRolledBack);
    // Frozen at a time inside the window after GC saw later ones.
    QCOMPARE(checkRecord(env, s, now - 4 * kDay, now), Check::ClockRolledBack);
}

void LicenseRecordTest::_malformed()
{
    const Signer s = makeSigner();
    const qint64 now = realNow();
    const QJsonObject good = recordObject(s, kDevice, kKey, now, now + 14 * kDay);

    QCOMPARE(LicenseRecord::check(QByteArray(), {s.pinned()}, kDevice, kKey, now, 0), Check::Missing);
    QCOMPARE(LicenseRecord::check(QByteArrayLiteral("not json"), {s.pinned()}, kDevice, kKey, now, 0), Check::Malformed);

    QJsonObject extra = good;
    extra[QStringLiteral("grace_days")] = 999;
    QCOMPARE(checkRecord(envelope(s, extra), s, now), Check::Malformed);

    QJsonObject missing = good;
    missing.remove(QStringLiteral("nonce"));
    QCOMPARE(checkRecord(envelope(s, missing), s, now), Check::Malformed);

    QJsonObject v2 = good;
    v2[QStringLiteral("v")] = 2;
    QCOMPARE(checkRecord(envelope(s, v2), s, now), Check::Malformed);

    QJsonObject otherKid = good;
    otherKid[QStringLiteral("kid")] = QStringLiteral("0000000000000000");
    QCOMPARE(checkRecord(envelope(s, otherKid), s, now), Check::Malformed);

    QJsonObject backwards = good;
    backwards[QStringLiteral("offline_until")] = iso(now - kDay);
    QCOMPARE(checkRecord(envelope(s, backwards), s, now), Check::Malformed);

    QJsonObject localTime = good;
    localTime[QStringLiteral("issued")] = QStringLiteral("2026-11-17T10:00:00+01:00");
    QCOMPARE(checkRecord(envelope(s, localTime), s, now), Check::Malformed);

    // Standard base64 with padding is not the contract's encoding.
    QJsonObject padded = envelope(s, good);
    padded[QStringLiteral("signature")] = padded.value(QStringLiteral("signature")).toString() + QStringLiteral("==");
    QCOMPARE(checkRecord(padded, s, now), Check::Malformed);

    QJsonObject shortSig = envelope(s, good);
    shortSig[QStringLiteral("signature")] = b64url(QByteArray(63, 'x'));
    QCOMPARE(checkRecord(shortSig, s, now), Check::Malformed);
}

void LicenseRecordTest::_pinnedKeysFromHex()
{
    const Signer a = makeSigner();
    const Signer b = makeSigner();
    const QString list = QString::fromLatin1(a.pub.toHex()).toUpper() + QStringLiteral(" ; not-a-key,")
                         + QString::fromLatin1(b.pub.toHex()) + QStringLiteral("\n")
                         + QString::fromLatin1(a.pub.toHex()) + QStringLiteral(",abcd");
    const QList<LicenseRecord::PinnedKey> keys = LicenseRecord::keysFromHex(list);
    QCOMPARE(keys.size(), 2);
    QCOMPARE(keys.at(0).kid, a.kid);
    QCOMPARE(keys.at(0).publicKey, a.pub);
    QCOMPARE(keys.at(1).kid, b.kid);
    QVERIFY(LicenseRecord::keysFromHex(QString()).isEmpty());
}

void LicenseRecordTest::_keyHashNormalizes()
{
    const QString h = LicenseRecord::keyHash(QStringLiteral("RLGC-ABCDE-23456-FGHJK-789AB"));
    QCOMPARE(h.size(), 64);
    QCOMPARE(LicenseRecord::keyHash(QStringLiteral("rlgc abcde 23456 fghjk 789ab")), h);
    QCOMPARE(LicenseRecord::keyHash(QStringLiteral("RLGCABCDE23456FGHJK789AB")), h);
    QVERIFY(LicenseRecord::keyHash(QStringLiteral("RLGC-ABCDE-23456-FGHJK-789AC")) != h);

    QByteArray in("rotorlab-gc-license-key-v1");
    in.append('\0');
    in.append("RLGCABCDE23456FGHJK789AB");
    QCOMPARE(h, QString::fromLatin1(QCryptographicHash::hash(in, QCryptographicHash::Sha256).toHex()));
}

// ---------------------------------------------------------------------------
// RotorLab's own record fixtures
// ---------------------------------------------------------------------------

void LicenseRecordTest::_rotorlabServerFixtureRecord()
{
    // RotorLab 3.7.0 tests/fixtures/license/gc-record.json and gc-record.sig
    // (sha256 d94a5bed... and a96af17d...), made by the server with the shared
    // fixture seed. Fixed bytes: checks GC against the server's signer, record
    // shape and key hash.
    const QByteArray seed = QCryptographicHash::hash(QByteArrayLiteral("rotorlab-license-signing-test-key-v1"),
                                                     QCryptographicHash::Sha256);
    const QByteArray pub = QByteArray::fromHex("bf3e3dd99f4e69b8f60f204da016d4e496b3fe312047871a154f061fcc112f54");
    const LicenseRecord::PinnedKey pinned{QStringLiteral("73b5467a18b55e25"), pub};
    const QString key = QStringLiteral("RLGC-ABCDE-23456-FGHJK-789AB");
    const QString device = QStringLiteral("8f3c0b6e2a7d4c19b5e0f1a2c3d4e5f6");
    const QByteArray record = QByteArrayLiteral(
        R"({"device_id":"8f3c0b6e2a7d4c19b5e0f1a2c3d4e5f6","issued":"2026-11-20T09:15:02Z",)"
        R"("key_sha256":"dcfe45da9c180a0bb6147ff136d6c52a492fb8425038f0b242c08bf51f284572",)"
        R"("kid":"73b5467a18b55e25","nonce":"q3V1c2Vfb25seV9vbmNlIQ","offline_until":"2026-12-04T09:15:02Z",)"
        R"("product":"gc","v":1})");
    const QString sigB64 = QStringLiteral("5FElLimRQ4RbkTkqu5-y7EtB0l9NkXO9GU9s1RnxxNtO7aTzc_3b36ISD6lXjWA0wKJDakszZvDQJoy6sictBg");
    const QByteArray envelope = QJsonDocument(QJsonObject{
        {QStringLiteral("kid"), pinned.kid},
        {QStringLiteral("record"), b64url(record)},
        {QStringLiteral("signature"), sigB64}}).toJson(QJsonDocument::Compact);

    QCOMPARE(LicenseRecord::kidOf(pub), pinned.kid);
    QCOMPARE(LicenseRecord::keyHash(key), QStringLiteral("dcfe45da9c180a0bb6147ff136d6c52a492fb8425038f0b242c08bf51f284572"));
    // Ed25519 is deterministic: GC's signing input gives the server's bytes.
    QCOMPARE(b64url(Ed25519::sign(seed, LicenseRecord::signingInput(record))), sigB64);

    const qint64 issued = QDateTime(QDate(2026, 11, 20), QTime(9, 15, 2), QTimeZone::UTC).toSecsSinceEpoch();
    const qint64 until  = QDateTime(QDate(2026, 12, 4), QTime(9, 15, 2), QTimeZone::UTC).toSecsSinceEpoch();
    LicenseRecord::Record r;
    QCOMPARE(LicenseRecord::check(envelope, {pinned}, device, key, issued + kDay, 0, &r), Check::Valid);
    QCOMPARE(r.offlineUntil, until);
    QCOMPARE(r.nonce, QStringLiteral("q3V1c2Vfb25seV9vbmNlIQ"));
    QCOMPARE(LicenseRecord::check(envelope, {pinned}, device, QStringLiteral("rlgc abcde 23456 fghjk 789ab"),
                                  issued + kDay, 0), Check::Valid);
    QCOMPARE(LicenseRecord::check(envelope, {pinned}, device, QStringLiteral("RLGC-ABCDE-23456-FGHJK-789AC"),
                                  issued + kDay, 0), Check::WrongKey);
    QCOMPARE(LicenseRecord::check(envelope, {pinned}, kDevice, key, issued + kDay, 0), Check::WrongDevice);
    QCOMPARE(LicenseRecord::check(envelope, {pinned}, device, key, until, 0), Check::Expired);
    QCOMPARE(LicenseRecord::check(envelope, {pinned}, device, key, issued - 11 * 60, 0), Check::IssuedInFuture);
}

void LicenseRecordTest::_g98Fixtures()
{
    // RotorLab server test fixtures (made 2026-11-17T16:52:11Z), copied in
    // as constants: re-running the server's
    // fixture script makes new ones. The public key is a throwaway TEST key
    // used only in this test; it is never pinned in custom/license-pubkeys.txt.
    // The license key is not in the fixtures, so the key hash is not compared.
    const QByteArray pub = QByteArray::fromHex("28aef4582a35bd2044c70b3ecc3080f16f85c461932b086b724dbd09cda87bd1");
    const LicenseRecord::PinnedKey pinned{QStringLiteral("9bbfc697e4ec3fd3"), pub};
    QCOMPARE(LicenseRecord::kidOf(pub), pinned.kid);
    const QString device = QStringLiteral("0123456789abcdef0123456789abcdef");
    const qint64 made = QDateTime(QDate(2026, 11, 17), QTime(16, 52, 11), QTimeZone::UTC).toSecsSinceEpoch();

    struct Case { const char *name; const char *kid; const char *record; const char *signature; Check expect; };
    const Case cases[] = {
        {"valid", "9bbfc697e4ec3fd3",
         "eyJkZXZpY2VfaWQiOiIwMTIzNDU2Nzg5YWJjZGVmMDEyMzQ1Njc4OWFiY2RlZiIsImlzc3VlZCI6IjIwMjYtMTEtMTdUMTY6NTI6MTFaIiwia2V5X3NoYTI1NiI6IjM3NThjNDIwZWYzYWNkNzZlYWFiMzI4NjNlMTBkZDY5MTlkNDg1ODA5NjI0MTBmMzEyN2Y4MDFkYzY2ZTg2NGIiLCJraWQiOiI5YmJmYzY5N2U0ZWMzZmQzIiwibm9uY2UiOiJlZEFrekk3M2JUZTVBLTZadUhTcXBBIiwib2ZmbGluZV91bnRpbCI6IjIwMjYtMTItMDFUMTY6NTI6MTFaIiwicHJvZHVjdCI6ImdjIiwidiI6MX0",
         "xxyfg2JQPI0-nMk5_CCSGwioq2MQGJaDj2X5vJhqiWktiXktrQPdP7ors4U3KN6cUmLuYlWS6QfIOrOjZy0YBw",
         Check::Valid},
        {"expired", "9bbfc697e4ec3fd3",
         "eyJkZXZpY2VfaWQiOiIwMTIzNDU2Nzg5YWJjZGVmMDEyMzQ1Njc4OWFiY2RlZiIsImlzc3VlZCI6IjIwMjYtMTAtMjhUMTY6NTI6MTFaIiwia2V5X3NoYTI1NiI6IjM3NThjNDIwZWYzYWNkNzZlYWFiMzI4NjNlMTBkZDY5MTlkNDg1ODA5NjI0MTBmMzEyN2Y4MDFkYzY2ZTg2NGIiLCJraWQiOiI5YmJmYzY5N2U0ZWMzZmQzIiwibm9uY2UiOiJFVm9TNG9wVlpDandrOWFoNU1rWWxBIiwib2ZmbGluZV91bnRpbCI6IjIwMjYtMTEtMTFUMTY6NTI6MTFaIiwicHJvZHVjdCI6ImdjIiwidiI6MX0",
         "LrPN6bx1DOkAM98UVEUrwWLcsQd9VBCXRFO4NmoiLkHGTD-vik3HI5wa2RwIbasMynJeEfDVyizCVhL2EOrgAw",
         Check::Expired},
        {"other_device", "9bbfc697e4ec3fd3",
         "eyJkZXZpY2VfaWQiOiJmZWRjYmE5ODc2NTQzMjEwZmVkY2JhOTg3NjU0MzIxMCIsImlzc3VlZCI6IjIwMjYtMTEtMTdUMTY6NTI6MTFaIiwia2V5X3NoYTI1NiI6IjM3NThjNDIwZWYzYWNkNzZlYWFiMzI4NjNlMTBkZDY5MTlkNDg1ODA5NjI0MTBmMzEyN2Y4MDFkYzY2ZTg2NGIiLCJraWQiOiI5YmJmYzY5N2U0ZWMzZmQzIiwibm9uY2UiOiI1V0xqNmtGb1g4bzZybS1FMjdLblhRIiwib2ZmbGluZV91bnRpbCI6IjIwMjYtMTItMDFUMTY6NTI6MTFaIiwicHJvZHVjdCI6ImdjIiwidiI6MX0",
         "3dEVRwh7JdBwzik-l576rKajl1N2kvxoo629rWqEUfqzL9Sgo4g7FYP73jjMItKFaRQUDNVPiSKHv4VfrWhoCA",
         Check::WrongDevice},
        {"tampered", "9bbfc697e4ec3fd3",
         "eyJkZXZpY2VfaWQiOiIxMTIzNDU2Nzg5YWJjZGVmMDEyMzQ1Njc4OWFiY2RlZiIsImlzc3VlZCI6IjIwMjYtMTEtMTdUMTY6NTI6MTFaIiwia2V5X3NoYTI1NiI6IjM3NThjNDIwZWYzYWNkNzZlYWFiMzI4NjNlMTBkZDY5MTlkNDg1ODA5NjI0MTBmMzEyN2Y4MDFkYzY2ZTg2NGIiLCJraWQiOiI5YmJmYzY5N2U0ZWMzZmQzIiwibm9uY2UiOiJNZEZDVlB2YzJwdmUxS2ZlMDdqRndnIiwib2ZmbGluZV91bnRpbCI6IjIwMjYtMTItMDFUMTY6NTI6MTFaIiwicHJvZHVjdCI6ImdjIiwidiI6MX0",
         "HjnVis6zqQ31s-nHnzlMDwF1TlKHsUsPrjBeahYmc7Sk1vs5fN04v73uP6RAqkFkNWYKz1RCFB5in7oOLpe2AQ",
         Check::BadSignature},
        {"unknown_kid", "0000000000000000",
         "eyJkZXZpY2VfaWQiOiIwMTIzNDU2Nzg5YWJjZGVmMDEyMzQ1Njc4OWFiY2RlZiIsImlzc3VlZCI6IjIwMjYtMTEtMTdUMTY6NTI6MTFaIiwia2V5X3NoYTI1NiI6IjM3NThjNDIwZWYzYWNkNzZlYWFiMzI4NjNlMTBkZDY5MTlkNDg1ODA5NjI0MTBmMzEyN2Y4MDFkYzY2ZTg2NGIiLCJraWQiOiI5YmJmYzY5N2U0ZWMzZmQzIiwibm9uY2UiOiJHNlRFWWd4WlppUWplQnA0aFliRnVBIiwib2ZmbGluZV91bnRpbCI6IjIwMjYtMTItMDFUMTY6NTI6MTFaIiwicHJvZHVjdCI6ImdjIiwidiI6MX0",
         "pyaOvP6JNt5BJZcFRQZNkaVAO27VR4M1RecSatMgEuFHAD32dk8zW2sDgngfOzveuApsEZn20KgJI5lSQijOBQ",
         Check::UnknownKey},
        {"issued_ahead", "9bbfc697e4ec3fd3",
         "eyJkZXZpY2VfaWQiOiIwMTIzNDU2Nzg5YWJjZGVmMDEyMzQ1Njc4OWFiY2RlZiIsImlzc3VlZCI6IjIwMjYtMTEtMTdUMTc6NTI6MTFaIiwia2V5X3NoYTI1NiI6IjM3NThjNDIwZWYzYWNkNzZlYWFiMzI4NjNlMTBkZDY5MTlkNDg1ODA5NjI0MTBmMzEyN2Y4MDFkYzY2ZTg2NGIiLCJraWQiOiI5YmJmYzY5N2U0ZWMzZmQzIiwibm9uY2UiOiJCRU04ZExuenNUQ2E3UU9JYVQxdHB3Iiwib2ZmbGluZV91bnRpbCI6IjIwMjYtMTItMDFUMTc6NTI6MTFaIiwicHJvZHVjdCI6ImdjIiwidiI6MX0",
         "1WrzqtVLpV1BA87RoV4FEwxwC7SbFwoDlkhdxabRsvhYiowTkoJ818MVBJgwOnyuEYveuyMVQpWnKZD73CNYCA",
         Check::IssuedInFuture},
    };
    for (const Case &c : cases) {
        const QByteArray envelope = QJsonDocument(QJsonObject{
            {QStringLiteral("kid"), QLatin1String(c.kid)},
            {QStringLiteral("record"), QLatin1String(c.record)},
            {QStringLiteral("signature"), QLatin1String(c.signature)}}).toJson(QJsonDocument::Compact);
        const Check got = LicenseRecord::check(envelope, {pinned}, device, QString(), made, 0);
        QVERIFY2(got == c.expect, qPrintable(QStringLiteral("%1: got %2, expected %3")
                                                 .arg(QLatin1String(c.name), QLatin1String(LicenseRecord::name(got)),
                                                      QLatin1String(LicenseRecord::name(c.expect)))));
    }
}

// ---------------------------------------------------------------------------
// The gate at start
// ---------------------------------------------------------------------------

void LicenseRecordTest::_startValidRecordOpens()
{
    const QString dev = _deviceId();
    const Signer s = makeSigner();
    const qint64 now = realNow();
    _storeEnvelope(envelopeBytes(envelope(s, recordObject(s, dev, kKey, now - 3600, now + 14 * kDay))));

    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    lm._pinned = {s.pinned()};
    lm._nowOverride = now;
    lm._storeKey(kKey);
    lm._loadRecord();
    QVERIFY(lm.activated());
    QCOMPARE(lm._key, kKey);
    QVERIFY(lm.statusText().contains(QStringLiteral("can open without a connection until")));

    // The same record with no pinned key (a build that does not trust it).
    lm._pinned.clear();
    lm._loadRecord();
    QVERIFY(!lm.activated());
}

void LicenseRecordTest::_startEditedRecordStaysClosed()
{
    const QString dev = _deviceId();
    const Signer s = makeSigner();
    const qint64 now = realNow();
    const QByteArray bytes = bytesOf(recordObject(s, dev, kKey, now - 20 * kDay, now - 6 * kDay));
    const QByteArray sig = signRecord(s, bytes);

    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    lm._pinned = {s.pinned()};
    lm._nowOverride = now;
    lm._storeKey(kKey);

    // Somebody moved offline_until in the settings file.
    QByteArray edited = bytes;
    edited.replace(iso(now - 6 * kDay).toLatin1(), iso(now + 365 * kDay).toLatin1());
    _storeEnvelope(envelopeBytes(envelope(s, edited, sig)));
    lm._loadRecord();
    QVERIFY(!lm.activated());
    QVERIFY2(lm.statusText().contains(QStringLiteral("could not verify the stored activation")),
             qPrintable(lm.statusText()));
    QCOMPARE(lm._offlineUntil, qint64(0));   // the record is not used

    // A record signed with a key of their own.
    const Signer attacker = makeSigner();
    _storeEnvelope(envelopeBytes(envelope(attacker, recordObject(attacker, dev, kKey, now, now + 365 * kDay))));
    lm._loadRecord();
    QVERIFY(!lm.activated());

    // The 5.2.0 shape written where the signed record goes.
    _storeEnvelope(QByteArrayLiteral("eyJrIjoiVEVTVCIsImQiOiJ4In0="));
    lm._loadRecord();
    QVERIFY(!lm.activated());

    // The key stays, so the gate's re-check can still ask RotorLab.
    QCOMPARE(lm._key, kKey);
}

void LicenseRecordTest::_startWrongDeviceStaysClosed()
{
    const QString dev = _deviceId();
    const Signer s = makeSigner();
    const qint64 now = realNow();
    // A genuine record copied from another install.
    _storeEnvelope(envelopeBytes(envelope(s, recordObject(s, QStringLiteral("ffffffffffffffffffffffffffffffff"),
                                                          kKey, now, now + 14 * kDay))));
    LicenseManager lm;
    lm._vehicleConnected = &oneVehicle;   // not a time problem: a vehicle does not open it
    lm._pinned = {s.pinned()};
    lm._nowOverride = now;
    lm._loadRecord();
    QVERIFY(!lm.activated());
    QVERIFY(lm.statusText().contains(QStringLiteral("for a different device")));
    Q_UNUSED(dev)
}

void LicenseRecordTest::_startExpiredAsksForConnection()
{
    const QString dev = _deviceId();
    const Signer s = makeSigner();
    const qint64 now = realNow();
    _storeEnvelope(envelopeBytes(envelope(s, recordObject(s, dev, kKey, now - 15 * kDay, now - kDay))));
    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    lm._pinned = {s.pinned()};
    lm._nowOverride = now;
    lm._storeKey(kKey);
    lm._loadRecord();
    QVERIFY(!lm.activated());
    QCOMPARE(lm.statusText(),
             QStringLiteral("Activation expired. Connect to the internet so RotorLab GC can check the license."));
    QCOMPARE(lm._key, kKey);   // the gate's re-check sends it
}

void LicenseRecordTest::_startFutureIssuedRefused()
{
    const QString dev = _deviceId();
    const Signer s = makeSigner();
    const qint64 now = realNow();
    _storeEnvelope(envelopeBytes(envelope(s, recordObject(s, dev, kKey, now, now + 14 * kDay))));
    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    lm._pinned = {s.pinned()};
    lm._nowOverride = now - 2 * kDay;   // clock set back before the record was issued
    lm._loadRecord();
    QVERIFY(!lm.activated());
    QCOMPARE(lm.statusText(), LicenseManager::clockText());
}

void LicenseRecordTest::_startClockRollbackRefused()
{
    const QString dev = _deviceId();
    const Signer s = makeSigner();
    const qint64 issued = realNow() - 10 * kDay;
    _storeEnvelope(envelopeBytes(envelope(s, recordObject(s, dev, kKey, issued, issued + 14 * kDay))));

    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    lm._pinned = {s.pinned()};
    // A start on day 13 records the time ...
    lm._nowOverride = issued + 13 * kDay;
    lm._loadRecord();
    QVERIFY(lm.activated());
    QCOMPARE(QSettings().value(QStringLiteral("svc/hw")).toLongLong(), issued + 13 * kDay);
    // ... so a clock set back to day 3 to stretch the window is refused.
    lm._nowOverride = issued + 3 * kDay;
    lm._loadRecord();
    QVERIFY(!lm.activated());
    QCOMPARE(lm.statusText(), LicenseManager::clockText());
    // Back by less than a day (a time zone or DST mix-up) still opens.
    lm._nowOverride = issued + 13 * kDay - 3600;
    lm._loadRecord();
    QVERIFY(lm.activated());
}

// ---------------------------------------------------------------------------
// Online replies
// ---------------------------------------------------------------------------

void LicenseRecordTest::_activateNeedsSignedRecord()
{
    const Signer s = makeSigner();
    const Signer attacker = makeSigner();
    const qint64 now = realNow();
    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    lm._pinned = {s.pinned()};
    lm._nowOverride = now;
    lm._loadRecord();
    QVERIFY(!lm.activated());
    const QString dev = lm._deviceId;

    // "activated" alone, as a 3.4 server or anything answering in its place sends.
    lm._handleReply(LicenseManager::Op::Activate, kKey, reply(QStringLiteral("activated"), {}), true, 200);
    QVERIFY(!lm.activated());
    QVERIFY(!stored("svc/sr"));
    QVERIFY(lm.statusText().contains(QStringLiteral("could not verify the license server's reply")));

    // Signed by a key GC does not pin.
    lm._handleReply(LicenseManager::Op::Activate, kKey,
                    reply(QStringLiteral("activated"),
                          envelope(attacker, recordObject(attacker, dev, kKey, now, now + 14 * kDay))),
                    true, 200);
    QVERIFY(!lm.activated());

    // For another key.
    lm._handleReply(LicenseManager::Op::Activate, kKey,
                    reply(QStringLiteral("activated"),
                          envelope(s, recordObject(s, dev, QStringLiteral("TEST-9999-9999-9999"), now, now + 14 * kDay))),
                    true, 200);
    QVERIFY(!lm.activated());

    // The real thing.
    lm._handleReply(LicenseManager::Op::Activate, kKey,
                    reply(QStringLiteral("activated"), envelope(s, recordObject(s, dev, kKey, now, now + 14 * kDay))),
                    true, 200);
    QVERIFY(lm.activated());
    QVERIFY(stored("svc/sr"));
    QVERIFY(stored("svc/lk"));
    QVERIFY(QSettings().value(QStringLiteral("svc/sv")).toBool());
    QCOMPARE(lm._offlineUntil, now + 14 * kDay);

    // And the next start opens offline from it.
    LicenseManager next;
    next._pinned = {s.pinned()};
    next._nowOverride = now + kDay;
    next._loadRecord();
    QVERIFY(next.activated());
    QCOMPARE(next._key, kKey);
}

void LicenseRecordTest::_verifyRefreshesRecordAndResetsClockMark()
{
    const Signer s = makeSigner();
    const qint64 now = realNow();
    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    lm._pinned = {s.pinned()};
    lm._nowOverride = now;
    const QString dev = lm._deviceId;

    // The clock once ran a month ahead; GC remembers that time.
    {
        QSettings st;
        st.setValue(QStringLiteral("svc/hw"), now + 30 * kDay);
        st.sync();
    }
    lm._key = kKey;
    lm._inSession = false;
    lm._handleReply(LicenseManager::Op::Verify, kKey,
                    reply(QStringLiteral("valid"), envelope(s, recordObject(s, dev, kKey, now, now + 14 * kDay))),
                    true, 200);
    QVERIFY(lm.activated());
    // RotorLab's time replaced the stale mark, so the next start opens.
    QCOMPARE(QSettings().value(QStringLiteral("svc/hw")).toLongLong(), now);
    lm._loadRecord();
    QVERIFY(lm.activated());
}

// ---------------------------------------------------------------------------
// Migration from 5.2.0
// ---------------------------------------------------------------------------

void LicenseRecordTest::_legacyRecordNeverOpensOffline()
{
    // A 5.2.0 record has no
    // signature and anyone can write one, so it never opens GC offline, on
    // the first start or any later one, at any date. Its key is kept only
    // for the online check.
    const QString dev = _deviceId();
    _storeLegacy(kKey, dev);
    const qint64 now = realNow();
    const qint64 later = QDateTime(QDate(2035, 1, 1), QTime(12, 0), QTimeZone::UTC).toSecsSinceEpoch();

    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    for (const qint64 t : { now, now + 1, now + 3 * kDay, now + 8 * kDay, later, now - 2 * kDay }) {
        lm._nowOverride = t;
        lm._loadRecord();
        QVERIFY2(!lm.activated(), qPrintable(QString::number(t)));
        QCOMPARE(lm.statusText(), LicenseManager::legacyText());
        QCOMPARE(lm._key, kKey);              // still sent by the gate's online check
        QVERIFY(!lm._timeLapsed);
        QVERIFY(stored("svc/rk"));            // kept until a signed record replaces it
        QVERIFY(!stored("svc/mg"));           // no grace start is recorded any more
    }
    QVERIFY(lm.statusText().contains(QStringLiteral("must connect to the internet once to activate")));

    // A fresh manager, as on a real restart.
    LicenseManager next;
    next._vehicleConnected = &noVehicle;
    next._nowOverride = now;
    next._loadRecord();
    QVERIFY(!next.activated());
    QCOMPARE(next.statusText(), LicenseManager::legacyText());
}

void LicenseRecordTest::_legacyRecordNotLiftedByVehicle()
{
    // A connected vehicle never opens GC on an unsigned record (contract
    // section 8): not at start and not when it connects while the gate is up.
    // This covers what used to be the past-grace and clock-back cases (M1).
    const QString dev = _deviceId();
    _storeLegacy(kKey, dev);
    const qint64 now = realNow();

    for (const qint64 t : { now, now + 8 * kDay, now - 2 * kDay }) {
        LicenseManager lm;
        lm._vehicleConnected = &oneVehicle;
        lm._nowOverride = t;
        lm._loadRecord();
        QVERIFY2(!lm.activated(), qPrintable(QString::number(t)));
        QVERIFY(!lm._timeLapsed);
        QVERIFY(!lm._inSession);
        QVERIFY(!lm.attention());
        QCOMPARE(lm.statusText(), LicenseManager::legacyText());
        lm._vehicleAppeared();
        QVERIFY(!lm.activated());
        QCOMPARE(lm.statusText(), LicenseManager::legacyText());
    }

    // No vehicle at start, one connects later.
    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    lm._nowOverride = now;
    lm._loadRecord();
    QVERIFY(!lm.activated());
    lm._vehicleConnected = &oneVehicle;
    lm._vehicleAppeared();
    QVERIFY(!lm.activated());

    // The clock set back behind the recorded high-water mark: still closed.
    {
        QSettings st;
        st.setValue(QStringLiteral("svc/hw"), now + 5 * kDay);
        st.sync();
    }
    lm._nowOverride = now;
    lm._loadRecord();
    QVERIFY(!lm.activated());
    lm._vehicleAppeared();
    QVERIFY(!lm.activated());
}

void LicenseRecordTest::_legacyKeySentOnline()
{
#ifndef QGC_DAILY_BUILD
    QSKIP("Needs the Daily-only license server address hook to point GC at a local listener.");
#else
    // The gate's re-check (recheck(), called when the gate shows) sends the
    // legacy key to the online check, and the signed record that comes back
    // opens GC and replaces the old record. The "server" is a listener on
    // 127.0.0.1 only; nothing leaves this machine.
    const QString dev = _deviceId();
    _storeLegacy(kKey, dev);
    const Signer s = makeSigner();
    const qint64 now = realNow();
    const QByteArray answer = reply(QStringLiteral("valid"),
                                    envelope(s, recordObject(s, dev, kKey, now, now + 14 * kDay)));

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QByteArray request;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, &request, &answer]() {
        QTcpSocket *sock = server.nextPendingConnection();
        QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, &request, &answer]() {
            request += sock->readAll();
            const qsizetype end = request.indexOf("\r\n\r\n");
            if (end < 0) {
                return;
            }
            qsizetype length = 0;
            for (const QByteArray &line : request.left(end).split('\n')) {
                if (line.trimmed().toLower().startsWith("content-length:")) {
                    length = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                }
            }
            if (request.size() < end + 4 + length) {
                return;
            }
            sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                        + QByteArray::number(answer.size()) + "\r\n\r\n" + answer);
            sock->disconnectFromHost();
        });
        QObject::connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    });

    const QByteArray oldBase = qgetenv("ROTORLAB_BASE_URL");
    const bool hadBase = qEnvironmentVariableIsSet("ROTORLAB_BASE_URL");
    qputenv("ROTORLAB_BASE_URL", "http://127.0.0.1:" + QByteArray::number(server.serverPort()));
    const auto restore = qScopeGuard([&]() {
        if (hadBase) {
            qputenv("ROTORLAB_BASE_URL", oldBase);
        } else {
            qunsetenv("ROTORLAB_BASE_URL");
        }
    });

    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    lm._pinned = {s.pinned()};
    lm._nowOverride = now;
    lm._loadRecord();
    QVERIFY(!lm.activated());
    QCOMPARE(lm._key, kKey);

    lm.recheck();
    QTRY_VERIFY_WITH_TIMEOUT(!lm.busy(), 10000);

    // What was sent: a verify carrying the key from the old record.
    QVERIFY2(request.startsWith("POST /api/license/verify "), request.left(80).constData());
    const QByteArray body = request.mid(request.indexOf("\r\n\r\n") + 4);
    const QJsonObject sent = QJsonDocument::fromJson(body).object();
    QCOMPARE(sent.value(QStringLiteral("key")).toString(), kKey);
    QCOMPARE(sent.value(QStringLiteral("device_id")).toString(), dev);

    // What came back opened GC and replaced the old record.
    QVERIFY2(lm.activated(), qPrintable(lm.statusText()));
    QVERIFY(!stored("svc/rk"));
    QVERIFY(stored("svc/sr"));
    QVERIFY(stored("svc/lk"));
#endif
}

void LicenseRecordTest::_legacyGateOpensOnSignedReply()
{
    // The gate is up for a 5.2.0 record only. When the online check (the
    // launch verify or a later re-check) brings a signed record for this
    // device, GC opens in the same session, as when a key is typed, with or
    // without a vehicle connected.
    const QString dev = _deviceId();
    const Signer s = makeSigner();
    const qint64 now = realNow();
    const QByteArray good = reply(QStringLiteral("valid"),
                                  envelope(s, recordObject(s, dev, kKey, now, now + 14 * kDay)));

    for (const bool vehicle : { false, true }) {
        for (const bool offlineFirst : { false, true }) {
            const QString what = QStringLiteral("vehicle %1, offline first %2").arg(vehicle).arg(offlineFirst);
            _clearSettings();
            _storeLegacy(kKey, dev);
            LicenseManager lm;
            lm._vehicleConnected = vehicle ? &oneVehicle : &noVehicle;
            lm._pinned = {s.pinned()};
            lm._nowOverride = now;
            lm._loadRecord();
            QVERIFY2(!lm.activated(), qPrintable(what));
            QCOMPARE(lm.statusText(), LicenseManager::legacyText());

            lm._inSession = false;
            if (offlineFirst) {
                // The launch verify finds no network; the gate stays up.
                lm._handleReply(LicenseManager::Op::Verify, kKey, {}, false, 0);
                QVERIFY2(!lm.activated(), qPrintable(what));
                QVERIFY(lm._inSession);
            }
            // The launch verify, or the re-check once the network is back.
            lm._handleReply(LicenseManager::Op::Verify, kKey, good, true, 200);
            QVERIFY2(lm.activated(), qPrintable(what + QStringLiteral(" -> ") + lm.statusText()));
            QVERIFY(!lm.attention());
            QVERIFY(!stored("svc/rk"));   // the old record is replaced
            QVERIFY(stored("svc/sr"));
            QVERIFY(stored("svc/lk"));
        }
    }

    // Unchanged: a reply GC cannot verify does not open the legacy gate.
    _clearSettings();
    _storeLegacy(kKey, dev);
    {
        const Signer attacker = makeSigner();
        LicenseManager lm;
        lm._vehicleConnected = &oneVehicle;
        lm._pinned = {s.pinned()};
        lm._nowOverride = now;
        lm._loadRecord();
        lm._inSession = false;
        lm._handleReply(LicenseManager::Op::Verify, kKey,
                        reply(QStringLiteral("valid"),
                              envelope(attacker, recordObject(attacker, dev, kKey, now, now + 14 * kDay))),
                        true, 200);
        QVERIFY(!lm.activated());
        lm._handleReply(LicenseManager::Op::Verify, kKey, reply(QStringLiteral("valid"), {}), true, 200);
        QVERIFY(!lm.activated());
        QVERIFY(stored("svc/rk"));
    }

    // Unchanged: a gate that is up for a signed record that does not verify
    // (not a legacy gate) still only notes a re-check while running.
    _clearSettings();
    {
        const Signer attacker = makeSigner();
        _storeEnvelope(envelopeBytes(envelope(attacker, recordObject(attacker, dev, kKey, now - 15 * kDay, now - kDay))));
        LicenseManager lm;
        lm._vehicleConnected = &oneVehicle;
        lm._pinned = {s.pinned()};
        lm._nowOverride = now;
        lm._storeKey(kKey);
        lm._loadRecord();
        QVERIFY(!lm.activated());
        QVERIFY(!lm._legacyGate);
        lm._inSession = false;
        lm._handleReply(LicenseManager::Op::Verify, kKey, good, true, 200);
        QVERIFY(!lm.activated());
    }
}

void LicenseRecordTest::_legacyRecordReplacedBySignedRecord()
{
    const QString dev = _deviceId();
    _storeLegacy(kKey, dev);
    const Signer s = makeSigner();
    const qint64 now = realNow();

    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    lm._pinned = {s.pinned()};
    lm._nowOverride = now;
    lm._loadRecord();
    QVERIFY(!lm.activated());          // the old record never opens GC offline
    QCOMPARE(lm.statusText(), LicenseManager::legacyText());
    QCOMPARE(lm._key, kKey);
    QVERIFY(stored("svc/rk"));

    // The gate's launch re-check, with the key from the old record.
    lm._inSession = false;
    lm._handleReply(LicenseManager::Op::Verify, lm._key,
                    reply(QStringLiteral("valid"), envelope(s, recordObject(s, dev, kKey, now, now + 14 * kDay))),
                    true, 200);
    QVERIFY(lm.activated());
    QVERIFY(!stored("svc/rk"));        // the old record is gone
    QVERIFY(stored("svc/sr"));
    QVERIFY(stored("svc/lk"));
    QVERIFY(QSettings().value(QStringLiteral("svc/sv")).toBool());

    LicenseManager next;
    next._vehicleConnected = &noVehicle;
    next._pinned = {s.pinned()};
    next._nowOverride = now + 10 * kDay;   // offline, inside the signed window
    next._loadRecord();
    QVERIFY(next.activated());
    QCOMPARE(next._key, kKey);
}

void LicenseRecordTest::_legacyRecordNotHonoredAfterSignedRecord()
{
    const QString dev = _deviceId();
    {
        QSettings st;
        st.setValue(QStringLiteral("svc/sv"), true);   // this install held a signed record once
        st.sync();
    }
    // Somebody deleted the signed record and wrote a 5.2.0 one.
    _storeLegacy(kKey, dev);
    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    lm._nowOverride = realNow();
    lm._loadRecord();
    QVERIFY(!lm.activated());
    QCOMPARE(lm._key, kKey);   // used only for the online check
    QCOMPARE(lm.statusText(), QStringLiteral("Connect to the internet so RotorLab GC can check the license."));
}

// ---------------------------------------------------------------------------
// Never stop a flight
// ---------------------------------------------------------------------------

void LicenseRecordTest::_expiredAtStartWithVehicleOpens()
{
    // GC restarted mid-flight, offline, after the record ran out.
    const QString dev = _deviceId();
    const Signer s = makeSigner();
    const qint64 now = realNow();
    _storeEnvelope(envelopeBytes(envelope(s, recordObject(s, dev, kKey, now - 15 * kDay, now - kDay))));
    LicenseManager lm;
    lm._vehicleConnected = &oneVehicle;
    lm._pinned = {s.pinned()};
    lm._nowOverride = now;
    lm._storeKey(kKey);
    lm._loadRecord();
    QVERIFY(lm.activated());
    QVERIFY(lm.attention());
    QVERIFY(lm._inSession);
    QVERIFY(lm.statusText().startsWith(LicenseManager::expiredText()));
    QVERIFY(lm.statusText().contains(QStringLiteral("stays open for this session because a vehicle is connected")));

    // A re-check during the session only notes, even a bad verdict.
    lm._handleReply(LicenseManager::Op::Verify, kKey, QByteArrayLiteral(R"({"state":"expired"})"), true, 200);
    QVERIFY(lm.activated());
}

void LicenseRecordTest::_vehicleConnectingLiftsTimeOnlyGate()
{
    const QString dev = _deviceId();
    const Signer s = makeSigner();
    const qint64 now = realNow();
    _storeEnvelope(envelopeBytes(envelope(s, recordObject(s, dev, kKey, now - 15 * kDay, now - kDay))));
    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    lm._pinned = {s.pinned()};
    lm._nowOverride = now;
    lm._loadRecord();
    QVERIFY(!lm.activated());
    QCOMPARE(lm.statusText(), LicenseManager::expiredText());

    // A vehicle connects while the gate is up: the gate goes for the session.
    lm._vehicleAppeared();
    QVERIFY(lm.activated());
    QVERIFY(lm.attention());
}

void LicenseRecordTest::_forgedRecordNotLiftedByVehicle()
{
    // Only a genuine activation that ran out of time opens for a vehicle; a
    // forged or foreign record never does (there is no session to protect).
    const QString dev = _deviceId();
    const Signer attacker = makeSigner();
    const Signer s = makeSigner();
    const qint64 now = realNow();
    _storeEnvelope(envelopeBytes(envelope(attacker, recordObject(attacker, dev, kKey, now - 15 * kDay, now - kDay))));
    LicenseManager lm;
    lm._vehicleConnected = &oneVehicle;
    lm._pinned = {s.pinned()};
    lm._nowOverride = now;
    lm._loadRecord();
    QVERIFY(!lm.activated());
    lm._vehicleAppeared();
    QVERIFY(!lm.activated());
}

void LicenseRecordTest::_noLockDuringConnectedSession()
{
    const Signer s = makeSigner();
    const qint64 now = realNow();
    LicenseManager lm;
    lm._vehicleConnected = &oneVehicle;
    lm._pinned = {s.pinned()};
    lm._nowOverride = now;
    const QString dev = lm._deviceId;
    lm._handleReply(LicenseManager::Op::Activate, kKey,
                    reply(QStringLiteral("activated"), envelope(s, recordObject(s, dev, kKey, now, now + 14 * kDay))),
                    true, 200);
    QVERIFY(lm.activated());

    // The session runs past offline_until with a vehicle connected.
    lm._nowOverride = now + 20 * kDay;
    lm._handleReply(LicenseManager::Op::Verify, kKey, {}, false, 0);   // offline
    QVERIFY(lm.activated());
    lm._handleReply(LicenseManager::Op::Verify, kKey, reply(QStringLiteral("valid"), {}), true, 200);   // no record
    QVERIFY(lm.activated());
    const Signer attacker = makeSigner();
    lm._handleReply(LicenseManager::Op::Verify, kKey,
                    reply(QStringLiteral("valid"),
                          envelope(attacker, recordObject(attacker, dev, kKey, now, now + 14 * kDay))),
                    true, 200);   // a record GC cannot verify
    QVERIFY(lm.activated());
    lm._handleReply(LicenseManager::Op::Verify, kKey, QByteArrayLiteral(R"({"state":"revoked"})"), true, 200);
    QVERIFY(lm.activated());
    QVERIFY(lm.attention());
    QVERIFY(stored("svc/sr"));   // nothing cleared mid-session

    // The next start, no vehicle connected: now the expiry applies.
    LicenseManager next;
    next._vehicleConnected = &noVehicle;
    next._pinned = {s.pinned()};
    next._nowOverride = now + 20 * kDay;
    next._loadRecord();
    QVERIFY(!next.activated());
    QCOMPARE(next.statusText(), LicenseManager::expiredText());
}

// ---------------------------------------------------------------------------
// The gate's key field
// ---------------------------------------------------------------------------

void LicenseRecordTest::_gateKeyMaskedAfterDeactivate()
{
    // The gate is QML only: the key field binds to the gate's own _pending,
    // not to a C++ property. So this loads the real LicenseGate.qml from the
    // app's resources against a test LicenseManager and checks what the
    // field shows. TextInput.Normal is 0, TextInput.Password is 2.
    constexpr int kNormal = 0;
    constexpr int kPassword = 2;
    const QString typed = QStringLiteral("TEST-0000-0000-0007");

    LicenseManager lm;
    lm._vehicleConnected = &noVehicle;
    lm._key.clear();   // the gate's recheck() on show then sends nothing
    lm._setActivated(false);

    QQmlEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/qml"));
    engine.rootContext()->setContextProperty(QStringLiteral("LicenseManager"), &lm);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/custom/license/LicenseGate.qml")));
    QScopedPointer<QObject> gate(component.create());
    QVERIFY2(gate, qPrintable(component.errorString()));
    QObject *field = gate->findChild<QObject *>(QStringLiteral("licenseGateKeyField"));
    QObject *reveal = gate->findChild<QObject *>(QStringLiteral("licenseGateReveal"));
    QVERIFY(field);
    QVERIFY(reveal);
    const auto shown = [field]() { return field->property("displayText").toString(); };
    // The Accessible.name set in QML lives on the item's attached object.
    const auto accessibleName = [](QObject *item) {
        for (QObject *child : item->children()) {
            if (qstrcmp(child->metaObject()->className(), "QQuickAccessibleAttached") == 0) {
                return child->property("name").toString();
            }
        }
        return QString();
    };

    // Accessibility R3, R5: the field and the Show button say what they are,
    // and the button is reachable with Tab and reports its state.
    QCOMPARE(accessibleName(field), QStringLiteral("License key"));
    QCOMPARE(accessibleName(reveal), QStringLiteral("Show license key"));
    QCOMPARE(reveal->property("focusPolicy").toInt(), int(Qt::StrongFocus));
    QVERIFY(reveal->property("checkable").toBool());
    QVERIFY(!reveal->property("checked").toBool());

    // Typing a key works as before, and it is masked while typed.
    QVERIFY(field->setProperty("text", typed));
    QCOMPARE(gate->property("_pending").toString(), typed);
    QCOMPARE(field->property("echoMode").toInt(), kPassword);
    QVERIFY2(!shown().contains(QStringLiteral("0007")), qPrintable(shown()));

    // Show reveals it, Hide masks it again.
    QVERIFY(QMetaObject::invokeMethod(reveal, "clicked"));
    QCOMPARE(field->property("echoMode").toInt(), kNormal);
    QCOMPARE(shown(), typed);
    QVERIFY(reveal->property("checked").toBool());
    QCOMPARE(accessibleName(reveal), QStringLiteral("Hide license key"));
    QVERIFY(QMetaObject::invokeMethod(reveal, "clicked"));
    QCOMPARE(field->property("echoMode").toInt(), kPassword);
    QVERIFY(!shown().contains(QStringLiteral("0007")));

    // The key activates (left revealed), then "Deactivate this device"
    // brings the gate back: the field is empty and masked, the key is gone.
    QVERIFY(QMetaObject::invokeMethod(reveal, "clicked"));
    // (The gate's visible binding is not checked here: an item with no
    // window does not report effective visibility.)
    lm._setActivated(true);
    lm._setActivated(false);
    QCOMPARE(field->property("text").toString(), QString());
    QCOMPARE(gate->property("_pending").toString(), QString());
    QCOMPARE(field->property("echoMode").toInt(), kPassword);
    QVERIFY(!shown().contains(QStringLiteral("0007")));
    QVERIFY(!reveal->property("checked").toBool());
    QCOMPARE(accessibleName(reveal), QStringLiteral("Show license key"));

    // A new key can be typed as before.
    const QString next = QStringLiteral("TEST-0000-0000-0008");
    QVERIFY(field->setProperty("text", next));
    QCOMPARE(gate->property("_pending").toString(), next);
    QCOMPARE(field->property("echoMode").toInt(), kPassword);
    QVERIFY(!shown().contains(QStringLiteral("0008")));
}

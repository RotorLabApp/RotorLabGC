#include "RtpDepacketizerTest.h"
#include "RtpDepacketizer.h"

#include <QtTest/QTest>

#include <algorithm>

namespace {

using AU = RtpDepacketizer::AccessUnit;

constexpr quint32 kSsrcA = 0x11223344;
constexpr quint32 kSsrcB = 0x55667788;
constexpr qint64 kMs = 1000 * 1000;

QByteArray rtp(quint16 seq, quint32 ts, bool marker, const QByteArray &payload, int pt = 96, quint32 ssrc = kSsrcA)
{
    QByteArray p(12, 0);
    p[0] = char(0x80);
    p[1] = char((marker ? 0x80 : 0) | pt);
    p[2] = char(seq >> 8);
    p[3] = char(seq & 0xff);
    p[4] = char(ts >> 24);
    p[5] = char(ts >> 16);
    p[6] = char(ts >> 8);
    p[7] = char(ts);
    p[8] = char(ssrc >> 24);
    p[9] = char(ssrc >> 16);
    p[10] = char(ssrc >> 8);
    p[11] = char(ssrc);
    return p + payload;
}

QByteArray bytes(std::initializer_list<int> b)
{
    QByteArray out;
    for (int v : b) {
        out.append(char(v));
    }
    return out;
}

const QByteArray sc = bytes({ 0, 0, 0, 1 });
const QByteArray sps = bytes({ 0x67, 0x4d, 0x00, 0x1f });
const QByteArray pps = bytes({ 0x68, 0xee, 0x3c, 0x80 });
const QByteArray idr = bytes({ 0x65, 0x88, 0x84, 0x00, 0x10 });
const QByteArray pslice = bytes({ 0x41, 0x9a, 0x02 });
const QByteArray h265Idr = bytes({ 0x26, 0x01, 0xaf, 0x01 });   // IDR_W_RADL (19), TID 1

QByteArray stapA(const QList<QByteArray> &nals)
{
    QByteArray p = bytes({ 0x78 });   // F=0, NRI=3, type 24
    for (const QByteArray &n : nals) {
        p.append(char(n.size() >> 8));
        p.append(char(n.size() & 0xff));
        p.append(n);
    }
    return p;
}

/// Small deterministic generator (xorshift32): the fuzz tests repeat exactly
struct Rng
{
    quint32 s;
    quint32 next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<quint32>(n)); }
};

/// What every access unit must be, whatever came in: Annex B, inside the bound
bool wellFormed(const QList<AU> &out, int bound)
{
    for (const AU &au : out) {
        if ((au.data.size() < 5) || (au.data.size() > bound) || !au.data.startsWith(sc)) {
            return false;
        }
    }
    return true;
}

} // namespace

void RtpDepacketizerTest::_singleNalUnits()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    d.push(rtp(10, 9000, false, sps), 100, out);
    d.push(rtp(11, 9000, false, pps), 200, out);
    QVERIFY(out.isEmpty());
    d.push(rtp(12, 9000, true, idr), 300, out);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].data, sc + sps + sc + pps + sc + idr);
    QVERIFY(out[0].keyFrame);
    QCOMPARE(out[0].firstPacketNs, qint64(100));
    QCOMPARE(out[0].rtpTimestamp, quint32(9000));
    QCOMPARE(d.stats().accessUnits, quint64(1));
}

void RtpDepacketizerTest::_stapA()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    d.push(rtp(1, 0, false, stapA({ sps, pps })), 1, out);
    d.push(rtp(2, 0, true, idr), 2, out);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].data, sc + sps + sc + pps + sc + idr);
}

void RtpDepacketizerTest::_fuA()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    // IDR (0x65: NRI 3, type 5) as FU-A: indicator 0x7c (NRI 3, type 28), headers S/-/E with type 5
    d.push(rtp(1, 0, false, bytes({ 0x7c, 0x85, 0xa1, 0xa2 })), 1, out);
    d.push(rtp(2, 0, false, bytes({ 0x7c, 0x05, 0xb1 })), 2, out);
    d.push(rtp(3, 0, true, bytes({ 0x7c, 0x45, 0xc1, 0xc2 })), 3, out);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].data, sc + bytes({ 0x65, 0xa1, 0xa2, 0xb1, 0xc1, 0xc2 }));
    QVERIFY(out[0].keyFrame);
}

void RtpDepacketizerTest::_lostMiddleFragment()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    d.setReorderWindow(0, 0);   // window 0: a gap is a loss at once
    QList<AU> out;
    d.push(rtp(1, 0, true, idr), 1, out);
    QCOMPARE(out.size(), 1);
    // next IDR in 3 fragments, the middle one lost
    d.push(rtp(2, 3000, false, bytes({ 0x7c, 0x85, 0xa1 })), 2, out);
    d.push(rtp(4, 3000, true, bytes({ 0x7c, 0x45, 0xc1 })), 4, out);
    QCOMPARE(out.size(), 1);
    QCOMPARE(d.stats().sequenceGaps, quint64(1));
    QCOMPARE(d.stats().packetsMissing, quint64(1));
    QCOMPARE(d.stats().droppedAccessUnits, quint64(1));
    // a P frame cannot be decoded without the lost reference: dropped until the next IDR
    d.push(rtp(5, 6000, true, pslice), 5, out);
    QCOMPARE(out.size(), 1);
    d.push(rtp(6, 9000, true, idr), 6, out);
    QCOMPARE(out.size(), 2);
    QVERIFY(out[1].keyFrame);
}

void RtpDepacketizerTest::_latePacketDropped()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    d.setReorderWindow(0, 0);   // window 0: a gap is a loss at once
    QList<AU> out;
    d.push(rtp(100, 0, true, idr), 1, out);
    d.push(rtp(102, 3000, true, idr), 2, out);    // 101 missing
    d.push(rtp(101, 1500, true, pslice), 3, out); // arrives late: window 0, refused
    QCOMPARE(out.size(), 2);
    QCOMPARE(d.stats().refused, quint64(1));
    QCOMPARE(d.stats().late, quint64(1));
    QCOMPARE(d.stats().sequenceGaps, quint64(1));
}

void RtpDepacketizerTest::_waitsForKeyFrame()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    d.push(rtp(1, 0, true, pslice), 1, out);
    d.push(rtp(2, 3000, true, pslice), 2, out);
    QVERIFY(out.isEmpty());
    QCOMPARE(d.stats().droppedAccessUnits, quint64(2));
    d.push(rtp(3, 6000, true, idr), 3, out);
    d.push(rtp(4, 9000, true, pslice), 4, out);
    QCOMPARE(out.size(), 2);
    QVERIFY(!out[1].keyFrame);
}

void RtpDepacketizerTest::_timestampChangeClosesUnit()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    d.push(rtp(1, 0, false, idr), 1, out);   // sender without marker bits
    QVERIFY(out.isEmpty());
    d.push(rtp(2, 3000, false, pslice), 2, out);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].data, sc + idr);
}

void RtpDepacketizerTest::_h265ApAndFu()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H265);
    QList<AU> out;
    const QByteArray vps = bytes({ 0x40, 0x01, 0x0c });
    const QByteArray hsps = bytes({ 0x42, 0x01, 0x01 });
    const QByteArray hpps = bytes({ 0x44, 0x01, 0xc1 });
    QByteArray ap = bytes({ 0x60, 0x01 });   // type 48
    for (const QByteArray &n : { vps, hsps, hpps }) {
        ap.append(char(0));
        ap.append(char(n.size()));
        ap.append(n);
    }
    d.push(rtp(1, 0, false, ap), 1, out);
    // IDR_W_RADL (19) as FU: header 0x62 0x01 (type 49), FU header S|19, E|19
    d.push(rtp(2, 0, false, bytes({ 0x62, 0x01, 0x93, 0xaa })), 2, out);
    d.push(rtp(3, 0, true, bytes({ 0x62, 0x01, 0x53, 0xbb })), 3, out);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].data, sc + vps + sc + hsps + sc + hpps + sc + bytes({ 0x26, 0x01, 0xaa, 0xbb }));
    QVERIFY(out[0].keyFrame);
}

void RtpDepacketizerTest::_refusesBadPackets()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    d.setReorderWindow(0, 0);   // window 0: a gap is a loss at once
    QList<AU> out;
    d.push(QByteArray(8, 0), 1, out);                        // too short
    QByteArray v1 = rtp(1, 0, true, idr);
    v1[0] = char(0x40);                                      // version 1
    d.push(v1, 1, out);
    d.push(rtp(2, 0, true, idr, 97), 1, out);                // other payload type
    QCOMPARE(d.stats().refused, quint64(3));
    QCOMPARE(d.stats().packets, quint64(0));
    d.push(rtp(3, 0, true, bytes({ 0x7d, 0x85, 0x00, 0x01, 0xa1 })), 1, out);   // FU-B (29)
    QVERIFY(out.isEmpty());
    QCOMPARE(d.stats().refused, quint64(4));
    for (int type : { 0, 25, 26, 27, 30, 31 }) {
        d.push(rtp(quint16(4 + type), quint32(type), true, bytes({ 0x60 | type, 0x00, 0x01 })), 1, out);
    }
    QVERIFY(out.isEmpty());
    QCOMPARE(d.stats().refused, quint64(10));
}

void RtpDepacketizerTest::_senderRestart()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    d.push(rtp(10, 0, true, idr), 1, out);
    d.push(rtp(40000, 777, true, idr), 2, out);   // new sender, random sequence
    QCOMPARE(out.size(), 2);
    QCOMPARE(d.stats().sequenceGaps, quint64(0));
    QCOMPARE(d.stats().senderChanges, quint64(1));
}

void RtpDepacketizerTest::_headerExtensionAndPadding()
{
    // CSRC count 2, a one-word header extension and 3 bytes of padding around a good IDR
    QByteArray p = rtp(1, 0, true, QByteArray());
    p[0] = char(0x80 | 0x20 | 0x10 | 2);
    p += bytes({ 1, 2, 3, 4, 5, 6, 7, 8 });               // CSRCs
    p += bytes({ 0xbe, 0xde, 0x00, 0x01, 9, 9, 9, 9 });   // extension, length 1 word
    p += idr;
    p += bytes({ 0, 0, 3 });                              // padding, count in the last byte

    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    d.push(p, 1, out);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].data, sc + idr);
    QCOMPARE(d.stats().malformed, quint64(0));
}

void RtpDepacketizerTest::_malformedRtpHeader()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;

    QByteArray csrc = rtp(1, 0, true, idr);
    csrc[0] = char(0x80 | 15);                           // 15 CSRCs (60 bytes) in a 17-byte packet
    d.push(csrc, 1, out);

    QByteArray extShort = rtp(2, 0, true, bytes({ 0xbe, 0xde }));
    extShort[0] = char(0x90);                            // extension flag, header cut short
    d.push(extShort, 1, out);

    QByteArray extLong = rtp(3, 0, true, bytes({ 0xbe, 0xde, 0x00, 0x64 }) + idr);
    extLong[0] = char(0x90);                             // extension of 100 words, 5 bytes there
    d.push(extLong, 1, out);

    QByteArray extMax = rtp(4, 0, true, bytes({ 0xbe, 0xde, 0xff, 0xff }) + idr);
    extMax[0] = char(0x90);                              // largest extension length
    d.push(extMax, 1, out);

    QByteArray padZero = rtp(5, 0, true, idr + bytes({ 0 }));
    padZero[0] = char(0xa0);                             // padding count 0 (it counts itself)
    d.push(padZero, 1, out);

    QByteArray padLong = rtp(6, 0, true, idr + bytes({ 200 }));
    padLong[0] = char(0xa0);                             // more padding than payload
    d.push(padLong, 1, out);

    QByteArray padAll = rtp(7, 0, true, bytes({ 0, 0, 3 }));
    padAll[0] = char(0xa0);                              // padding is the whole payload
    d.push(padAll, 1, out);

    d.push(rtp(8, 0, true, QByteArray()), 1, out);      // header only

    QByteArray padHeader = rtp(9, 0, true, QByteArray());
    padHeader[0] = char(0xa0);
    padHeader[11] = char(12);                            // padding count reaching into the header
    d.push(padHeader, 1, out);

    QVERIFY(out.isEmpty());
    QCOMPARE(d.stats().malformed, quint64(9));
    QCOMPARE(d.stats().packets, quint64(0));

    // Nothing of the above stuck: a good packet works as the first one
    d.push(rtp(100, 0, true, idr), 2, out);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].data, sc + idr);
}

void RtpDepacketizerTest::_oversizedPacket()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    QByteArray big = rtp(1, 0, true, idr);
    big.append(QByteArray(RtpDepacketizer::kMaxPacketBytes, char(0x41)));
    d.push(big, 1, out);
    QVERIFY(out.isEmpty());
    QCOMPARE(d.stats().oversized, quint64(1));
    QCOMPARE(d.stats().refused, quint64(1));
    QCOMPARE(d.stats().packets, quint64(0));

    // The largest UDP payload is taken in
    QByteArray largest = rtp(2, 0, true, bytes({ 0x65 }));
    largest.append(QByteArray(65507 - largest.size(), char(0x11)));
    d.push(largest, 2, out);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].data.size(), qsizetype(4 + 65507 - 12));
}

void RtpDepacketizerTest::_malformedStapA()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    const QList<QByteArray> bad = {
        bytes({ 0x78 }),                                      // no unit at all
        bytes({ 0x78, 0x00 }),                                // size field cut short
        bytes({ 0x78, 0x00, 0x10, 0x67 }),                    // size beyond the packet
        bytes({ 0x78, 0x00, 0x00, 0x67 }),                    // size 0
        bytes({ 0x78, 0x00, 0x02, 0x78, 0x00 }),              // STAP-A inside STAP-A
        bytes({ 0x78, 0x00, 0x02, 0x7c, 0x85 }),              // FU-A inside STAP-A
        bytes({ 0x78, 0x00, 0x01, 0x85 }),                    // forbidden bit inside
        stapA({ sps, pps }) + bytes({ 0x00 }),                // good units, then 1 stray byte
        bytes({ 0xf8, 0x00, 0x01, 0x65 }),                    // forbidden bit on the STAP-A
        stapA({ sps, idr }) + bytes({ 0xff, 0xff }),          // good units, then a size of 65535
    };
    quint16 seq = 1;
    for (const QByteArray &payload : bad) {
        d.push(rtp(seq, seq * 3000u, true, payload), seq, out);
        ++seq;
    }
    QVERIFY(out.isEmpty());
    QCOMPARE(d.stats().malformed, quint64(bad.size()));
    QCOMPARE(d.stats().droppedAccessUnits, quint64(bad.size()));

    // A good STAP-A afterwards: nothing of the bad ones is left in it
    d.push(rtp(seq, seq * 3000u, false, stapA({ sps, pps })), seq, out);
    ++seq;
    d.push(rtp(seq, (seq - 1) * 3000u, true, idr), seq, out);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].data, sc + sps + sc + pps + sc + idr);
}

void RtpDepacketizerTest::_malformedFuA()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    const QList<QByteArray> bad = {
        bytes({ 0x7c, 0xc5, 0xaa }),    // start and end together
        bytes({ 0x7c, 0x80, 0xaa }),    // fragment of NAL type 0
        bytes({ 0x7c, 0x98, 0xaa }),    // fragment of a STAP-A (24)
        bytes({ 0x7c, 0x9c, 0xaa }),    // fragment of a FU-A (28)
        bytes({ 0x7c, 0x85 }),          // no fragment bytes
        bytes({ 0x7c, 0x05, 0xaa }),    // middle without its start
        bytes({ 0x7c, 0x45, 0xaa }),    // end without its start
    };
    quint16 seq = 1;
    for (const QByteArray &payload : bad) {
        d.push(rtp(seq, seq * 3000u, true, payload), seq, out);
        ++seq;
    }
    // Start of an IDR, then the end of another NAL type
    d.push(rtp(seq, seq * 3000u, false, bytes({ 0x7c, 0x85, 0xaa })), seq, out);
    d.push(rtp(seq + 1, seq * 3000u, true, bytes({ 0x7c, 0x41, 0xbb })), seq + 1, out);
    seq += 2;

    QVERIFY(out.isEmpty());
    QCOMPARE(d.stats().malformed, quint64(bad.size() + 1));
    QCOMPARE(d.stats().droppedAccessUnits, quint64(bad.size() + 1));

    d.push(rtp(seq, seq * 3000u, true, idr), seq, out);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].data, sc + idr);
}

void RtpDepacketizerTest::_fragmentCutShort()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;

    // A fragmented IDR interrupted by a single NAL unit in the same access unit
    d.push(rtp(1, 0, false, bytes({ 0x7c, 0x85, 0xaa })), 1, out);
    d.push(rtp(2, 0, true, pslice), 2, out);
    // The marker arrives on a middle fragment: the NAL unit never ends
    d.push(rtp(3, 3000, false, bytes({ 0x7c, 0x85, 0xaa })), 3, out);
    d.push(rtp(4, 3000, true, bytes({ 0x7c, 0x05, 0xbb })), 4, out);
    // The timestamp changes inside a fragmented NAL unit
    d.push(rtp(5, 6000, false, bytes({ 0x7c, 0x85, 0xaa })), 5, out);
    d.push(rtp(6, 9000, false, bytes({ 0x7c, 0x05, 0xbb })), 6, out);
    // A new start before the previous one ended
    d.push(rtp(7, 12000, false, bytes({ 0x7c, 0x85, 0xaa })), 7, out);
    d.push(rtp(8, 12000, true, bytes({ 0x7c, 0x85, 0xcc })), 8, out);

    QVERIFY(out.isEmpty());
    QCOMPARE(d.stats().malformed, quint64(1));   // the middle fragment after the timestamp change
    QVERIFY(d.stats().droppedAccessUnits >= 4);

    d.push(rtp(9, 15000, true, idr), 9, out);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].data, sc + idr);
}

void RtpDepacketizerTest::_malformedH265()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H265);
    QList<AU> out;
    const QList<QByteArray> malformed = {
        bytes({ 0x26, 0x01 }),                                  // payload header only
        bytes({ 0xa6, 0x01, 0xaa }),                            // forbidden bit
        bytes({ 0x26, 0x00, 0xaa }),                            // TemporalId 0
        bytes({ 0x60, 0x01, 0x00, 0x02, 0x40, 0x01 }),          // AP unit shorter than a NAL header
        bytes({ 0x60, 0x01, 0x00, 0x10, 0x40, 0x01, 0x0c }),    // AP unit beyond the packet
        bytes({ 0x60, 0x01, 0x00, 0x03, 0x62, 0x01, 0x93 }),    // FU inside an AP
        bytes({ 0x60, 0x01, 0x00, 0x03, 0x40, 0x00, 0x0c }),    // AP unit with TemporalId 0
        bytes({ 0x60, 0x01, 0xaa }),                            // AP with a stray byte
        bytes({ 0x62, 0x01, 0x93 }),                            // FU without fragment bytes
        bytes({ 0x62, 0x01, 0xd3, 0xaa }),                      // FU start and end together
        bytes({ 0x62, 0x01, 0xb0, 0xaa }),                      // FU of an AP (48)
        bytes({ 0x62, 0x01, 0x13, 0xaa }),                      // FU middle without its start
    };
    quint16 seq = 1;
    for (const QByteArray &payload : malformed) {
        d.push(rtp(seq, seq * 3000u, true, payload), seq, out);
        ++seq;
    }
    // PACI (50) and an unspecified type (55): not supported, refused
    d.push(rtp(seq, seq * 3000u, true, bytes({ 0x64, 0x01, 0x26, 0x01, 0xaa })), seq, out);
    ++seq;
    d.push(rtp(seq, seq * 3000u, true, bytes({ 0x6e, 0x01, 0xaa })), seq, out);
    ++seq;

    QVERIFY(out.isEmpty());
    QCOMPARE(d.stats().malformed, quint64(malformed.size()));
    QCOMPARE(d.stats().refused, quint64(2));
    QCOMPARE(d.stats().droppedAccessUnits, quint64(malformed.size() + 2));

    d.push(rtp(seq, seq * 3000u, true, h265Idr), seq, out);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].data, sc + h265Idr);
}

void RtpDepacketizerTest::_oversizedAccessUnit()
{
    constexpr int kBound = 64;
    RtpDepacketizer d(RtpDepacketizer::Codec::H264, 96, kBound);
    QCOMPARE(d.maxAccessUnitBytes(), kBound);
    QList<AU> out;
    const QByteArray piece(30, char(0x5a));

    // A fragmented IDR of 90 bytes
    d.push(rtp(1, 0, false, bytes({ 0x7c, 0x85 }) + piece), 1, out);
    d.push(rtp(2, 0, false, bytes({ 0x7c, 0x05 }) + piece), 2, out);
    d.push(rtp(3, 0, true, bytes({ 0x7c, 0x45 }) + piece), 3, out);
    QVERIFY(out.isEmpty());
    QCOMPARE(d.stats().oversized, quint64(1));
    QCOMPARE(d.stats().malformed, quint64(0));

    // A STAP-A of three 30-byte units
    QByteArray unit = bytes({ 0x65 }) + piece.left(29);
    d.push(rtp(4, 3000, true, stapA({ unit, unit, unit })), 4, out);
    QVERIFY(out.isEmpty());
    QCOMPARE(d.stats().oversized, quint64(2));

    // Many small single NAL units in one access unit
    for (quint16 seq = 5; seq < 30; ++seq) {
        d.push(rtp(seq, 6000, false, idr), seq, out);
    }
    d.push(rtp(30, 6000, true, idr), 30, out);
    QVERIFY(out.isEmpty());
    QCOMPARE(d.stats().oversized, quint64(3));
    QCOMPARE(d.stats().droppedAccessUnits, quint64(3));

    // Exactly at the bound is fine
    const QByteArray fits = bytes({ 0x65 }) + QByteArray(kBound - 5, char(0x22));
    d.push(rtp(31, 9000, true, fits), 31, out);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].data.size(), qsizetype(kBound));

    // One byte more is not
    d.push(rtp(32, 12000, true, fits + bytes({ 0x22 })), 32, out);
    QCOMPARE(out.size(), 1);
    QCOMPARE(d.stats().oversized, quint64(4));

    // Bounds out of range are clamped
    QVERIFY(RtpDepacketizer(RtpDepacketizer::Codec::H264, 96, 0).maxAccessUnitBytes() > 0);
    QVERIFY(RtpDepacketizer(RtpDepacketizer::Codec::H264, 96, -5).maxAccessUnitBytes() > 0);
}

void RtpDepacketizerTest::_sequenceWrap()
{
    {
        RtpDepacketizer d(RtpDepacketizer::Codec::H264);
        d.setReorderWindow(0, 0);
        QList<AU> out;
        d.push(rtp(65534, 0, true, idr), 1, out);
        d.push(rtp(65535, 3000, true, pslice), 2, out);
        d.push(rtp(0, 6000, true, pslice), 3, out);
        d.push(rtp(1, 9000, true, pslice), 4, out);
        QCOMPARE(out.size(), 4);
        QCOMPARE(d.stats().sequenceGaps, quint64(0));
        QCOMPARE(d.stats().packetsMissing, quint64(0));

        // late across the wrap, and a duplicate: refused
        d.push(rtp(65535, 3000, true, pslice), 5, out);
        d.push(rtp(1, 9000, true, pslice), 6, out);
        QCOMPARE(out.size(), 4);
        QCOMPARE(d.stats().refused, quint64(2));
        QCOMPARE(d.stats().late, quint64(1));
        QCOMPARE(d.stats().duplicates, quint64(1));

        // a loss after the wrap
        d.push(rtp(3, 12000, true, idr), 7, out);
        QCOMPARE(d.stats().sequenceGaps, quint64(1));
        QCOMPARE(d.stats().packetsMissing, quint64(1));
        QCOMPARE(out.size(), 5);
    }
    {
        // a loss across the wrap: 65535, then 1 (0 lost)
        RtpDepacketizer d(RtpDepacketizer::Codec::H264);
        d.setReorderWindow(0, 0);
        QList<AU> out;
        d.push(rtp(65535, 0, true, idr), 1, out);
        d.push(rtp(1, 3000, true, idr), 2, out);
        QCOMPARE(d.stats().sequenceGaps, quint64(1));
        QCOMPARE(d.stats().packetsMissing, quint64(1));
        QCOMPARE(d.stats().senderChanges, quint64(0));
        QCOMPARE(out.size(), 2);
    }
    {
        // the largest gap still counted as a loss, and one more is a sender restart
        RtpDepacketizer d(RtpDepacketizer::Codec::H264);
        d.setReorderWindow(0, 0);
        QList<AU> out;
        d.push(rtp(65000, 0, true, idr), 1, out);
        d.push(rtp(quint16(65000 + 1001), 3000, true, idr), 2, out);
        QCOMPARE(d.stats().packetsMissing, quint64(1000));
        d.push(rtp(quint16(65000 + 1001 + 1002), 6000, true, idr), 3, out);
        QCOMPARE(d.stats().senderChanges, quint64(1));
        QCOMPARE(out.size(), 3);
    }
}

void RtpDepacketizerTest::_otherSenderRefused()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    d.push(rtp(1, 0, true, idr, 96, kSsrcA), 0, out);
    // Another sender while the first is live: refused, the first is not disturbed
    d.push(rtp(500, 0, false, bytes({ 0x7c, 0x85, 0xaa }), 96, kSsrcB), 100 * kMs, out);
    d.push(rtp(2, 3000, true, pslice, 96, kSsrcA), 200 * kMs, out);
    QCOMPARE(out.size(), 2);
    QCOMPARE(d.stats().refused, quint64(1));
    QCOMPARE(d.stats().sequenceGaps, quint64(0));

    // The first sender goes quiet: the other one takes over after the hold time
    const qint64 later = 200 * kMs + RtpDepacketizer::kSsrcTakeoverNs;
    d.push(rtp(501, 90000, true, idr, 96, kSsrcB), later, out);
    QCOMPARE(out.size(), 3);
    QCOMPARE(d.stats().senderChanges, quint64(1));
    // and now the first one is the stranger
    d.push(rtp(3, 6000, true, idr, 96, kSsrcA), later + kMs, out);
    QCOMPARE(out.size(), 3);
    QCOMPARE(d.stats().refused, quint64(2));
}

void RtpDepacketizerTest::_fuzzRandomPackets()
{
    constexpr int kBound = 4096;
    for (const RtpDepacketizer::Codec codec : { RtpDepacketizer::Codec::H264, RtpDepacketizer::Codec::H265 }) {
        RtpDepacketizer d(codec, 96, kBound);
        Rng rng{ (codec == RtpDepacketizer::Codec::H264) ? 0x1234567u : 0x7654321u };
        QList<AU> out;
        quint16 seq = 0;
        quint32 ts = 0;
        for (int i = 0; i < 30000; ++i) {
            const int kind = rng.below(100);
            int length = rng.below(64);
            if (kind < 2) {
                length = rng.below(RtpDepacketizer::kMaxPacketBytes + 64);
            } else if (kind < 30) {
                length = 12 + rng.below(1500);
            }
            QByteArray p(length, Qt::Uninitialized);
            for (int b = 0; b < length; ++b) {
                p[b] = char(rng.next());
            }
            if ((kind >= 30) && (length >= 12)) {
                // A believable header, so the payload parsers get the random bytes
                p[0] = char(0x80 | (rng.below(4) == 0 ? (rng.next() & 0x3f) : 0));
                p[1] = char((rng.below(3) == 0 ? 0x80 : 0) | 96);
                seq += (rng.below(20) == 0) ? quint16(rng.below(3000)) : 1;
                ts += (rng.below(4) == 0) ? 3000 : 0;
                p[2] = char(seq >> 8);
                p[3] = char(seq);
                p[4] = char(ts >> 24);
                p[5] = char(ts >> 16);
                p[6] = char(ts >> 8);
                p[7] = char(ts);
                p[8] = 0x11; p[9] = 0x22; p[10] = 0x33; p[11] = 0x44;
            }
            d.push(p, i * kMs, out);
            QVERIFY2(wellFormed(out, kBound), qPrintable(QStringLiteral("iteration %1").arg(i)));
            QVERIFY(d.heldPackets() < d.reorderPackets());
            QVERIFY((d.nextDeadlineNs() == 0) || (d.nextDeadlineNs() > i * kMs));
            out.clear();
        }
        QVERIFY(d.stats().malformed + d.stats().refused > 0);
        d.poll(30000 * kMs + d.reorderHoldNs(), out);
        QCOMPARE(d.heldPackets(), 0);
        QVERIFY(wellFormed(out, kBound));
    }
}

void RtpDepacketizerTest::_fuzzMutatedStream()
{
    // A good stream (STAP-A, fragmented IDR, P slices) with bytes flipped, cut and added
    constexpr int kBound = 8192;
    QList<QByteArray> payloads;
    payloads << stapA({ sps, pps }) << bytes({ 0x7c, 0x85, 0xa1, 0xa2 }) << bytes({ 0x7c, 0x05, 0xb1 })
             << bytes({ 0x7c, 0x45, 0xc1 }) << pslice << idr;
    for (const RtpDepacketizer::Codec codec : { RtpDepacketizer::Codec::H264, RtpDepacketizer::Codec::H265 }) {
        RtpDepacketizer d(codec, 96, kBound);
        Rng rng{ 0xc0ffeeu };
        QList<AU> out;
        quint16 seq = 100;
        quint32 ts = 0;
        for (int i = 0; i < 20000; ++i) {
            const int which = rng.below(payloads.size());
            QByteArray p = rtp(seq++, ts, which >= 3, payloads.at(which));
            if (which >= 3) {
                ts += 3000;
            }
            switch (rng.below(4)) {
            case 0:
                p[rng.below(int(p.size()))] = char(rng.next());
                break;
            case 1:
                p.truncate(rng.below(int(p.size()) + 1));
                break;
            case 2:
                for (int n = rng.below(40); n > 0; --n) {
                    p.append(char(rng.next()));
                }
                break;
            default:
                break;
            }
            d.push(p, i * kMs, out);
            QVERIFY2(wellFormed(out, kBound), qPrintable(QStringLiteral("iteration %1").arg(i)));
            QVERIFY(d.heldPackets() < d.reorderPackets());
            out.clear();
        }
    }
}

void RtpDepacketizerTest::_parameterSetsAndSsrc()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    QCOMPARE(d.ssrc(), quint32(0));
    d.push(rtp(1, 0, false, stapA({ sps, pps })), 1, out);
    d.push(rtp(2, 0, true, idr), 2, out);
    d.push(rtp(3, 3000, true, pslice), 3, out);
    QCOMPARE(out.size(), 2);
    QCOMPARE(out[0].parameterSets, sc + sps + sc + pps);
    QCOMPARE(out[1].parameterSets, sc + sps + sc + pps);   // stored, not in band
    QCOMPARE(out[1].data, sc + pslice);
    QCOMPARE(out[0].ssrc, kSsrcA);
    QCOMPARE(d.ssrc(), kSsrcA);

    // A new SPS replaces the stored one; parameter sets survive a reset (pause)
    const QByteArray sps2 = bytes({ 0x67, 0x64, 0x00, 0x28 });
    d.reset();
    d.push(rtp(10, 6000, false, sps2), 10, out);
    d.push(rtp(11, 6000, true, idr), 11, out);
    QCOMPARE(out.size(), 3);
    QCOMPARE(out[2].parameterSets, sc + sps2 + sc + pps);
}

namespace {

/// A frame of n FU-A pieces (IDR or P slice), sequence numbers from seq, one timestamp
QList<QByteArray> fragmentedFrame(quint16 seq, quint32 ts, bool key, int n)
{
    const int type = key ? 5 : 1;
    const char indicator = char(key ? 0x7c : 0x5c);
    QList<QByteArray> packets;
    for (int i = 0; i < n; ++i) {
        int fu = type;
        if (i == 0) {
            fu |= 0x80;
        }
        if (i == n - 1) {
            fu |= 0x40;
        }
        packets << rtp(quint16(seq + i), ts, i == n - 1, QByteArray(1, indicator) + QByteArray(1, char(fu)) + QByteArray(1, char(0x10 + i)));
    }
    return packets;
}

QByteArray fragmentedData(bool key, int n)
{
    QByteArray d = sc + QByteArray(1, char(key ? 0x65 : 0x41));
    for (int i = 0; i < n; ++i) {
        d.append(char(0x10 + i));
    }
    return d;
}

} // namespace

void RtpDepacketizerTest::_reorderWithinWindow()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QCOMPARE(d.reorderPackets(), RtpDepacketizer::kDefaultReorderPackets);
    QCOMPARE(d.reorderHoldNs(), RtpDepacketizer::kDefaultReorderHoldNs);
    QList<AU> out;

    // An IDR in 5 pieces with pieces 2 and 3 swapped, then a P frame in 6 pieces in reverse
    const QList<QByteArray> key = fragmentedFrame(1, 0, true, 5);
    qint64 t = kMs;
    for (int i : { 0, 2, 1, 3, 4 }) {
        d.push(key[i], t, out);
        t += kMs;
    }
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].data, fragmentedData(true, 5));
    QCOMPARE(out[0].firstPacketNs, 1 * kMs);
    QVERIFY(!out[0].afterLoss);

    const QList<QByteArray> p = fragmentedFrame(6, 3000, false, 6);
    for (int i = 5; i >= 0; --i) {
        d.push(p[i], (10 + (5 - i)) * kMs, out);
        QCOMPARE(out.size(), (i == 0) ? 2 : 1);   // nothing comes out before the first piece
    }
    QCOMPARE(out[1].data, fragmentedData(false, 6));
    QVERIFY(!out[1].afterLoss);
    QCOMPARE(d.heldPackets(), 0);
    QCOMPARE(d.nextDeadlineNs(), qint64(0));

    QCOMPARE(d.stats().packetsMissing, quint64(0));
    QCOMPARE(d.stats().sequenceGaps, quint64(0));
    QCOMPARE(d.stats().droppedAccessUnits, quint64(0));
    QCOMPARE(d.stats().refused, quint64(0));
    QCOMPARE(d.stats().packets, quint64(11));
    // 2 came after 3; 10, 9, 8, 7, 6 each came after a later one
    QCOMPARE(d.stats().reordered, quint64(6));
    QCOMPARE(d.stats().held, quint64(1 + 5));
    // piece 11 waited from 10 ms until piece 6 came at 15 ms
    QCOMPARE(d.stats().holdNsMax, 5 * kMs);

    // A P frame continues normally
    d.push(rtp(12, 6000, true, pslice), 20 * kMs, out);
    QCOMPARE(out.size(), 3);
}

void RtpDepacketizerTest::_reorderBeyondWindowCount()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    d.setReorderWindow(8, 50 * kMs);
    QList<AU> out;
    d.push(rtp(1, 0, true, idr), 0, out);
    QCOMPARE(out.size(), 1);

    // 2 is missing; 3 to 9 (7 packets, 3000 apart) fit in the window and wait
    for (quint16 seq = 3; seq <= 9; ++seq) {
        d.push(rtp(seq, seq * 3000u, true, (seq == 3) ? idr : pslice), seq * kMs, out);
    }
    QCOMPARE(out.size(), 1);
    QCOMPARE(d.heldPackets(), 7);
    QCOMPARE(d.stats().packetsMissing, quint64(0));

    // 10 is 8 past the next expected (2): the window is exceeded, 2 is lost, 3 to 10 go out
    d.push(rtp(10, 30000, true, pslice), 10 * kMs, out);
    QCOMPARE(d.heldPackets(), 0);
    QCOMPARE(d.stats().sequenceGaps, quint64(1));
    QCOMPARE(d.stats().packetsMissing, quint64(1));
    QCOMPARE(out.size(), 1 + 8);
    QCOMPARE(out[1].data, sc + idr);       // the IDR at 3 resumes the stream
    QVERIFY(out[1].afterLoss);
    QVERIFY(!out[2].afterLoss);

    // 2 after all: late, refused, and nothing changes
    d.push(rtp(2, 1500, true, pslice), 11 * kMs, out);
    QCOMPARE(out.size(), 9);
    QCOMPARE(d.stats().late, quint64(1));
    QCOMPARE(d.stats().refused, quint64(1));
    QCOMPARE(d.stats().reordered, quint64(0));
}

void RtpDepacketizerTest::_reorderBeyondWindowTime()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);   // 64 packets, 50 ms
    QList<AU> out;
    d.push(rtp(1, 0, true, idr), 0, out);
    d.push(rtp(3, 6000, true, idr), 1 * kMs, out);   // 2 missing
    QCOMPARE(out.size(), 1);
    QCOMPARE(d.heldPackets(), 1);
    QCOMPARE(d.nextDeadlineNs(), 1 * kMs + 50 * kMs);

    d.poll(50 * kMs, out);                          // 49 ms: still waiting
    QCOMPARE(out.size(), 1);
    QCOMPARE(d.stats().packetsMissing, quint64(0));

    d.poll(51 * kMs, out);                          // 50 ms: 2 is a loss, 3 goes out
    QCOMPARE(out.size(), 2);
    QCOMPARE(d.heldPackets(), 0);
    QCOMPARE(d.nextDeadlineNs(), qint64(0));
    QCOMPARE(d.stats().packetsMissing, quint64(1));
    QVERIFY(out[1].afterLoss);
    QCOMPARE(out[1].firstPacketNs, 1 * kMs);        // its own arrival: the wait shows in latency
    QCOMPARE(d.stats().holdNsMax, 50 * kMs);

    d.push(rtp(2, 3000, true, pslice), 52 * kMs, out);
    QCOMPARE(out.size(), 2);
    QCOMPARE(d.stats().late, quint64(1));

    // A later packet's push also expires what waited too long, with no poll
    d.push(rtp(5, 12000, true, pslice), 60 * kMs, out);   // 4 missing
    d.push(rtp(6, 15000, true, pslice), 111 * kMs, out);  // 5 waited 51 ms
    QCOMPARE(d.stats().packetsMissing, quint64(2));
    QCOMPARE(d.heldPackets(), 0);
}

void RtpDepacketizerTest::_reorderAcrossWrap()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    d.push(rtp(65533, 0, true, idr), 0, out);
    // 65534, 65535, 0, 1, 2 arrive as 0, 65535, 2, 65534, 1
    const QList<quint16> order = { 0, 65535, 2, 65534, 1 };
    qint64 t = kMs;
    for (quint16 seq : order) {
        const quint32 ts = quint32(quint16(seq - 65533)) * 3000u;
        d.push(rtp(seq, ts, true, pslice), t, out);
        t += kMs;
    }
    QCOMPARE(out.size(), 6);
    for (int i = 1; i < out.size(); ++i) {
        QCOMPARE(out[i].rtpTimestamp, quint32(i) * 3000u);   // released in sequence order
        QVERIFY(!out[i].afterLoss);
    }
    QCOMPARE(d.stats().packetsMissing, quint64(0));
    QCOMPARE(d.stats().senderChanges, quint64(0));
    QCOMPARE(d.heldPackets(), 0);

    // and a loss across the wrap with a window: 65535 missing between 65534 and 0
    RtpDepacketizer e(RtpDepacketizer::Codec::H264);
    out.clear();
    e.push(rtp(65534, 0, true, idr), 0, out);
    e.push(rtp(0, 6000, true, idr), kMs, out);
    e.poll(kMs + e.reorderHoldNs(), out);
    QCOMPARE(out.size(), 2);
    QCOMPARE(e.stats().packetsMissing, quint64(1));
    QCOMPARE(e.stats().senderChanges, quint64(0));
}

void RtpDepacketizerTest::_duplicates()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    d.push(rtp(1, 0, true, idr), 0, out);
    d.push(rtp(1, 0, true, idr), 1, out);          // copy of one already parsed
    d.push(rtp(3, 6000, true, pslice), 2, out);    // waits for 2
    d.push(rtp(3, 6000, true, pslice), 3, out);    // copy of one waiting
    QCOMPARE(d.heldPackets(), 1);
    d.push(rtp(2, 3000, true, pslice), 4, out);
    d.push(rtp(3, 6000, true, pslice), 5, out);    // copy after it went out
    d.push(rtp(2, 3000, true, pslice), 6, out);
    QCOMPARE(out.size(), 3);
    QCOMPARE(d.stats().duplicates, quint64(4));
    QCOMPARE(d.stats().refused, quint64(4));
    QCOMPARE(d.stats().late, quint64(0));
    QCOMPARE(d.stats().packetsMissing, quint64(0));
    QCOMPARE(d.stats().packets, quint64(3));
    QVERIFY(!out[2].afterLoss);

    // A whole frame sent twice in a row (a link that duplicates everything)
    out.clear();
    const QList<QByteArray> frame = fragmentedFrame(4, 9000, false, 4);
    for (const QByteArray &packet : frame) {
        d.push(packet, 10, out);
        d.push(packet, 11, out);
    }
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].data, fragmentedData(false, 4));
    QCOMPARE(d.stats().duplicates, quint64(8));
}

void RtpDepacketizerTest::_bigJumpResetsWindow()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    d.push(rtp(100, 0, true, idr), 0, out);
    for (quint16 seq = 102; seq < 140; ++seq) {   // 101 missing: 38 wait
        d.push(rtp(seq, seq * 3000u, true, pslice), seq * 1000, out);
    }
    QCOMPARE(d.heldPackets(), 38);

    // Far outside the window either way: a restart, the window is empty again
    d.push(rtp(30000, 777, true, idr), 200000, out);
    QCOMPARE(d.heldPackets(), 0);
    QCOMPARE(d.nextDeadlineNs(), qint64(0));
    QCOMPARE(d.stats().senderChanges, quint64(1));
    QCOMPARE(out.size(), 2);
    QCOMPARE(out[1].rtpTimestamp, quint32(777));

    // The stream goes on from the new place; the old one's packets are refused or restart
    d.push(rtp(30001, 3777, true, pslice), 200001, out);
    QCOMPARE(out.size(), 3);
    d.push(rtp(30002 - 500, 0, true, pslice), 200002, out);   // 500 back: late, refused
    QCOMPARE(out.size(), 3);
    QCOMPARE(d.stats().late, quint64(1));

    // Back jumps further than kRestartJump restart too (the window never grows backwards)
    d.push(rtp(quint16(30002 - 2000), 0, true, idr), 200003, out);
    QCOMPARE(d.stats().senderChanges, quint64(2));
    QCOMPARE(d.heldPackets(), 0);
}

void RtpDepacketizerTest::_floodStaysBounded()
{
    constexpr int kWindow = 64;
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    d.push(rtp(0, 0, true, idr), 0, out);
    Rng rng{ 0x5eed5u };
    quint16 base = 1;
    qint64 t = kMs;
    qint64 maxHeldBytes = 0;
    for (int i = 0; i < 200000; ++i) {
        // Mostly inside or just past the window, never the expected packet; all at one instant
        // for long stretches, so only the count bound holds them in
        const int kind = rng.below(10);
        quint16 seq;
        if (kind < 6) {
            seq = quint16(base + 1 + rng.below(kWindow - 1));
        } else if (kind < 8) {
            seq = quint16(base + kWindow + rng.below(RtpDepacketizer::kRestartJump - kWindow));
        } else if (kind < 9) {
            seq = quint16(base - rng.below(RtpDepacketizer::kRestartJump));
        } else {
            seq = quint16(rng.next());
        }
        QByteArray payload = bytes({ 0x7c, 0x05 }) + QByteArray(1 + rng.below(1400), char(0x33));
        d.push(rtp(seq, quint32(i / 20) * 3000u, rng.below(8) == 0, payload), t, out);
        out.clear();
        QVERIFY2(d.heldPackets() < kWindow, qPrintable(QStringLiteral("iteration %1").arg(i)));
        maxHeldBytes = std::max(maxHeldBytes, qint64(d.heldPackets()) * 1401);
        if (d.nextDeadlineNs() > 0) {
            QVERIFY(d.nextDeadlineNs() <= t + d.reorderHoldNs());
        }
        base = quint16(seq);
        if ((i % 1000) == 0) {
            t += kMs;
        }
    }
    QVERIFY(maxHeldBytes < kWindow * 1401);
    d.poll(t + d.reorderHoldNs(), out);
    QCOMPARE(d.heldPackets(), 0);

    // A flood of copies of one waiting packet holds one slot
    RtpDepacketizer e(RtpDepacketizer::Codec::H264);
    e.push(rtp(10, 0, true, idr), 0, out);
    for (int i = 0; i < 10000; ++i) {
        e.push(rtp(12, 3000, true, pslice), 1, out);
    }
    QCOMPARE(e.heldPackets(), 1);
    QCOMPARE(e.stats().duplicates, quint64(9999));
}

void RtpDepacketizerTest::_inOrderNoDelay()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    quint16 seq = 65000;    // through the wrap too
    for (int frame = 0; frame < 300; ++frame) {
        const bool key = (frame % 30) == 0;
        const QList<QByteArray> packets = fragmentedFrame(seq, quint32(frame) * 3000u, key, 4);
        for (int i = 0; i < packets.size(); ++i) {
            const qsizetype before = out.size();
            d.push(packets[i], (frame * 4 + i) * kMs, out);
            QCOMPARE(d.heldPackets(), 0);
            QCOMPARE(d.nextDeadlineNs(), qint64(0));
            // the frame comes out with its last packet, in the same call: no wait
            QCOMPARE(out.size(), before + ((i == packets.size() - 1) ? 1 : 0));
        }
        seq = quint16(seq + 4);
    }
    QCOMPARE(out.size(), 300);
    QCOMPARE(d.stats().held, quint64(0));
    QCOMPARE(d.stats().holdNsMax, qint64(0));
    QCOMPARE(d.stats().reordered, quint64(0));
    QCOMPARE(d.stats().packetsMissing, quint64(0));
}

void RtpDepacketizerTest::_afterLossOnlyOnRealLoss()
{
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    d.push(rtp(1, 0, true, idr), 0, out);
    d.push(rtp(3, 6000, true, pslice), kMs, out);
    d.push(rtp(2, 3000, true, pslice), 2 * kMs, out);   // reordered and recovered
    QCOMPARE(out.size(), 3);
    for (const AU &au : std::as_const(out)) {
        QVERIFY(!au.afterLoss);
    }
    // a real loss: 4 never comes; the P frame after it is dropped, the next IDR is marked
    d.push(rtp(5, 12000, true, pslice), 3 * kMs, out);
    d.poll(3 * kMs + d.reorderHoldNs(), out);
    QCOMPARE(out.size(), 3);
    d.push(rtp(6, 15000, true, idr), 60 * kMs, out);
    QCOMPARE(out.size(), 4);
    QVERIFY(out[3].afterLoss);
    d.push(rtp(7, 18000, true, pslice), 61 * kMs, out);
    QCOMPARE(out.size(), 5);
    QVERIFY(!out[4].afterLoss);
}

namespace {

/// What an H.264 decoder makes of the units, in order: a unit is an error when it has a
/// slice before the SPS and PPS are known, or a P slice before any IDR (no reference).
int decodeErrors(const QList<AU> &out)
{
    bool haveSps = false;
    bool havePps = false;
    bool haveReference = false;
    int errors = 0;
    for (const AU &au : out) {
        bool error = false;
        qsizetype at = au.data.indexOf(sc);
        while (at >= 0) {
            const qsizetype start = at + sc.size();
            const qsizetype next = au.data.indexOf(sc, start);
            if (start < au.data.size()) {
                const int type = static_cast<quint8>(au.data.at(start)) & 0x1f;
                if (type == 7) {
                    haveSps = true;
                } else if (type == 8) {
                    havePps = true;
                } else if (type == 5) {
                    if (haveSps && havePps) {
                        haveReference = true;
                    } else {
                        error = true;
                    }
                } else if (type == 1) {
                    if (!haveSps || !havePps || !haveReference) {
                        error = true;
                    }
                }
            }
            at = next;
        }
        if (error) {
            errors++;
        }
    }
    return errors;
}

/// A sender's stream as the ffmpeg RTP muxer sends libx264 output: a key frame is a STAP-A
/// with the SPS and PPS, then the IDR in FU-A pieces; P frames are FU-A pieces. One frame
/// every 3000 ticks (30 fps); a key frame every gop frames.
QList<QByteArray> senderStream(quint16 seq, int frames, int gop, int pieces)
{
    QList<QByteArray> packets;
    for (int f = 0; f < frames; ++f) {
        const quint32 ts = 90000 + 3000 * static_cast<quint32>(f);
        const bool key = (f % gop) == 0;
        if (key) {
            packets << rtp(seq, ts, false, stapA({ sps, pps }));
            seq = static_cast<quint16>(seq + 1);
        }
        packets << fragmentedFrame(seq, ts, key, pieces);
        seq = static_cast<quint16>(seq + pieces);
    }
    return packets;
}

/// Indexes: order first, then every packet from next on
QList<int> arrival(const QList<int> &order, int next, int count)
{
    QList<int> all = order;
    for (int i = next; i < count; ++i) {
        all << i;
    }
    return all;
}

} // namespace

void RtpDepacketizerTest::_startReorderedFirstPackets()
{
    // The sender starts after GC listens and its first packets come out of order. Before the
    // fix the STAP-A (sent first, arrived second) was refused as late and the first IDR went
    // to the decoder without its SPS and PPS: every frame of that GOP failed to decode.
    const QList<QByteArray> stream = senderStream(40000, 8, 4, 6);
    QCOMPARE(stream.size(), 2 * (1 + 6) + 6 * 6);

    const QList<QList<int>> starts = {
        { 1, 0, 3, 2 },     // the STAP-A one place late, then two IDR pieces swapped
        { 2, 0, 1 },        // the first IDR piece and the STAP-A both after the second piece
        { 3, 1, 2, 0 },     // the STAP-A three places late
    };
    for (const QList<int> &start : starts) {
        RtpDepacketizer d(RtpDepacketizer::Codec::H264);
        QList<AU> out;
        qint64 t = 0;
        for (int i : start) {
            d.push(stream[i], t, out);
            t += kMs / 10;
        }
        QVERIFY(out.isEmpty());
        for (int i = static_cast<int>(start.size()); i < stream.size(); ++i) {
            d.push(stream[i], t, out);
            t += kMs / 10;
            if (i == 6) {
                // the first IDR goes out with its last piece: no added wait at the start
                QCOMPARE(out.size(), 1);
            }
        }
        QCOMPARE(out.size(), 8);
        QVERIFY(out[0].keyFrame);
        QCOMPARE(out[0].data, sc + sps + sc + pps + fragmentedData(true, 6));
        QCOMPARE(out[0].rtpTimestamp, quint32(90000));
        QCOMPARE(decodeErrors(out), 0);
        for (const AU &au : std::as_const(out)) {
            QVERIFY(!au.afterLoss);
        }
        QCOMPARE(d.stats().late, quint64(0));
        QCOMPARE(d.stats().refused, quint64(0));
        QCOMPARE(d.stats().packetsMissing, quint64(0));
        QCOMPARE(d.stats().droppedAccessUnits, quint64(0));
        QCOMPARE(d.stats().packets, quint64(stream.size()));
        QCOMPARE(d.heldPackets(), 0);
        QCOMPARE(d.nextDeadlineNs(), qint64(0));
    }

    // The same across the sequence number wrap
    const QList<QByteArray> wrapped = senderStream(65534, 4, 4, 6);
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    for (int i : arrival({ 2, 0, 1 }, 3, static_cast<int>(wrapped.size()))) {
        d.push(wrapped[i], 0, out);
    }
    QCOMPARE(out.size(), 4);
    QCOMPARE(out[0].data, sc + sps + sc + pps + fragmentedData(true, 6));
    QCOMPARE(decodeErrors(out), 0);
    QCOMPARE(d.stats().late, quint64(0));
}

void RtpDepacketizerTest::_startMidGop()
{
    // GC starts listening in the middle of a GOP, in the middle of a P frame, and the first
    // packets it gets are out of order: nothing reaches the decoder before the next IDR, and
    // that IDR comes with its parameter sets.
    const QList<QByteArray> stream = senderStream(20000, 12, 6, 4);   // key frames at frames 0 and 6
    // Packets 0..4 are frame 0 (STAP-A and 4 IDR pieces), then 4 per P frame: frame 2 is 9..12.
    // GC gets frame 2 from its second piece (the first was sent before it listened), third first.
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    QList<AU> out;
    qint64 t = 0;
    for (int i : arrival({ 11, 10, 12 }, 13, static_cast<int>(stream.size()))) {
        d.push(stream[i], t, out);
        t += kMs;
        d.poll(t, out);
    }
    QCOMPARE(out.size(), 6);    // frames 6 to 11
    QVERIFY(out[0].keyFrame);
    QVERIFY(out[0].data.startsWith(sc + sps + sc + pps));
    QCOMPARE(out[0].rtpTimestamp, quint32(90000 + 6 * 3000));
    QCOMPARE(decodeErrors(out), 0);
    for (int i = 1; i < out.size(); ++i) {
        QVERIFY(!out[i].keyFrame);
        QCOMPARE(out[i].rtpTimestamp, out[i - 1].rtpTimestamp + 3000);
    }
    QCOMPARE(d.stats().late, quint64(0));
    QCOMPARE(d.stats().packetsMissing, quint64(0));
    // the cut P frame 2 and the P frames 3 to 5 (no key frame yet)
    QCOMPARE(d.stats().droppedAccessUnits, quint64(4));

    // Earlier packets that come after the first frame closed are refused as late, never put
    // in front of a unit already out
    RtpDepacketizer e(RtpDepacketizer::Codec::H264);
    out.clear();
    for (int i : arrival({ 1, 2, 3, 4, 0 }, 5, static_cast<int>(stream.size()))) {
        e.push(stream[i], 0, out);
    }
    QCOMPARE(e.stats().late, quint64(1));
    QCOMPARE(out.size(), 12);
    QCOMPARE(out[0].data, fragmentedData(true, 4));   // the IDR as it closed, nothing added after
}

void RtpDepacketizerTest::_startLateCannotRewind()
{
    // No window: the STAP-A that comes after the first IDR piece cannot be put back. That
    // IDR is incomplete and dropped, never decoded without its parameter sets; the picture
    // starts at the next key frame.
    const QList<QByteArray> stream = senderStream(500, 8, 4, 5);
    RtpDepacketizer d(RtpDepacketizer::Codec::H264);
    d.setReorderWindow(0, 0);
    QList<AU> out;
    for (int i : arrival({ 1, 0 }, 2, static_cast<int>(stream.size()))) {
        d.push(stream[i], 0, out);
    }
    QCOMPARE(d.stats().late, quint64(1));
    QCOMPARE(out.size(), 4);    // frames 4 to 7
    QVERIFY(out[0].keyFrame);
    QCOMPARE(out[0].rtpTimestamp, quint32(90000 + 4 * 3000));
    QVERIFY(out[0].data.startsWith(sc + sps + sc + pps));
    QCOMPARE(decodeErrors(out), 0);

    // A window, but a first key frame larger than it: the STAP-A comes after more IDR pieces
    // than the window holds, so the start is not rebuilt and that IDR is dropped as well
    const QList<QByteArray> big = senderStream(900, 8, 4, 12);
    RtpDepacketizer w(RtpDepacketizer::Codec::H264);
    w.setReorderWindow(8, 50 * kMs);
    out.clear();
    for (int i : arrival({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 0 }, 10, static_cast<int>(big.size()))) {
        w.push(big[i], 0, out);
    }
    QCOMPARE(w.stats().late, quint64(1));
    QCOMPARE(out.size(), 4);
    QCOMPARE(out[0].rtpTimestamp, quint32(90000 + 4 * 3000));
    QCOMPARE(decodeErrors(out), 0);
}

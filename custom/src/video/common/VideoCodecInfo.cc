/****************************************************************************
 *
 * RotorLab GC - what a video stream's parameter sets say, without decoding.
 *
 ****************************************************************************/

#include "VideoCodecInfo.h"

namespace {

/// MSB-first bit reader over an RBSP. Reading past the end sets ok = false and
/// returns zeros; callers check ok once at the end.
class BitReader
{
public:
    explicit BitReader(const QByteArray &d) : _d(d) {}

    bool ok() const { return _ok; }

    quint32 u(int n)
    {
        quint32 v = 0;
        for (int i = 0; i < n; ++i) {
            if (_bit >= qint64(_d.size()) * 8) {
                _ok = false;
                return 0;
            }
            const quint8 byte = quint8(_d.at(qsizetype(_bit >> 3)));
            v = (v << 1) | ((byte >> (7 - (_bit & 7))) & 1u);
            ++_bit;
        }
        return v;
    }

    void skip(int n)
    {
        _bit += n;
        if (_bit > qint64(_d.size()) * 8) {
            _ok = false;
        }
    }

    quint32 ue()
    {
        int zeros = 0;
        while (_ok && u(1) == 0) {
            if (++zeros > 31) {
                _ok = false;
                return 0;
            }
        }
        if (!_ok) {
            return 0;
        }
        if (zeros == 0) {
            return 0;
        }
        return ((1u << zeros) - 1u) + u(zeros);
    }

    qint32 se()
    {
        const quint32 k = ue();
        return (k & 1u) ? qint32((k + 1u) / 2u) : -qint32(k / 2u);
    }

private:
    const QByteArray &_d;
    qint64 _bit = 0;
    bool _ok = true;
};

bool sizeOk(int w, int h)
{
    return w > 0 && h > 0 && w <= 16384 && h <= 16384;
}

VideoCodecInfo parseH264(const QByteArray &nal)
{
    VideoCodecInfo info;
    if (nal.size() < 4) {
        return info;
    }
    const QByteArray rbsp = VideoCodecInfo::unescapeRbsp(nal.mid(1));
    BitReader b(rbsp);
    const quint32 profile = b.u(8);
    const quint32 constraints = b.u(8);
    const quint32 level = b.u(8);
    (void) b.ue();                                  // seq_parameter_set_id

    quint32 chroma = 1;
    bool separatePlanes = false;
    switch (profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86:
    case 118: case 128: case 138: case 139: case 134: case 135: {
        chroma = b.ue();
        if (chroma > 3) {
            return info;
        }
        if (chroma == 3) {
            separatePlanes = b.u(1) != 0;
        }
        (void) b.ue();                              // bit_depth_luma_minus8
        (void) b.ue();                              // bit_depth_chroma_minus8
        (void) b.u(1);                              // qpprime_y_zero_transform_bypass_flag
        if (b.u(1)) {                               // seq_scaling_matrix_present_flag
            const int lists = (chroma != 3) ? 8 : 12;
            for (int i = 0; i < lists && b.ok(); ++i) {
                if (!b.u(1)) {
                    continue;
                }
                const int size = (i < 6) ? 16 : 64;
                int last = 8;
                int next = 8;
                for (int j = 0; j < size && b.ok(); ++j) {
                    if (next != 0) {
                        const qint32 delta = b.se();
                        next = (last + delta + 256) % 256;
                    }
                    last = (next == 0) ? last : next;
                }
            }
        }
        break;
    }
    default:
        break;
    }

    (void) b.ue();                                  // log2_max_frame_num_minus4
    const quint32 pocType = b.ue();
    if (pocType == 0) {
        (void) b.ue();                              // log2_max_pic_order_cnt_lsb_minus4
    } else if (pocType == 1) {
        (void) b.u(1);
        (void) b.se();
        (void) b.se();
        const quint32 cycle = b.ue();
        if (cycle > 255) {
            return info;
        }
        for (quint32 i = 0; i < cycle && b.ok(); ++i) {
            (void) b.se();
        }
    } else if (pocType > 2) {
        return info;
    }
    (void) b.ue();                                  // max_num_ref_frames
    (void) b.u(1);                                  // gaps_in_frame_num_value_allowed_flag
    const quint32 wMbs = b.ue() + 1;
    const quint32 hMapUnits = b.ue() + 1;
    const quint32 frameMbsOnly = b.u(1);
    if (!frameMbsOnly) {
        (void) b.u(1);                              // mb_adaptive_frame_field_flag
    }
    (void) b.u(1);                                  // direct_8x8_inference_flag
    quint32 cl = 0, cr = 0, ct = 0, cb = 0;
    if (b.u(1)) {
        cl = b.ue(); cr = b.ue(); ct = b.ue(); cb = b.ue();
    }
    if (!b.ok() || wMbs > 1024 || hMapUnits > 1024) {
        return info;
    }

    const int chromaArrayType = separatePlanes ? 0 : int(chroma);
    const int fieldFactor = 2 - int(frameMbsOnly);
    int cropX = 1;
    int cropY = fieldFactor;
    if (chromaArrayType == 1) {
        cropX = 2;
        cropY = 2 * fieldFactor;
    } else if (chromaArrayType == 2) {
        cropX = 2;
        cropY = fieldFactor;
    }
    const qint64 w = qint64(wMbs) * 16 - qint64(cropX) * (qint64(cl) + cr);
    const qint64 h = qint64(fieldFactor) * hMapUnits * 16 - qint64(cropY) * (qint64(ct) + cb);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || !sizeOk(int(w), int(h))) {
        return info;
    }

    info.valid = true;
    info.width = int(w);
    info.height = int(h);
    info.codecString = QStringLiteral("avc1.%1%2%3")
        .arg(profile, 2, 16, QLatin1Char('0'))
        .arg(constraints, 2, 16, QLatin1Char('0'))
        .arg(level, 2, 16, QLatin1Char('0'));
    return info;
}

quint32 reverseBits32(quint32 v)
{
    quint32 r = 0;
    for (int i = 0; i < 32; ++i) {
        r = (r << 1) | (v & 1u);
        v >>= 1;
    }
    return r;
}

VideoCodecInfo parseH265(const QByteArray &nal)
{
    VideoCodecInfo info;
    info.h265 = true;
    if (nal.size() < 4) {
        return info;
    }
    const QByteArray rbsp = VideoCodecInfo::unescapeRbsp(nal.mid(2));
    BitReader b(rbsp);
    (void) b.u(4);                                  // sps_video_parameter_set_id
    const int maxSubLayersMinus1 = int(b.u(3));
    (void) b.u(1);                                  // sps_temporal_id_nesting_flag

    // profile_tier_level(1, maxSubLayersMinus1)
    const quint32 profileSpace = b.u(2);
    const quint32 tier = b.u(1);
    const quint32 profileIdc = b.u(5);
    const quint32 compat = b.u(32);
    quint8 constraint[6];
    for (quint8 &c : constraint) {
        c = quint8(b.u(8));
    }
    const quint32 level = b.u(8);
    bool subProfile[8] = {};
    bool subLevel[8] = {};
    for (int i = 0; i < maxSubLayersMinus1; ++i) {
        subProfile[i] = b.u(1) != 0;
        subLevel[i] = b.u(1) != 0;
    }
    if (maxSubLayersMinus1 > 0) {
        for (int i = maxSubLayersMinus1; i < 8; ++i) {
            (void) b.u(2);
        }
    }
    for (int i = 0; i < maxSubLayersMinus1; ++i) {
        if (subProfile[i]) {
            b.skip(88);
        }
        if (subLevel[i]) {
            b.skip(8);
        }
    }

    (void) b.ue();                                  // sps_seq_parameter_set_id
    const quint32 chroma = b.ue();
    if (chroma > 3) {
        return info;
    }
    bool separatePlanes = false;
    if (chroma == 3) {
        separatePlanes = b.u(1) != 0;
    }
    const quint32 w = b.ue();
    const quint32 h = b.ue();
    quint32 cl = 0, cr = 0, ct = 0, cb = 0;
    if (b.u(1)) {
        cl = b.ue(); cr = b.ue(); ct = b.ue(); cb = b.ue();
    }
    if (!b.ok()) {
        return info;
    }
    const quint32 effChroma = separatePlanes ? 0 : chroma;
    const int subW = (effChroma == 1 || effChroma == 2) ? 2 : 1;
    const int subH = (effChroma == 1) ? 2 : 1;
    const qint64 width = qint64(w) - qint64(subW) * (qint64(cl) + cr);
    const qint64 height = qint64(h) - qint64(subH) * (qint64(ct) + cb);
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        return info;
    }

    QString s = QStringLiteral("hvc1.");
    if (profileSpace > 0) {
        s += QChar(QLatin1Char(char('A' + profileSpace - 1)));
    }
    s += QString::number(profileIdc);
    s += QStringLiteral(".%1").arg(QString::number(reverseBits32(compat), 16).toUpper());
    s += QStringLiteral(".%1%2").arg(tier ? QStringLiteral("H") : QStringLiteral("L")).arg(level);
    int lastNonZero = -1;
    for (int i = 0; i < 6; ++i) {
        if (constraint[i] != 0) {
            lastNonZero = i;
        }
    }
    for (int i = 0; i <= lastNonZero; ++i) {
        s += QStringLiteral(".%1").arg(QString::number(constraint[i], 16).toUpper());
    }

    info.valid = true;
    info.width = int(width);
    info.height = int(height);
    info.codecString = s;
    return info;
}

} // namespace

QList<QByteArray> VideoCodecInfo::splitAnnexB(const QByteArray &annexB)
{
    QList<QByteArray> out;
    const qsizetype n = annexB.size();
    const char *d = annexB.constData();
    qsizetype i = 0;
    qsizetype start = -1;
    while (i + 2 < n) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (start >= 0) {
                qsizetype end = i;
                while (end > start && d[end - 1] == 0) {
                    --end;                          // the zero of a 4-byte start code, or trailing zeros
                }
                if (end > start) {
                    out.append(annexB.mid(start, end - start));
                }
            }
            i += 3;
            start = i;
        } else {
            ++i;
        }
    }
    if (start >= 0 && start < n) {
        out.append(annexB.mid(start));
    }
    return out;
}

QByteArray VideoCodecInfo::unescapeRbsp(const QByteArray &p)
{
    QByteArray out;
    out.reserve(p.size());
    int zeros = 0;
    for (qsizetype i = 0; i < p.size(); ++i) {
        const char c = p.at(i);
        if (zeros >= 2 && c == 0x03) {
            zeros = 0;
            continue;
        }
        out.append(c);
        zeros = (c == 0) ? zeros + 1 : 0;
    }
    return out;
}

bool VideoCodecInfo::isSps(bool h265, const QByteArray &nal)
{
    if (nal.isEmpty()) {
        return false;
    }
    const quint8 h = quint8(nal.at(0));
    return h265 ? (((h >> 1) & 0x3f) == 33) : ((h & 0x1f) == 7);
}

VideoCodecInfo VideoCodecInfo::fromSps(bool h265, const QByteArray &spsNal)
{
    return h265 ? parseH265(spsNal) : parseH264(spsNal);
}

VideoCodecInfo VideoCodecInfo::fromAnnexB(bool h265, const QByteArray &annexB)
{
    for (const QByteArray &nal : splitAnnexB(annexB)) {
        if (isSps(h265, nal)) {
            return fromSps(h265, nal);
        }
    }
    VideoCodecInfo none;
    none.h265 = h265;
    return none;
}

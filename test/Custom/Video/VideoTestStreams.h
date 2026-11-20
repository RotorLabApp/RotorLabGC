#pragma once

/// G95 test streams: valid H.264 and H.265 parameter sets built bit by bit,
/// access units with dummy slice data (the muxer never decodes), tap packets,
/// and a small ISO BMFF box walker. No encoder, no camera, no files.

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QList>
#include <QtCore/QString>

#include "VideoPacketTap.h"

namespace VideoTest {

class BitWriter
{
public:
    void u(quint32 v, int n)
    {
        for (int i = n - 1; i >= 0; --i) {
            bit((v >> i) & 1u);
        }
    }
    void ue(quint32 v)
    {
        const quint32 x = v + 1;
        int len = 0;
        while ((x >> len) > 1) {
            ++len;
        }
        u(0, len);
        u(x, len + 1);
    }
    void se(qint32 v)
    {
        ue(v <= 0 ? quint32(-2 * v) : quint32(2 * v - 1));
    }
    /// rbsp_trailing_bits
    QByteArray finish()
    {
        bit(1);
        while (_n != 0) {
            bit(0);
        }
        return _out;
    }

private:
    void bit(quint32 b)
    {
        _cur = quint8((_cur << 1) | (b & 1u));
        if (++_n == 8) {
            _out.append(char(_cur));
            _cur = 0;
            _n = 0;
        }
    }
    QByteArray _out;
    quint8 _cur = 0;
    int _n = 0;
};

/// Emulation prevention (00 00 0x -> 00 00 03 0x for x <= 3).
inline QByteArray escape(const QByteArray &rbsp)
{
    QByteArray out;
    int zeros = 0;
    for (const char c : rbsp) {
        if (zeros >= 2 && quint8(c) <= 3) {
            out.append(char(3));
            zeros = 0;
        }
        out.append(c);
        zeros = (c == 0) ? zeros + 1 : 0;
    }
    return out;
}

inline QByteArray sc() { return QByteArray("\x00\x00\x00\x01", 4); }

/// H.264 High profile SPS (NAL header included), level 4.0, 4:2:0, with
/// cropping to `width` x `height` (multiples of 2).
inline QByteArray h264Sps(int width, int height)
{
    const int wMbs = (width + 15) / 16;
    const int hMbs = (height + 15) / 16;
    BitWriter b;
    b.u(100, 8);        // profile_idc High
    b.u(0, 8);          // constraint flags
    b.u(40, 8);         // level_idc
    b.ue(0);            // sps id
    b.ue(1);            // chroma_format_idc 4:2:0
    b.ue(0);            // bit_depth_luma_minus8
    b.ue(0);            // bit_depth_chroma_minus8
    b.u(0, 1);          // qpprime_y_zero_transform_bypass
    b.u(0, 1);          // seq_scaling_matrix_present
    b.ue(0);            // log2_max_frame_num_minus4
    b.ue(2);            // pic_order_cnt_type
    b.ue(1);            // max_num_ref_frames
    b.u(0, 1);          // gaps
    b.ue(quint32(wMbs - 1));
    b.ue(quint32(hMbs - 1));
    b.u(1, 1);          // frame_mbs_only
    b.u(1, 1);          // direct_8x8_inference
    const int cropR = (wMbs * 16 - width) / 2;
    const int cropB = (hMbs * 16 - height) / 2;
    if (cropR || cropB) {
        b.u(1, 1);
        b.ue(0);
        b.ue(quint32(cropR));
        b.ue(0);
        b.ue(quint32(cropB));
    } else {
        b.u(0, 1);
    }
    b.u(0, 1);          // vui_parameters_present
    return QByteArray(1, char(0x67)) + escape(b.finish());
}

inline QByteArray h264Pps()
{
    BitWriter b;
    b.ue(0); b.ue(0);   // pps id, sps id
    b.u(0, 1);          // entropy_coding_mode (CAVLC)
    b.u(0, 1);          // bottom_field_pic_order_in_frame_present
    b.ue(0);            // num_slice_groups_minus1
    b.ue(0); b.ue(0);   // num_ref_idx l0, l1
    b.u(0, 1);          // weighted_pred
    b.u(0, 2);          // weighted_bipred_idc
    b.se(0); b.se(0); b.se(0);
    b.u(1, 1);          // deblocking_filter_control_present
    b.u(0, 1);          // constrained_intra_pred
    b.u(0, 1);          // redundant_pic_cnt_present
    return QByteArray(1, char(0x68)) + escape(b.finish());
}

/// HEVC profile_tier_level for Main, level 3.1, no sub-layers.
inline void h265Ptl(BitWriter &b)
{
    b.u(0, 2);              // general_profile_space
    b.u(0, 1);              // general_tier_flag
    b.u(1, 5);              // general_profile_idc Main
    b.u(0x60000000u, 32);   // compatibility flags 1 and 2
    b.u(0x9, 4);            // progressive 1, interlaced 0, non_packed 0, frame_only 1
    b.u(0, 32);             // 43 reserved bits and inbld: 44 zero bits
    b.u(0, 12);
    b.u(93, 8);             // general_level_idc (3.1)
}

inline QByteArray h265Vps()
{
    BitWriter b;
    b.u(0, 4);          // vps id
    b.u(1, 1);          // base_layer_internal
    b.u(1, 1);          // base_layer_available
    b.u(0, 6);          // max_layers_minus1
    b.u(0, 3);          // max_sub_layers_minus1
    b.u(1, 1);          // temporal_id_nesting
    b.u(0xffff, 16);
    h265Ptl(b);
    b.u(1, 1);          // sub_layer_ordering_info_present
    b.ue(4); b.ue(0); b.ue(0);
    b.u(0, 6);          // vps_max_layer_id
    b.ue(0);            // num_layer_sets_minus1
    b.u(0, 1);          // timing_info_present
    b.u(0, 1);          // extension
    return QByteArray("\x40\x01", 2) + escape(b.finish());
}

inline QByteArray h265Sps(int width, int height)
{
    BitWriter b;
    b.u(0, 4);          // vps id
    b.u(0, 3);          // max_sub_layers_minus1
    b.u(1, 1);          // temporal_id_nesting
    h265Ptl(b);
    b.ue(0);            // sps id
    b.ue(1);            // chroma 4:2:0
    b.ue(quint32(width));
    b.ue(quint32(height));
    b.u(0, 1);          // conformance_window
    b.ue(0); b.ue(0);   // bit depths
    b.ue(4);            // log2_max_pic_order_cnt_lsb_minus4
    b.u(1, 1);          // sub_layer_ordering_info_present
    b.ue(4); b.ue(0); b.ue(0);
    b.ue(0);            // log2_min_luma_coding_block_size_minus3
    b.ue(3);            // log2_diff_max_min_luma_coding_block_size
    b.ue(0);            // log2_min_luma_transform_block_size_minus2
    b.ue(3);            // log2_diff_max_min_luma_transform_block_size
    b.ue(0); b.ue(0);   // max_transform_hierarchy_depth inter, intra
    b.u(0, 1);          // scaling_list_enabled
    b.u(0, 1);          // amp
    b.u(0, 1);          // sample_adaptive_offset
    b.u(0, 1);          // pcm
    b.ue(0);            // num_short_term_ref_pic_sets
    b.u(0, 1);          // long_term_ref_pics_present
    b.u(0, 1);          // temporal_mvp
    b.u(0, 1);          // strong_intra_smoothing
    b.u(0, 1);          // vui
    b.u(0, 1);          // extension
    return QByteArray("\x42\x01", 2) + escape(b.finish());
}

inline QByteArray h265Pps()
{
    BitWriter b;
    b.ue(0); b.ue(0);
    b.u(0, 1); b.u(0, 1); b.u(0, 3); b.u(0, 1); b.u(0, 1);
    b.ue(0); b.ue(0);
    b.se(0);
    b.u(0, 1); b.u(0, 1); b.u(0, 1);
    b.se(0); b.se(0);
    b.u(0, 1); b.u(0, 1); b.u(0, 1); b.u(0, 1); b.u(0, 1); b.u(0, 1);
    b.u(0, 1); b.u(0, 1); b.u(0, 1); b.u(0, 1);
    b.ue(0);
    b.u(0, 1); b.u(0, 1);
    return QByteArray("\x44\x01", 2) + escape(b.finish());
}

inline QByteArray parameterSets(bool h265, int width, int height)
{
    if (h265) {
        return sc() + h265Vps() + sc() + h265Sps(width, height) + sc() + h265Pps();
    }
    return sc() + h264Sps(width, height) + sc() + h264Pps();
}

/// One access unit: in-band parameter sets on keyframes, then a slice of
/// `size` dummy bytes (0x5a, no start code inside).
inline QByteArray accessUnit(bool h265, bool key, int size, int width = 1280, int height = 720)
{
    QByteArray au;
    if (key) {
        au += parameterSets(h265, width, height);
    }
    QByteArray slice;
    if (h265) {
        slice = key ? QByteArray("\x26\x01", 2) : QByteArray("\x02\x01", 2);
    } else {
        slice = QByteArray(1, key ? char(0x65) : char(0x41));
    }
    slice += QByteArray(qMax(1, size), char(0x5a));
    au += sc() + slice;
    return au;
}

/// A tap packet as the C1 (UDP RTP) path publishes it, 30 fps.
inline VideoTapPacket packet(bool h265, quint64 i, bool key, int size = 2000,
                             quint64 generation = 1, int width = 1280, int height = 720,
                             qint64 steadyBase = 1000000000LL, qint64 utcBase = 1795000000000LL)
{
    VideoTapPacket p;
    p.codec = h265 ? VideoTapCodec::H265 : VideoTapCodec::H264;
    p.data = accessUnit(h265, key, size, width, height);
    p.parameterSets = parameterSets(h265, width, height);
    p.keyFrame = key;
    p.discontinuity = (i == 0);
    p.hasRtpTimestamp = true;
    p.rtpTimestamp = quint32(0xfffff000u + quint32(i * 3000));     // wraps early on purpose
    p.pts90k = p.dts90k = qint64(p.rtpTimestamp);
    p.receiveSteadyNs = steadyBase + qint64(i) * 33333333LL;
    p.receiveUtcMs = utcBase + qint64(i) * 33;
    p.generation = generation;
    p.index = i + 1;
    return p;
}

inline QByteArray sha256Hex(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

// ------------------------------------------------------------------ boxes

struct Box
{
    QByteArray type;
    qint64 offset = 0;          ///< of the box header in the buffer
    qint64 size = 0;            ///< whole box
    qint64 header = 8;
    QList<Box> children;
    QByteArray payload(const QByteArray &buf) const { return buf.mid(offset + header, size - header); }
};

inline quint32 be32(const QByteArray &b, qint64 at)
{
    if (at < 0 || at + 4 > b.size()) {
        return 0;
    }
    return (quint32(quint8(b[at])) << 24) | (quint32(quint8(b[at + 1])) << 16)
         | (quint32(quint8(b[at + 2])) << 8) | quint32(quint8(b[at + 3]));
}

inline quint64 be64(const QByteArray &b, qint64 at)
{
    return (quint64(be32(b, at)) << 32) | be32(b, at + 4);
}

inline bool isContainer(const QByteArray &t)
{
    return t == "moov" || t == "trak" || t == "mdia" || t == "minf" || t == "stbl" || t == "mvex"
        || t == "moof" || t == "traf" || t == "dinf";
}

/// Walk [from, to) of `buf`. `ok` false on a size that runs off the end.
inline QList<Box> walk(const QByteArray &buf, qint64 from, qint64 to, bool *ok)
{
    QList<Box> out;
    qint64 at = from;
    while (at < to) {
        if (at + 8 > to) {
            *ok = false;
            return out;
        }
        Box box;
        box.offset = at;
        box.size = be32(buf, at);
        box.type = buf.mid(at + 4, 4);
        if (box.size == 1) {
            box.size = qint64(be64(buf, at + 8));
            box.header = 16;
        }
        if (box.size < box.header || at + box.size > to) {
            *ok = false;
            return out;
        }
        if (isContainer(box.type)) {
            box.children = walk(buf, at + box.header, at + box.size, ok);
        }
        out.append(box);
        at += box.size;
    }
    return out;
}

inline const Box *find(const QList<Box> &boxes, const QByteArray &type)
{
    for (const Box &b : boxes) {
        if (b.type == type) {
            return &b;
        }
        if (const Box *c = find(b.children, type)) {
            return c;
        }
    }
    return nullptr;
}

/// What a moof says about its first sample and its timeline.
struct FragmentFacts
{
    bool ok = false;
    quint64 tfdt = 0;
    int samples = 0;
    quint64 duration = 0;       ///< sum of sample durations (trun, else tfhd default)
    bool firstSync = false;
};

inline FragmentFacts facts(const QByteArray &buf, const Box &moof, quint32 trexDefaultFlags)
{
    FragmentFacts f;
    const Box *tfhd = find(moof.children, "tfhd");
    const Box *tfdt = find(moof.children, "tfdt");
    const Box *trun = find(moof.children, "trun");
    if (!tfhd || !tfdt || !trun) {
        return f;
    }
    // tfdt
    const qint64 t0 = tfdt->offset + tfdt->header;
    const quint8 tver = quint8(buf[t0]);
    f.tfdt = tver == 1 ? be64(buf, t0 + 4) : be32(buf, t0 + 4);
    // tfhd defaults
    const qint64 h0 = tfhd->offset + tfhd->header;
    const quint32 hflags = be32(buf, h0) & 0xffffff;
    qint64 p = h0 + 8;                      // flags, track_ID
    if (hflags & 0x1) p += 8;               // base_data_offset
    if (hflags & 0x2) p += 4;               // sample_description_index
    quint32 defDuration = 0;
    if (hflags & 0x8) { defDuration = be32(buf, p); p += 4; }
    if (hflags & 0x10) p += 4;              // default size
    quint32 defFlags = trexDefaultFlags;
    if (hflags & 0x20) { defFlags = be32(buf, p); p += 4; }
    // trun
    const qint64 r0 = trun->offset + trun->header;
    const quint32 rflags = be32(buf, r0) & 0xffffff;
    f.samples = int(be32(buf, r0 + 4));
    qint64 q = r0 + 8;
    if (rflags & 0x1) q += 4;
    bool hasFirst = false;
    quint32 firstFlags = 0;
    if (rflags & 0x4) { hasFirst = true; firstFlags = be32(buf, q); q += 4; }
    quint32 sample0Flags = defFlags;
    for (int i = 0; i < f.samples; ++i) {
        quint32 d = defDuration;
        if (rflags & 0x100) { d = be32(buf, q); q += 4; }
        if (rflags & 0x200) q += 4;
        quint32 fl = defFlags;
        if (rflags & 0x400) { fl = be32(buf, q); q += 4; }
        if (rflags & 0x800) q += 4;
        if (i == 0) sample0Flags = fl;
        f.duration += d;
    }
    const quint32 first = hasFirst ? firstFlags : sample0Flags;
    f.firstSync = (first & 0x00010000u) == 0;
    f.ok = true;
    return f;
}

/// trex default_sample_flags from an init segment (0 when absent).
inline quint32 trexFlags(const QByteArray &buf, const QList<Box> &boxes)
{
    const Box *trex = find(boxes, "trex");
    if (!trex) {
        return 0;
    }
    return be32(buf, trex->offset + trex->header + 20);
}

} // namespace VideoTest

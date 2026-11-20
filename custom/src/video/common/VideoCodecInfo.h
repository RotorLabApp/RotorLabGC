/****************************************************************************
 *
 * RotorLab GC - what a video stream's parameter sets say, without decoding.
 *
 * G95 and G109 need the picture size and the RFC 6381 codec string
 * (avc1.PPCCLL, hvc1.*) for the Fleet Server's registration bodies, the MP4
 * track header and the recording's time map. Both come from the first SPS.
 * Plain Qt; every read is bounds-checked, so a hostile SPS gives `valid ==
 * false`, never a crash.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QString>

struct VideoCodecInfo
{
    bool    valid = false;
    bool    h265 = false;
    int     width = 0;
    int     height = 0;
    QString codecString;    ///< "avc1.64001f", "hvc1.1.6.L93.B0"; empty when not valid

    /// From Annex B parameter sets (or a whole access unit that carries them).
    /// Uses the first SPS found.
    static VideoCodecInfo fromAnnexB(bool h265, const QByteArray &annexB);

    /// From one SPS NAL unit, with its NAL header, without a start code.
    static VideoCodecInfo fromSps(bool h265, const QByteArray &spsNal);

    /// The NAL units of an Annex B buffer (3- or 4-byte start codes), without
    /// start codes. Empty units are skipped.
    static QList<QByteArray> splitAnnexB(const QByteArray &annexB);

    /// Remove emulation prevention bytes (00 00 03 -> 00 00).
    static QByteArray unescapeRbsp(const QByteArray &nalPayload);

    /// True when `nal` (with header) is an SPS of that codec.
    static bool isSps(bool h265, const QByteArray &nal);
};

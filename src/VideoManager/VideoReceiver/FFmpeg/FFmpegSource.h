/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QPair>
#include <QtCore/QString>

/// G88: maps a QGC video URI to what the FFmpeg receiver opens. Pure (no FFmpeg, no
/// network), so it is unit tested in FFmpegSourceTest.
///
/// - "udp://host:port" (RTP H.264) and "udp265://host:port" (RTP H.265): Kind::Rtp, GC's
///   own UDP socket and RTP depacketizer (design C1). host must be an IP address.
/// - "rtsp://", "rtsps://": Kind::Avformat, libavformat's RTSP demuxer.
/// - "mpegts://host:port": Kind::Avformat, MPEG-TS over UDP (FFmpeg "udp://host:port").
/// - "tcp://host:port": Kind::Avformat, MPEG-TS from a TCP server.
struct FFmpegSource
{
    enum class Kind { Invalid, Rtp, Avformat };

    Kind kind = Kind::Invalid;
    QString error;              ///< why the URI is refused (no credentials in it)
    QString scheme;             ///< lower case, as given
    QString codecName;          ///< Kind::Rtp: "h264" or "hevc"
    QString host;               ///< Kind::Rtp and UDP/TCP sources: address or name, without brackets
    int port = 0;
    /// Kind::Avformat: the URL handed to avformat_open_input. May carry RTSP credentials:
    /// never log it, log logText(uri) instead.
    QByteArray url;
    QByteArray inputFormat;     ///< Kind::Avformat: demuxer to force ("rtsp" or "mpegts")
    /// Kind::Avformat: avformat_open_input options, in order (key, value)
    QList<QPair<QByteArray, QByteArray>> formatOptions;

    bool isValid() const { return kind != Kind::Invalid; }

    /// lowLatency: GC's "Low Latency Mode" setting. RTSP: RTP reorder queue 0 packets when
    /// on, 4 when off (a reordered packet on Wi-Fi is then not a loss). Kind::Rtp has no
    /// reorder window either way.
    static FFmpegSource fromUri(const QString &uri, bool lowLatency = true);

    /// The receiver's log text for a URI: credentials removed (VideoReceiver::redactedUri).
    static QString logText(const QString &uri);
};

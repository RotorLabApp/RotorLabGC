#pragma once

#include "UnitTest.h"

/// G88: QGC video URIs to what the FFmpeg receiver opens: GC's own RTP input for UDP
/// H.264/H.265, libavformat with per-input options for RTSP, MPEG-TS and TCP. Every URI is
/// user or vehicle input, so junk, injected options and credentials are covered too.
class FFmpegSourceTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _udpH264();
    void _udp265();
    void _udpPortOnly();
    void _udpIpv6();
    void _rtsp();
    void _rtspDefaultPortAndTls();
    void _mpegts();
    void _tcp();
    void _lowLatencyReorderQueue();
    void _noProcessWideWhitelist();
    void _refusedSchemes();
    void _badUris();
    void _credentialsNeverInErrors();
};

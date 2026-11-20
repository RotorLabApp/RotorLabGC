#pragma once

#include "UnitTest.h"

/// G88: the FFmpeg receiver itself (built only with the FFmpeg n7.1 headers): the G83 sink
/// check, start refusals, the frame bridge's bounds, and hostile UDP input on loopback
/// (malformed, oversized and random datagrams never decode, never crash, and stop is prompt),
/// and a UDP port held by another program (m4080: told once, retried, cleared when free).
class FFmpegReceiverTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _sinkOnlyFromVideoOutput();
    void _librariesMatch();
    void _badUriRefused();
    void _frameBridgeOddSizes();
    void _frameBridgeRefusesShortPlanes();
    void _hostileUdpInput();
    void _stopIsPrompt();
    void _packetTapOnLoopback();
    void _packetTapTimeoutAndGenerations();
    void _udpPortInUse();
    void _uriChangeWhileBinding();      ///< G88 M1
    void _avformatPacketBound();        ///< G88 M3
};

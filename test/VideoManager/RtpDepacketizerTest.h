#pragma once

#include "UnitTest.h"

/// G88, design C1: RTP H.264 (RFC 6184) and H.265 (RFC 7798) to Annex B access units, and
/// the depacketizer's handling of hostile input: malformed RTP headers and payloads,
/// oversized packets and access units, other senders and sequence number wrap, and the
/// reorder window (in order release, its count and time bounds, duplicates, floods).
class RtpDepacketizerTest : public UnitTest
{
    Q_OBJECT

private slots:
    // Well-formed streams
    void _singleNalUnits();
    void _stapA();
    void _fuA();
    void _lostMiddleFragment();
    void _latePacketDropped();
    void _waitsForKeyFrame();
    void _timestampChangeClosesUnit();
    void _h265ApAndFu();
    void _refusesBadPackets();
    void _senderRestart();
    void _headerExtensionAndPadding();
    void _parameterSetsAndSsrc();

    // Hostile input
    void _malformedRtpHeader();
    void _oversizedPacket();
    void _malformedStapA();
    void _malformedFuA();
    void _fragmentCutShort();
    void _malformedH265();
    void _oversizedAccessUnit();
    void _sequenceWrap();
    void _otherSenderRefused();
    void _fuzzRandomPackets();
    void _fuzzMutatedStream();

    // Reorder window
    void _reorderWithinWindow();
    void _reorderBeyondWindowCount();
    void _reorderBeyondWindowTime();
    void _reorderAcrossWrap();
    void _duplicates();
    void _bigJumpResetsWindow();
    void _floodStaysBounded();
    void _inOrderNoDelay();
    void _afterLossOnlyOnRealLoss();

    // Stream start
    void _startReorderedFirstPackets();
    void _startMidGop();
    void _startLateCannotRewind();
};

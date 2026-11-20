#pragma once

#include "UnitTest.h"

/// G109: the S20 client against an in-process mock Fleet Server on 127.0.0.1
/// (never production). Order and seq, one request at a time on a keep-alive
/// connection, init first and again after a change, backlog drop to the next
/// keyframe with seq gaps, the backoff schedule, 409 and 404 each start a new
/// session, 415 (the mock answers as the Fleet Server does:
/// 200 {"ok": true} to init, frag and DELETE),
/// 413, 400 once then stop, 401 stops and clears, the Fleet Connector going
/// to Revoked mid-stream, the bearer token only in the header, and a pin
/// mismatch that sends nothing. The flight named on the same stream with
/// PATCH at arm and "" after landing; the bitrate guard's measure and texts.
class LiveSessionTest : public UnitTest
{
    Q_OBJECT

private slots:
    void init() override;
    void cleanup() override;

    void _orderAndSeqOneAtATime();
    void _initFirstAndAfterChange();
    void _backlogDropsToNextKeyframe();
    void _backoffSchedule();
    void _conflictStartsNewSession();
    void _unsupportedMediaStops();
    void _endedStreamStartsNewSession();
    void _tooLargeContinues();
    void _badRequestNewSessionOnceThenStop();
    void _unauthorizedStopsAndClears();
    void _revokedMidStreamAborts();
    void _stopSendsDelete();
    void _bearerOnlyInHeader();
    void _pinMismatchSendsNothing();
    void _notEnrolledSendsNothing();
    void _fleetStateLogic();

    // PATCH /live/<id> {"flight_ref"} at arm and "" after landing.
    void _flightRefPatchedOnSameStream();
    void _flightRefPatchRefusedOrEnded();
    // What the bitrate guard reads from the session, and its texts.
    void _fragmentSentFeedsGuard();
    void _guardTexts();
    // S20/S21 end-to-end: a quality change keeps the stream (new init, same live_id).
    void _qualityChangeKeepsStream();
};

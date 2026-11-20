#pragma once

#include "UnitTest.h"

class TakManager;

/// The inbound "Command By ATAK" path fed with real CoT XML: what the parser
/// reads from an event, which events may carry a command at all, the replay
/// and edit guard, the station's "marker ignored" lines and the refusals the
/// station applies before and at dispatch (GC 5.1.2: G35, G36, G38, G39, G43,
/// G44, G49, G50). Each test starts from empty TAK settings.
class TakMarkerTest : public UnitTest
{
    Q_OBJECT

    static void readAll(TakManager &tak);
    static void commandsOn(TakManager &tak);

private slots:
    void init() override;
    void cleanup() override;

    void _senderFromParentLinkOnly();
    void _placedMarkersOnly();
    void _replayAndEditNotRerun();
    void _malformedMarkerLine();
    void _negativeAltitudeRefused();
    void _negativeValueAnyForm();
    void _signAndUnicodeSpaceRefused();
    void _gotoOneAircraftKnownPoint();
    void _takeoffHeldToArmSwitch();
    void _emptyPrefixShown();
    void _floodBounded();
    void _rateLimited();
    void _markerTimeLimits();
    void _approvedNotStarved();
    void _repeatFloor();
    void _unlistedFloodTurnsOver();
    void _droppedNotRunLater();
    void _dropNotice();
    // G47, G48: addressing (a routing rule, not authentication)
    void _addressedMarkers();
    void _otherStationMarkers();
    void _prefixConflictFailsClosed();
    void _prefixRules();
    void _testButtonAddressing();
    void _addressingWithAircraft();
    // Review fixes (security M1, M3, M4)
    void _prefixPerInstall();
    void _upgradedRotorNotSet();
    void _prefixSeenNotSaved();
    void _stationShapedFleetCallsign();
    // A Fleet name is never station presence
    void _fleetNameOnOtherAircraftNoConflict();
    void _realPrefixTrackStillConflicts();
};

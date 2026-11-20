/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "TerrainQuery.h"

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QVariant>
#include <QtGui/QColor>
#include <QtPositioning/QGeoCoordinate>
#include <QtCore/QTimer>
#include <QtCore/QLoggingCategory>


Q_DECLARE_LOGGING_CATEGORY(FlightPathSegmentLog)

// Important Note: The altitudes in the coordinates must be AMSL
class FlightPathSegment : public QObject
{
    Q_OBJECT

public:
    enum SegmentType {
        SegmentTypeTakeoff,         // Takeoff segments: the contact span at the start is ground contact, not a strike (CL-1a)
        SegmentTypeGeneric,         // Generic segments take into account the entire segment for terrain collisions
        SegmentTypeLand,            // Land segments: the contact span at the end is ground contact, not a strike (CL-1a)
        SegmentTypeTerrainFrame,    // Flight path in between the two coords follows terrain
    };
    Q_ENUM(SegmentType)

    /// RotorLab GC G76: what the loaded terrain shows about this leg in the 2D Plan view, with the
    /// state rules and colors of the 3D views (PLAN-CONTRACT.md section 4: CL-1, CL-1a, CL-2, CL-5).
    /// None of these states means a flight may go.
    enum TerrainState {
        TerrainNotChecked,      ///< no terrain query is made for this leg (Fly view); drawn as before G76
        TerrainUnknown,         ///< terrain clearance not checked: no terrain data, query pending or failed, or no AMSL height
        TerrainNoStrike,        ///< ground known and the sample at least strikeMarginM above it
        TerrainGroundContact,   ///< CL-1a: inside the contact span of a takeoff or land leg, HAG -0.5 m to under strikeMarginM
        TerrainStrike,          ///< ground known and the sample less than strikeMarginM above it, or below it
    };
    Q_ENUM(TerrainState)

    /// G76: how a leg out of the planned home starts when the plan starts on the ground.
    enum LaunchStart {
        LaunchStartNone,        ///< the leg starts at coord1AMSLAlt
        LaunchStartOnGround,    ///< a takeoff climb that is not straight up: it starts on the loaded ground at launch
        LaunchStartClimbAtLaunch, ///< A-11: an ArduPilot takeoff with no position climbs straight up at launch from the
                                  ///< ground to coord1AMSLAlt (the takeoff height), then flies the leg from there
    };
    Q_ENUM(LaunchStart)

    /// Contract section 4 (decision 756, OQ7): a sample less than this above known ground is a strike.
    static constexpr double strikeMarginM = 2.0;
    /// Contract CL-1a (rev 5): inside a contact span, a sample down to this far below the ground is ground contact.
    static constexpr double contactBelowGroundM = 0.5;
    /// A map piece is drawn as a straight map line up to this length, then as a great circle.
    static constexpr double maxMapPieceM = 50000.0;

    /// One run of samples in the same state, as distances along the leg in metres.
    struct TerrainRun {
        TerrainState state;
        double startM;
        double endM;
    };

    /// Distance along the leg of each terrain sample, with the spacing TerrainPathQuery returns.
    static QList<double> sampleDistances(int count, double distanceBetween, double finalDistanceBetween);
    /// The state of each sample. coord1AMSLAlt and coord2AMSLAlt are the leg end heights (AMSL, NaN when
    /// unknown); amslTerrainHeights is the ground under each sample (NaN when unknown). A straight leg is the
    /// straight AMSL line; a terrain frame leg follows the ground at a height above it interpolated between
    /// the heights above ground at its two ends (contract A-7).
    /// `launchStart` (takeoff legs only): LaunchStartOnGround starts the leg on the loaded ground under its start
    /// rather than at coord1AMSLAlt (A-0). LaunchStartClimbAtLaunch (A-11): the first sample is the foot of a
    /// straight-up climb at launch, which is ground contact (CL-1a); the leg itself runs from coord1AMSLAlt.
    static QList<TerrainState> classifySamples(SegmentType segmentType, double coord1AMSLAlt, double coord2AMSLAlt,
                                               double totalDistance, const QList<double>& distances,
                                               const QList<double>& amslTerrainHeights, LaunchStart launchStart = LaunchStartNone);
    /// G76: delay before retry `attempt` (1 based) of a failed or timed out terrain query; 0 when there is no more
    /// retry. 2, 5, 15, then 30 s, at most maxTerrainRetries retries.
    static int terrainRetryDelayMs(int attempt);
    static constexpr int maxTerrainRetries = 8;
    /// G76: a terrain query with no answer after this long is dropped and counted as failed.
    static constexpr int terrainQueryTimeoutMs = 30000;
    /// One point at height above ground `hag` (NaN when unknown). `inContactSpan`: a takeoff or landing contact
    /// span (CL-1a), where -0.5 m to under strikeMarginM is ground contact rather than a strike.
    static TerrainState hagState(double hag, bool inContactSpan);
    /// A mission item's own point (contract A-9: a point within strikeMarginM of the ground, or below it, is a
    /// strike), with the CL-1a contact allowance when the item is a takeoff or a landing.
    static TerrainState itemTerrainState(double amslAlt, double amslTerrainHeight, bool takeoffOrLand);
    /// The leg's state: strike if any sample is a strike; otherwise unknown if any sample is unknown or
    /// there is none; otherwise ground contact if any sample is; otherwise no strike.
    static TerrainState legState(const QList<TerrainState>& sampleStates);
    /// Runs of equal state covering 0 to totalDistance; a change of state is drawn halfway between samples.
    /// No samples: one unknown run.
    static QList<TerrainRun> terrainRuns(const QList<TerrainState>& sampleStates, const QList<double>& distances, double totalDistance);
    /// Dash period of unknown runs on a leg of this length; a dash is 60 percent of it.
    static double dashPeriodM(double totalDistance);
    /// Drawing priority when runs are merged: strike, then unknown, then ground contact, then no strike.
    static int statePriority(TerrainState state);
    /// G76 (GUI review): no drawn run is shorter than minRunM, so short alternating runs near a strike edge
    /// never read as the dashes of unknown ground. A short run is absorbed by a neighbor that outranks it
    /// (statePriority); a short run that outranks its neighbors is widened to minRunM at their expense. A
    /// strike or unknown run is therefore never removed, only widened. Runs keep covering the same span.
    static QList<TerrainRun> mergeShortRuns(const QList<TerrainRun>& runs, double minRunM);
    /// The map pieces of a leg: each { "path": [QGeoCoordinate], "state": TerrainState, "dashed": bool,
    /// "tick": bool }. Unknown runs are cut into dashes; strike runs also get tick marks across the line
    /// ("tick" pieces) every dash period. Dashes and ticks are the cues that do not rely on color.
    static QVariantList buildMapPieces(const QGeoCoordinate& coord1, const QGeoCoordinate& coord2, const QList<TerrainRun>& runs);
    /// The 3D views' color for a state (custom/src/plan3d/Plan3DColors.h); invalid for TerrainNotChecked.
    static QColor terrainStateColor(TerrainState state, bool darkTheme);
    /// terrainStateColor as a "#rrggbb" name for QML; empty for TerrainNotChecked (QML then uses the palette).
    Q_INVOKABLE QString terrainStateColorName(int state, bool darkTheme) const;

    FlightPathSegment(SegmentType segmentType, const QGeoCoordinate& coord1, double coord1AMSLAlt, const QGeoCoordinate& coord2, double coord2AMSLAlt, bool queryTerrainData, QObject* parent);

    Q_PROPERTY(QGeoCoordinate   coordinate1             MEMBER _coord1                                          NOTIFY coordinate1Changed)
    Q_PROPERTY(QGeoCoordinate   coordinate2             MEMBER _coord2                                          NOTIFY coordinate2Changed)
    Q_PROPERTY(double           coord1AMSLAlt           MEMBER _coord1AMSLAlt                                   NOTIFY coord1AMSLAltChanged)
    Q_PROPERTY(double           coord2AMSLAlt           MEMBER _coord2AMSLAlt                                   NOTIFY coord2AMSLAltChanged)
    Q_PROPERTY(bool             specialVisual           READ specialVisual              WRITE setSpecialVisual  NOTIFY specialVisualChanged)
    Q_PROPERTY(QVariantList     amslTerrainHeights      MEMBER _amslTerrainHeights                              NOTIFY amslTerrainHeightsChanged)
    Q_PROPERTY(double           distanceBetween         MEMBER _distanceBetween                                 NOTIFY distanceBetweenChanged)
    Q_PROPERTY(double           finalDistanceBetween    MEMBER _finalDistanceBetween                            NOTIFY finalDistanceBetweenChanged)
    Q_PROPERTY(double           totalDistance           MEMBER _totalDistance                                   NOTIFY totalDistanceChanged)
    Q_PROPERTY(bool             terrainCollision        MEMBER _terrainCollision                                NOTIFY terrainCollisionChanged)
    Q_PROPERTY(TerrainState     terrainState            READ terrainState                                       NOTIFY terrainStateChanged)
    Q_PROPERTY(QVariantList     mapPieces               READ mapPieces                                          NOTIFY mapPiecesChanged)
    Q_PROPERTY(SegmentType      segmentType             MEMBER _segmentType                                     CONSTANT)

    QGeoCoordinate      coordinate1         (void) const { return _coord1; }
    QGeoCoordinate      coordinate2         (void) const { return _coord2; }
    double              coord1AMSLAlt       (void) const { return _coord1AMSLAlt; }
    double              coord2AMSLAlt       (void) const { return _coord2AMSLAlt; }
    const QVariantList& amslTerrainHeights  (void) const { return _amslTerrainHeights; }
    double              distanceBetween     (void) const { return _distanceBetween; }
    double              finalDistanceBetween(void) const { return _finalDistanceBetween; }
    double              totalDistance       (void) const { return _totalDistance; }
    bool                specialVisual       (void) const { return _specialVisual; }
    bool                terrainCollision    (void) const { return _terrainCollision; }
    SegmentType         segmentType         (void) const { return _segmentType; }
    TerrainState        terrainState        (void) const { return _terrainState; }
    const QVariantList& mapPieces           (void) const { return _mapPieces; }

    void setSpecialVisual(bool specialVisual);

    /// G76: how the leg starts when it leaves the planned home on the ground (classifySamples' launchStart)
    LaunchStart         launchStart         (void) const { return _launchStart; }
    void                setLaunchStart      (LaunchStart launchStart);

    /// G76 (A-10, CL-2): the leg's height is not in the plan (the leg back to launch of an RTL, flown at the
    /// aircraft's own return height). Every sample is unknown, never a strike or no strike.
    bool                heightNotInPlan     (void) const { return _heightNotInPlan; }
    void                setHeightNotInPlan  (bool heightNotInPlan);

    /// Test entry: apply a terrain path answer as if TerrainPathQuery had returned it (unit tests have no tiles)
    void                applyTerrainDataForTest(const QList<double>& heights, double distanceBetween, double finalDistanceBetween);
    /// Test entry: a failed terrain path answer, as if TerrainPathQuery had reported failure
    void                applyTerrainFailureForTest(void);
    /// Test entry: the pending terrain query timed out
    void                expireTerrainQueryForTest(void) { _terrainQueryTimedOut(); }
    /// Test entry: run the scheduled retry now
    void                fireTerrainRetryForTest(void);
    /// Retries made since the last answer with data, and whether one is scheduled, with its delay
    int                 terrainRetryCount   (void) const { return _terrainRetryCount; }
    bool                terrainRetryPending (void) const { return _terrainRetryTimer.isActive(); }
    int                 terrainRetryIntervalMs(void) const { return _terrainRetryTimer.interval(); }
    bool                terrainQueryPending (void) const { return _currentTerrainPathQuery != nullptr; }

    /// G76: set by MissionController when it wires the segment to the two items of its leg, with a key for the
    /// segment type and takeoff shape it was built for. A later recalc reuses the segment only for the same two
    /// live items and the same key, so a segment whose item was deleted (its connections gone with it) is never
    /// reused, even if a new item is allocated at the same address. Key 0: never reused.
    quint32             reuseKey            (void) const { return _reuseKey; }
    /// `launchItem`: the takeoff item whose height starts the leg (LaunchStartClimbAtLaunch), or nullptr.
    void                setReuseKey         (const QObject* item1, const QObject* item2, quint32 reuseKey, const QObject* launchItem = nullptr)
                                            { _reuseItem1 = const_cast<QObject*>(item1); _reuseItem2 = const_cast<QObject*>(item2);
                                              _reuseLaunchItem = const_cast<QObject*>(launchItem); _hasReuseLaunchItem = launchItem != nullptr; _reuseKey = reuseKey; }
    bool                reusableFor         (const QObject* item1, const QObject* item2, quint32 reuseKey, const QObject* launchItem = nullptr) const
                                            { return _reuseKey != 0 && _reuseKey == reuseKey && !_reuseItem1.isNull() && !_reuseItem2.isNull()
                                                     && _reuseItem1.data() == item1 && _reuseItem2.data() == item2
                                                     && _hasReuseLaunchItem == (launchItem != nullptr)
                                                     && (!_hasReuseLaunchItem || (!_reuseLaunchItem.isNull() && _reuseLaunchItem.data() == launchItem)); }

public slots:
    void setCoordinate1     (const QGeoCoordinate& coordinate);
    void setCoordinate2     (const QGeoCoordinate& coordinate);
    void setCoord1AMSLAlt   (double alt);
    void setCoord2AMSLAlt   (double alt);

signals:
    void coordinate1Changed         (QGeoCoordinate coordinate);
    void coordinate2Changed         (QGeoCoordinate coordinate);
    void coord1AMSLAltChanged       (void);
    void coord2AMSLAltChanged       (void);
    void specialVisualChanged       (bool specialVisual);
    void amslTerrainHeightsChanged  (void);
    void distanceBetweenChanged     (double distanceBetween);
    void finalDistanceBetweenChanged(double finalDistanceBetween);
    void totalDistanceChanged       (double totalDistance);
    void terrainCollisionChanged    (bool terrainCollision);
    void terrainStateChanged        (void);
    void mapPiecesChanged           (void);

private slots:
    void _sendTerrainPathQuery      (void);
    void _terrainDataReceived       (bool success, const TerrainPathQuery::PathHeightInfo_t& pathHeightInfo);
    void _updateTotalDistance       (void);
    void _updateTerrainCollision    (void);
    void _terrainQueryTimedOut      (void);
    void _retryTerrainPathQuery     (void);

private:
    void _updateMapPieces           (void);
    void _dropPendingTerrainQuery   (void);
    void _applyTerrainData          (bool success, const TerrainPathQuery::PathHeightInfo_t& pathHeightInfo);
    void _resetTerrainRetries       (void);

    QGeoCoordinate      _coord1;
    QGeoCoordinate      _coord2;
    double              _coord1AMSLAlt =                qQNaN();
    double              _coord2AMSLAlt =                qQNaN();
    bool                _queryTerrainData;
    bool                _terrainCollision =             false;
    TerrainState        _terrainState =                 TerrainUnknown;
    bool                _terrainStale =                 true;   ///< no terrain answer yet for the current coordinates
    QList<TerrainRun>   _terrainRuns;
    QVariantList        _mapPieces;
    bool                _specialVisual =                false;
    QTimer              _delayedTerrainPathQueryTimer;
    TerrainPathQuery*   _currentTerrainPathQuery =      nullptr;
    QVariantList        _amslTerrainHeights;
    double              _distanceBetween =              0;
    double              _finalDistanceBetween =         0;
    double              _totalDistance =                0;
    SegmentType         _segmentType =                  SegmentTypeGeneric;
    quint32             _reuseKey =                     0;
    LaunchStart         _launchStart =                  LaunchStartNone;
    bool                _heightNotInPlan =              false;
    QTimer              _terrainRetryTimer;             ///< G76: retry after a failed or timed out terrain query
    QTimer              _terrainQueryTimeoutTimer;      ///< G76: a query with no answer counts as failed
    int                 _terrainRetryCount =            0;
    QPointer<QObject>   _reuseItem1;
    QPointer<QObject>   _reuseItem2;
    QPointer<QObject>   _reuseLaunchItem;
    bool                _hasReuseLaunchItem =           false;
};

/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtQuick/QQuickItem>
#include <QtQuick/QSGGeometryNode>
#include <QtQuick/QSGGeometry>
#include <QtCore/QLoggingCategory>
#include <QtCore/QTimer>

Q_DECLARE_LOGGING_CATEGORY(TerrainProfileLog)

class MissionController;
class QmlObjectListModel;
class FlightPathSegment;

Q_MOC_INCLUDE("MissionController.h")

class TerrainProfile : public QQuickItem
{
    Q_OBJECT

public:
    TerrainProfile(QQuickItem *parent = nullptr);

    Q_PROPERTY(double               visibleWidth        MEMBER _visibleWidth                                NOTIFY visibleWidthChanged)
    Q_PROPERTY(MissionController*   missionController   READ missionController  WRITE setMissionController  NOTIFY missionControllerChanged)
    Q_PROPERTY(double               pixelsPerMeter      MEMBER _pixelsPerMeter                              NOTIFY pixelsPerMeterChanged)
    Q_PROPERTY(double               minAMSLAlt          MEMBER _minAMSLAlt                                  NOTIFY minAMSLAltChanged)
    Q_PROPERTY(double               maxAMSLAlt          MEMBER _maxAMSLAlt                                  NOTIFY maxAMSLAltChanged)

    /// RotorLab GC G76: how many legs of the plan are in each terrain state (FlightPathSegment::TerrainState),
    /// for the state line of the Plan view terrain profile. A leg is one simple leg or one segment of a pattern.
    Q_PROPERTY(int                  strikeLegCount      READ strikeLegCount                                 NOTIFY legStatesChanged)
    Q_PROPERTY(int                  unknownLegCount     READ unknownLegCount                                NOTIFY legStatesChanged)
    Q_PROPERTY(int                  noStrikeLegCount    READ noStrikeLegCount                               NOTIFY legStatesChanged)
    /// G76: the length of the profile's distance axis: every drawn leg, including the leg back to launch of an RTL,
    /// which the mission's total distance leaves out; never less than the mission's total distance.
    Q_PROPERTY(double               profileDistance     READ profileDistance                                NOTIFY profileDistanceChanged)

    double profileDistance(void) const { return _profileDistance; }

    int strikeLegCount  (void) const { return _strikeLegCount; }
    int unknownLegCount (void) const { return _unknownLegCount; }
    int noStrikeLegCount(void) const { return _noStrikeLegCount; }

    struct LegStateCounts {
        int strike   = 0;   ///< TerrainStrike
        int unknown  = 0;   ///< TerrainUnknown
        int noStrike = 0;   ///< TerrainNoStrike and TerrainGroundContact (the contact of a takeoff or landing is not a strike)
    };
    /// Counts the legs by state; TerrainNotChecked legs (no terrain query) are not counted.
    static LegStateCounts countLegStates(const QList<FlightPathSegment*>& segments);
    /// The profile's distance axis length for legs drawn over `drawnDistance` meters in a mission whose total
    /// distance is `missionTotalDistance` (NaN when unknown).
    static double profileDistanceFor(double drawnDistance, double missionTotalDistance);
    /// The map's color for a leg state (FlightPathSegment::terrainStateColor, the 3D views' colors) as "#rrggbb",
    /// so the state line shows the same colors as the map.
    Q_INVOKABLE QString terrainStateColorName(int state, bool darkTheme) const;

    /// Test entry: how many repaints the profile has asked for since it was created
    int paintRequestCountForTest(void) const { return _paintRequestCount; }

    MissionController*  missionController(void) { return _missionController; }

    void setMissionController(MissionController* missionController);

    // Overrides from QQuickItem
    QSGNode* updatePaintNode(QSGNode* oldNode, QQuickItem::UpdatePaintNodeData* updatePaintNodeData);

    // Override from QQmlParserStatus
    void componentComplete(void) final;

signals:
    void missionControllerChanged   (void);
    void visibleWidthChanged        (void);
    void pixelsPerMeterChanged      (void);
    void minAMSLAltChanged          (void);
    void maxAMSLAltChanged          (void);
    void _updateSignal              (void);
    void legStatesChanged           (void);
    void profileDistanceChanged     (void);

private slots:
    void _newVisualItems            (void);
    void _updateLegStates           (void);
    void _requestPaint              (void);

private:
    void    _createGeometry                 (QSGGeometryNode*& geometryNode, QSGGeometry*& geometry, QSGGeometry::DrawingMode drawingMode, const QColor& color);
    void    _updateSegmentCounts            (FlightPathSegment* segment, int& cFlightProfileSegments, int& cTerrainPoints, int& cMissingTerrainSegments, int& cTerrainCollisionSegments, double& minTerrainHeight, double& maxTerrainHeight);
    void    _addTerrainProfileSegment       (FlightPathSegment* segment, double currentDistance, double amslAltRange, QSGGeometry::Point2D* terrainProfileVertices, int& terrainVertexIndex);
    void    _addMissingTerrainSegment       (FlightPathSegment* segment, double currentDistance, QSGGeometry::Point2D* missingTerrainVertices, int& missingTerrainVertexIndex);
    void    _addTerrainCollisionSegment     (FlightPathSegment* segment, double currentDistance, double amslAltRange, QSGGeometry::Point2D* terrainCollisionVertices, int& terrainCollisionVertexIndex);
    void    _addFlightProfileSegment        (FlightPathSegment* segment, double currentDistance, double amslAltRange, QSGGeometry::Point2D* flightProfileVertices, int& flightProfileVertexIndex);
    bool    _shouldAddFlightProfileSegment  (FlightPathSegment* segment);
    bool    _shouldAddMissingTerrainSegment (FlightPathSegment* segment);

    MissionController*  _missionController =    nullptr;
    QmlObjectListModel* _visualItems =          nullptr;
    double              _visibleWidth =         0;
    double              _pixelsPerMeter =       0;
    double              _minAMSLAlt =           0;
    double              _maxAMSLAlt =           0;
    int                 _strikeLegCount =       0;
    int                 _unknownLegCount =      0;
    int                 _noStrikeLegCount =     0;
    double              _profileDistance =      0;
    int                 _paintRequestCount =    0;
    QTimer              _legStatesTimer;

    static const int _lineWidth =       7;

    Q_DISABLE_COPY(TerrainProfile)
};

QML_DECLARE_TYPE(TerrainProfile)

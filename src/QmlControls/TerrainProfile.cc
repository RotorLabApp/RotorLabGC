/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "TerrainProfile.h"
#include "MissionController.h"
#include "QmlObjectListModel.h"
#include "FlightPathSegment.h"
#include "ComplexMissionItem.h"
#include "QGCLoggingCategory.h"
#include "QGCApplication.h"
#include "QGC.h"

#include <QtQuick/QSGFlatColorMaterial>

#include <algorithm>
#include <cmath>

QGC_LOGGING_CATEGORY(TerrainProfileLog, "TerrainProfileLog")

TerrainProfile::TerrainProfile(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(QQuickItem::ItemHasContents, true);

    connect(this, &QQuickItem::heightChanged,           this, &QQuickItem::update);
    connect(this, &TerrainProfile::visibleWidthChanged, this, &QQuickItem::update);

    // This collapse multiple _updateSignals in a row to a single update. Keep this the only queued slot of
    // _updateSignal on this object (see setMissionController).
    connect(this, &TerrainProfile::_updateSignal, this, &TerrainProfile::_requestPaint, Qt::QueuedConnection);
    qgcApp()->addCompressedSignal(QMetaMethod::fromSignal(&TerrainProfile::_updateSignal));

    _legStatesTimer.setSingleShot(true);
    _legStatesTimer.setInterval(0);
    connect(&_legStatesTimer, &QTimer::timeout, this, &TerrainProfile::_updateLegStates);
}

void TerrainProfile::_requestPaint(void)
{
    _paintRequestCount++;
    update();
}

void TerrainProfile::componentComplete(void)
{
    QQuickItem::componentComplete();
}

void TerrainProfile::setMissionController(MissionController* missionController)
{
    if (missionController != _missionController) {
        _missionController =    missionController;
        _visualItems =          _missionController->visualItems();

        emit missionControllerChanged();

        connect(_missionController, &MissionController::visualItemsChanged,         this, &TerrainProfile::_newVisualItems);

        connect(this,               &TerrainProfile::visibleWidthChanged,           this, &TerrainProfile::_updateSignal, Qt::QueuedConnection);
        connect(_missionController, &MissionController::recalcTerrainProfile,       this, &TerrainProfile::_updateSignal, Qt::QueuedConnection);
        // G76: the leg state line follows every profile update (recalcTerrainProfile is also emitted when a leg's
        // state changes). Not through a second queued slot on _updateSignal: _updateSignal is a compressed signal,
        // and QGCApplication::compressEvent treats two queued pointer-to-member slots on the same receiver as the
        // same call, so it kept only the newer one and dropped the repaint (41f1eff: the profile lost its terrain).
        // A direct connection starts a zero-time single-shot timer instead; timer events are never compressed.
        connect(_missionController, &MissionController::recalcTerrainProfile,       &_legStatesTimer, qOverload<>(&QTimer::start));
        connect(_missionController, &MissionController::visualItemsChanged,         &_legStatesTimer, qOverload<>(&QTimer::start));
        connect(_missionController, &MissionController::missionTotalDistanceChanged, &_legStatesTimer, qOverload<>(&QTimer::start));
        _updateLegStates();
    }
}

TerrainProfile::LegStateCounts TerrainProfile::countLegStates(const QList<FlightPathSegment*>& segments)
{
    LegStateCounts counts;
    for (const FlightPathSegment* segment: segments) {
        if (!segment) {
            continue;
        }
        switch (segment->terrainState()) {
        case FlightPathSegment::TerrainStrike:
            counts.strike++;
            break;
        case FlightPathSegment::TerrainUnknown:
            counts.unknown++;
            break;
        case FlightPathSegment::TerrainNoStrike:
        case FlightPathSegment::TerrainGroundContact:
            counts.noStrike++;
            break;
        case FlightPathSegment::TerrainNotChecked:
            break;
        }
    }
    return counts;
}

double TerrainProfile::profileDistanceFor(double drawnDistance, double missionTotalDistance)
{
    const double drawn      = std::isfinite(drawnDistance) ? drawnDistance : 0.0;
    const double mission    = std::isfinite(missionTotalDistance) ? missionTotalDistance : 0.0;
    return std::max(drawn, mission);
}

QString TerrainProfile::terrainStateColorName(int state, bool darkTheme) const
{
    if (state < FlightPathSegment::TerrainNotChecked || state > FlightPathSegment::TerrainStrike) {
        return QString();
    }
    const QColor color = FlightPathSegment::terrainStateColor(static_cast<FlightPathSegment::TerrainState>(state), darkTheme);
    return color.isValid() ? color.name() : QString();
}

void TerrainProfile::_updateLegStates(void)
{
    // Runs on the GUI thread (never from updatePaintNode, which runs on the render thread)
    QList<FlightPathSegment*> segments;
    double drawnDistance = 0;   // the same walk as updatePaintNode's currentDistance
    if (_visualItems) {
        for (int viIndex = 0; viIndex < _visualItems->count(); viIndex++) {
            VisualMissionItem*  visualItem  = _visualItems->value<VisualMissionItem*>(viIndex);
            ComplexMissionItem* complexItem = _visualItems->value<ComplexMissionItem*>(viIndex);
            if (complexItem) {
                if (complexItem->flightPathSegments()->count() == 0) {
                    drawnDistance += complexItem->complexDistance();
                }
                for (int segmentIndex = 0; segmentIndex < complexItem->flightPathSegments()->count(); segmentIndex++) {
                    FlightPathSegment* segment = complexItem->flightPathSegments()->value<FlightPathSegment*>(segmentIndex);
                    segments.append(segment);
                    drawnDistance += segment ? segment->totalDistance() : 0.0;
                }
            }
            if (visualItem && visualItem->simpleFlightPathSegment()) {
                segments.append(visualItem->simpleFlightPathSegment());
                drawnDistance += visualItem->simpleFlightPathSegment()->totalDistance();
            }
        }
    }

    // G76: the distance axis ends at the end of the last drawn leg, so the leg back to launch of an RTL is shown
    const double newProfileDistance = profileDistanceFor(drawnDistance, _missionController ? _missionController->missionTotalDistance() : qQNaN());
    if (!QGC::fuzzyCompare(newProfileDistance, _profileDistance)) {
        _profileDistance = newProfileDistance;
        emit profileDistanceChanged();
        emit _updateSignal();
    }

    const LegStateCounts counts = countLegStates(segments);
    if (counts.strike != _strikeLegCount || counts.unknown != _unknownLegCount || counts.noStrike != _noStrikeLegCount) {
        _strikeLegCount     = counts.strike;
        _unknownLegCount    = counts.unknown;
        _noStrikeLegCount   = counts.noStrike;
        emit legStatesChanged();
    }
}

void TerrainProfile::_newVisualItems(void)
{
    _visualItems = _missionController->visualItems();
    emit _updateSignal();
}

void TerrainProfile::_createGeometry(QSGGeometryNode*& geometryNode, QSGGeometry*& geometry, QSGGeometry::DrawingMode drawingMode, const QColor& color)
{
    QSGFlatColorMaterial* terrainMaterial = new QSGFlatColorMaterial;
    terrainMaterial->setColor(color);

    geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0);
    geometry->setDrawingMode(drawingMode);
    geometry->setLineWidth(2);

    geometryNode = new QSGGeometryNode;
    geometryNode->setFlag(QSGNode::OwnsGeometry);
    geometryNode->setFlag(QSGNode::OwnsMaterial);
    geometryNode->setFlag(QSGNode::OwnedByParent);
    geometryNode->setMaterial(terrainMaterial);
    geometryNode->setGeometry(geometry);
}

void TerrainProfile::_updateSegmentCounts(FlightPathSegment* segment, int& cFlightProfileSegments, int& cTerrainProfilePoints, int& cMissingTerrainSegments, int& cTerrainCollisionSegments, double& minTerrainHeight, double& maxTerrainHeight)
{
    if (_shouldAddFlightProfileSegment(segment)) {
        if (segment->segmentType() == FlightPathSegment::SegmentTypeTerrainFrame) {
            // We show a full above terrain profile for flight segment
            cFlightProfileSegments += segment->amslTerrainHeights().count() - 1;
        } else {
            cFlightProfileSegments++;
        }
    }

    if (_shouldAddMissingTerrainSegment(segment)) {
        cMissingTerrainSegments += 1;
    } else {
        cTerrainProfilePoints += segment->amslTerrainHeights().count();
        for (int i=0; i<segment->amslTerrainHeights().count(); i++) {
            minTerrainHeight = std::fmin(minTerrainHeight, segment->amslTerrainHeights()[i].value<double>());
            maxTerrainHeight = std::fmax(maxTerrainHeight, segment->amslTerrainHeights()[i].value<double>());
        }
    }
    if (segment->terrainCollision()) {
        cTerrainCollisionSegments++;
    }
}

void TerrainProfile::_addTerrainProfileSegment(FlightPathSegment* segment, double currentDistance, double amslAltRange, QSGGeometry::Point2D* terrainVertices, int& terrainProfileVertexIndex)
{
    double terrainDistance = 0;
    for (int heightIndex=0; heightIndex<segment->amslTerrainHeights().count(); heightIndex++) {
        // Move along the x axis which is distance
        if (heightIndex == 0) {
            // The first point in the segment is at the position of the last point. So nothing to do here.
        } else if (heightIndex == segment->amslTerrainHeights().count() - 2) {
            // The distance between the last two heights differs with each terrain query
            terrainDistance += segment->finalDistanceBetween();
        } else {
            // The distance between all terrain heights except for the last is the same
            terrainDistance += segment->distanceBetween();
        }

        // Move along the y axis which is a view or terrain height as a percentage between the min/max AMSL altitude for all segments
        double amslTerrainHeight    = segment->amslTerrainHeights()[heightIndex].value<double>();
        double terrainHeightPercent = (amslTerrainHeight - _minAMSLAlt) / amslAltRange;

        float x = (currentDistance + terrainDistance) * _pixelsPerMeter;
        float y = height() - (terrainHeightPercent * height());
        terrainVertices[terrainProfileVertexIndex++].set(x, y);
    }
}

void TerrainProfile::_addMissingTerrainSegment(FlightPathSegment* segment, double currentDistance, QSGGeometry::Point2D* missingTerrainVertices, int& missingterrainProfileVertexIndex)
{
    if (_shouldAddMissingTerrainSegment(segment)) {
        float x = currentDistance * _pixelsPerMeter;
        float y = height();
        missingTerrainVertices[missingterrainProfileVertexIndex++].set(x, y);
        missingTerrainVertices[missingterrainProfileVertexIndex++].set(x + (segment->totalDistance() * _pixelsPerMeter), y);
    }
}

void TerrainProfile::_addTerrainCollisionSegment(FlightPathSegment* segment, double currentDistance, double amslAltRange, QSGGeometry::Point2D* terrainCollisionVertices, int& terrainCollisionVertexIndex)
{
    if (segment->terrainCollision()) {
        double amslCoord1Height =       segment->coord1AMSLAlt();
        double amslCoord2Height =       segment->coord2AMSLAlt();
        double coord1HeightPercent =    (amslCoord1Height - _minAMSLAlt) / amslAltRange;
        double coord2HeightPercent =    (amslCoord2Height - _minAMSLAlt) / amslAltRange;

        float x = currentDistance * _pixelsPerMeter;
        float y = height() - (coord1HeightPercent * height());

        terrainCollisionVertices[terrainCollisionVertexIndex++].set(x, y);

        x += segment->totalDistance() * _pixelsPerMeter;
        y = height() - (coord2HeightPercent * height());

        terrainCollisionVertices[terrainCollisionVertexIndex++].set(x, y);
    }
}

void TerrainProfile::_addFlightProfileSegment(FlightPathSegment* segment, double currentDistance, double amslAltRange, QSGGeometry::Point2D* flightProfileVertices, int& flightProfileVertexIndex)
{
    if (!_shouldAddFlightProfileSegment(segment)) {
        return;
    }

    if (segment->segmentType() == FlightPathSegment::SegmentTypeTerrainFrame) {
        double terrainDistance = 0;
        double distanceToSurface = segment->coord1AMSLAlt() - segment->amslTerrainHeights().first().value<double>();
        for (int heightIndex=0; heightIndex<segment->amslTerrainHeights().count(); heightIndex++) {
            // Move along the x axis which is distance
            if (heightIndex == 0) {
                // The first point in the segment is at the position of the last point. So nothing to do here.
            } else if (heightIndex == segment->amslTerrainHeights().count() - 2) {
                // The distance between the last two heights differs with each terrain query
                terrainDistance += segment->finalDistanceBetween();
            } else {
                // The distance between all terrain heights except for the last is the same
                terrainDistance += segment->distanceBetween();
            }

            if (heightIndex > 1) {
                // Add first coord of segment
                auto previousVertex = flightProfileVertices[flightProfileVertexIndex-1];
                flightProfileVertices[flightProfileVertexIndex++].set(previousVertex.x, previousVertex.y);
            }

            // Add second coord of segment (or very first one)
            double amslTerrainHeight    = segment->amslTerrainHeights()[heightIndex].value<double>() + distanceToSurface;
            double terrainHeightPercent = (amslTerrainHeight - _minAMSLAlt) / amslAltRange;

            float x = (currentDistance + terrainDistance) * _pixelsPerMeter;
            float y = height() - (terrainHeightPercent * height());
            flightProfileVertices[flightProfileVertexIndex++].set(x, y);

        }
    } else {
        double amslCoord1Height =       segment->coord1AMSLAlt();
        double amslCoord2Height =       segment->coord2AMSLAlt();
        double coord1HeightPercent =    (amslCoord1Height - _minAMSLAlt) / amslAltRange;
        double coord2HeightPercent =    (amslCoord2Height - _minAMSLAlt) / amslAltRange;

        float x = currentDistance * _pixelsPerMeter;
        float y = height() - (coord1HeightPercent * height());

        flightProfileVertices[flightProfileVertexIndex++].set(x, y);

        x += segment->totalDistance() * _pixelsPerMeter;
        y = height() - (coord2HeightPercent * height());

        flightProfileVertices[flightProfileVertexIndex++].set(x, y);
    }
}

QSGNode* TerrainProfile::updatePaintNode(QSGNode* oldNode, QQuickItem::UpdatePaintNodeData* /*updatePaintNodeData*/)
{
    QSGNode*        rootNode =                  static_cast<QSGNode *>(oldNode);
    QSGGeometry*    terrainProfileGeometry =    nullptr;
    QSGGeometry*    missingTerrainGeometry =    nullptr;
    QSGGeometry*    flightProfileGeometry =     nullptr;
    QSGGeometry*    terrainCollisionGeometry =  nullptr;
    int             cTerrainProfilePoints =     0;
    int             cMissingTerrainSegments =   0;
    int             cFlightProfileSegments =    0;
    int             cTerrainCollisionSegments = 0;
    double          minTerrainHeight =          qQNaN();
    double          maxTerrainHeight =          qQNaN();

    // First we need to determine:
    //  - how many terrain profile vertices we need
    //  - how many missing terrain segments there are
    //  - how many flight profile segments we need
    //  - how many terrain collision segments there are
    //  - what is the total distance so we can calculate pixels per meter

    for (int viIndex=0; viIndex<_visualItems->count(); viIndex++) {
        VisualMissionItem*  visualItem =    _visualItems->value<VisualMissionItem*>(viIndex);
        ComplexMissionItem* complexItem =   _visualItems->value<ComplexMissionItem*>(viIndex);

        if (visualItem->simpleFlightPathSegment()) {
            FlightPathSegment* segment = visualItem->simpleFlightPathSegment();
            _updateSegmentCounts(segment, cFlightProfileSegments, cTerrainProfilePoints, cMissingTerrainSegments, cTerrainCollisionSegments, minTerrainHeight, maxTerrainHeight);
        }

        if (complexItem) {
            for (int segmentIndex=0; segmentIndex<complexItem->flightPathSegments()->count(); segmentIndex++) {
                FlightPathSegment* segment = complexItem->flightPathSegments()->value<FlightPathSegment*>(segmentIndex);
                _updateSegmentCounts(segment, cFlightProfileSegments, cTerrainProfilePoints, cMissingTerrainSegments, cTerrainCollisionSegments, minTerrainHeight, maxTerrainHeight);
            }
        }
    }

    // The profile view min/max is setup to include a full terrain profile as well as the flight path segments.
    _minAMSLAlt = std::fmin(_missionController->minAMSLAltitude(), minTerrainHeight);
    _maxAMSLAlt = std::fmax(_missionController->maxAMSLAltitude(), maxTerrainHeight);

    // We add a buffer to the min/max alts such that the visuals don't draw lines right at the edges of the display
    double amslAltRange = _maxAMSLAlt - _minAMSLAlt;
    double amslAltRangeBuffer = amslAltRange * 0.1;
    _maxAMSLAlt += amslAltRangeBuffer;
    if (_minAMSLAlt > 0.0) {
        _minAMSLAlt -= amslAltRangeBuffer;
        _minAMSLAlt = std::fmax(_minAMSLAlt, 0.0);
    }
    amslAltRange = _maxAMSLAlt - _minAMSLAlt;

    static int counter = 0;
    qCDebug(TerrainProfileLog) << "missionController min/max" << _missionController->minAMSLAltitude() << _missionController->maxAMSLAltitude();
    qCDebug(TerrainProfileLog) << QStringLiteral("updatePaintNode counter:%1 cFlightProfileSegments:%2 cTerrainProfilePoints:%3 cMissingTerrainSegments:%4 cTerrainCollisionSegments:%5 _minAMSLAlt:%6 _maxAMSLAlt:%7 maxTerrainHeight:%8")
                                  .arg(counter++).arg(cFlightProfileSegments).arg(cTerrainProfilePoints).arg(cMissingTerrainSegments).arg(cTerrainCollisionSegments).arg(_minAMSLAlt).arg(_maxAMSLAlt).arg(maxTerrainHeight);

    // G76: over the whole profile distance, so the RTL leg back to launch is not cut off at the right edge
    _pixelsPerMeter = _visibleWidth / (_profileDistance > 0 ? _profileDistance : _missionController->missionTotalDistance());

    // Instantiate nodes
    if (!rootNode) {
        rootNode = new QSGNode;

        QSGGeometryNode* terrainProfileNode =   nullptr;
        QSGGeometryNode* missingTerrainNode =   nullptr;
        QSGGeometryNode* flightProfileNode =    nullptr;
        QSGGeometryNode* terrainCollisionNode = nullptr;

        _createGeometry(terrainProfileNode,     terrainProfileGeometry,     QSGGeometry::DrawLineStrip, "green");
        _createGeometry(missingTerrainNode,     missingTerrainGeometry,     QSGGeometry::DrawLines,     "yellow");
        _createGeometry(flightProfileNode,      flightProfileGeometry,      QSGGeometry::DrawLines,     "orange");
        _createGeometry(terrainCollisionNode,   terrainCollisionGeometry,   QSGGeometry::DrawLines,     "red");

        rootNode->appendChildNode(terrainProfileNode);
        rootNode->appendChildNode(missingTerrainNode);
        rootNode->appendChildNode(flightProfileNode);
        rootNode->appendChildNode(terrainCollisionNode);
    }

    // Allocate space for the vertices

    QSGNode* node = rootNode->childAtIndex(0);
    terrainProfileGeometry = static_cast<QSGGeometryNode*>(node)->geometry();
    terrainProfileGeometry->allocate(cTerrainProfilePoints);
    node->markDirty(QSGNode::DirtyGeometry);

    node = rootNode->childAtIndex(1);
    missingTerrainGeometry = static_cast<QSGGeometryNode*>(node)->geometry();
    missingTerrainGeometry->allocate(cMissingTerrainSegments * 2);
    node->markDirty(QSGNode::DirtyGeometry);

    node = rootNode->childAtIndex(2);
    flightProfileGeometry = static_cast<QSGGeometryNode*>(node)->geometry();
    flightProfileGeometry->allocate(cFlightProfileSegments * 2);
    node->markDirty(QSGNode::DirtyGeometry);

    node = rootNode->childAtIndex(3);
    terrainCollisionGeometry = static_cast<QSGGeometryNode*>(node)->geometry();
    terrainCollisionGeometry->allocate(cTerrainCollisionSegments * 2);
    node->markDirty(QSGNode::DirtyGeometry);

    int                     flightProfileVertexIndex =          0;
    int                     terrainProfileVertexIndex =         0;
    int                     missingterrainProfileVertexIndex =  0;
    int                     terrainCollisionVertexIndex =       0;
    double                  currentDistance =                   0;
    QSGGeometry::Point2D*   flightProfileVertices =             flightProfileGeometry->vertexDataAsPoint2D();
    QSGGeometry::Point2D*   terrainProfileVertices =            terrainProfileGeometry->vertexDataAsPoint2D();
    QSGGeometry::Point2D*   missingTerrainVertices =            missingTerrainGeometry->vertexDataAsPoint2D();
    QSGGeometry::Point2D*   terrainCollisionVertices =          terrainCollisionGeometry->vertexDataAsPoint2D();

    // This step places the vertices for display into the nodes
    for (int viIndex=0; viIndex<_visualItems->count(); viIndex++) {
        VisualMissionItem*  visualItem =    _visualItems->value<VisualMissionItem*>(viIndex);
        ComplexMissionItem* complexItem =   _visualItems->value<ComplexMissionItem*>(viIndex);

        if (complexItem) {
            if (complexItem->flightPathSegments()->count() == 0) {
                currentDistance += complexItem->complexDistance();
            } else {
                for (int segmentIndex=0; segmentIndex<complexItem->flightPathSegments()->count(); segmentIndex++) {
                    FlightPathSegment* segment = complexItem->flightPathSegments()->value<FlightPathSegment*>(segmentIndex);

                    _addFlightProfileSegment    (segment, currentDistance, amslAltRange,    flightProfileVertices,      flightProfileVertexIndex);
                    _addTerrainProfileSegment   (segment, currentDistance, amslAltRange,    terrainProfileVertices,     terrainProfileVertexIndex);
                    _addMissingTerrainSegment   (segment, currentDistance,                  missingTerrainVertices,     missingterrainProfileVertexIndex);
                    _addTerrainCollisionSegment (segment, currentDistance, amslAltRange,    terrainCollisionVertices,   terrainCollisionVertexIndex);

                    currentDistance += segment->totalDistance();
                }
            }
        }

        if (visualItem->simpleFlightPathSegment()) {
            FlightPathSegment* segment = visualItem->simpleFlightPathSegment();

            _addFlightProfileSegment    (segment, currentDistance, amslAltRange,    flightProfileVertices,      flightProfileVertexIndex);
            _addTerrainProfileSegment   (segment, currentDistance, amslAltRange,    terrainProfileVertices,     terrainProfileVertexIndex);
            _addMissingTerrainSegment   (segment, currentDistance,                  missingTerrainVertices,     missingterrainProfileVertexIndex);
            _addTerrainCollisionSegment (segment, currentDistance, amslAltRange,    terrainCollisionVertices,   terrainCollisionVertexIndex);

            currentDistance += segment->totalDistance();
        }
    }

    setImplicitWidth(_visibleWidth/*(_totalDistance * pixelsPerMeter) + (_horizontalMargin * 2)*/);
    setWidth(implicitWidth());

    emit implicitWidthChanged();
    emit widthChanged();
    emit pixelsPerMeterChanged();
    emit minAMSLAltChanged();
    emit maxAMSLAltChanged();

    return rootNode;
}

bool TerrainProfile::_shouldAddFlightProfileSegment(FlightPathSegment* segment)
{
    bool shouldAdd = !qIsNaN(segment->coord1AMSLAlt()) && !qIsNaN(segment->coord2AMSLAlt());
    if (segment->segmentType() == FlightPathSegment::SegmentTypeTerrainFrame) {
        shouldAdd &= segment->amslTerrainHeights().count() != 0;
    }
    return shouldAdd;
}

bool TerrainProfile::_shouldAddMissingTerrainSegment(FlightPathSegment* segment)
{
    return segment->amslTerrainHeights().count() == 0;
}

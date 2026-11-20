/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "FlightPathSegment.h"
#include "QGC.h"
#include "QGCLoggingCategory.h"

#include <QtCore/QVariant>

#include <algorithm>
#include <cmath>

QGC_LOGGING_CATEGORY(FlightPathSegmentLog, "FlightPathSegmentLog")

FlightPathSegment::FlightPathSegment(SegmentType segmentType, const QGeoCoordinate& coord1, double amslCoord1Alt, const QGeoCoordinate& coord2, double amslCoord2Alt, bool queryTerrainData, QObject* parent)
    : QObject           (parent)
    , _coord1           (coord1)
    , _coord2           (coord2)
    , _coord1AMSLAlt     (amslCoord1Alt)
    , _coord2AMSLAlt     (amslCoord2Alt)
    , _queryTerrainData (queryTerrainData)
    , _segmentType      (segmentType)
{
    _terrainState = _queryTerrainData ? TerrainUnknown : TerrainNotChecked;

    _delayedTerrainPathQueryTimer.setSingleShot(true);
    _delayedTerrainPathQueryTimer.setInterval(200);
    _delayedTerrainPathQueryTimer.callOnTimeout(this, &FlightPathSegment::_sendTerrainPathQuery);
    _terrainRetryTimer.setSingleShot(true);
    _terrainRetryTimer.callOnTimeout(this, &FlightPathSegment::_retryTerrainPathQuery);
    _terrainQueryTimeoutTimer.setSingleShot(true);
    _terrainQueryTimeoutTimer.setInterval(terrainQueryTimeoutMs);
    _terrainQueryTimeoutTimer.callOnTimeout(this, &FlightPathSegment::_terrainQueryTimedOut);
    _updateTotalDistance();

    qCDebug(FlightPathSegmentLog) << this << "new" << coord1 << coord2 << amslCoord1Alt << amslCoord2Alt << _totalDistance;

    _sendTerrainPathQuery();
    _updateTerrainCollision();
}

void FlightPathSegment::setCoordinate1(const QGeoCoordinate &coordinate)
{
    if (_coord1 != coordinate) {
        _coord1 = coordinate;
        emit coordinate1Changed(_coord1);
        // G76: the terrain answer in hand is for the old line; until the new one arrives the leg is unknown
        _dropPendingTerrainQuery();
        _resetTerrainRetries();
        _terrainStale = true;
        _delayedTerrainPathQueryTimer.start();
        _updateTotalDistance();
        _updateTerrainCollision();
    }
}

void FlightPathSegment::setCoordinate2(const QGeoCoordinate &coordinate)
{
    if (_coord2 != coordinate) {
        _coord2 = coordinate;
        emit coordinate2Changed(_coord2);
        _dropPendingTerrainQuery();
        _resetTerrainRetries();
        _terrainStale = true;
        _delayedTerrainPathQueryTimer.start();
        _updateTotalDistance();
        _updateTerrainCollision();
    }
}

void FlightPathSegment::setCoord1AMSLAlt(double alt)
{
    if (!QGC::fuzzyCompare(alt, _coord1AMSLAlt)) {
        _coord1AMSLAlt = alt;
        emit coord1AMSLAltChanged();
        _updateTerrainCollision();
    }
}

void FlightPathSegment::setCoord2AMSLAlt(double alt)
{
    if (!QGC::fuzzyCompare(alt, _coord2AMSLAlt)) {
        _coord2AMSLAlt = alt;
        emit coord2AMSLAltChanged();
        _updateTerrainCollision();
    }
}

void FlightPathSegment::setLaunchStart(LaunchStart launchStart)
{
    if (launchStart != _launchStart) {
        _launchStart = launchStart;
        _updateTerrainCollision();
    }
}

void FlightPathSegment::setHeightNotInPlan(bool heightNotInPlan)
{
    if (heightNotInPlan != _heightNotInPlan) {
        _heightNotInPlan = heightNotInPlan;
        _updateTerrainCollision();
    }
}

void FlightPathSegment::setSpecialVisual(bool specialVisual)
{
    if (_specialVisual != specialVisual) {
        _specialVisual = specialVisual;
        emit specialVisualChanged(specialVisual);
    }
}

void FlightPathSegment::_dropPendingTerrainQuery(void)
{
    if (_currentTerrainPathQuery) {
        // We are already waiting on another query. We don't care about those results any more.
        // The query deletes itself when it answers (autoDelete).
        disconnect(_currentTerrainPathQuery, &TerrainPathQuery::terrainDataReceived, this, &FlightPathSegment::_terrainDataReceived);
        _currentTerrainPathQuery = nullptr;
    }
    _terrainQueryTimeoutTimer.stop();
}

void FlightPathSegment::_resetTerrainRetries(void)
{
    _terrainRetryTimer.stop();
    _terrainRetryCount = 0;
}

int FlightPathSegment::terrainRetryDelayMs(int attempt)
{
    if (attempt < 1 || attempt > maxTerrainRetries) {
        return 0;
    }
    switch (attempt) {
    case 1:     return 2000;
    case 2:     return 5000;
    case 3:     return 15000;
    default:    return 30000;
    }
}

void FlightPathSegment::_retryTerrainPathQuery(void)
{
    qCDebug(FlightPathSegmentLog) << this << "_retryTerrainPathQuery" << _terrainRetryCount;
    _sendTerrainPathQuery();
}

void FlightPathSegment::fireTerrainRetryForTest(void)
{
    if (_terrainRetryTimer.isActive()) {
        _terrainRetryTimer.stop();
        _retryTerrainPathQuery();
    }
}

void FlightPathSegment::_terrainQueryTimedOut(void)
{
    if (!_currentTerrainPathQuery) {
        return;
    }
    qCDebug(FlightPathSegmentLog) << this << "_terrainQueryTimedOut";
    // The query may still answer later; that answer is no longer wanted
    _dropPendingTerrainQuery();
    _applyTerrainData(false, TerrainPathQuery::PathHeightInfo_t());
}

void FlightPathSegment::_sendTerrainPathQuery(void)
{
    if (_queryTerrainData && _coord1.isValid() && _coord2.isValid()) {
        qCDebug(FlightPathSegmentLog) << this << "_sendTerrainPathQuery";
        // Clear any previous query
        _dropPendingTerrainQuery();

        // Clear old terrain data
        _amslTerrainHeights.clear();
        _distanceBetween = 0;
        _finalDistanceBetween = 0;
        emit distanceBetweenChanged(0);
        emit finalDistanceBetweenChanged(0);
        emit amslTerrainHeightsChanged();

        _currentTerrainPathQuery = new TerrainPathQuery(true /* autoDelete */);
        connect(_currentTerrainPathQuery, &TerrainPathQuery::terrainDataReceived, this, &FlightPathSegment::_terrainDataReceived);
        _terrainQueryTimeoutTimer.start();
        _currentTerrainPathQuery->requestData(_coord1, _coord2);
    }
}

void FlightPathSegment::_terrainDataReceived(bool success, const TerrainPathQuery::PathHeightInfo_t& pathHeightInfo)
{
    qCDebug(FlightPathSegmentLog) << this << "_terrainDataReceived" << success << pathHeightInfo.heights.count();

    _currentTerrainPathQuery->deleteLater();
    _currentTerrainPathQuery = nullptr;
    _terrainQueryTimeoutTimer.stop();

    _applyTerrainData(success, pathHeightInfo);
}

void FlightPathSegment::applyTerrainDataForTest(const QList<double>& heights, double distanceBetween, double finalDistanceBetween)
{
    _delayedTerrainPathQueryTimer.stop();
    _dropPendingTerrainQuery();
    TerrainPathQuery::PathHeightInfo_t info;
    info.distanceBetween        = distanceBetween;
    info.finalDistanceBetween   = finalDistanceBetween;
    info.heights                = heights;
    _applyTerrainData(true, info);
}

void FlightPathSegment::applyTerrainFailureForTest(void)
{
    _delayedTerrainPathQueryTimer.stop();
    _dropPendingTerrainQuery();
    _applyTerrainData(false, TerrainPathQuery::PathHeightInfo_t());
}

void FlightPathSegment::_applyTerrainData(bool success, const TerrainPathQuery::PathHeightInfo_t& pathHeightInfo)
{
    if (!success) {
        // Never keep the profile of an earlier query: the collision test and the profile would use stale heights
        if (!_amslTerrainHeights.isEmpty()) {
            _amslTerrainHeights.clear();
            emit amslTerrainHeightsChanged();
        }
    } else {
        if (!QGC::fuzzyCompare(pathHeightInfo.distanceBetween, _distanceBetween)) {
            _distanceBetween = pathHeightInfo.distanceBetween;
            emit distanceBetweenChanged(_distanceBetween);
        }
        if (!QGC::fuzzyCompare(pathHeightInfo.finalDistanceBetween, _finalDistanceBetween)) {
            _finalDistanceBetween = pathHeightInfo.finalDistanceBetween;
            emit finalDistanceBetweenChanged(_finalDistanceBetween);
        }

        _amslTerrainHeights.clear();
        for (const double& amslTerrainHeight: pathHeightInfo.heights) {
            _amslTerrainHeights.append(amslTerrainHeight);
        }
        emit amslTerrainHeightsChanged();
    }

    // A failed query leaves no heights, so the leg stays unknown (CL-2) until a retry brings the ground. G76: before
    // the retry, one failed or lost query left the leg unknown for the whole session, even with the ground loaded.
    if (success && !_amslTerrainHeights.isEmpty()) {
        _resetTerrainRetries();
    } else if (_queryTerrainData && !_terrainRetryTimer.isActive()) {
        const int delayMs = terrainRetryDelayMs(_terrainRetryCount + 1);
        if (delayMs > 0) {
            _terrainRetryCount++;
            _terrainRetryTimer.start(delayMs);
        }
    }
    _terrainStale = false;
    _updateTerrainCollision();
}

void FlightPathSegment::_updateTotalDistance(void)
{
    double newTotalDistance = 0;
    if (_coord1.isValid() && _coord2.isValid()) {
        newTotalDistance = _coord1.distanceTo(_coord2);
    }

    if (!QGC::fuzzyCompare(newTotalDistance, _totalDistance)) {
        _totalDistance = newTotalDistance;
        emit totalDistanceChanged(_totalDistance);
    }
}

void FlightPathSegment::_updateTerrainCollision(void)
{
    TerrainState        newState;
    QList<TerrainRun>   newRuns;

    if (!_queryTerrainData) {
        newState = TerrainNotChecked;
        newRuns.append({ TerrainNotChecked, 0, _totalDistance });
    } else {
        QList<double> terrain;
        if (!_terrainStale) {
            terrain.reserve(_amslTerrainHeights.count());
            for (const QVariant& height: _amslTerrainHeights) {
                bool ok = false;
                const double value = height.toDouble(&ok);
                terrain.append(ok ? value : qQNaN());
            }
        }
        const QList<double>         distances   = sampleDistances(terrain.count(), _distanceBetween, _finalDistanceBetween);
        // A-10: a leg whose height is not in the plan is never checked, so every sample is unknown
        const double                alt1        = _heightNotInPlan ? qQNaN() : _coord1AMSLAlt;
        const double                alt2        = _heightNotInPlan ? qQNaN() : _coord2AMSLAlt;
        const QList<TerrainState>   states      = classifySamples(_segmentType, alt1, alt2, _totalDistance, distances, terrain, _launchStart);
        newState    = legState(states);
        newRuns     = mergeShortRuns(terrainRuns(states, distances, _totalDistance), dashPeriodM(_totalDistance));
    }

    const bool newTerrainCollision = newState == TerrainStrike;

    qCDebug(FlightPathSegmentLog) << this << "_updateTerrainCollision new:old" << newTerrainCollision << _terrainCollision << newState;

    if (newTerrainCollision != _terrainCollision) {
        _terrainCollision = newTerrainCollision;
        emit terrainCollisionChanged(_terrainCollision);
    }
    if (newState != _terrainState) {
        _terrainState = newState;
        emit terrainStateChanged();
    }
    _terrainRuns = newRuns;
    _updateMapPieces();
}

void FlightPathSegment::_updateMapPieces(void)
{
    const QVariantList newPieces = buildMapPieces(_coord1, _coord2, _terrainRuns);
    if (newPieces != _mapPieces) {
        _mapPieces = newPieces;
        emit mapPiecesChanged();
    }
}

int FlightPathSegment::statePriority(TerrainState state)
{
    switch (state) {
    case TerrainStrike:         return 4;
    case TerrainUnknown:        return 3;
    case TerrainGroundContact:  return 2;
    case TerrainNoStrike:       return 1;
    case TerrainNotChecked:     break;
    }
    return 0;
}

QList<FlightPathSegment::TerrainRun> FlightPathSegment::mergeShortRuns(const QList<TerrainRun>& runs, double minRunM)
{
    constexpr double epsilon = 1e-9;

    // Drop empty runs and join equal neighbors
    auto normalize = [](const QList<TerrainRun>& in) {
        QList<TerrainRun> out;
        for (const TerrainRun& run: in) {
            if (!(run.endM - run.startM > epsilon)) {
                continue;
            }
            if (!out.isEmpty() && out.last().state == run.state) {
                out.last().endM = run.endM;
            } else if (!out.isEmpty()) {
                TerrainRun joined = run;
                joined.startM = out.last().endM;  // keep the runs contiguous
                out.append(joined);
            } else {
                out.append(run);
            }
        }
        return out;
    };

    QList<TerrainRun> result = normalize(runs);
    if (result.count() < 2 || !(minRunM > 0)) {
        return result;
    }

    const double spanStart  = result.first().startM;
    const double spanEnd    = result.last().endM;
    if (spanEnd - spanStart <= minRunM) {
        // The whole leg is shorter than one run: draw it in its highest priority state
        TerrainRun only = result.first();
        for (const TerrainRun& run: result) {
            if (statePriority(run.state) > statePriority(only.state)) {
                only.state = run.state;
            }
        }
        only.startM = spanStart;
        only.endM   = spanEnd;
        return { only };
    }

    const int maxIterations = 4 * static_cast<int>(result.count()) + 16;
    for (int iteration = 0; iteration < maxIterations && result.count() > 1; iteration++) {
        // The shortest run under the minimum
        int shortest = -1;
        for (int i = 0; i < result.count(); i++) {
            const double length = result[i].endM - result[i].startM;
            if (length < minRunM - epsilon && (shortest < 0 || length < result[shortest].endM - result[shortest].startM)) {
                shortest = i;
            }
        }
        if (shortest < 0) {
            break;
        }

        const bool  hasLeft     = shortest > 0;
        const bool  hasRight    = shortest < result.count() - 1;
        const int   runPriority = statePriority(result[shortest].state);
        int         neighbor    = -1;
        if (hasLeft) {
            neighbor = shortest - 1;
        }
        if (hasRight && (neighbor < 0 || statePriority(result[shortest + 1].state) > statePriority(result[neighbor].state))) {
            neighbor = shortest + 1;
        }

        if (statePriority(result[neighbor].state) >= runPriority) {
            // Absorbed by the neighbor that outranks it
            if (neighbor < shortest) {
                result[neighbor].endM = result[shortest].endM;
            } else {
                result[neighbor].startM = result[shortest].startM;
            }
            result.removeAt(shortest);
        } else {
            // It outranks both neighbors: widen it to the minimum, half from each side where there is room
            double need     = minRunM - (result[shortest].endM - result[shortest].startM);
            double fromLeft = hasLeft ? std::min(need / 2.0, result[shortest - 1].endM - result[shortest - 1].startM) : 0.0;
            double fromRight = hasRight ? std::min(need - fromLeft, result[shortest + 1].endM - result[shortest + 1].startM) : 0.0;
            if (hasLeft && fromLeft + fromRight < need) {
                fromLeft = std::min(need - fromRight, result[shortest - 1].endM - result[shortest - 1].startM);
            }
            result[shortest].startM -= fromLeft;
            result[shortest].endM   += fromRight;
            if (hasLeft) {
                result[shortest - 1].endM = result[shortest].startM;
            }
            if (hasRight) {
                result[shortest + 1].startM = result[shortest].endM;
            }
        }
        result = normalize(result);
    }

    if (!result.isEmpty()) {
        result.first().startM   = spanStart;
        result.last().endM      = spanEnd;
    }
    return result;
}

QList<double> FlightPathSegment::sampleDistances(int count, double distanceBetween, double finalDistanceBetween)
{
    // Same walk as TerrainProfile::_addTerrainProfileSegment: the last step is finalDistanceBetween
    QList<double> distances;
    distances.reserve(std::max(count, 0));
    double x = 0;
    for (int i = 0; i < count; i++) {
        distances.append(x);
        x += (i == count - 2) ? finalDistanceBetween : distanceBetween;
    }
    return distances;
}

QList<FlightPathSegment::TerrainState> FlightPathSegment::classifySamples(SegmentType segmentType, double coord1AMSLAlt, double coord2AMSLAlt,
                                                                          double totalDistance, const QList<double>& distances,
                                                                          const QList<double>& amslTerrainHeights, LaunchStart launchStart)
{
    const int n = static_cast<int>(amslTerrainHeights.count());
    QList<TerrainState> states;
    if (n == 0) {
        return states;
    }

    // A takeoff climb that is not straight up starts on the loaded ground at launch (A-0: the launch reference is
    // the ground at home when it is loaded), not at the plan's home altitude, which may be stale and sit below or
    // above the loaded ground
    const bool takeoff = segmentType == SegmentTypeTakeoff;
    if (takeoff && launchStart == LaunchStartOnGround && std::isfinite(amslTerrainHeights.first())) {
        coord1AMSLAlt = amslTerrainHeights.first();
    }

    // Height above ground of every sample, NaN when unknown
    QList<double> hag(n, qQNaN());
    const bool      terrainFrame    = segmentType == SegmentTypeTerrainFrame;
    const double    hag1            = coord1AMSLAlt - amslTerrainHeights.first();
    const double    hag2            = coord2AMSLAlt - amslTerrainHeights.last();
    for (int i = 0; i < n; i++) {
        double t = 0;
        if (totalDistance > 0 && i < distances.count()) {
            t = std::clamp(distances[i] / totalDistance, 0.0, 1.0);
        } else if (n > 1) {
            t = static_cast<double>(i) / (n - 1);
        }
        const double ground = amslTerrainHeights[i];
        double value;
        if (terrainFrame) {
            // A-7: the leg follows the ground at a height above it interpolated between its two ends
            value = std::isfinite(ground) ? hag1 + ((hag2 - hag1) * t) : qQNaN();
        } else {
            value = (coord1AMSLAlt + ((coord2AMSLAlt - coord1AMSLAlt) * t)) - ground;
        }
        hag[i] = std::isfinite(value) ? value : qQNaN();
    }

    // CL-1a contact span. Takeoff: from the start up to, not including, the first sample whose HAG reaches M.
    // Land: the samples after the last sample whose HAG is at least M. An unknown sample ends a span.
    QList<bool> inSpan(n, false);
    auto endsSpan = [](double h) { return !std::isfinite(h) || h >= strikeMarginM; };
    if (segmentType == SegmentTypeTakeoff) {
        for (int i = 0; i < n && !endsSpan(hag[i]); i++) {
            inSpan[i] = true;
        }
    } else if (segmentType == SegmentTypeLand) {
        for (int i = n - 1; i >= 0 && !endsSpan(hag[i]); i--) {
            inSpan[i] = true;
        }
    }

    states.reserve(n);
    for (int i = 0; i < n; i++) {
        states.append(hagState(hag[i], inSpan[i]));
    }

    // A-11: an ArduPilot takeoff with no position climbs straight up at launch, from the ground to the takeoff
    // height, and only then flies the leg. The leg itself starts at the top of that climb (coord1AMSLAlt), so it
    // is not a slant from the ground: a slant from the ground met every rise of the loaded ground near launch
    // (the Boulder plan: the ground 3 m higher 30 m out made its first 30 m a strike). The first sample is the
    // foot of the climb, on the ground at launch: ground contact (CL-1a). A climb whose top is more than 0.5 m
    // below the ground there stays a strike, and an unknown top or ground stays unknown.
    if (takeoff && launchStart == LaunchStartClimbAtLaunch && std::isfinite(hag[0]) && hag[0] >= 0) {
        states[0] = TerrainGroundContact;
    }
    return states;
}

FlightPathSegment::TerrainState FlightPathSegment::hagState(double hag, bool inContactSpan)
{
    if (!std::isfinite(hag)) {
        return TerrainUnknown;
    }
    if (hag >= strikeMarginM) {
        return TerrainNoStrike;
    }
    return (inContactSpan && hag >= -contactBelowGroundM) ? TerrainGroundContact : TerrainStrike;
}

FlightPathSegment::TerrainState FlightPathSegment::itemTerrainState(double amslAlt, double amslTerrainHeight, bool takeoffOrLand)
{
    return hagState(amslAlt - amslTerrainHeight, takeoffOrLand);
}

FlightPathSegment::TerrainState FlightPathSegment::legState(const QList<TerrainState>& sampleStates)
{
    if (sampleStates.isEmpty()) {
        return TerrainUnknown;
    }
    if (sampleStates.contains(TerrainStrike)) {
        return TerrainStrike;
    }
    if (sampleStates.contains(TerrainUnknown) || sampleStates.contains(TerrainNotChecked)) {
        return TerrainUnknown;
    }
    if (sampleStates.contains(TerrainGroundContact)) {
        return TerrainGroundContact;
    }
    return TerrainNoStrike;
}

QList<FlightPathSegment::TerrainRun> FlightPathSegment::terrainRuns(const QList<TerrainState>& sampleStates, const QList<double>& distances, double totalDistance)
{
    QList<TerrainRun> runs;
    const double total = std::max(totalDistance, 0.0);
    const int n = static_cast<int>(sampleStates.count());
    if (n == 0) {
        runs.append({ TerrainUnknown, 0, total });
        return runs;
    }

    double start = 0;
    for (int i = 0; i < n; i++) {
        double end = total;
        if (i < n - 1 && (i + 1) < distances.count()) {
            end = std::clamp((distances[i] + distances[i + 1]) / 2.0, start, total);
        }
        if (!runs.isEmpty() && runs.last().state == sampleStates[i]) {
            runs.last().endM = end;
        } else {
            runs.append({ sampleStates[i], start, end });
        }
        start = end;
    }
    runs.last().endM = total;
    return runs;
}

double FlightPathSegment::dashPeriodM(double totalDistance)
{
    if (!(totalDistance > 0)) {
        return 0;
    }
    // About 40 m per dash and gap, with at least 4 and at most 50 dashes on a leg, so the dashes show at
    // any zoom where the leg itself shows
    const double periods = std::clamp(std::round(totalDistance / 40.0), 4.0, 50.0);
    return totalDistance / periods;
}

QVariantList FlightPathSegment::buildMapPieces(const QGeoCoordinate& coord1, const QGeoCoordinate& coord2, const QList<TerrainRun>& runs)
{
    QVariantList pieces;
    if (!coord1.isValid() || !coord2.isValid()) {
        return pieces;
    }

    const double total      = coord1.distanceTo(coord2);
    const double azimuth    = coord1.azimuthTo(coord2);

    auto pointAt = [&](double d) -> QGeoCoordinate {
        if (d <= 0) {
            return coord1;
        }
        if (d >= total) {
            return coord2;
        }
        return coord1.atDistanceAndAzimuth(d, azimuth);
    };
    auto addPiece = [&](double a, double b, TerrainState state, bool dashed) {
        QVariantList path;
        path.append(QVariant::fromValue(pointAt(a)));
        // Pieces longer than maxMapPieceM follow the great circle, as MissionLineView drew before G76
        const int parts = std::max(1, static_cast<int>(std::ceil((b - a) / maxMapPieceM)));
        for (int j = 1; j < parts; j++) {
            path.append(QVariant::fromValue(pointAt(a + (j * (b - a) / parts))));
        }
        path.append(QVariant::fromValue(pointAt(b)));
        QVariantMap piece;
        piece[QStringLiteral("path")]   = path;
        piece[QStringLiteral("state")]  = static_cast<int>(state);
        piece[QStringLiteral("dashed")] = dashed;
        piece[QStringLiteral("tick")]   = false;
        pieces.append(piece);
    };
    // A short mark across the line at distance d, halfLength each side of it
    auto addTick = [&](double d, double halfLength) {
        const QGeoCoordinate center = pointAt(d);
        const double along = (d < total) ? center.azimuthTo(coord2) : coord1.azimuthTo(coord2);
        QVariantList path;
        path.append(QVariant::fromValue(center.atDistanceAndAzimuth(halfLength, along - 90.0)));
        path.append(QVariant::fromValue(center.atDistanceAndAzimuth(halfLength, along + 90.0)));
        QVariantMap piece;
        piece[QStringLiteral("path")]   = path;
        piece[QStringLiteral("state")]  = static_cast<int>(TerrainStrike);
        piece[QStringLiteral("dashed")] = false;
        piece[QStringLiteral("tick")]   = true;
        pieces.append(piece);
    };

    if (!(total > 0)) {
        // Both ends at the same place: nothing to draw along the ground but keep the leg's state
        addPiece(0, 0, runs.isEmpty() ? TerrainUnknown : runs.first().state, false);
        return pieces;
    }

    const double period = dashPeriodM(total);
    const double dash   = 0.6 * period;
    for (const TerrainRun& run: runs) {
        const double a = std::clamp(run.startM, 0.0, total);
        const double b = std::clamp(run.endM, 0.0, total);
        if (b <= a) {
            continue;
        }
        if (run.state == TerrainStrike) {
            addPiece(a, b, run.state, false);
            // Strike: tick marks across the line, one per dash period and at least one, a cue that does not rely
            // on color and cannot be mistaken for the dashes of unknown ground
            const double halfLength = 0.25 * period;
            const int ticks = std::max(1, static_cast<int>(std::floor(((b - a) / period) + 1e-6)));
            for (int k = 0; k < ticks; k++) {
                addTick(a + ((k + 0.5) * (b - a) / ticks), halfLength);
            }
            continue;
        }
        if (run.state != TerrainUnknown) {
            addPiece(a, b, run.state, false);
            continue;
        }
        // Unknown ground is dashed; the pattern is anchored to the start of the leg so dashes line up across runs
        const double epsilon = 1e-6 * period;
        for (int j = static_cast<int>(std::floor(a / period)); (j * period) < (b - epsilon); j++) {
            const double p = j * period;
            const double s = std::max(a, p);
            const double e = std::min(b, p + dash);
            if (e - s > epsilon) {
                addPiece(s, e, TerrainUnknown, true);
            }
        }
    }
    return pieces;
}

QColor FlightPathSegment::terrainStateColor(TerrainState state, bool darkTheme)
{
    // The 3D views' colors, PLAN-CONTRACT.md section 4 (custom/src/plan3d/Plan3DColors.h). Red means a strike and nothing else.
    switch (state) {
    case TerrainStrike:         return QColor(0xff, 0x55, 0x44);                                                    // #ff5544
    case TerrainNoStrike:       return QColor(0x2d, 0xd4, 0xbf);                                                    // #2dd4bf
    case TerrainUnknown:        return darkTheme ? QColor(0xb0, 0x7c, 0xff) : QColor(0x7c, 0x3a, 0xed);             // #b07cff / #7c3aed
    case TerrainGroundContact:  return darkTheme ? QColor(0x94, 0xa3, 0xb8) : QColor(0x64, 0x74, 0x8b);             // #94a3b8 / #64748b
    case TerrainNotChecked:     break;
    }
    return QColor();
}

QString FlightPathSegment::terrainStateColorName(int state, bool darkTheme) const
{
    if (state < TerrainNotChecked || state > TerrainStrike) {
        return QString();
    }
    const QColor color = terrainStateColor(static_cast<TerrainState>(state), darkTheme);
    return color.isValid() ? color.name() : QString();
}

/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - flight detection.
 *
 ****************************************************************************/

#include "FlightWatcher.h"

#include <cmath>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtPositioning/QGeoCoordinate>

#include "Fact.h"
#include "FactGroup.h"
#include "MultiVehicleManager.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

QGC_LOGGING_CATEGORY(FlightWatcherLog, "gcs.custom.rotorlab.flight")

namespace {

/// Another arm inside this window is the same flight. A touch-and-go, a
/// battery swap that turns out to be a false start, or a bounce on landing
/// should not become two records in somebody's logbook.
constexpr int kSettleMs = 30 * 1000;

/// Fast enough to catch the top of a climb, slow enough to be free.
constexpr int kSampleMs = 1000;

/// A flight this short is a motor test on the bench, not a sortie.
constexpr double kMinFlightS = 5.0;

double factDouble(FactGroup *fg, const char *name)
{
    if (!fg) {
        return std::nan("");
    }
    if (Fact *f = fg->getFact(QLatin1String(name))) {
        return f->rawValue().toDouble();
    }
    return std::nan("");
}

bool sane(double v) { return !std::isnan(v) && !std::isinf(v); }

} // namespace

FlightWatcher::FlightWatcher(QObject *parent)
    : QObject(parent)
{
    _sampler.setInterval(kSampleMs);
    connect(&_sampler, &QTimer::timeout, this, &FlightWatcher::_sample);

    _settleTimer.setSingleShot(true);
    _settleTimer.setInterval(kSettleMs);
    connect(&_settleTimer, &QTimer::timeout, this, &FlightWatcher::_settle);

    // Before anything else: a sortie the last run saw land but never settled.
    _restorePending();

    MultiVehicleManager *const mvm = MultiVehicleManager::instance();
    if (!mvm) {
        return;
    }
    connect(mvm, &MultiVehicleManager::vehicleAdded, this, &FlightWatcher::_watch);
    // Vehicles can already be connected before this object exists.
    if (QmlObjectListModel *list = mvm->vehicles()) {
        for (int i = 0; i < list->count(); ++i) {
            _watch(qobject_cast<Vehicle *>((*list)[i]));
        }
    }
}

void FlightWatcher::_watch(Vehicle *vehicle)
{
    if (!vehicle) {
        return;
    }
    connect(vehicle, &Vehicle::armedChanged, this, [this, vehicle](bool armed) {
        _onArmed(vehicle, armed);
    });
    // A vehicle that appears already armed is a GC that started mid-sortie.
    if (vehicle->armed()) {
        _onArmed(vehicle, true);
    }
}

void FlightWatcher::_onArmed(Vehicle *vehicle, bool armed)
{
    const int sysid = vehicle->id();
    Live &live = _live[sysid];

    if (armed == live.armed) {
        return;
    }
    live.armed = armed;

    if (armed) {
        ++_armedCount;
        _settleTimer.stop();            // this is the same flight continuing

        if (!live.takeoffAt.isValid()) {
            // A fresh sortie. QGC stores no takeoff time anywhere, so this is
            // the only record of it.
            live.takeoffAt    = QDateTime::currentDateTimeUtc();
            live.maxHeightM   = 0;
            live.maxDistanceM = 0;
            live.accumulatedS = 0;
            live.hasPosition  = false;
        }
        live.landingAt = {};
        if (!_sampler.isActive()) {
            _sampler.start();
        }
        emit armedChanged(true);
        return;
    }

    // Disarmed.
    _armedCount = qMax(0, _armedCount - 1);
    live.landingAt = QDateTime::currentDateTimeUtc();
    if (live.takeoffAt.isValid()) {
        live.accumulatedS = live.takeoffAt.msecsTo(live.landingAt) / 1000.0;
    }

    // armedPosition is captured by QGC on the arming edge and is NOT cleared
    // on disarm, so it is still the right answer here - and it is a better
    // operating-area position than homePosition, which the vehicle may re-set
    // in flight.
    const QGeoCoordinate armedAt = vehicle->armedPosition();
    if (armedAt.isValid()
        && !(qFuzzyIsNull(armedAt.latitude()) && qFuzzyIsNull(armedAt.longitude()))) {
        live.lat = armedAt.latitude();
        live.lon = armedAt.longitude();
        live.hasPosition = true;
    }

    if (_armedCount == 0) {
        _sampler.stop();
        // Do not file yet. Another arm inside the window is the same flight.
        _settleTimer.start();
    }
    // On disk now, not when the window closes: the station may be shut inside it.
    _persistPending();
    emit armedChanged(_armedCount > 0);
}

void FlightWatcher::_sample()
{
    MultiVehicleManager *const mvm = MultiVehicleManager::instance();
    QmlObjectListModel *const list = mvm ? mvm->vehicles() : nullptr;
    for (int i = 0; list && i < list->count(); ++i) {
        Vehicle *const v = qobject_cast<Vehicle *>((*list)[i]);
        if (!v || !v->armed()) {
            continue;
        }
        Live &live = _live[v->id()];
        FactGroup *const fg = v->vehicleFactGroup();

        // Height above the launch point, which is what an operator means by
        // "how high did it get".
        const double agl = factDouble(fg, "altitudeRelative");
        if (sane(agl)) {
            live.maxHeightM = qMax(live.maxHeightM, agl);
        }
        // Instantaneous, and NaN whenever either coordinate is invalid - hence
        // the gate rather than a bare qMax.
        const double dist = factDouble(fg, "distanceToHome");
        if (sane(dist)) {
            live.maxDistanceM = qMax(live.maxDistanceM, dist);
        }
    }
}

void FlightWatcher::_settle()
{
    // The window closed with nothing re-armed. Everything that flew is now a
    // completed sortie.
    for (auto it = _live.begin(); it != _live.end(); ++it) {
        Live &live = it.value();
        if (live.armed || !live.takeoffAt.isValid() || !live.landingAt.isValid()) {
            continue;
        }

        const double durationS = live.accumulatedS;
        if (durationS >= kMinFlightS) {
            Flight f;
            f.sysid        = it.key();
            f.takeoffAt    = live.takeoffAt;
            f.landingAt    = live.landingAt;
            f.durationS    = durationS;
            f.maxHeightM   = live.maxHeightM;
            f.maxDistanceM = live.maxDistanceM;
            f.lat          = live.lat;
            f.lon          = live.lon;
            f.hasPosition  = live.hasPosition;
            qCDebug(FlightWatcherLog) << "flight complete on sysid" << f.sysid
                                      << durationS << "s";
            emit flightComplete(f);
        } else {
            qCDebug(FlightWatcherLog) << "ignoring a" << durationS
                                      << "s arm on sysid" << it.key();
        }

        // Reset for the next sortie.
        live.takeoffAt = {};
        live.landingAt = {};
        live.accumulatedS = 0;
    }
    _persistPending();
}

// ------------------------------------------------------- the re-arm window on disk

QString FlightWatcher::pendingPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/rotorlab-pending-flights.json");
}

bool FlightWatcher::savePending(const QString &path, const QList<Flight> &flights)
{
    if (flights.isEmpty()) {
        return !QFileInfo::exists(path) || QFile::remove(path);
    }
    QJsonArray arr;
    for (const Flight &f : flights) {
        QJsonObject o;
        o[QStringLiteral("sysid")]          = f.sysid;
        o[QStringLiteral("takeoff_at")]     = f.takeoffAt.toString(Qt::ISODateWithMs);
        o[QStringLiteral("landing_at")]     = f.landingAt.toString(Qt::ISODateWithMs);
        o[QStringLiteral("duration_s")]     = f.durationS;
        o[QStringLiteral("max_height_m")]   = f.maxHeightM;
        o[QStringLiteral("max_distance_m")] = f.maxDistanceM;
        o[QStringLiteral("lat")]            = f.lat;
        o[QStringLiteral("lon")]            = f.lon;
        o[QStringLiteral("has_position")]   = f.hasPosition;
        arr.append(o);
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        qCWarning(FlightWatcherLog) << "cannot write" << path << out.errorString();
        return false;
    }
    out.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return out.commit();
}

QList<FlightWatcher::Flight> FlightWatcher::loadPending(const QString &path)
{
    QList<Flight> out;
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        return out;
    }
    const QJsonArray arr = QJsonDocument::fromJson(in.readAll()).array();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        Flight f;
        f.sysid        = o.value(QStringLiteral("sysid")).toInt();
        f.takeoffAt    = QDateTime::fromString(o.value(QStringLiteral("takeoff_at")).toString(),
                                               Qt::ISODateWithMs);
        f.landingAt    = QDateTime::fromString(o.value(QStringLiteral("landing_at")).toString(),
                                               Qt::ISODateWithMs);
        f.durationS    = o.value(QStringLiteral("duration_s")).toDouble();
        f.maxHeightM   = o.value(QStringLiteral("max_height_m")).toDouble();
        f.maxDistanceM = o.value(QStringLiteral("max_distance_m")).toDouble();
        f.lat          = o.value(QStringLiteral("lat")).toDouble();
        f.lon          = o.value(QStringLiteral("lon")).toDouble();
        f.hasPosition  = o.value(QStringLiteral("has_position")).toBool();
        f.recovered    = true;
        if (f.takeoffAt.isValid() && f.landingAt.isValid()) {
            out << f;
        }
    }
    return out;
}

void FlightWatcher::_persistPending()
{
    QList<Flight> pending;
    for (auto it = _live.cbegin(); it != _live.cend(); ++it) {
        const Live &live = it.value();
        // Disarmed and waiting for the window to close. A sortie re-armed in
        // the window keeps its last saved landing until it disarms again.
        if (live.armed || !live.takeoffAt.isValid() || !live.landingAt.isValid()) {
            continue;
        }
        Flight f;
        f.sysid        = it.key();
        f.takeoffAt    = live.takeoffAt;
        f.landingAt    = live.landingAt;
        f.durationS    = live.accumulatedS;
        f.maxHeightM   = live.maxHeightM;
        f.maxDistanceM = live.maxDistanceM;
        f.lat          = live.lat;
        f.lon          = live.lon;
        f.hasPosition  = live.hasPosition;
        pending << f;
    }
    if (pending.isEmpty() && _armedCount > 0) {
        return;     // flying again inside the window: keep what is on disk
    }
    savePending(pendingPath(), pending);
}

void FlightWatcher::_restorePending()
{
    const QList<Flight> left = loadPending(pendingPath());
    if (left.isEmpty()) {
        return;
    }
    // Queued: this runs inside the constructor, before anyone has connected.
    QTimer::singleShot(0, this, [this, left]() {
        for (const Flight &f : left) {
            if (f.durationS >= kMinFlightS) {
                qCDebug(FlightWatcherLog) << "flight recovered from the last run on sysid"
                                          << f.sysid << f.durationS << "s";
                emit flightComplete(f);
            }
        }
        _persistPending();
    });
}

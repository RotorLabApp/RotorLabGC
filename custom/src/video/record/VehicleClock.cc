/****************************************************************************
 *
 * RotorLab GC - latest vehicle time and position per system id (G95).
 *
 ****************************************************************************/

#include "VehicleClock.h"

#include <chrono>
#include <cmath>

#include <QtCore/QMutexLocker>
#include <QtPositioning/QGeoCoordinate>

#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

namespace {

/// 2001-01-01 in microseconds: below this a GPS_RAW_INT.time_usec is a time
/// since boot, not a UNIX time.
constexpr qint64 kUnixUsFloor = 978307200LL * 1000000LL;

} // namespace

VehicleClock::VehicleClock(QObject *parent)
    : QObject(parent)
{
    MultiVehicleManager *manager = MultiVehicleManager::instance();
    if (!manager) {
        return;
    }
    connect(manager, &MultiVehicleManager::vehicleAdded, this, &VehicleClock::_watch);
    if (QmlObjectListModel *vehicles = manager->vehicles()) {
        for (int i = 0; i < vehicles->count(); ++i) {
            _watch(qobject_cast<Vehicle *>((*vehicles)[i]));
        }
    }
}

VehicleClock::~VehicleClock() = default;

qint64 VehicleClock::steadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void VehicleClock::_watch(Vehicle *vehicle)
{
    if (!vehicle) {
        return;
    }
    const int sysid = vehicle->id();
    connect(vehicle, &Vehicle::mavlinkMessageReceived, this, [this, sysid](const mavlink_message_t &message) {
        if (message.msgid == MAVLINK_MSG_ID_SYSTEM_TIME) {
            mavlink_system_time_t t{};
            mavlink_msg_system_time_decode(&message, &t);
            noteSystemTime(sysid, qint64(t.time_boot_ms), qint64(t.time_unix_usec), steadyNowNs());
        } else if (message.msgid == MAVLINK_MSG_ID_GPS_RAW_INT) {
            mavlink_gps_raw_int_t g{};
            mavlink_msg_gps_raw_int_decode(&message, &g);
            if (g.fix_type >= 3) {
                noteGpsTime(sysid, qint64(g.time_usec), steadyNowNs());
            }
        }
    });
    connect(vehicle, &Vehicle::coordinateChanged, this, [this, sysid](const QGeoCoordinate &c) {
        if (c.isValid()) {
            notePosition(sysid, c.latitude(), c.longitude(),
                         c.type() == QGeoCoordinate::Coordinate3D ? c.altitude() : std::nan(""),
                         steadyNowNs());
        }
    });
}

void VehicleClock::noteSystemTime(int sysid, qint64 bootMs, qint64 unixUs, qint64 steadyNs)
{
    QMutexLocker lock(&_mutex);
    VehicleSample &s = _samples[sysid];
    s.hasBoot = true;
    s.bootMs = bootMs;
    if (unixUs >= kUnixUsFloor) {
        s.hasGpsUtc = true;
        s.gpsUtcUs = unixUs;
    }
    s.timeSteadyNs = steadyNs;
}

void VehicleClock::noteGpsTime(int sysid, qint64 timeUsec, qint64 steadyNs)
{
    if (timeUsec < kUnixUsFloor) {
        return;                         // a time since boot, not UTC
    }
    QMutexLocker lock(&_mutex);
    VehicleSample &s = _samples[sysid];
    s.hasGpsUtc = true;
    s.gpsUtcUs = timeUsec;
    s.timeSteadyNs = steadyNs;
}

void VehicleClock::notePosition(int sysid, double lat, double lon, double alt, qint64 steadyNs)
{
    if (!std::isfinite(lat) || !std::isfinite(lon)) {
        return;
    }
    QMutexLocker lock(&_mutex);
    VehicleSample &s = _samples[sysid];
    s.hasPosition = true;
    s.lat = lat;
    s.lon = lon;
    s.hasAlt = std::isfinite(alt);
    s.altM = s.hasAlt ? alt : 0.0;
    s.posSteadyNs = steadyNs;
}

VehicleSample VehicleClock::snapshot(int sysid) const
{
    QMutexLocker lock(&_mutex);
    return _samples.value(sysid);
}

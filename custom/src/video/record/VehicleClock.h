/****************************************************************************
 *
 * RotorLab GC - latest vehicle time and position per system id, for the
 * recording's time map (G95).
 *
 * Read only: listens to SYSTEM_TIME and GPS_RAW_INT as they arrive and to
 * Vehicle::coordinate(). Lives on the GUI thread; snapshot() is thread safe
 * so the recording thread can read it. Every value carries the steady-clock
 * time it was received (the same clock as the video tap's receive times), so
 * the time map can say how old it was. A value never received is absent.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QMutex>
#include <QtCore/QObject>
#include <QtCore/QString>

class Vehicle;

struct VehicleSample
{
    bool    hasBoot = false;
    qint64  bootMs = 0;             ///< SYSTEM_TIME.time_boot_ms
    bool    hasGpsUtc = false;
    qint64  gpsUtcUs = 0;           ///< SYSTEM_TIME.time_unix_usec, else GPS_RAW_INT.time_usec when it is a UNIX time
    qint64  timeSteadyNs = 0;       ///< when the time values were received
    bool    hasPosition = false;
    double  lat = 0.0;
    double  lon = 0.0;
    bool    hasAlt = false;
    double  altM = 0.0;             ///< the vehicle coordinate's altitude (AMSL as QGC reports it)
    qint64  posSteadyNs = 0;        ///< when the position was received
};

class VehicleClock : public QObject
{
    Q_OBJECT

public:
    explicit VehicleClock(QObject *parent = nullptr);
    ~VehicleClock() override;

    /// The latest values for `sysid` (all absent when unknown). Any thread.
    VehicleSample snapshot(int sysid) const;

    /// Feed values directly (tests, and the MAVLink handlers below).
    void noteSystemTime(int sysid, qint64 bootMs, qint64 unixUs, qint64 steadyNs);
    void noteGpsTime(int sysid, qint64 timeUsec, qint64 steadyNs);
    void notePosition(int sysid, double lat, double lon, double alt, qint64 steadyNs);

    /// The steady clock the video tap uses (std::chrono::steady_clock), in ns.
    static qint64 steadyNowNs();

private:
    void _watch(Vehicle *vehicle);

    mutable QMutex _mutex;
    QHash<int, VehicleSample> _samples;
};

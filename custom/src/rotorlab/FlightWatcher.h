/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - when did a flight happen, and how big was it?
 *
 * Unlike the Fleet Server, RotorLab.app never learns about a flight until the
 * whole story is told at once. There is no "open a flight" call and no
 * server-side timeout that closes one, so the GC has to decide for itself when
 * a sortie has ended - and it files only then. Never on arm, never mid-flight.
 *
 * WHAT QGC ALREADY HAS, AND WHAT IT DOES NOT
 *
 * `flightTime` and `flightDistance` exist on the vehicle fact group, but
 * Vehicle::_flightTimerStop() only stops the updater: it writes no final value
 * and resets nothing until the next arm. So the fact is up to a second short
 * and lives in a window. That is fine as a cross-check and wrong as a record,
 * so the timestamps here are latched from the armed edges instead.
 *
 * There is no max-altitude and no max-distance anywhere in QGC, and
 * `distanceToHome` is instantaneous and NaN whenever either coordinate is
 * invalid. Both maxima are therefore sampled and latched here while armed.
 *
 * `Vehicle::armedPosition()` IS already captured on the arming edge and
 * survives disarm, so it is the honest operating-area position - better than
 * `homePosition`, which the vehicle can re-set mid-flight.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QTimer>

class Vehicle;

class FlightWatcher : public QObject
{
    Q_OBJECT

public:
    explicit FlightWatcher(QObject *parent = nullptr);

    /// One completed sortie, as the GC observed it.
    struct Flight {
        int       sysid = 0;
        QDateTime takeoffAt;        ///< UTC, latched on the arming edge
        QDateTime landingAt;        ///< UTC, latched on the disarming edge
        double    durationS = 0;
        double    maxHeightM = 0;   ///< above the launch point
        double    maxDistanceM = 0; ///< from home
        double    lat = 0;
        double    lon = 0;
        bool      hasPosition = false;
        /// Read back from disk at start-up: the station closed during the
        /// re-arm window, so the receiver checks it was not already queued.
        bool      recovered = false;
    };

    /// A sortie that has disarmed but not yet settled is written here at
    /// every disarm, so closing GC inside the 30 s window does not lose it.
    /// It is read back and reported as complete at the next start.
    static QString pendingPath();
    static bool savePending(const QString &path, const QList<Flight> &flights);
    static QList<Flight> loadPending(const QString &path);

    /// True while any vehicle is armed. The connector uses this to keep the
    /// crew screen and checklist offers away from a live sortie.
    bool anyArmed() const { return _armedCount > 0; }

signals:
    /// A sortie ended and stayed ended. Emitted once the re-arm window has
    /// closed, so a touch-and-go is one flight rather than two.
    void flightComplete(const FlightWatcher::Flight &flight);
    /// The arming edges, for whoever wants to offer a checklist.
    void armedChanged(bool anyArmed);

private:
    friend class RotorLabSpoolTest;

    void _watch(Vehicle *vehicle);
    void _onArmed(Vehicle *vehicle, bool armed);
    void _sample();
    void _settle();
    /// Every disarmed sortie still inside its window, to pendingPath().
    void _persistPending();
    /// Report what the last run left in pendingPath(), then clear it.
    void _restorePending();

    struct Live {
        QDateTime takeoffAt;
        QDateTime landingAt;
        double    maxHeightM = 0;
        double    maxDistanceM = 0;
        double    lat = 0;
        double    lon = 0;
        bool      hasPosition = false;
        bool      armed = false;
        double    accumulatedS = 0;     ///< across a touch-and-go
    };

    QHash<int, Live> _live;
    QTimer _sampler;        ///< 1 Hz, only while something is armed
    QTimer _settleTimer;    ///< 30 s after the last disarm
    int    _armedCount = 0;
};

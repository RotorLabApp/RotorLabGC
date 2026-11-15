/****************************************************************************
 *
 * RotorLab GC Fleet Connector - unified-server telemetry mapping.
 *
 ****************************************************************************/

#include "FleetTelemetry.h"
#include "FleetStore.h"
#include "tak/TakManager.h"

#include <cmath>

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QUuid>
#include <QtCore/QVariantMap>
#include <QtPositioning/QGeoCoordinate>

#include "Fact.h"
#include "FactGroup.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

namespace {

double factDouble(FactGroup *group, const char *name)
{
    if (!group) {
        return std::nan("");
    }
    if (Fact *fact = group->getFact(QLatin1String(name))) {
        return fact->rawValue().toDouble();
    }
    return std::nan("");
}

int factInt(FactGroup *group, const char *name)
{
    if (!group) {
        return -1;
    }
    if (Fact *fact = group->getFact(QLatin1String(name))) {
        const QVariant value = fact->rawValue();
        return value.isValid() ? value.toInt() : -1;
    }
    return -1;
}

bool sane(double value)
{
    return !std::isnan(value) && !std::isinf(value);
}

QString newFlightRef()
{
    return QUuid::createUuid().toString(QUuid::Id128);
}

} // namespace

FleetTelemetry::FleetTelemetry(QObject *parent)
    : QObject(parent)
{
    MultiVehicleManager *manager = MultiVehicleManager::instance();
    if (!manager) {
        return;
    }
    connect(manager, &MultiVehicleManager::vehicleAdded,
            this, &FleetTelemetry::_onVehicleAdded);
    connect(manager, &MultiVehicleManager::vehicleRemoved,
            this, &FleetTelemetry::_onVehicleRemoved);

    if (QmlObjectListModel *vehicles = manager->vehicles()) {
        for (int i = 0; i < vehicles->count(); ++i) {
            if (Vehicle *vehicle = qobject_cast<Vehicle *>((*vehicles)[i])) {
                _watch(vehicle);
            }
        }
    }
}

void FleetTelemetry::_onVehicleAdded(Vehicle *vehicle)
{
    _watch(vehicle);
}

void FleetTelemetry::_onVehicleRemoved(Vehicle *vehicle)
{
    if (vehicle) {
        _uidBySysid.remove(vehicle->id());
    }
}

void FleetTelemetry::_watch(Vehicle *vehicle)
{
    if (!vehicle) {
        return;
    }
    connect(vehicle, &Vehicle::mavlinkMessageReceived, this,
            [this, vehicle](const mavlink_message_t &message) {
        if (message.msgid != MAVLINK_MSG_ID_AUTOPILOT_VERSION
            || _uidBySysid.contains(vehicle->id())) {
            return;
        }
        mavlink_autopilot_version_t version{};
        mavlink_msg_autopilot_version_decode(&message, &version);

        QByteArray raw(reinterpret_cast<const char *>(version.uid2), sizeof(version.uid2));
        while (raw.endsWith('\0')) {
            raw.chop(1);
        }
        QString uid;
        if (!raw.isEmpty()) {
            uid = QString::fromLatin1(raw.toHex());
        } else if (version.uid != 0) {
            uid = QStringLiteral("%1").arg(version.uid, 16, 16, QLatin1Char('0'));
        }
        if (!uid.isEmpty()) {
            _uidBySysid.insert(vehicle->id(), uid);
        }
    });
}

QString FleetTelemetry::flightRefForChecklist(const QString &kind, int sysid,
                                               const QJsonObject &runContext)
{
    QJsonObject state = FleetStore::flightState(sysid);
    if (kind == QLatin1String("preflight")) {
        if (state.value(QStringLiteral("active")).toBool()) {
            return state.value(QStringLiteral("ref")).toString();
        }
        if (state.value(QStringLiteral("ref")).toString().isEmpty()
            || state.value(QStringLiteral("completed")).toBool()) {
            state = {};
            state.insert(QStringLiteral("ref"), newFlightRef());
            state.insert(QStringLiteral("pending"), true);
            state.insert(QStringLiteral("started"), 0);
            state.insert(QStringLiteral("details"), runContext);
            state.insert(QStringLiteral("vehicle"), QStringLiteral("Vehicle %1").arg(sysid));
            FleetStore::setFlightState(sysid, state);
        }
    }
    return state.value(QStringLiteral("ref")).toString();
}

QJsonObject FleetTelemetry::buildReport(const QJsonObject &runContext, int maxSamples)
{
    QJsonObject report;
    QJsonArray samples;
    QJsonArray completed;
    QHash<int, QVariantMap> fleet;
    if (TakManager *tak = TakManager::instance()) {
        for (const QVariant &row : tak->fleetModel()) {
            const QVariantMap entry = row.toMap();
            fleet.insert(entry.value(QStringLiteral("sysid")).toInt(), entry);
        }
    }

    MultiVehicleManager *manager = MultiVehicleManager::instance();
    QmlObjectListModel *vehicles = manager ? manager->vehicles() : nullptr;
    for (int i = 0; vehicles && i < vehicles->count() && samples.size() < maxSamples; ++i) {
        Vehicle *vehicle = qobject_cast<Vehicle *>((*vehicles)[i]);
        if (!vehicle) {
            continue;
        }

        const int sysid = vehicle->id();
        const bool armed = vehicle->armed();
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QJsonObject state = FleetStore::flightState(sysid);
        QString ref = state.value(QStringLiteral("ref")).toString();
        const bool wasActive = state.value(QStringLiteral("active")).toBool();
        const bool ended = !armed && wasActive;

        if (armed && !wasActive) {
            if (ref.isEmpty() || state.value(QStringLiteral("completed")).toBool()) {
                ref = newFlightRef();
            }
            const QString callsign =
                fleet.value(sysid).value(QStringLiteral("callsign")).toString();
            state.insert(QStringLiteral("ref"), ref);
            state.insert(QStringLiteral("active"), true);
            state.insert(QStringLiteral("pending"), false);
            state.insert(QStringLiteral("completed"), false);
            state.insert(QStringLiteral("started"), double(now.toSecsSinceEpoch()));
            state.insert(QStringLiteral("details"), runContext);
            state.insert(QStringLiteral("vehicle"),
                         callsign.isEmpty() ? QStringLiteral("Vehicle %1").arg(sysid) : callsign);
            state.insert(QStringLiteral("uid"), _uidBySysid.value(sysid));
            FleetStore::setFlightState(sysid, state);
        } else if (ended) {
            state.insert(QStringLiteral("active"), false);
            state.insert(QStringLiteral("completed"), true);
            state.insert(QStringLiteral("ended"), double(now.toSecsSinceEpoch()));
            FleetStore::setFlightState(sysid, state);
        }

        QJsonObject sample;
        sample.insert(QStringLiteral("at"), now.toString(Qt::ISODateWithMs));
        sample.insert(QStringLiteral("vehicle"),
                      state.value(QStringLiteral("vehicle"))
                          .toString(QStringLiteral("Vehicle %1").arg(sysid)).left(80));
        sample.insert(QStringLiteral("armed"), armed);
        if (!ref.isEmpty() && (armed || ended)) {
            sample.insert(QStringLiteral("flight_ref"), ref);
        }

        const QString mode = vehicle->flightMode();
        if (!mode.isEmpty()) {
            sample.insert(QStringLiteral("mode"), mode.left(40));
        }
        const QGeoCoordinate coordinate = vehicle->coordinate();
        if (coordinate.isValid() && sane(coordinate.latitude()) && sane(coordinate.longitude())
            && !(qFuzzyIsNull(coordinate.latitude()) && qFuzzyIsNull(coordinate.longitude()))) {
            sample.insert(QStringLiteral("lat"), coordinate.latitude());
            sample.insert(QStringLiteral("lon"), coordinate.longitude());
            if (sane(coordinate.altitude())) {
                sample.insert(QStringLiteral("alt_m"), coordinate.altitude());
            }
        }

        FactGroup *facts = vehicle->vehicleFactGroup();
        const double speed = factDouble(facts, "groundSpeed");
        const double heading = factDouble(facts, "heading");
        if (sane(speed) && speed >= 0) {
            sample.insert(QStringLiteral("speed_ms"), speed);
        }
        if (sane(heading) && heading >= 0 && heading <= 360) {
            sample.insert(QStringLiteral("heading"), heading);
        }
        if (QmlObjectListModel *batteries = vehicle->batteries()) {
            for (int battery = 0; battery < batteries->count(); ++battery) {
                FactGroup *group = qobject_cast<FactGroup *>((*batteries)[battery]);
                const int percent = factInt(group, "percentRemaining");
                if (percent >= 0 && percent <= 100) {
                    sample.insert(QStringLiteral("battery_pct"), percent);
                    break;
                }
            }
        }
        samples.append(sample);

        if (ended && !ref.isEmpty()) {
            QJsonObject flight;
            flight.insert(QStringLiteral("flight_ref"), ref);
            flight.insert(QStringLiteral("sysid"), sysid);
            flight.insert(QStringLiteral("vehicle"), sample.value(QStringLiteral("vehicle")));
            flight.insert(QStringLiteral("uid"), state.value(QStringLiteral("uid")));
            flight.insert(QStringLiteral("started"), state.value(QStringLiteral("started")));
            flight.insert(QStringLiteral("ended"), state.value(QStringLiteral("ended")));
            flight.insert(QStringLiteral("duration_s"),
                          qMax(0.0, flight.value(QStringLiteral("ended")).toDouble()
                                       - flight.value(QStringLiteral("started")).toDouble()));
            flight.insert(QStringLiteral("details"), state.value(QStringLiteral("details")));
            completed.append(flight);
            emit flightEnded(flight);
        }
    }
    report.insert(QStringLiteral("samples"), samples);
    report.insert(QStringLiteral("completed"), completed);
    return report;
}

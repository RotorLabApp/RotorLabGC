/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - airframe resolution.
 *
 ****************************************************************************/

#include "AirframeResolver.h"
#include "RotorLabStore.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QVariantMap>

#include "MultiVehicleManager.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "RemoteIDSettings.h"
#include "SettingsManager.h"
#include "Vehicle.h"
#include "tak/TakManager.h"

QGC_LOGGING_CATEGORY(AirframeResolverLog, "gcs.custom.rotorlab.airframe")

namespace {

/// RemoteID basicIDType 1 is "SerialNumber (ANSI/CTA-2063)". The setting's
/// default is 2 (CAA), so this is a genuine declaration by the operator and
/// not just whatever the field happened to contain.
constexpr int kBasicIdTypeSerial = 1;

/// The tail and callsign this GC holds for a sysid, from the Fleet screen.
QVariantMap fleetEntry(int sysid)
{
    if (TakManager *tak = TakManager::instance()) {
        for (const QVariant &row : tak->fleetModel()) {
            const QVariantMap m = row.toMap();
            if (m.value(QStringLiteral("sysid")).toInt() == sysid) {
                return m;
            }
        }
    }
    return {};
}

int connectedVehicleCount()
{
    MultiVehicleManager *const mvm = MultiVehicleManager::instance();
    QmlObjectListModel *const list = mvm ? mvm->vehicles() : nullptr;
    return list ? list->count() : 0;
}

} // namespace

AirframeResolver::AirframeResolver(QObject *parent)
    : QObject(parent)
{
    _loadMap();
}

// ------------------------------------------------------------------ identity

void AirframeResolver::noteHardwareId(int sysid, const QString &uid)
{
    if (!uid.isEmpty()) {
        _uidBySysid.insert(sysid, uid);
    }
}

void AirframeResolver::forgetHardwareId(int sysid)
{
    _uidBySysid.remove(sysid);
}

QString AirframeResolver::hardwareId(int sysid) const
{
    return _uidBySysid.value(sysid);
}

QString AirframeResolver::_trustworthyRemoteIdSerial()
{
    RemoteIDSettings *const rid = SettingsManager::instance()
                                    ? SettingsManager::instance()->remoteIDSettings() : nullptr;
    if (!rid) {
        return {};
    }
    // Only when the operator declared it a CTA-2063 serial. Anything else in
    // that field is a CAA registration number or a UTM UUID, and matching a
    // registry `rid_serial` against one of those is a coincidence at best.
    if (rid->basicIDType()->rawValue().toInt() != kBasicIdTypeSerial) {
        return {};
    }
    // And only with one aircraft connected. The setting is global: with two,
    // it would hand back the same serial for both.
    if (connectedVehicleCount() != 1) {
        return {};
    }
    return rid->basicID()->rawValue().toString().trimmed();
}

QString AirframeResolver::airframeName(int sysid) const
{
    const QVariantMap fe = fleetEntry(sysid);
    const QString callsign = fe.value(QStringLiteral("callsign")).toString().trimmed();
    if (!callsign.isEmpty()) {
        return callsign;
    }
    return fe.value(QStringLiteral("tail")).toString().trimmed();
}

QString AirframeResolver::stableKey(const Vehicle *vehicle) const
{
    if (!vehicle) {
        return {};
    }
    const int sysid = const_cast<Vehicle *>(vehicle)->id();

    const QString uid = _uidBySysid.value(sysid);
    if (!uid.isEmpty()) {
        return QStringLiteral("uid:%1").arg(uid);
    }
    const QString tail = fleetEntry(sysid).value(QStringLiteral("tail")).toString().trimmed();
    if (!tail.isEmpty()) {
        return QStringLiteral("tail:%1").arg(tail.toUpper());
    }
    // Weakest of the three, and the reason this is a last resort: swap a flight
    // controller between airframes and the sysid stays the same while the
    // aircraft does not.
    return QStringLiteral("sysid:%1").arg(sysid);
}

// ------------------------------------------------------------------ resolving

AirframeResolver::Match AirframeResolver::resolve(const Vehicle *vehicle,
                                                  const QJsonArray &registry) const
{
    Match none;
    if (!vehicle) {
        return none;
    }
    const int sysid = const_cast<Vehicle *>(vehicle)->id();

    // 0. Already answered for this airframe.
    const QString key = stableKey(vehicle);
    if (_map.contains(key)) {
        return { _map.value(key), How::Remembered, key };
    }

    const QVariantMap fe   = fleetEntry(sysid);
    const QString tail     = fe.value(QStringLiteral("tail")).toString().trimmed();
    const QString callsign = fe.value(QStringLiteral("callsign")).toString().trimmed();
    const QString ridSerial = _trustworthyRemoteIdSerial();
    const QString uid       = _uidBySysid.value(sysid);

    // Stop at the first match, in the document's order.
    for (const QJsonValue &v : registry) {
        const QJsonObject a = v.toObject();
        const int id = a.value(QStringLiteral("id")).toInt();

        const QString aRid  = a.value(QStringLiteral("rid_serial")).toString().trimmed();
        if (!ridSerial.isEmpty() && !aRid.isEmpty()
            && aRid.compare(ridSerial, Qt::CaseInsensitive) == 0) {
            return { id, How::RemoteIdSerial, ridSerial };
        }
    }
    for (const QJsonValue &v : registry) {
        const QJsonObject a = v.toObject();
        const QString aSerial = a.value(QStringLiteral("serial")).toString().trimmed();
        if (!uid.isEmpty() && !aSerial.isEmpty()
            && aSerial.compare(uid, Qt::CaseInsensitive) == 0) {
            return { a.value(QStringLiteral("id")).toInt(), How::HardwareId, uid };
        }
    }
    for (const QJsonValue &v : registry) {
        const QJsonObject a = v.toObject();
        const QString aReg = a.value(QStringLiteral("registration")).toString().trimmed();
        if (!tail.isEmpty() && !aReg.isEmpty()
            && aReg.compare(tail, Qt::CaseInsensitive) == 0) {
            return { a.value(QStringLiteral("id")).toInt(), How::Tail, tail };
        }
    }
    for (const QJsonValue &v : registry) {
        const QJsonObject a = v.toObject();
        const QString aName = a.value(QStringLiteral("name")).toString().trimmed();
        if (!callsign.isEmpty() && !aName.isEmpty()
            && aName.compare(callsign, Qt::CaseInsensitive) == 0) {
            return { a.value(QStringLiteral("id")).toInt(), How::Name, callsign };
        }
    }

    // Nothing matched. Do NOT invent an aircraft and do not file against a
    // guess - the crew is asked once and the answer is remembered.
    return none;
}

void AirframeResolver::remember(const Vehicle *vehicle, int assetId)
{
    const QString key = stableKey(vehicle);
    if (key.isEmpty() || assetId < 0) {
        return;
    }
    _map.insert(key, assetId);
    _saveMap();
    qCDebug(AirframeResolverLog) << "remembered" << key << "->" << assetId;
}

// --------------------------------------------------------------- persistence

void AirframeResolver::_loadMap()
{
    const QJsonDocument doc = QJsonDocument::fromJson(RotorLabStore::airframeMapJson());
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject o = doc.object();
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
        _map.insert(it.key(), it.value().toInt());
    }
}

void AirframeResolver::_saveMap() const
{
    QJsonObject o;
    for (auto it = _map.constBegin(); it != _map.constEnd(); ++it) {
        o[it.key()] = it.value();
    }
    RotorLabStore::setAirframeMapJson(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

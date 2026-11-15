/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - which aircraft in the registry is this one?
 *
 * A filed flight needs `asset_id`, and getting it wrong puts a sortie on the
 * wrong airframe's hours. So the resolver stops at the first confident match
 * and otherwise asks the crew rather than guessing:
 *
 *   1. Remote ID serial   - but see the warning below
 *   2. autopilot hardware id (AUTOPILOT_VERSION uid2, else the legacy uid)
 *   3. tail number, from the GC's own Fleet screen
 *   4. name, case-insensitive
 *
 * THE REMOTE ID TRAP
 *
 * The document lists the Remote ID serial first, which is right in principle:
 * it is the one identifier tied to the airframe by regulation. But this GC's
 * Remote ID serial is a single GLOBAL application setting that an operator
 * types in once (RemoteID/basicID). It is not read back from the aircraft -
 * nothing here decodes an inbound OPEN_DRONE_ID_BASIC_ID - and it is not per
 * vehicle. With two aircraft connected it would confidently return the same
 * serial for both and file half the sorties against the wrong airframe.
 *
 * So it is used only when it can be trusted: the operator has declared it a
 * CTA-2063 serial (basicIDType == 1), and exactly one vehicle is connected.
 * Otherwise the resolver skips straight to identifier 2. Skipping is safe;
 * guessing is not.
 *
 * Once the crew has answered for an airframe, the answer is remembered against
 * the most stable identifier that airframe offers, so they are never asked
 * twice for the same one.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QObject>
#include <QtCore/QString>

class Vehicle;

class AirframeResolver : public QObject
{
    Q_OBJECT

public:
    explicit AirframeResolver(QObject *parent = nullptr);

    /// How an aircraft was matched, so the UI can be honest about confidence.
    enum class How { None, Remembered, RemoteIdSerial, HardwareId, Tail, Name };

    struct Match {
        int     assetId = -1;
        How     how = How::None;
        QString detail;         ///< what it matched on, for the log
    };

    /// Resolve `vehicle` against `registry` - the raw `aircraft` array from
    /// GET /api/v1/aircraft. `assetId` is -1 when nothing matched confidently.
    Match resolve(const Vehicle *vehicle, const QJsonArray &registry) const;

    /// The crew answered. Remember it so they are not asked again.
    void remember(const Vehicle *vehicle, int assetId);

    /// The name this GC holds for the airframe on its Fleet screen - the
    /// callsign, else the tail number, else empty. This is what gets sent as
    /// `asset_name` when no registry match was confident enough to give an
    /// `asset_id`: the server matches it against the org's registry
    /// case-insensitively, so a flight still lands on the right aircraft, and
    /// an unmatched name still files rather than parking forever.
    QString airframeName(int sysid) const;

    /// A stable key for this airframe, or empty when it offers nothing stable.
    /// The hardware id when the board reports one, else the tail, else the
    /// sysid - which is the weakest, because swapping a flight controller
    /// between airframes silently keeps the mapping.
    QString stableKey(const Vehicle *vehicle) const;

    /// Cache the hardware id decoded from AUTOPILOT_VERSION. Fed by whoever is
    /// already watching MAVLink.
    void noteHardwareId(int sysid, const QString &uid);
    void forgetHardwareId(int sysid);
    QString hardwareId(int sysid) const;

private:
    void _loadMap();
    void _saveMap() const;

    /// The operator-typed Remote ID serial, but only when it can be trusted -
    /// see the header. Empty otherwise.
    static QString _trustworthyRemoteIdSerial();

    QHash<QString, int> _map;       ///< stable key -> asset_id
    QHash<int, QString> _uidBySysid;
};

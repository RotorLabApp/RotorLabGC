/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - who the crew recorded as pilot in command.
 *
 * The station keeps a local, append-only log of every change of pilot the
 * crew makes on the crew screen (time UTC, vehicle, from, to, reason), and at
 * filing turns it into the flight's `pic_periods`. The body's `pilot_id`
 * stays the pilot at takeoff.
 *
 * The crew designates the pilot. GC records what the crew picked; it does not
 * decide who may fly, and nothing here holds up arming, takeoff or a command.
 * A log write that fails is logged and the flight files anyway.
 *
 * Everything that is not the file itself is a pure function, so the unit
 * tests hold it to the server's shapes (RotorLab's API: PicPeriod,
 * SortieList, POST /api/v1/flights).
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>

namespace RotorLabPic {

/// The first RotorLab release that takes `pic_periods`, `crew_rows` and
/// `operating_basis` on the flight write and serves `/api/v1/sorties`. One
/// place to change when the release is named. Older servers get today's
/// `pilot_id` and `crew` string only.
constexpr int kMinMajor = 3;
constexpr int kMinMinor = 5;
constexpr int kMinPatch = 4;

bool serverTakesPic(const QString &serverVersion);

/// The gate the connector uses: the release constant above, or a RotorLab
/// whose GET /api/v1/sorties answered 200 (a server that carries the fields
/// before its version number does, such as a test build).
bool picEnabled(const QString &serverVersion, bool sortiesAnswered);

/// One change of pilot, as the crew made it.
struct Handoff {
    QDateTime at;           ///< UTC, when the crew pressed the button
    int       sysid = -1;   ///< the vehicle it was made for; -1 for none
    int       fromId = -1;
    QString   fromName;
    int       toId = -1;    ///< -1: no pilot named from here (Skip)
    QString   toName;
    QString   reason;       ///< the crew's words, optional
};

QJsonObject toJson(const Handoff &h);
/// False when the line cannot be read (no time).
bool fromJson(const QJsonObject &o, Handoff *out);

/// Who the log says was the pilot at `t` for `sysid`: the last change at or
/// before `t` made for that vehicle or for no vehicle. Returns false when the
/// log has no change before `t`.
bool pilotAt(const QList<Handoff> &log, const QDateTime &t, int sysid,
             int *pilotId, QString *pilotName);

/// The flight's `pilot_id`: the pilot the log names at takeoff, or -1 for
/// none. With `picEnabled` and nobody named at takeoff there is no pilot_id,
/// even when one was named in the air: that pilot files as a period from the
/// change, never for the whole flight. Against an older RotorLab, with
/// nothing in the log before takeoff, it is `currentPilot` as before.
int filingPilot(const QList<Handoff> &log, const QDateTime &takeoff, int sysid,
                int currentPilot, bool picEnabled);

/// The flight's `pic_periods`: the pilot at takeoff from `takeoff`, then one
/// period per change made during the flight, each ending where the next one
/// begins (the last runs to the landing). A change to nobody ends the period
/// before it and starts none. At most 24 (the server's limit); the first is
/// always kept.
QJsonArray buildPeriods(const QList<Handoff> &log, const QDateTime &takeoff,
                        const QDateTime &landing, int sysid);

// --- /api/v1/sorties ---

/// The sortie planned on `assetId` in a SortieList reply: the one that is out
/// first, then the first planned. Fills `info` with id, job_title,
/// operating_basis and window. Returns its seats, each as {id, name, role,
/// duty, dutyLabel, controls, plannedPic, plannedBy}, the planned remote pilot
/// first. Empty when no sortie is planned on that aircraft.
QVariantList seatsFor(const QJsonObject &sortiesReply, int assetId, QVariantMap *info);

/// The crew screen's pilot list: the sortie's seats first, each with its
/// duty label, then every other pilot on the roster. Nobody is filtered or
/// refused; a seat that is not on the roster is still listed.
QVariantList mergeCrew(const QVariantList &seats, const QVariantList &pilots);

/// The planned remote pilot's id among the seats, or -1.
int plannedPic(const QVariantList &seats);

/// `crew_rows` for the flight: every seat but the pilot in command, by member
/// and duty.
QJsonArray crewRows(const QVariantList &seats, int picId);

/// The regime's own term: "Remote pilot
/// in command" for a Part 107 flight (or a US station with no basis named),
/// "Remote pilot" for a GB or EU station, and "Pilot" otherwise, including a
/// US flight on another basis.
QString pilotWord(const QString &operatingBasis, const QString &jurisdiction);

/// The fly view line: "Remote pilot in command: Lee Smith (from 14:02)".
/// Local time, no seconds. Empty when nobody is named.
QString picLine(const QString &name, const QDateTime &from,
                const QString &operatingBasis, const QString &jurisdiction);

} // namespace RotorLabPic

/// The append-only file behind it, one per UTC day under the app data folder,
/// plus the changes made since this GC started, which the filing reads. A
/// write that fails is reported and never stops anything.
class PicLog : public QObject
{
    Q_OBJECT

public:
    explicit PicLog(QObject *parent = nullptr);
    /// Tests point it at a folder of their own.
    PicLog(const QString &dir, QObject *parent);

    /// Record a change. Appended to memory at once and to the file.
    void append(const RotorLabPic::Handoff &h);
    const QList<RotorLabPic::Handoff> &entries() const { return _entries; }

signals:
    void logMessage(const QString &line);

private:
    QString _dir;
    QList<RotorLabPic::Handoff> _entries;   ///< made since this GC started, oldest first
};

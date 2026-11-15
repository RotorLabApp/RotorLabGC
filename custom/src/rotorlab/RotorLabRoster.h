/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - who and what we may file against.
 *
 * Three lists, fetched together and cached to disk: the pilots, the aircraft
 * registry, and the checklist templates. All three are served from cache when
 * the link is down, so the crew screen opens and a pre-flight runs in a field
 * with no signal.
 *
 * A device key authenticates as the DEVICE, not a person. It cannot know who is
 * flying, so the pilot is chosen from this roster and sent explicitly on every
 * flight. Omit it and the customer's logbook fills with flights flown by
 * "GC north-laptop" - invisible until somebody runs a currency report.
 *
 * Currency is reported here and the server says in the payload that it must not
 * be used to prevent a flight. It is shown, and it is never a gate.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>

class RotorLabClient;
struct HttpReply;

class RotorLabRoster : public QObject
{
    Q_OBJECT

public:
    explicit RotorLabRoster(RotorLabClient *client, QObject *parent = nullptr);

    /// Fetch all three. Called on connect and on the 15-minute tick: a pilot
    /// hired on Tuesday must appear without restarting the GC.
    void fetchAll();

    /// Read what is cached, without touching the network.
    void loadCache();

    /// Read the lists from the given JSON bodies, shaped as the server sends
    /// them ({"pilots": [...]}, {"aircraft": [...]}, {"templates": [...]},
    /// {"airworthiness": {...}, "workorders": [...]}). Nothing is written to
    /// the cache. loadCache() goes through here; so do the unit tests.
    void loadFrom(const QByteArray &pilotsJson, const QByteArray &aircraftJson,
                  const QByteArray &templatesJson, const QByteArray &readinessJson);

    QVariantList pilots() const;
    QVariantList aircraft() const;
    /// id / name, for the crew screen's aircraft picker.
    QVariantList assets() const;

    /// Checklist templates of one kind ("preflight" | "postflight"), with
    /// `active: 0` rows dropped.
    QVariantList templates(const QString &kind) const;
    bool hasTemplates(const QString &kind) const;

    /// True once both the pilots and the aircraft lists have been read, each
    /// from the server or from the cache. A pilots reply alone does not make
    /// the roster loaded: the crew screen needs both. Never a gate on flying;
    /// the crew screen tells "no pilots on the roster" from "could not load
    /// it", and stays skippable either way.
    bool loaded() const { return _pilotsIn && _aircraftIn; }

    /// The server's pilots and aircraft replies, as the fetches deliver them.
    /// A reply that is not a 200 changes nothing, so the cache keeps serving.
    /// Public so the unit tests can feed replies in any order without a server.
    void applyPilotsReply(const HttpReply &r);
    void applyAircraftReply(const HttpReply &r);

    /// The raw aircraft array, for the resolver.
    QJsonArray aircraftRaw() const { return _aircraft; }

    /// What RotorLab currently says about one airframe:
    ///
    ///   state      the server's own key, verbatim: "go" | "advisory" |
    ///              "limits" | "unknown" | "no_go", or "" when it said nothing.
    ///              Any other word is passed through as sent, never relabeled.
    ///   readiness  the word RotorLab prints for that state: the server's own
    ///              `readiness` when present, else its published word for the
    ///              state ("No flags", "No flags, N notes", "Limits on record",
    ///              "Condition unknown", "No go"), else the state as sent
    ///   why        the server's own reasons, verbatim, worst first
    ///   note       "RotorLab lists this aircraft as <readiness>: <why>" for
    ///              every state but go and ""; empty for those
    ///   work       OPEN work orders naming this aircraft, worst first (see
    ///              workFrom())
    ///   groundingWork  true when any of that work is grounding work
    ///
    /// Service bulletins are deliberately NOT fetched separately. The server
    /// already fans an applicable bulletin out into a work order against each
    /// affected aircraft, so asking /bulletins as well would list the same
    /// obligation twice.
    ///
    /// SHOWN, NEVER ENFORCED. This mirrors the server's own rule and the
    /// connector's: a no-go airframe still arms, still flies and still files.
    /// Deciding not to launch is the remote PIC's call, made by a human who has
    /// read the reasons, not by a ground station acting on a cached answer that
    /// may be hours old.
    QVariantMap readinessFor(int assetId) const;

    /// True once readiness has been read from anywhere. Distinguishes "this
    /// airframe is fine" from "we have never been told".
    bool readinessLoaded() const { return _readinessLoaded; }

    /// Pure form of readinessFor(), for the unit tests: one aircraft from an
    /// /api/v1/health `airworthiness` map and an /api/v1/workorders list.
    static QVariantMap readinessFrom(const QJsonObject &airworthiness,
                                     const QJsonArray &workorders, int assetId);

    /// RotorLab's printed word for one airworthiness entry (see readinessFor).
    static QString readinessWord(const QJsonObject &entry);

    /// The open work orders against one aircraft, worst first: grounding work
    /// (priority "grounding" or `grounding_hold` set), then urgent, soon,
    /// routine, then by due date. Done and cancelled work is left out. Each:
    ///   text, priority, status, dueBy   the server's fields, verbatim
    ///   grounding                       priority grounding or grounding_hold
    ///   overdue, dueSoon                the server's `overdue` and `due_soon`,
    ///                                   counted from the organization's today
    ///   line                            the sentence the crew screen shows
    static QVariantList workFrom(const QJsonArray &workorders, int assetId);

signals:
    void rosterChanged();
    void readinessChanged();
    void templatesChanged();
    void logMessage(const QString &line);

private:
    void _fetchPilots();
    void _fetchAircraft();
    void _fetchTemplates();
    void _fetchReadiness();
    void _saveReadiness();

    RotorLabClient *_client = nullptr;
    QJsonArray _pilots;
    QJsonArray _aircraft;
    QJsonArray _templates;
    QJsonObject _airworthiness;     ///< asset id (as a string) -> { state, readiness, why[], ... }
    QJsonArray  _workorders;
    bool       _pilotsIn = false;      ///< pilots read from the server or the cache
    bool       _aircraftIn = false;    ///< aircraft read from the server or the cache
    bool       _readinessLoaded = false;
};

/// The crew screen, checklist and missions rules of the RotorLab Connector,
/// kept free of the network and the vehicles so the unit tests can hold them
/// to what the screens say. None of them can hold up a flight: each decides
/// only whether something is shown.
namespace RotorLabCrew {

/// When the crew screen opens.
enum class Open {
    Now,            ///< open it
    AfterDisarm,    ///< the station asked on its own while armed: wait for the disarm
    No,             ///< the connector is off or has no key
};

/// `active` is enabled and configured. `onDemand` is the operator's own
/// Change pilot, which opens at any time, also in flight: it is non-modal and
/// hides while a guided confirm is on screen. The station's own ask (after a
/// verify, or offline with a cached roster) never opens over an armed
/// aircraft and waits for the disarm instead.
Open whenToOpen(bool active, bool anyArmed, bool onDemand);

/// Whether to offer the pre-flight now (A71). Offered once per sortie, before
/// the first arm, when an aircraft is connected and the organization has
/// published a pre-flight. Only an offer: the card never holds arming, hides
/// while the guided confirm is on screen, and is cleared at the arm.
bool offerPreflight(bool active, bool anyArmed, bool vehicleConnected,
                    bool hasTemplates, bool offeredThisSortie,
                    const QString &currentOffer);

/// What the empty missions list says (A72). A Device Key reads the missions
/// published to the whole organization in RotorLab; with a pilot on the crew
/// screen the list is narrowed to the missions that pilot made. Nothing is
/// assigned to anyone, so the text never says "assigned".
QString missionsEmptyText(bool loaded, const QString &pilotName);

/// Whether the page's error goes when every aircraft has disarmed: only when
/// it is still the "not saved while an aircraft is armed" refusal. Any later
/// error (a rejected key, a blocked account) stays.
bool clearErrorAtDisarm(const QString &lastError, const QString &armedRefusal);

/// Whether a Verify (online, or offline with a cached roster) asks who is
/// flying. Never when a pilot and an aircraft are already set this session;
/// always after a new key is entered, as on the first check.
bool askAfterVerify(bool askSetting, bool pilotSet, bool aircraftSet, bool newKey);

/// Whether a successful filing answer brings the connector back to Connected:
/// it was Offline and has been verified before (so the server version is
/// known). Otherwise the state stays as it is.
bool reconnectOnFiled(bool offline, bool verifiedBefore);

} // namespace RotorLabCrew

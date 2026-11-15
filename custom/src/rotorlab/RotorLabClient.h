/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - the calls, over the shared transport.
 *
 * rotorlab.app serves a real certificate from a public authority, so this
 * client validates normally against the system trust store and pins nothing.
 * There is no fingerprint in any response, no PIN_MISMATCH state and no re-pin
 * flow. Both clients use ordinary system certificate validation.
 *
 * Because it is constructed on TlsPolicy::PublicCa, it has no
 * pinned-fingerprint override.
 *
 * This class is endpoint paths and nothing else. Every method below is one line
 * over HttpClient.
 *
 ****************************************************************************/

#pragma once

#include "net/HttpClient.h"

class RotorLabClient : public HttpClient
{
    Q_OBJECT

public:
    explicit RotorLabClient(QObject *parent = nullptr);
    ~RotorLabClient() override;

    // --- status: unmetered, and the only correct way to check a key ---
    void getVersion(Handler done);
    void getUsage(Handler done);

    // --- what we may file against ---
    void getPilots(Handler done);
    void getAircraft(Handler done);
    void getChecklists(Handler done);

    // --- filing ---
    void postFlight(const QJsonObject &body, Handler done);
    void postChecklist(const QJsonObject &body, Handler done);

    /// The log file, raw bytes, after the flight has a numeric id.
    QNetworkReply *postFlightLog(int flightId, const QString &filePath,
                                 const QString &displayName,
                                 Handler done, Progress progress = {});

    // --- missions: READS only, on the same `flights` scope ---

    /// Missions planned in RotorLab. `pilotId` >= 0 narrows to one pilot's
    /// assignments; -1 asks for everything the org can see.
    void getMissions(int pilotId, Handler done);

    /// A native QGC `.plan` for one mission. The answer is a FILE, so read
    /// HttpReply::raw and write those bytes untouched - re-serialising a plan
    /// through QJsonObject is how a field QGC does not model gets dropped.
    void getMissionPlan(int missionId, Handler done);

    // --- the day's sorties and the crew each plans (RotorLab 3.5.4 and
    //     later; the connector gates the call on the server version) ---

    /// `day` YYYY-MM-DD, empty for the organization's today. `assetId` >= 0
    /// narrows to the sorties planned on that aircraft.
    void getSorties(const QString &day, int assetId, Handler done);

    // --- airframe readiness: advisory, never a gate ---

    /// `airworthiness`, keyed by asset id: { state, why[] }.
    void getHealth(Handler done);
    /// Open work per aircraft. NOTE there is deliberately no getBulletins():
    /// the server already fans an applicable service bulletin out into a work
    /// order against each affected aircraft, so asking for bulletins as well
    /// lists the same obligation twice. See RotorLabRoster::readinessFor().
    void getWorkorders(Handler done);
};

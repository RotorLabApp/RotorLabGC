/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - persistence.
 *
 * Everything the connector remembers between runs EXCEPT two things:
 *
 *   the device key   lives in the OS credential store - see Credential.h
 *   the spool        lives on disk as one file per pending item - see the
 *                    spool, which must survive a Disconnect and must never be
 *                    thrown away by a settings reset
 *
 * Everything else is ordinary configuration and cache under the QSettings
 * group "RotorLabConnector".
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDate>
#include <QtCore/QString>
#include <QtCore/QStringList>

class RotorLabStore
{
public:
    // --- connection ---
    /// Off on a fresh install. The connector does nothing at all until an
    /// operator turns it on.
    static bool    enabled();
    static void    setEnabled(bool on);

    /// The one address this ground station talks to: `https://rotorlab.app`.
    /// Deliberately not settable and not stored - there is no server field, no
    /// settings key and no way for a stale or hand-edited value to send a
    /// customer's device key somewhere else.
    static QString baseUrl();

    /// The account name the device key is filed under in the credential store.
    /// Constant, but named here so there is exactly one spelling of it.
    static QString keyAccount();

    /// `usage.key.label` - "GC north-laptop". Display only, and the single most
    /// useful thing on a support call when a customer has four ground stations.
    static QString keyLabel();
    static void    setKeyLabel(const QString &label);

    // --- caches, so the crew screen and a pre-flight work with the link down ---
    static QByteArray pilotsJson();
    static void       setPilotsJson(const QByteArray &json);
    static QByteArray aircraftJson();
    static void       setAircraftJson(const QByteArray &json);
    static QByteArray templatesJson();
    static void       setTemplatesJson(const QByteArray &json);

    /// Airworthiness / open work / bulletins, cached like the roster so a
    /// station with no signal still shows what it last knew. Advisory only.
    static QByteArray readinessJson();
    static void       setReadinessJson(const QByteArray &json);

    /// autopilot identifier -> RotorLab asset_id, so a crew is never asked
    /// twice about the same airframe.
    static QByteArray airframeMapJson();
    static void       setAirframeMapJson(const QByteArray &json);

    // --- behaviour, three switches and no more ---
    static bool fileFlightsAutomatically();      ///< default true
    static void setFileFlightsAutomatically(bool on);
    static bool uploadLogs();                    ///< default true
    static void setUploadLogs(bool on);
    static bool askWhoIsFlying();                ///< default true
    static void setAskWhoIsFlying(bool on);

    // --- compliance defaults ---
    //
    // Sent on every filed flight, and OMITTED when unset. Nothing validates
    // these server-side: a typo is stored verbatim and reads as blank in the
    // app, so an empty value is honest and a guess is worse than nothing.
    //
    // `over_people` and `dangerous_goods` deliberately have no home here.
    // Nobody is asked them per sortie, so there is no answer to store, and
    // sending "no" because a default said so is a claim on somebody's
    // compliance record that the crew never made.
    static QString jurisdiction();               ///< ISO 3166-1 alpha-2, e.g. "GB"
    static void    setJurisdiction(const QString &code);
    static QString operatingCategory();          ///< per jurisdiction, e.g. "open_a2"
    static void    setOperatingCategory(const QString &code);
    static QString visualMode();                 ///< "vlos" | "evlos" | "bvlos"
    static void    setVisualMode(const QString &mode);

    // --- run context, per sortie ---
    // The pilot is deliberately NOT here. It must never be carried across a
    // restart or a dismissal, or a flight gets somebody else's name on it.
    static QString lastSite();
    static void    setLastSite(const QString &site);
    static QString lastFlightType();
    static void    setLastFlightType(const QString &type);
    static QString lastFlightTypeOther();
    static void    setLastFlightTypeOther(const QString &text);

    // --- notices about filed flights, kept until the operator dismisses them ---
    static QStringList filingPilotNotices();
    static void        setFilingPilotNotices(const QStringList &notices);
    static QStringList filingOtherNotices();
    static void        setFilingOtherNotices(const QStringList &notices);
    static QString lastCrew();
    static void    setLastCrew(const QString &crew);

    /// An opaque id for this install: 8 random hex characters, made once and
    /// kept in settings (also across a Disconnect). It goes into every
    /// external_ref in place of anything that names the machine or the user.
    static QString installId();

    /// "gc-<installid>-<yyyy-MM-dd>-<uuid12>". Unique per record and opaque:
    /// no host name, no user name, no counter.
    static QString formatExternalRef(const QString &installId, const QDate &dayUtc,
                                     const QString &uuid12);

    /// True when a device key is stored - i.e. the connector is configured.
    static bool configured();

    /// Forget the key and everything cached. Does NOT touch the spool, and
    /// does not revoke anything in RotorLab - an organization admin does that
    /// under Organization, Integrations, Devices.
    static void clearAll();
};

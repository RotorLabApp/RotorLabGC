/****************************************************************************
 *
 * RotorLab GC - the RotorLab Connector.
 *
 ****************************************************************************/

#include "RotorLabConnector.h"
#include "AirframeResolver.h"
#include "Credential.h"
#include "FilingRules.h"
#include "FlightSpool.h"
#include "MissionLibrary.h"
#include "PicLog.h"
#include "RotorLabClient.h"
#include "RotorLabRoster.h"
#include "RotorLabStore.h"
#include "fleet/FleetConnector.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDate>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QUuid>

#include "MultiVehicleManager.h"
#include "net/LogCollector.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

#include "QGCLoggingCategory.h"

QGC_LOGGING_CATEGORY(RotorLabConnectorLog, "gcs.custom.rotorlab.connector")

namespace {

/// Roster, aircraft and templates together. A pilot hired on Tuesday must
/// appear without the customer restarting the GC.
constexpr int kRefreshMs = 15 * 60 * 1000;

/// `rl_` followed by 43 characters, and always like this.
constexpr int kKeyBodyLength = 43;

/// The server's cap on a free-text airframe name. Truncating here keeps a long
/// callsign from turning the whole flight into a 400.
constexpr int kAssetNameMax = 80;

/// The operating-category vocabulary of each framework. These are codes, not
/// labels - they are stored verbatim and nothing checks them, so a typo reads
/// as blank everywhere in the app.
struct Framework { const char *code; const char *categories; };
const Framework kFrameworks[] = {
    { "US", "part107 part107_waiver rec public" },
    { "GB", "open_a1 open_a2 open_a3 specific certified" },
    { "EU", "open_a1 open_a2 open_a3 sts01 sts02 specific luc certified" },
    { "CA", "basic advanced sfoc micro" },
    { "AU", "excluded reoc recreational" },
    { "NZ", "part101 part102" },
    { "JP", "cat1 cat2 cat3" },
    { "ZA", "roc private" },
    { "IN", "nano micro small medium large" },
    { "BR", "class1 class2 class3" },
    { "SG", "op ap recreational" },
};

} // namespace

Q_APPLICATION_STATIC(RotorLabConnector, _rotorLabConnectorInstance);

RotorLabConnector::RotorLabConnector(QObject *parent)
    : QObject(parent)
    , _client(new RotorLabClient(this))
    , _roster(new RotorLabRoster(_client, this))
    , _airframes(new AirframeResolver(this))
    , _spool(new FlightSpool(_client, this))
    , _missions(new MissionLibrary(_client, this))
    , _flights(new FlightWatcher(this))
{
    _refreshTimer.setInterval(kRefreshMs);
    connect(&_refreshTimer, &QTimer::timeout, this, &RotorLabConnector::_refreshTick);
    connect(_roster, &RotorLabRoster::rosterChanged,    this, &RotorLabConnector::rosterChanged);
    connect(_roster, &RotorLabRoster::templatesChanged, this, &RotorLabConnector::rosterChanged);
    connect(_roster, &RotorLabRoster::logMessage,       this, &RotorLabConnector::_log);
    connect(_spool,  &FlightSpool::changed,             this, &RotorLabConnector::queueChanged);
    connect(_spool,  &FlightSpool::logMessage,          this, &RotorLabConnector::_log);
    connect(_flights, &FlightWatcher::flightComplete,   this, &RotorLabConnector::_onFlightComplete);
    connect(_flights, &FlightWatcher::armedChanged,     this, &RotorLabConnector::_onArmedChanged);
    connect(_spool,  &FlightSpool::workRaised,          this, &RotorLabConnector::_onWorkRaised);
    connect(_spool,  &FlightSpool::filingNotice,        this, &RotorLabConnector::_onFilingNotice);
    connect(_spool,  &FlightSpool::serverAnswered,      this, &RotorLabConnector::_onServerAnswered);
    connect(_missions, &MissionLibrary::changed,        this, &RotorLabConnector::missionsChanged);
    connect(_missions, &MissionLibrary::logMessage,     this, &RotorLabConnector::_log);
    _picLog = new PicLog(this);
    connect(_picLog,  &PicLog::logMessage,              this, &RotorLabConnector::_log);
    connect(_roster,  &RotorLabRoster::rosterChanged,   this, &RotorLabConnector::sortieChanged);
    connect(_missions, &MissionLibrary::planReady,      this, &RotorLabConnector::missionPlanReady);
    connect(_missions, &MissionLibrary::failed,         this, &RotorLabConnector::_setStatus);
    connect(_roster,  &RotorLabRoster::readinessChanged, this, &RotorLabConnector::rosterChanged);
    _maxLogBytes = RotorLabFiling::kMaxLogBytes;
    if (MultiVehicleManager *const mvm = MultiVehicleManager::instance()) {
        // A log waiting for its aircraft is collected once that aircraft is active.
        connect(mvm, &MultiVehicleManager::activeVehicleChanged,
                this, &RotorLabConnector::_onActiveVehicleChanged);
    }
    // ...and once every aircraft has disarmed, if one was flying at the switch.
    connect(_flights, &FlightWatcher::armedChanged, this, [this](bool anyArmed) {
        if (!anyArmed) {
            _onActiveVehicleChanged();
        }
    });

    // The post-flight is offered once the aircraft has been disarmed and has
    // stayed disarmed - not the instant a motor stops.
    _postflightTimer.setSingleShot(true);
    _postflightTimer.setInterval(30 * 1000);
    connect(&_postflightTimer, &QTimer::timeout, this, &RotorLabConnector::_offerPostflight);

    // The pre-flight is offered before the first arm: when an aircraft
    // connects (after a moment, so a vehicle that is already armed reports so
    // first), and when the templates arrive.
    connect(_roster, &RotorLabRoster::templatesChanged, this, &RotorLabConnector::_maybeOfferPreflight);
    if (MultiVehicleManager *const mvm = MultiVehicleManager::instance()) {
        connect(mvm, &MultiVehicleManager::vehicleAdded, this, [this](Vehicle *) {
            QTimer::singleShot(3000, this, &RotorLabConnector::_maybeOfferPreflight);
        });
    }

    _assetId = -1;
    // Notices about flights already filed survive a restart until dismissed.
    _filingPilotNotices = RotorLabStore::filingPilotNotices();
    _filingOtherNotices = RotorLabStore::filingOtherNotices();
    _publishFilingNotices(/*save*/ false);
    _applyCredentials();

    // Apply the hand-over at STARTUP, not only when the toggle changes.
    //
    // Without this a restart leaves the Fleet Connector believing it is still
    // this station's route while the RotorLab Connector is enabled, and the two
    // settings rows then hide each other: the Fleet row because RotorLab is on,
    // the RotorLab row because Fleet still reads as active. The operator is left
    // with neither, and no way back to either page.
    if (FleetConnector *fleet = FleetConnector::instance()) {
        fleet->setSuspendedByRotorLab(enabled());
    }

    if (enabled() && configured()) {
        // Do not assume rotorlab.app is reachable; the verify decides between
        // CONNECTED and OFFLINE.
        verifyNow();
    } else {
        _setState(NotConfigured);
    }
    // An aircraft may already be connected, with templates from the cache.
    QTimer::singleShot(3000, this, &RotorLabConnector::_maybeOfferPreflight);
}

RotorLabConnector::~RotorLabConnector() = default;

RotorLabConnector *RotorLabConnector::instance()
{
    return _rotorLabConnectorInstance();
}

// ------------------------------------------------------------------- identity

bool RotorLabConnector::enabled() const    { return RotorLabStore::enabled(); }
bool RotorLabConnector::configured() const { return RotorLabStore::configured(); }
QString RotorLabConnector::baseUrl() const { return RotorLabStore::baseUrl(); }
QString RotorLabConnector::keyLabel() const { return RotorLabStore::keyLabel(); }

QString RotorLabConnector::keyStoreName() const
{
    return Credential::storeName();
}

QString RotorLabConnector::maskedKey() const
{
    const QString key = Credential::load(RotorLabStore::keyAccount());
    if (key.isEmpty()) {
        return {};
    }
    // Enough to tell two keys apart on a support call, not enough to use.
    return QStringLiteral("rl_%1…%2")
        .arg(key.mid(3, 4), key.right(4));
}

void RotorLabConnector::setEnabled(bool on)
{
    if (on == RotorLabStore::enabled()) {
        return;
    }
    RotorLabStore::setEnabled(on);
    emit enabledChanged();

    // Exactly one connector files this station's flights. Stand the other one
    // down - or bring it back - before doing anything else, so there is never
    // a moment where both are reporting.
    if (FleetConnector *fleet = FleetConnector::instance()) {
        fleet->setSuspendedByRotorLab(on);
    }

    if (on) {
        _log(QStringLiteral("enabled"));
        if (configured()) {
            verifyNow();
        } else {
            _setState(NotConfigured);
        }
    } else {
        _log(QStringLiteral("disabled"));
        _refreshTimer.stop();
        _client->reset();
        _setState(NotConfigured);
    }
    emit stateChanged();
}

bool RotorLabConnector::looksLikeDeviceKey(const QString &typed)
{
    const QString t = typed.trimmed();
    return t.startsWith(QStringLiteral("rl_")) && t.length() == kKeyBodyLength + 3;
}

// ------------------------------------------------------------------ behaviour

bool RotorLabConnector::fileFlightsAutomatically() const { return RotorLabStore::fileFlightsAutomatically(); }
bool RotorLabConnector::uploadLogs() const              { return RotorLabStore::uploadLogs(); }
bool RotorLabConnector::askWhoIsFlying() const          { return RotorLabStore::askWhoIsFlying(); }

void RotorLabConnector::setFileFlightsAutomatically(bool on)
{
    if (on != RotorLabStore::fileFlightsAutomatically()) {
        RotorLabStore::setFileFlightsAutomatically(on);
        emit settingsChanged();
    }
}

void RotorLabConnector::setUploadLogs(bool on)
{
    if (on != RotorLabStore::uploadLogs()) {
        RotorLabStore::setUploadLogs(on);
        emit settingsChanged();
    }
}

void RotorLabConnector::setAskWhoIsFlying(bool on)
{
    if (on != RotorLabStore::askWhoIsFlying()) {
        RotorLabStore::setAskWhoIsFlying(on);
        emit settingsChanged();
    }
}

// ----------------------------------------------------------------- compliance

QString RotorLabConnector::jurisdiction() const      { return RotorLabStore::jurisdiction(); }
QString RotorLabConnector::operatingCategory() const { return RotorLabStore::operatingCategory(); }
QString RotorLabConnector::visualMode() const        { return RotorLabStore::visualMode(); }

void RotorLabConnector::setJurisdiction(const QString &code)
{
    if (code == RotorLabStore::jurisdiction()) {
        return;
    }
    RotorLabStore::setJurisdiction(code);
    // The category vocabulary is per framework, so a category chosen under the
    // old one is now meaningless. Clearing it is the honest move: an empty
    // field is "not stated", and a stale code reads as blank in the app while
    // looking like somebody answered.
    if (!operatingCategories().contains(RotorLabStore::operatingCategory())) {
        RotorLabStore::setOperatingCategory({});
    }
    emit settingsChanged();
}

void RotorLabConnector::setOperatingCategory(const QString &code)
{
    if (code != RotorLabStore::operatingCategory()) {
        RotorLabStore::setOperatingCategory(code);
        emit settingsChanged();
    }
}

void RotorLabConnector::setVisualMode(const QString &mode)
{
    if (mode != RotorLabStore::visualMode()) {
        RotorLabStore::setVisualMode(mode);
        emit settingsChanged();
    }
}

QStringList RotorLabConnector::jurisdictions() const
{
    QStringList out;
    for (const Framework &f : kFrameworks) {
        out << QLatin1String(f.code);
    }
    return out;
}

QStringList RotorLabConnector::operatingCategories() const
{
    const QString j = RotorLabStore::jurisdiction();
    for (const Framework &f : kFrameworks) {
        if (j == QLatin1String(f.code)) {
            return QString::fromLatin1(f.categories).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        }
    }
    return {};
}

QStringList RotorLabConnector::visualModes() const
{
    return { QStringLiteral("vlos"), QStringLiteral("evlos"), QStringLiteral("bvlos") };
}

QString RotorLabConnector::filingAsText() const
{
    QStringList parts;
    if (!jurisdiction().isEmpty())      parts << jurisdiction();
    if (!operatingCategory().isEmpty()) parts << operatingCategory();
    if (!visualMode().isEmpty())        parts << visualMode().toUpper();
    return parts.join(QStringLiteral(" / "));
}

QJsonObject RotorLabConnector::complianceFields() const
{
    QJsonObject o;
    // Only what was actually chosen. Nothing validates these server-side, so
    // an omitted field is honest and a guess is worse than nothing on a record
    // somebody may have to stand behind.
    if (!jurisdiction().isEmpty())      o[QStringLiteral("jurisdiction")]       = jurisdiction();
    if (!operatingCategory().isEmpty()) o[QStringLiteral("operating_category")] = operatingCategory();
    if (!visualMode().isEmpty())        o[QStringLiteral("visual_mode")]        = visualMode();
    // over_people and dangerous_goods are deliberately never sent. Nobody is
    // asked them per sortie, and "" means "not stated", which is not the same
    // as "no".
    return o;
}

// ---------------------------------------------------------------------- state

QString RotorLabConnector::stateText() const
{
    switch (_state) {
    case NotConfigured:  return tr("Not configured");
    case Verifying:      return tr("Checking the key…");
    case Connected:      return tr("Connected");
    case Offline:        return tr("Offline, still filing to the queue");
    case KeyRejected:    return tr("This key was rejected");
    case AccountBlocked: return tr("The account needs attention");
    case Unavailable:    return tr("RotorLab unavailable, still filing to the queue");
    }
    return {};
}

void RotorLabConnector::_setState(State s)
{
    if (_state == s) {
        return;
    }
    _state = s;

    // The roster refresh runs only while connected. KEY_REJECTED stops and
    // stays stopped until a human acts: retrying a rejected key fills logs,
    // and if it was revoked because it leaked, retrying it is noise on
    // somebody's security review.
    if (s == Connected) {
        if (!_refreshTimer.isActive()) {
            _refreshTimer.start();
        }
    } else {
        _refreshTimer.stop();
    }

    _log(QStringLiteral("state -> %1").arg(stateText()));
    emit stateChanged();
}

void RotorLabConnector::_setStatus(const QString &text)
{
    if (_statusText != text) {
        _statusText = text;
        emit statusChanged();
    }
}

void RotorLabConnector::_setError(const QString &text)
{
    if (_lastError != text) {
        _lastError = text;
        emit statusChanged();
    }
}

void RotorLabConnector::_setBusy(bool on)
{
    if (_busy != on) {
        _busy = on;
        emit statusChanged();
    }
}

void RotorLabConnector::_log(const QString &line)
{
    qCDebug(RotorLabConnectorLog) << line;
    emit logMessage(line);
}

// ------------------------------------------------------------------- requests

void RotorLabConnector::_applyCredentials()
{
    // A development build may point at a local RotorLab over plain http; a
    // stable build cannot, because baseUrl() is a constant there. The scheme
    // is upgraded either way where it can be, because this is a bearer
    // credential on the wire.
    _client->setBaseUrl(HttpClient::normalizeBaseUrl(RotorLabStore::baseUrl(),
                                                     /*allowLocalhostHttp*/ true));
    _client->setToken(Credential::load(RotorLabStore::keyAccount()));
}

void RotorLabConnector::setKeyAndVerify(const QString &key)
{
    const QString typed = key.trimmed();
    if (!looksLikeDeviceKey(typed)) {
        _setError(tr("That does not look like a Device Key. Device Keys begin with "
                     "rl_ and are created by an organization admin in RotorLab under "
                     "Organization, Integrations, Devices."));
        return;
    }

    // The key store is never touched while an aircraft is armed (it can raise
    // an unlock prompt). Say why, rather than blaming the store.
    if (_flights && _flights->anyArmed()) {
        _armedKeyRefusal = tr("The key is not saved while an aircraft is armed. Enter it again "
                              "after landing; the current key stays in use.");
        _setError(_armedKeyRefusal);
        return;
    }
    _armedKeyRefusal.clear();

    if (!Credential::store(RotorLabStore::keyAccount(), typed)) {
        // Fail closed. If the credential store refused it we do not quietly
        // write the key somewhere less safe instead.
        _setError(tr("The key could not be saved to the %1.").arg(keyStoreName()));
        return;
    }

    _setError({});
    _applyCredentials();
    emit identityChanged();
    _newKey = true;
    verifyNow();
}

void RotorLabConnector::verifyNow()
{
    if (!configured()) {
        _setState(NotConfigured);
        return;
    }
    _applyCredentials();
    _setBusy(true);
    _setState(Verifying);

    _client->getVersion([this](const HttpReply &r) {
        _setBusy(false);
        if (!_handleCommon(r)) {
            // Unreachable, and there is a roster on disk from the last time
            // there was signal. Ask anyway. A crew that starts the station in
            // a dead spot is exactly the crew whose whole day would otherwise
            // file with no pilot on it - and nobody finds out until a currency
            // report months later. The cached roster exists for this.
            if ((_state == Offline || _state == Unavailable) && !_roster->pilots().isEmpty()
                && RotorLabCrew::askAfterVerify(RotorLabStore::askWhoIsFlying(), _pilotId >= 0,
                                                _assetId >= 0, _newKey)) {
                _askWhoIsFlying();
            }
            _newKey = false;
            return;
        }
        const QString version = r.body.value(QStringLiteral("version")).toString();
        _serverVersion = version;
        _spool->setServerVersion(version);
        _setError({});
        _setStatus({});
        _setState(Connected);
        // A key that verifies resumes filing. A 401 on a filing stops the
        // spool; without this a new or restored key would leave everything
        // waiting for Retry now.
        _spool->setFilingAllowed(true);
        _log(QStringLiteral("verified against RotorLab %1").arg(version));
        _fetchUsage();
        _roster->fetchAll();
        _fetchMissions();
        _probeSorties();

        // Ask who is flying. After a successful verify, and again on every GC
        // start while the connector is enabled - never mid-flight (a GC
        // restart or a Verify press while armed asks at the disarm), and never
        // on the path between the app opening and the aircraft controls.
        // Not again when this session already has a pilot and an aircraft:
        // a Verify or a reconnect is not a new crew.
        if (RotorLabCrew::askAfterVerify(RotorLabStore::askWhoIsFlying(), _pilotId >= 0,
                                         _assetId >= 0, _newKey)) {
            _askWhoIsFlying();
        }
        _newKey = false;
    });
}

void RotorLabConnector::_fetchUsage()
{
    _client->getUsage([this](const HttpReply &r) {
        if (r.status != 200) {
            return;             // the label is a nicety, never a failure
        }
        const QJsonObject key = r.body.value(QStringLiteral("key")).toObject();
        const QString label = key.value(QStringLiteral("label")).toString();
        if (!label.isEmpty()) {
            RotorLabStore::setKeyLabel(label);
            emit identityChanged();
        }
        // RotorLab does not publish its log limit on /api/v1 today; when a
        // release adds max_file_bytes here it is used, else 512 MB.
        _maxLogBytes = RotorLabFiling::maxLogBytesFrom(r.body);
        // The zeroes in this response are NOT a problem and are never shown as
        // one: a device key has no engine quota, which is exactly right for a
        // credential that lives unattended on a laptop.
    });
}

void RotorLabConnector::_refreshTick()
{
    // All three together. A pilot hired on Tuesday must appear without the
    // customer restarting the GC.
    if (_state == Connected) {
        _roster->fetchAll();
    }
}

// ---------------------------------------------------------- roster + context

QVariantList RotorLabConnector::pilots() const       { return _roster->pilots(); }
QVariantList RotorLabConnector::assets() const       { return _roster->assets(); }
bool         RotorLabConnector::rosterLoaded() const { return _roster->loaded(); }

QString RotorLabConnector::rosterSourceText() const
{
    return tr("RotorLab roster");
}

QStringList RotorLabConnector::flightTypes() const
{
    // Exactly the server's published list: the `flight_type` enum on
    // POST /api/v1/flights in RotorLab's API. Anything else is refused
    // with a 400, so the picker offers nothing else; "other" carries its own
    // text in `flight_type_other`.
    return RotorLabFiling::flightTypeCodes();
}

QStringList RotorLabConnector::flightTypeLabels() const
{
    // What the picker shows; the codes above are what is sent.
    QStringList labels;
    for (const QString &code : RotorLabFiling::flightTypeCodes()) {
        labels << RotorLabFiling::flightTypeLabel(code);
    }
    return labels;
}

QStringList RotorLabConnector::promptFields() const
{
    // RotorLab has no prompt-field endpoint, so this list is fixed here.
    return { QStringLiteral("pilot_id"), QStringLiteral("asset_id"),
             QStringLiteral("site"), QStringLiteral("flight_type"),
             QStringLiteral("flight_type_other"), QStringLiteral("crew") };
    // battery_serials (Packs) is left out until a RotorLab release takes
    // packs on the flight write (A55, phase 3).
}

QString RotorLabConnector::site() const       { return RotorLabStore::lastSite(); }

// Mapped on read, so a word remembered by an earlier build (mapping, patrol,
// search, delivery) becomes a published type before anyone sees or sends it.
QString RotorLabConnector::flightType() const
{
    return RotorLabFiling::normalizeFlightType(RotorLabStore::lastFlightType(),
                                               RotorLabStore::lastFlightTypeOther(),
                                               /*forSending*/ false).type;
}

QString RotorLabConnector::flightTypeOther() const
{
    return RotorLabFiling::normalizeFlightType(RotorLabStore::lastFlightType(),
                                               RotorLabStore::lastFlightTypeOther(),
                                               /*forSending*/ false).other;
}

void RotorLabConnector::setFlightTypeOther(const QString &text)
{
    RotorLabStore::setLastFlightTypeOther(RotorLabFiling::cleanFlightTypeOther(text));
    emit runContextChanged();
}

void RotorLabConnector::setPackSerials(const QString &text)
{
    _packSerials = RotorLabFiling::parsePackSerials(text);
    if (!_packSerials.isEmpty()) {
        _log(QStringLiteral("packs on this sortie: %1").arg(_packSerials.join(QStringLiteral(", "))));
    }
    emit runContextChanged();
}

QString RotorLabConnector::crew() const       { return RotorLabStore::lastCrew(); }

void RotorLabConnector::setCrew(int pilotId, const QString &pilotName, int assetId,
                                const QString &site, const QString &flightType,
                                const QString &crew)
{
    // A change of pilot goes to the handoff log with its time, so a change in
    // the air files as periods (pic_periods) and the flight still names the
    // pilot at takeoff.
    _recordHandoff(_pilotId, _pilotName, pilotId, pilotName);
    _pilotId   = pilotId;
    _pilotName = pilotName;
    _assetId   = assetId;

    // The sortie's crew, when the screen showed one for this aircraft: the
    // rest of the seats file as crew_rows, and its operating basis with them.
    if (assetId >= 0 && assetId == _sortieAssetId) {
        _crewSeats = _sortieSeats;
        _crewOperatingBasis = _sortieInfo.value(QStringLiteral("operatingBasis")).toString();
    } else {
        _crewSeats.clear();
        _crewOperatingBasis.clear();
    }

    // Site, flight type and crew are conveniences and are remembered. The
    // pilot is a claim about who was in command of an aircraft, so it is held
    // in memory for this run only; the handoff log is a record of what the
    // crew did and is never read back into the run context.
    RotorLabStore::setLastSite(site);
    RotorLabStore::setLastFlightType(flightType);
    RotorLabStore::setLastCrew(crew);

    _crewScreenPending = false;
    _crewScreenOnDemand = false;
    _crewScreenDeferred = false;
    _log(pilotId >= 0 ? QStringLiteral("pilot on duty: %1").arg(pilotName)
                      : QStringLiteral("crew screen answered with no pilot"));
    emit runContextChanged();
    // The missions list follows the pilot, so it says whose it is.
    if (_state == Connected) {
        _fetchMissions();
    }
}

void RotorLabConnector::dismissCrewScreen()
{
    // Skip never clears a pilot already named: with one set it only closes
    // the screen, like Cancel. A Verify or a reconnect must not be a way to
    // lose the station's pilot.
    if (_pilotId >= 0) {
        _handoffReason.clear();
        closeCrewScreen();
        return;
    }
    // Deliberately does NOT restore a previous pilot. An unset pilot is a gap
    // somebody who was there can fill in later; a wrong one is a false record.
    _recordHandoff(_pilotId, _pilotName, -1, QString());
    _pilotId = -1;
    _pilotName.clear();
    _packSerials.clear();
    _crewScreenPending = false;
    _crewScreenOnDemand = false;
    _crewScreenDeferred = false;
    emit runContextChanged();
    if (_state == Connected) {
        _fetchMissions();
    }
}

void RotorLabConnector::openCrewScreen()
{
    // Change pilot. The operator asked, so it opens now, armed or not: a
    // pilot swap mid-sortie must not wait for a landing or a restart (A79).
    if (RotorLabCrew::whenToOpen(enabled() && configured(), _flights->anyArmed(),
                                 /*onDemand*/ true) != RotorLabCrew::Open::Now) {
        return;
    }
    _crewScreenDeferred = false;
    _crewScreenOnDemand = true;
    _crewScreenPending = true;
    emit runContextChanged();
}

void RotorLabConnector::closeCrewScreen()
{
    if (!_crewScreenPending) {
        return;
    }
    // No answer is not Skip: the pilot already named stays named.
    _crewScreenPending = false;
    _crewScreenOnDemand = false;
    emit runContextChanged();
}

void RotorLabConnector::_askWhoIsFlying()
{
    switch (RotorLabCrew::whenToOpen(enabled() && configured(), _flights->anyArmed(),
                                     /*onDemand*/ false)) {
    case RotorLabCrew::Open::Now:
        _crewScreenDeferred = false;
        if (!_crewScreenPending) {
            _crewScreenOnDemand = false;
            _crewScreenPending = true;
            emit runContextChanged();
        }
        break;
    case RotorLabCrew::Open::AfterDisarm:
        // Never pops up over a crew that is flying (A78). Asked at the disarm.
        if (!_crewScreenDeferred) {
            _crewScreenDeferred = true;
            _log(QStringLiteral("aircraft armed: asking who is flying after the disarm"));
        }
        break;
    case RotorLabCrew::Open::No:
        break;
    }
}

void RotorLabConnector::refreshRoster()
{
    _roster->fetchAll();
}

// ------------------------------------------------------------ pilot in command

bool RotorLabConnector::picSupported() const
{
    return RotorLabPic::picEnabled(_serverVersion, _sortiesAnswered);
}

void RotorLabConnector::_probeSorties()
{
    // Whether this RotorLab carries the pilot in command fields: its sorties
    // route answers. The organization's today, any aircraft. Until it answers
    // (or against an older RotorLab) the flight files with today's single
    // pilot_id and crew string.
    _client->getSorties(QString(), -1, [this](const HttpReply &r) {
        const bool answered = r.netOk && r.status == 200;
        if (answered != _sortiesAnswered) {
            _sortiesAnswered = answered;
            _log(answered ? QStringLiteral("RotorLab takes pilot in command periods")
                          : QStringLiteral("RotorLab does not serve sorties: one pilot per flight"));
            emit stateChanged();
            emit runContextChanged();
        }
    });
}

QVariantList RotorLabConnector::crewPilots() const
{
    return RotorLabPic::mergeCrew(_sortieSeats, _roster->pilots());
}

QString RotorLabConnector::sortieText() const
{
    QStringList parts;
    const QString title  = _sortieInfo.value(QStringLiteral("jobTitle")).toString();
    const QString window = _sortieInfo.value(QStringLiteral("window")).toString();
    if (!title.isEmpty())  parts << title;
    if (!window.isEmpty()) parts << window;
    return parts.join(QStringLiteral(", "));
}

int RotorLabConnector::plannedPicId() const
{
    return RotorLabPic::plannedPic(_sortieSeats);
}

QString RotorLabConnector::pilotWord() const
{
    return RotorLabPic::pilotWord(_crewOperatingBasis, jurisdiction());
}

QString RotorLabConnector::picText() const
{
    if (!enabled() || _pilotId < 0) {
        return {};
    }
    return RotorLabPic::picLine(_pilotName, _picFrom, _crewOperatingBasis, jurisdiction());
}

void RotorLabConnector::loadSortie(int assetId)
{
    // Read only from a RotorLab that serves it. Against an older one, or with
    // no aircraft picked, the crew screen lists the roster as before.
    if (!enabled() || !configured() || _state != Connected || !picSupported() || assetId < 0) {
        if (!_sortieSeats.isEmpty() || !_sortieInfo.isEmpty() || _sortieAssetId != -1) {
            _sortieSeats.clear();
            _sortieInfo.clear();
            _sortieAssetId = -1;
            emit sortieChanged();
        }
        return;
    }
    _client->getSorties(QString(), assetId, [this, assetId](const HttpReply &r) {
        if (r.status != 200) {
            // Never a failure the crew has to deal with: the roster is still
            // there, and every pilot on it stays selectable.
            _log(QStringLiteral("could not read the day's sorties: %1")
                     .arg(RotorLabFiling::clipServerText(r.error)));
            return;
        }
        QVariantMap info;
        _sortieSeats = RotorLabPic::seatsFor(r.body, assetId, &info);
        _sortieInfo = info;
        _sortieAssetId = assetId;
        emit sortieChanged();
    });
}

void RotorLabConnector::setHandoffReason(const QString &reason)
{
    _handoffReason = reason.trimmed().left(200);
}

int RotorLabConnector::_currentSysid()
{
    MultiVehicleManager *const mvm = MultiVehicleManager::instance();
    if (Vehicle *const v = mvm ? mvm->activeVehicle() : nullptr) {
        return v->id();
    }
    return -1;
}

void RotorLabConnector::_recordHandoff(int fromId, const QString &fromName,
                                       int toId, const QString &toName)
{
    const QString reason = _handoffReason;
    _handoffReason.clear();
    if (fromId == toId) {
        return;             // the same pilot answered again: not a change
    }
    RotorLabPic::Handoff h;
    h.at       = QDateTime::currentDateTimeUtc();
    h.sysid    = _currentSysid();
    h.fromId   = fromId;
    h.fromName = fromId >= 0 ? fromName : QString();
    h.toId     = toId;
    h.toName   = toId >= 0 ? toName : QString();
    h.reason   = reason;
    // Memory at once, then the file. A write that fails is logged; nothing
    // waits on it.
    _picLog->append(h);
    _picFrom = toId >= 0 ? h.at : QDateTime();
    _log(toId >= 0 ? QStringLiteral("pilot changed to %1").arg(toName)
                   : QStringLiteral("no pilot named"));
}

// ---------------------------------------------------------------- the filing

QString RotorLabConnector::_mintExternalRef()
{
    // Globally unique and opaque: this install's random id, the date and a
    // UUID - never a counter, which resets when the GC restarts and would
    // eventually collide with a record already filed, and never the host or
    // user name, which would put the machine's name in RotorLab's records.
    // Minted once per record and stored with it, so a retry sends the same
    // ref; records already in the spool keep theirs.
    return RotorLabStore::formatExternalRef(RotorLabStore::installId(),
                                            QDateTime::currentDateTimeUtc().date(),
                                            QUuid::createUuid().toString(QUuid::Id128).left(12));
}

QJsonObject RotorLabConnector::_flightBody(const FlightWatcher::Flight &flight,
                                           const QString &externalRef) const
{
    QJsonObject b;
    b[QStringLiteral("external_ref")] = externalRef;

    // Resolve the airframe. Without an asset_id OR an asset_name the server
    // refuses the flight outright and the spool parks it for a human - which
    // is where an unattended station's whole day goes to die.
    int assetId = _assetId;
    if (assetId < 0) {
        MultiVehicleManager *const mvm = MultiVehicleManager::instance();
        QmlObjectListModel *const list = mvm ? mvm->vehicles() : nullptr;
        for (int i = 0; list && i < list->count(); ++i) {
            Vehicle *const v = qobject_cast<Vehicle *>((*list)[i]);
            if (v && v->id() == flight.sysid) {
                const AirframeResolver::Match m =
                    _airframes->resolve(v, _roster->aircraftRaw());
                assetId = m.assetId;
                break;
            }
        }
    }
    if (assetId >= 0) {
        b[QStringLiteral("asset_id")] = assetId;
    } else {
        // No confident registry match. Send the name this station knows the
        // airframe by: the server resolves it against the org's registry
        // case-insensitively, so the flight still lands on the right aircraft
        // and builds its hours. An unmatched name still files and still counts
        // toward the pilot, which is strictly better than parking the record.
        const QString name =
            _airframes->airframeName(flight.sysid).left(kAssetNameMax);
        if (!name.isEmpty()) {
            b[QStringLiteral("asset_name")] = name;
        }
    }

    // Without this the flight is attributed to the DEVICE, and the customer's
    // logbook fills with sorties flown by a laptop - invisible until somebody
    // runs a currency report.
    //
    // The pilot at takeoff, from the handoff log. A change made in the air
    // does not rename the whole flight: it files as a period below (or, on a
    // RotorLab without periods, the flight keeps the pilot at takeoff).
    // With periods, a pilot named only in the air is never the flight's
    // pilot_id: nobody named at takeoff means no pilot_id.
    const int pilotAtTakeoff = RotorLabPic::filingPilot(
        _picLog->entries(), flight.takeoffAt, flight.sysid, _pilotId, picSupported());
    if (pilotAtTakeoff >= 0) {
        b[QStringLiteral("pilot_id")] = pilotAtTakeoff;
    }

    if (picSupported()) {
        const QJsonArray periods = RotorLabPic::buildPeriods(
            _picLog->entries(), flight.takeoffAt, flight.landingAt, flight.sysid);
        if (!periods.isEmpty()) {
            b[QStringLiteral("pic_periods")] = periods;
        }
        const QJsonArray rows = RotorLabPic::crewRows(_crewSeats, pilotAtTakeoff);
        if (!rows.isEmpty()) {
            b[QStringLiteral("crew_rows")] = rows;
        }
        if (!_crewOperatingBasis.isEmpty()) {
            b[QStringLiteral("operating_basis")] = _crewOperatingBasis;
        }
    }

    b[QStringLiteral("flown_at")] = flight.takeoffAt.toLocalTime()
                                        .toString(QStringLiteral("yyyy-MM-dd"));
    b[QStringLiteral("duration_min")] = flight.durationS / 60.0;
    b[QStringLiteral("takeoff_at")]   = flight.takeoffAt.toString(Qt::ISODate);
    b[QStringLiteral("landing_at")]   = flight.landingAt.toString(Qt::ISODate);

    const QString s = site();
    const RotorLabFiling::FlightType t = RotorLabFiling::normalizeFlightType(
        RotorLabStore::lastFlightType(), RotorLabStore::lastFlightTypeOther(),
        /*forSending*/ true);
    const QString c = crew();
    if (!s.isEmpty()) b[QStringLiteral("site")]        = s;
    if (!t.type.isEmpty()) b[QStringLiteral("flight_type")] = t.type;
    // Only ever with other, and never empty with it.
    if (!t.other.isEmpty()) b[QStringLiteral("flight_type_other")] = t.other;
    if (!c.isEmpty()) b[QStringLiteral("crew")]        = c;

    // No packs: RotorLab 3.5.1 takes no battery_serials on the flight write,
    // and the field arrives with A55 (phase 3) under names still to agree.

    // Latched by the watcher: QGC tracks neither maximum.
    if (flight.maxHeightM > 0) {
        b[QStringLiteral("max_height_ft")] = flight.maxHeightM / 0.3048;
    }
    if (flight.maxDistanceM > 0) {
        b[QStringLiteral("max_distance_m")] = flight.maxDistanceM;
    }
    if (flight.hasPosition) {
        b[QStringLiteral("lat")] = flight.lat;
        b[QStringLiteral("lon")] = flight.lon;
    }

    b[QStringLiteral("remarks")] = tr("Filed by RotorLab GC.");

    // Only what the operator actually set.
    const QJsonObject compliance = complianceFields();
    for (auto it = compliance.constBegin(); it != compliance.constEnd(); ++it) {
        b.insert(it.key(), it.value());
    }
    return b;
}

void RotorLabConnector::_onFlightComplete(const FlightWatcher::Flight &flight)
{
    if (!enabled() || !configured()) {
        return;
    }
    if (!RotorLabStore::fileFlightsAutomatically()) {
        _log(QStringLiteral("a flight ended; automatic filing is switched off"));
        return;
    }
    if (flight.recovered) {
        // Saved at disarm by a run that closed inside the re-arm window.
        const QString takeoff = flight.takeoffAt.toString(Qt::ISODate);
        if (_spool->hasFlightTakenOffAt(takeoff)) {
            return;             // that run queued it after all
        }
        _log(QStringLiteral("filing a flight that landed before GC was closed (takeoff %1)")
                 .arg(takeoff));
    }

    // Minted ONCE, here, and stored with the record. Every retry for ever uses
    // this same value - regenerating on retry is how one flight becomes four
    // in a logbook after a bad night on a hotspot.
    const QString ref = _mintExternalRef();
    const QJsonObject body = _flightBody(flight, ref);

    if (!body.contains(QStringLiteral("asset_id"))) {
        // Two different outcomes, and saying the wrong one sends a crew looking
        // for a problem that is not there.
        if (body.contains(QStringLiteral("asset_name"))) {
            _log(tr("no registry match for the aircraft: filing under the name "
                    "\"%1\"; RotorLab will attach it to that airframe")
                     .arg(body.value(QStringLiteral("asset_name")).toString()));
        } else {
            _log(tr("could not tell which aircraft flew. The flight is queued; "
                    "set the aircraft on the crew screen and it will file"));
        }
    }
    if (!body.contains(QStringLiteral("pilot_id"))) {
        _log(tr("filing a flight with no pilot named: nobody was set on the crew screen"));
    }

    _lastFiledFlightRef = ref;
    _spool->addFlight(ref, body, flight.sysid);

    if (RotorLabStore::uploadLogs()) {
        _collectLogFor(ref, flight.sysid,
                       double(flight.landingAt.toSecsSinceEpoch()), flight.durationS);
    }
}

void RotorLabConnector::_collectLogFor(const QString &flightRef, int sysid,
                                       double endedAt, double durationS,
                                       const QString &itemId)
{
    // RotorLab stores up to 512 MB for one log (RotorLab's
    // limit). A log over the limit is not pulled or sent; the spool
    // card names it, with the reason, so the crew can attach it by hand.
    const qint64 maxBytes = _maxLogBytes > 0 ? _maxLogBytes : RotorLabFiling::kMaxLogBytes;
    auto notCollected = [this, itemId, flightRef, sysid, endedAt, durationS](const QString &why) {
        _logNotCollected(itemId, flightRef, sysid, endedAt, durationS, why);
    };
    auto queue = [this, itemId, flightRef](const QString &path) {
        if (itemId.isEmpty()) {
            _spool->addLog(flightRef + QStringLiteral("-log"), path,
                           QFileInfo(path).fileName(), flightRef);
        } else {
            _spool->attachCollectedLog(itemId, path);
        }
    };

    // Already on disk? Then no MAVLink transfer is needed at all. Looked for
    // at any size, so one over the limit is named rather than pulled again.
    const QString onDisk = LogCollector::findOnDisk(endedAt, durationS, 0);
    if (!onDisk.isEmpty()) {
        const qint64 size = QFileInfo(onDisk).size();
        if (size > maxBytes) {
            notCollected(RotorLabFiling::logTooLargeText(size, maxBytes,
                                                         QDir::toNativeSeparators(onDisk)));
        } else {
            queue(onDisk);
        }
        return;
    }

    // The log download talks only to the active vehicle, so a log from any
    // other aircraft waits, and the card says which aircraft to make active.
    MultiVehicleManager *const mvm = MultiVehicleManager::instance();
    Vehicle *const active = mvm ? mvm->activeVehicle() : nullptr;
    if (sysid != 0 && (!active || active->id() != sysid)) {
        notCollected(_makeActiveText(sysid));
        return;
    }

    // A log download floods a link another aircraft may be flying on. GC
    // collects when every aircraft has disarmed (or on Try again then).
    if (_flights->anyArmed()) {
        notCollected(tr("Waiting until no aircraft is armed. GC collects this log once every "
                        "aircraft has disarmed."));
        return;
    }

    LogCollector *const lc = LogCollector::instance();
    QString whyNot;
    if (!lc->canCollect(&whyNot)) {
        notCollected(tr("Not collected yet: %1. Press Try again when the aircraft is "
                        "connected and disarmed.").arg(whyNot));
        return;
    }

    // One shot: disconnect both handlers as soon as either fires, so a later
    // collection for a different flight cannot deliver into this one.
    auto conns = std::make_shared<QList<QMetaObject::Connection>>();
    auto done  = [conns]() {
        for (const QMetaObject::Connection &c : *conns) {
            QObject::disconnect(c);
        }
        conns->clear();
    };

    conns->append(connect(lc, &LogCollector::collected, this,
                          [this, done, queue](const QString &path) {
        done();
        queue(path);
        _log(tr("collected the flight log"));
    }));
    conns->append(connect(lc, &LogCollector::failed, this,
                          [done, notCollected](const QString &why, bool) {
        done();
        // Never a failure of the flight: the record files either way, and the
        // card says why so the crew can try again or attach the file later.
        notCollected(tr("Not collected: %1.").arg(why));
    }));

    lc->collect(sysid, endedAt, durationS, maxBytes);
}

void RotorLabConnector::_logNotCollected(const QString &itemId, const QString &flightRef,
                                         int sysid, double endedAt, double durationS,
                                         const QString &why)
{
    _log(QStringLiteral("flight %1: log %2").arg(flightRef, why));
    if (itemId.isEmpty()) {
        _spool->addUncollectedLog(flightRef + QStringLiteral("-log"), flightRef,
                                  sysid, endedAt, durationS, why);
    } else {
        _spool->setLogProblem(itemId, why);
    }
}

QString RotorLabConnector::_makeActiveText(int sysid) const
{
    const QString name = _airframes->airframeName(sysid);
    const QString who = name.isEmpty()
                            ? tr("vehicle %1").arg(sysid)
                            : tr("%1 (vehicle %2)").arg(name, QString::number(sysid));
    bool connected = false;
    MultiVehicleManager *const mvm = MultiVehicleManager::instance();
    QmlObjectListModel *const list = mvm ? mvm->vehicles() : nullptr;
    for (int i = 0; list && i < list->count(); ++i) {
        Vehicle *const v = qobject_cast<Vehicle *>((*list)[i]);
        if (v && v->id() == sysid) {
            connected = true;
            break;
        }
    }
    return connected
        ? tr("Not collected yet: %1 is not the active aircraft. Make it the active aircraft "
             "and GC collects this log once it is disarmed.").arg(who)
        : tr("Not collected yet: %1 is not connected. Connect it and make it the active "
             "aircraft, and GC collects this log once it is disarmed.").arg(who);
}

void RotorLabConnector::_onActiveVehicleChanged()
{
    if (!enabled() || !configured()) {
        return;
    }
    // A short wait: a vehicle that has just become active is still loading.
    QTimer::singleShot(10 * 1000, this, [this]() {
        MultiVehicleManager *const mvm = MultiVehicleManager::instance();
        Vehicle *const active = mvm ? mvm->activeVehicle() : nullptr;
        // Never while any aircraft is armed: the armed edge back to none
        // calls this again.
        if (!active || _flights->anyArmed() || LogCollector::instance()->busy()) {
            return;
        }
        // One per activation, never in a loop: a failure only updates the card.
        for (const FlightSpool::LogToCollect &l : _spool->logsToCollect()) {
            if (l.sysid == active->id()) {
                _collectLogFor(l.flightRef, l.sysid, l.endedAt, l.durationS, l.id);
                return;
            }
        }
    });
}

QJsonObject RotorLabConnector::_retryFlightBody(const QJsonObject &stored, int sysid) const
{
    // The sortie as it was measured, from the stored record; the settings as
    // they are now, through the same _flightBody() a new flight uses.
    FlightWatcher::Flight f;
    f.sysid        = sysid;
    f.takeoffAt    = QDateTime::fromString(stored.value(QStringLiteral("takeoff_at")).toString(),
                                           Qt::ISODate);
    f.landingAt    = QDateTime::fromString(stored.value(QStringLiteral("landing_at")).toString(),
                                           Qt::ISODate);
    f.durationS    = stored.value(QStringLiteral("duration_min")).toDouble() * 60.0;
    f.maxHeightM   = stored.value(QStringLiteral("max_height_ft")).toDouble() * 0.3048;
    f.maxDistanceM = stored.value(QStringLiteral("max_distance_m")).toDouble();
    f.hasPosition  = stored.contains(QStringLiteral("lat"));
    f.lat          = stored.value(QStringLiteral("lat")).toDouble();
    f.lon          = stored.value(QStringLiteral("lon")).toDouble();
    const QString ref = stored.value(QStringLiteral("external_ref")).toString();
    return RotorLabFiling::retryFlightBody(stored, _flightBody(f, ref));
}


// ------------------------------------------------------------------- missions

QVariantList RotorLabConnector::missions() const   { return _missions->missions(); }
bool    RotorLabConnector::missionsLoaded() const  { return _missions->loaded(); }
bool    RotorLabConnector::missionsBusy() const    { return _missions->busy(); }
QString RotorLabConnector::missionsError() const   { return _missions->lastError(); }

void RotorLabConnector::refreshMissions()
{
    if (!enabled() || !configured()) {
        return;
    }
    _fetchMissions();
}

void RotorLabConnector::_fetchMissions()
{
    // A Device Key reads the missions published to the whole organization.
    // Narrowed to whoever the crew screen says is flying, to the missions that
    // pilot made. With nobody set the organization's whole published list is
    // asked for, which is the honest answer, not an empty list. RotorLab has
    // no assignment here, so nothing on screen says "assigned" (A72).
    _missionsPilotName = _pilotId >= 0 ? _pilotName : QString();
    emit missionsChanged();
    _missions->fetch(_pilotId);
}

QString RotorLabConnector::missionsEmptyText() const
{
    return RotorLabCrew::missionsEmptyText(_missions->loaded(), _missionsPilotName);
}

void RotorLabConnector::loadMission(int missionId)
{
    if (!enabled() || !configured()) {
        return;
    }
    _missions->download(missionId);
}

bool RotorLabConnector::serverNamesPilots() const
{
    return RotorLabFiling::serverNamesPilots(_serverVersion);
}

QVariantMap RotorLabConnector::readinessFor(int assetId) const
{
    return _roster->readinessFor(assetId);
}

// ----------------------------------------------------------------- checklists

QVariantList RotorLabConnector::preflightTemplates() const
{
    return _roster->templates(QStringLiteral("preflight"));
}

QVariantList RotorLabConnector::postflightTemplates() const
{
    return _roster->templates(QStringLiteral("postflight"));
}

QString RotorLabConnector::newChecklistRef(const QString &kind, int sysid) const
{
    // Derived from the same shape as a flight reference, and minted once when
    // the crew opens the runner. Reused on every retry for ever - a new one
    // files a second copy of a run that happened once.
    Q_UNUSED(sysid)
    return QStringLiteral("%1-%2").arg(_mintExternalRef(), kind);
}

void RotorLabConnector::_onServerAnswered()
{
    // Retry now, or any filing, got through after a restart or an outage:
    // the station is connected again without a Verify. The server version
    // is the one the last verify read.
    if (RotorLabCrew::reconnectOnFiled(_state == Offline, !_serverVersion.isEmpty())) {
        _setStatus({});
        _setState(Connected);
        _log(QStringLiteral("RotorLab answered a filing: connected again"));
    }
}

void RotorLabConnector::_onWorkRaised(const QStringList &items)
{
    if (items.isEmpty()) {
        return;
    }
    // Every item to the log, one line to the page. This is information the crew
    // may act on before the next sortie - never a gate, never a dialog, and it
    // arrives after the flight is already filed, so nothing about it can hold
    // an aircraft on the ground.
    for (const QString &s : items) {
        _log(tr("filing raised: %1").arg(s));
    }
    _setStatus(items.size() == 1
                   ? tr("Filing raised: %1").arg(items.first())
                   : tr("Filing raised %n maintenance item(s).", nullptr, items.size()));
}

void RotorLabConnector::_onFilingNotice(const QJsonObject &flightBody,
                                        const QStringList &pilotNotes,
                                        const QStringList &otherNotes)
{
    // Like work raised: every line to the log, and the sentences kept on the
    // settings page and the station until someone dismisses them, across a
    // restart. A crew that is flying when the reply lands still sees it after
    // landing. Each says which flight, so two flights never collapse into one.
    QString aircraft = flightBody.value(QStringLiteral("asset_name")).toString();
    const int assetId = flightBody.value(QStringLiteral("asset_id")).toInt(-1);
    if (aircraft.isEmpty() && assetId >= 0) {
        for (const QVariant &a : _roster->assets()) {
            const QVariantMap m = a.toMap();
            if (m.value(QStringLiteral("id")).toInt() == assetId) {
                aircraft = m.value(QStringLiteral("name")).toString();
                break;
            }
        }
    }
    const QString prefix = RotorLabFiling::flightNoticePrefix(
        flightBody.value(QStringLiteral("takeoff_at")).toString(), aircraft,
        QDate::currentDate());
    const QString ref = flightBody.value(QStringLiteral("external_ref")).toString();

    auto add = [&](QStringList &list, const QStringList &notes) {
        for (const QString &n : notes) {
            const QString line = prefix.isEmpty() ? n : prefix + QStringLiteral(": ") + n;
            _log(ref.isEmpty() ? n : tr("flight %1: %2").arg(ref, n));
            list.removeAll(line);
            list << line;
        }
    };
    add(_filingPilotNotices, pilotNotes);
    add(_filingOtherNotices, otherNotes);

    // At most six in all. The oldest other notice goes first; a no-pilot
    // notice is the last to go.
    while (_filingPilotNotices.size() + _filingOtherNotices.size() > 6) {
        if (!_filingOtherNotices.isEmpty()) {
            _filingOtherNotices.removeFirst();
        } else {
            _filingPilotNotices.removeFirst();
        }
    }
    _publishFilingNotices(/*save*/ true);
}

void RotorLabConnector::_publishFilingNotices(bool save)
{
    if (save) {
        RotorLabStore::setFilingPilotNotices(_filingPilotNotices);
        RotorLabStore::setFilingOtherNotices(_filingOtherNotices);
    }
    _filingNotice = filingNotices().join(QLatin1Char('\n'));
    emit statusChanged();
}

void RotorLabConnector::dismissFilingNotices(const QStringList &shown)
{
    for (const QString &n : shown) {
        _filingPilotNotices.removeAll(n);
        _filingOtherNotices.removeAll(n);
    }
    _publishFilingNotices(/*save*/ true);
}

void RotorLabConnector::dismissFilingNotice()
{
    _filingPilotNotices.clear();
    _filingOtherNotices.clear();
    _publishFilingNotices(/*save*/ true);
}

void RotorLabConnector::_onArmedChanged(bool anyArmed)
{
    if (_anyArmed != anyArmed) {
        _anyArmed = anyArmed;
        emit statusChanged();       // the station's filing-notice card hides
    }
    if (anyArmed) {
        // Never offer anything to a crew that is flying, and never leave an
        // offer on screen while they are.
        _postflightTimer.stop();
        if (!_checklistOffer.isEmpty()) {
            _checklistOffer.clear();
            emit checklistsChanged();
        }
        return;
    }
    _preflightOfferedThisSortie = false;
    _postflightTimer.start();
    // Landed: "not saved while an aircraft is armed" no longer applies.
    if (!_flights->anyArmed()) {
        if (RotorLabCrew::clearErrorAtDisarm(_lastError, _armedKeyRefusal)) {
            _setError({});
        }
        _armedKeyRefusal.clear();
    }
    if (_crewScreenDeferred && !_flights->anyArmed()) {
        _askWhoIsFlying();
    }
}

void RotorLabConnector::_maybeOfferPreflight()
{
    bool vehicleConnected = false;
    if (MultiVehicleManager *const mvm = MultiVehicleManager::instance()) {
        QmlObjectListModel *const list = mvm->vehicles();
        vehicleConnected = list && list->count() > 0;
    }
    if (!RotorLabCrew::offerPreflight(enabled() && configured(), _flights->anyArmed(),
                                      vehicleConnected,
                                      _roster->hasTemplates(QStringLiteral("preflight")),
                                      _preflightOfferedThisSortie, _checklistOffer)) {
        return;
    }
    // An offer, never a step: the card is ignorable, hides while the guided
    // confirm is up, and is cleared at the arm. Arming is never held.
    _preflightOfferedThisSortie = true;
    _checklistOffer = QStringLiteral("preflight");
    emit checklistsChanged();
}

void RotorLabConnector::_offerPostflight()
{
    if (!enabled() || !configured() || _flights->anyArmed()) {
        return;
    }
    if (!_roster->hasTemplates(QStringLiteral("postflight"))) {
        return;             // the customer published none; offer nothing
    }
    _checklistOffer = QStringLiteral("postflight");
    emit checklistsChanged();
}

void RotorLabConnector::dismissChecklistOffer()
{
    if (!_checklistOffer.isEmpty()) {
        _checklistOffer.clear();
        emit checklistsChanged();
    }
}

void RotorLabConnector::fileChecklist(const QString &externalRef, int templateId,
                                      const QString &kind, const QString &aircraft,
                                      double started, bool passed,
                                      const QVariantList &items, const QString &notes)
{
    Q_UNUSED(aircraft)
    Q_UNUSED(passed)        // the server counts pass/fail/skip itself

    QJsonArray results;
    for (const QVariant &v : items) {
        const QVariantMap m = v.toMap();
        QJsonObject r;
        // Exactly three keys and exactly three words. The API refuses an
        // unknown key rather than ignoring it, on purpose: a silently dropped
        // answer is a checklist that reads as complete and is not.
        r[QStringLiteral("text")] = m.value(QStringLiteral("text")).toString();
        const QString result = m.value(QStringLiteral("result")).toString();
        r[QStringLiteral("result")] = result.isEmpty()
            ? (m.value(QStringLiteral("ok")).toBool() ? QStringLiteral("pass")
                                                      : QStringLiteral("fail"))
            : result;
        const QString note = m.value(QStringLiteral("note")).toString();
        if (!note.isEmpty()) {
            r[QStringLiteral("note")] = note;
        }
        results.append(r);
    }

    QJsonObject body;
    body[QStringLiteral("external_ref")] = externalRef;
    if (templateId > 0) {
        // Prefer a template the server offered: a run against one lines up
        // with every other run of that template in the customer's records.
        body[QStringLiteral("template_id")] = templateId;
    } else {
        body[QStringLiteral("kind")] = kind;
        body[QStringLiteral("name")] = kind == QLatin1String("preflight")
                                        ? tr("Pre-flight (GC)") : tr("Post-flight (GC)");
    }
    body[QStringLiteral("results")] = results;
    if (!notes.isEmpty()) {
        body[QStringLiteral("notes")] = notes;
    }
    body[QStringLiteral("completed_at")] =
        QDateTime::fromSecsSinceEpoch(qint64(started), Qt::UTC).toString(Qt::ISODate);

    // A pre-flight happens before its flight exists, so it waits for the next
    // one rather than filing free of it.
    const QString dependsOn = (kind == QLatin1String("preflight"))
                                ? FlightSpool::awaitNextFlight()
                                : _lastFiledFlightRef;

    _spool->addChecklist(externalRef, body, dependsOn);
    dismissChecklistOffer();
    _log(QStringLiteral("%1 checklist queued to file").arg(kind));
}

// ---------------------------------------------------------------- the queue

QString RotorLabConnector::queueText() const        { return _spool->summaryText(); }
QString RotorLabConnector::queueLastAttempt() const { return _spool->lastAttemptText(); }
int     RotorLabConnector::queuePending() const     { return _spool->pendingCount(); }
int     RotorLabConnector::queueNeedsAttention() const { return _spool->needsAttentionCount(); }
QVariantList RotorLabConnector::queueModel() const  { return _spool->model(); }

void RotorLabConnector::retryQueueNow()
{
    _spool->setFilingAllowed(true);
    _spool->drainNow();
}

void RotorLabConnector::retryQueueItem(const QString &id)
{
    // A log that was never collected: collect it again, from disk or the aircraft.
    FlightSpool::LogToCollect log;
    if (_spool->logToCollect(id, &log)) {
        _collectLogFor(log.flightRef, log.sysid, log.endedAt, log.durationS, log.id);
        return;
    }
    // A flight: rebuilt from this station's current settings, same external_ref,
    // so a pilot or aircraft fixed here reaches the record.
    QJsonObject stored;
    int sysid = 0;
    if (_spool->flightForRetry(id, &stored, &sysid)) {
        const QJsonObject body = _retryFlightBody(stored, sysid);
        if (body != stored) {
            _log(QStringLiteral("flight %1: Try again adds the pilot, aircraft or site it was "
                                "missing").arg(stored.value(QStringLiteral("external_ref"))
                                                   .toString()));
        }
        _spool->retry(id, body);
        return;
    }
    _spool->retry(id);
}

void RotorLabConnector::forgetQueueItem(const QString &id) { _spool->forget(id); }

bool RotorLabConnector::_handleCommon(const HttpReply &r)
{
    if (!r.netOk) {
        // No HTTP answer at all: DNS, connect, timeout. Keep spooling and show
        // one line - not a dialog. They may be flying.
        _setStatus(r.error);
        _setState(Offline);
        return false;
    }
    if (r.status == 401) {
        _setError(tr("RotorLab rejected this Device Key. It may have been revoked "
                     "by an organization admin under Organization, Integrations, Devices."));
        _setState(KeyRejected);
        return false;
    }
    if (r.status == 403) {
        // Scope, plan, trial or suspension. Show the server's sentence
        // verbatim and keep spooling - a subscription lapsing on a Tuesday
        // must not lose Tuesday's flying.
        const QString sentence = RotorLabFiling::clipServerText(r.error);
        _setError(sentence.isEmpty() ? tr("This account cannot file right now.") : sentence);
        _setState(AccountBlocked);
        return false;
    }
    if (r.status >= 500) {
        // RotorLab answered, so the route is fine: a server-side outage, not
        // a local network problem. Keep spooling.
        _setStatus(tr("RotorLab answered with a server error (HTTP %1). Flights keep "
                      "filing to the queue and send when it is back.").arg(r.status));
        _setState(Unavailable);
        return false;
    }
    if (r.status == 400) {
        // Ours, and never worth retrying.
        _log(QStringLiteral("400 from RotorLab: %1").arg(RotorLabFiling::clipServerText(r.error)));
        return false;
    }
    return r.status == 200;
}

// ------------------------------------------------------------------- teardown

void RotorLabConnector::disconnectFromRotorLab()
{
    _client->reset();
    _refreshTimer.stop();
    RotorLabStore::clearAll();      // keeps the spool
    // ...and the notices about flights already filed: they are about records
    // in RotorLab, not about this key.
    _publishFilingNotices(/*save*/ true);
    _applyCredentials();
    _setError({});
    _setStatus({});
    _setState(NotConfigured);
    emit identityChanged();
    emit settingsChanged();
}

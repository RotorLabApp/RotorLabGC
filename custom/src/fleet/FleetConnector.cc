/****************************************************************************
 *
 * RotorLab GC Fleet Connector.
 *
 ****************************************************************************/

#include "FleetConnector.h"
#include "FleetChecklists.h"
#include "FleetLogs.h"
#include "FleetStore.h"
#include "FleetTelemetry.h"

#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

#include <QtCore/QApplicationStatic>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSysInfo>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

#include "QGCApplication.h"
#include "QGCLoggingCategory.h"

QGC_LOGGING_CATEGORY(FleetConnectorLog, "gcs.custom.fleet.connector")

Q_APPLICATION_STATIC(FleetConnector, _fleetConnectorInstance);

namespace {

// Backoff for UNREACHABLE. A Fleet Server on a laptop in the back of a truck
// goes away and comes back all day; this is not an error a crew must dismiss.
constexpr int kBackoffMs[] = {1000, 2000, 5000, 15000, 30000};
constexpr int kBackoffCount = int(sizeof(kBackoffMs) / sizeof(kBackoffMs[0]));

constexpr int kRosterMs = 5 * 60 * 1000;

} // namespace

FleetConnector::FleetConnector(QObject *parent)
    : QObject(parent)
    , _client(new FleetClient(this))
    , _telemetry(new FleetTelemetry(this))
    , _checklists(new FleetChecklists(_client, this))
    , _logs(new FleetLogs(_client, this))
{
    connect(_logs, &FleetLogs::logMessage,  this, &FleetConnector::_log);
    connect(_logs, &FleetLogs::modelChanged, this, &FleetConnector::flightLogsChanged);
    connect(_checklists, &FleetChecklists::logMessage, this, &FleetConnector::_log);
    connect(_checklists, &FleetChecklists::templatesChanged, this, &FleetConnector::checklistsChanged);
    connect(_checklists, &FleetChecklists::outboxChanged,    this, &FleetConnector::checklistsChanged);
    connect(_telemetry, &FleetTelemetry::flightEnded, this, [this](const QJsonObject &flight) {
        const QString ref = flight.value(QStringLiteral("flight_ref")).toString();
        const QJsonObject run = flight.value(QStringLiteral("details")).toObject();
        QJsonObject details;
        if (run.contains(QStringLiteral("pilot_id"))) {
            details.insert(QStringLiteral("pilot_id"), run.value(QStringLiteral("pilot_id")));
        }
        if (run.contains(QStringLiteral("asset_id"))) {
            details.insert(QStringLiteral("aircraft_id"), run.value(QStringLiteral("asset_id")));
        }
        for (const QString &field : {QStringLiteral("pilot_name"), QStringLiteral("aircraft_name"),
                                     QStringLiteral("site"), QStringLiteral("flight_type"),
                                     QStringLiteral("flight_type_other"),
                                     QStringLiteral("crew")}) {
            if (run.contains(field)) {
                details.insert(field, run.value(field));
            }
        }
        if (!details.contains(QStringLiteral("aircraft_name"))) {
            details.insert(QStringLiteral("aircraft_name"),
                            flight.value(QStringLiteral("vehicle")));
        }
        QJsonObject queued;
        queued.insert(QStringLiteral("flight_ref"), ref);
        queued.insert(QStringLiteral("details"), details);
        FleetStore::enqueueFlightDetails(queued);
        _logs->queueFlight(flight);
    });

    // The post-flight is offered once the aircraft has been disarmed and has
    // stayed disarmed - not the instant a motor stops.
    _postflightTimer.setSingleShot(true);
    _postflightTimer.setInterval(30 * 1000);
    connect(&_postflightTimer, &QTimer::timeout, this, [this]() {
        if (!_anyArmed && _checklists->hasTemplates(QStringLiteral("postflight"))) {
            _checklistOffer = QStringLiteral("postflight");
            emit checklistsChanged();
        }
    });

    _rosterTimer.setInterval(kRosterMs);
    connect(&_rosterTimer, &QTimer::timeout, this, &FleetConnector::_rosterTick);

    _reconnectTimer.setSingleShot(true);
    connect(&_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (_state == Unreachable) {
            _fetchRoster();
        }
    });

    _telemetryTimer.setInterval(1000);
    connect(&_telemetryTimer, &QTimer::timeout, this, &FleetConnector::_telemetryTick);

    _loadFromStore();
}

FleetConnector::~FleetConnector() = default;

FleetConnector *FleetConnector::instance()
{
    return _fleetConnectorInstance();
}

// ---------------------------------------------------------------- accessors

QString FleetConnector::stateText() const
{
    switch (_state) {
    case NotConnected: return tr("Not connected");
    case Enrolling:    return tr("Enrolling…");
    case FirstSync:    return tr("Setting up…");
    case Connected:    return tr("Connected");
    case Unreachable:  return tr("Reconnecting");
    case Revoked:      return tr("This ground station was removed");
    }
    return {};
}

bool    FleetConnector::enrolled() const     { return FleetStore::enrolled(); }
QString FleetConnector::baseUrl() const      { return FleetStore::baseUrl(); }
QString FleetConnector::stationName() const  { return FleetStore::stationName(); }
QString FleetConnector::machineName() const  { return QSysInfo::machineHostName(); }
int     FleetConnector::stationId() const    { return FleetStore::stationId(); }
QString FleetConnector::certInfo() const
{
    if (!enrolled()) {
        return {};
    }
    const QString pin = FleetStore::serverPin();
    if (!pin.isEmpty()) {
        return tr("Pinned to the server key from the enrollment code (SHA-256 %1...)").arg(pin.left(16));
    }
    if (FleetStore::baseUrl().startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)) {
        return tr("None: plain http on this computer");
    }
    return tr("Validated by the system trust store");
}

QString FleetConnector::lastReportText() const
{
    if (!_lastReportAt.isValid()) {
        return tr("no reports yet");
    }
    const qint64 secs = _lastReportAt.secsTo(QDateTime::currentDateTimeUtc());
    if (secs < 2)    return tr("just now");
    if (secs < 60)   return tr("%1 seconds ago").arg(secs);
    if (secs < 3600) return tr("%1m %2s ago").arg(secs / 60).arg(secs % 60);
    return tr("%1h ago").arg(secs / 3600);
}

void FleetConnector::setSuspendedByRotorLab(bool suspended)
{
    if (_suspended == suspended) {
        return;
    }
    _suspended = suspended;

    if (suspended) {
        // Stand down cleanly. Abandon what is on the wire first so a reply
        // from the old session cannot drag the state machine back up, then
        // stop every loop. Nothing is cleared: the token, roster, checklist
        // outbox, and log queue all stay exactly as they are.
        _client->reset();
        _setState(NotConnected);
        _log(QStringLiteral("standing down - the RotorLab Connector is this station's route"));
    } else if (FleetStore::enrolled()) {
        _log(QStringLiteral("resuming - the RotorLab Connector was switched off"));
        _setState(FirstSync);
        _startFirstSync();
    }

    emit stateChanged();
}

// ---------------------------------------------------------------- state

void FleetConnector::_setState(State s)
{
    if (_state == s) {
        return;
    }
    _state = s;

    // Sample collection continues through ordinary outages so sequence
    // numbers and timestamps remain durable. Terminal states stop all sends.
    const bool reporting = (s == Connected);
    if (reporting) {
        if (!_rosterTimer.isActive())  _rosterTimer.start();
        _backoffIndex = 0;
        _clearReconnect();
    } else {
        _telemetryInFlight = false;
        if (s == Revoked || s == NotConnected) {
            _rosterTimer.stop();
            _clearReconnect();
        }
    }
    const bool collectSamples = FleetStore::enrolled() && !_suspended
        && s != Revoked && s != NotConnected;
    if (collectSamples) {
        if (!_telemetryTimer.isActive()) {
            _telemetryTimer.start();
        }
    } else {
        _telemetryTimer.stop();
    }

    _log(QStringLiteral("state -> %1").arg(stateText()));
    emit stateChanged();
    emit tickChanged();
}

void FleetConnector::_setStatus(const QString &text)
{
    if (_statusText != text) {
        _statusText = text;
        emit statusChanged();
    }
}

void FleetConnector::_setError(const QString &text)
{
    if (_lastError != text) {
        _lastError = text;
        emit statusChanged();
    }
}

void FleetConnector::_setBusy(bool on)
{
    if (_busy != on) {
        _busy = on;
        emit statusChanged();
    }
}

void FleetConnector::_log(const QString &line)
{
    qCDebug(FleetConnectorLog) << line;
    emit logMessage(line);
}

void FleetConnector::_scheduleReconnect()
{
    const int ms = kBackoffMs[qMin(_backoffIndex, kBackoffCount - 1)];
    if (_backoffIndex < kBackoffCount - 1) {
        ++_backoffIndex;
    }
    _reconnectTimer.start(ms);
}

void FleetConnector::_clearReconnect()
{
    _reconnectTimer.stop();
}

// ---------------------------------------------------------------- store

void FleetConnector::_loadFromStore()
{
    _client->setBaseUrl(FleetStore::baseUrl());
    _client->setToken(FleetStore::token());
    _client->setPinnedSpkiSha256(FleetStore::serverPin());

    // Serve the roster from cache immediately: a crew picking a pilot must not
    // wait on a fetch, and must still be able to pick one with the link down.
    const QByteArray rosterCache = FleetStore::rosterJson();
    if (!rosterCache.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(rosterCache);
        if (doc.isObject()) {
            _applyRoster(doc.object(), false);
        }
    }

    // Site, flight type, crew and aircraft carry across a restart. The pilot
    // deliberately does not.
    QJsonObject ctx;
    const int    asset = FleetStore::lastAssetId();
    const QString s    = FleetStore::lastSite();
    const QString ft   = FleetStore::lastFlightType();
    const QString cw   = FleetStore::lastCrew();
    if (asset >= 0)    ctx[QStringLiteral("asset_id")]    = asset;
    if (!s.isEmpty())  ctx[QStringLiteral("site")]        = s;
    if (!ft.isEmpty()) ctx[QStringLiteral("flight_type")] = ft;
    if (!cw.isEmpty()) ctx[QStringLiteral("crew")]        = cw;
    _runContext = ctx;

    if (FleetStore::enrolled()) {
        // Do not assume the server is there; the first roster request decides
        // between CONNECTED and UNREACHABLE.
        _setState(FirstSync);
        _startFirstSync();
    } else {
        _setState(NotConnected);
    }
    emit identityChanged();
}

// ---------------------------------------------------------------- enrollment

FleetConnector::EnrollCode FleetConnector::parseEnrollCode(const QString &typed)
{
    // Whitespace around a paste is the only thing removed. Case, '-' and '_'
    // are part of the code and the server compares it byte for byte.
    const QString t = typed.trimmed();
    EnrollCode out;
    if (t.startsWith(QStringLiteral("rotorlab-server://"), Qt::CaseInsensitive)) {
        const QUrlQuery q(QUrl(t).query(QUrl::FullyEncoded));
        out.code   = q.queryItemValue(QStringLiteral("c"), QUrl::FullyDecoded);
        out.pin    = q.queryItemValue(QStringLiteral("p"), QUrl::FullyDecoded).toLower();
        out.hasPin = q.hasQueryItem(QStringLiteral("p"));
        const QString a = q.queryItemValue(QStringLiteral("a"), QUrl::FullyDecoded);
        if (!a.isEmpty()) {
            out.address = a.contains(QStringLiteral("://")) ? a : QStringLiteral("https://") + a;
        }
        return out;
    }
    const qsizetype dot = t.indexOf(QLatin1Char('.'));
    if (dot < 0) {
        out.code = t;
    } else {
        out.code   = t.left(dot);
        out.pin    = t.mid(dot + 1).toLower();
        out.hasPin = true;
    }
    return out;
}

bool FleetConnector::isValidCode(const QString &code)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{16}$"));
    return re.match(code).hasMatch();
}

bool FleetConnector::isValidPin(const QString &pin)
{
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));
    return re.match(pin).hasMatch();
}

bool FleetConnector::enrollCodeReady(const QString &typed)
{
    const EnrollCode c = parseEnrollCode(typed);
    return isValidCode(c.code) && (!c.hasPin || isValidPin(c.pin));
}

QJsonObject FleetConnector::enrollBody(const QString &code, const QString &name)
{
    QJsonObject body;
    body[QStringLiteral("code")] = code;
    body[QStringLiteral("label")] = name.trimmed().left(80);
#ifdef Q_OS_ANDROID
    body[QStringLiteral("platform")] = QStringLiteral("android");
#else
    body[QStringLiteral("platform")] = QStringLiteral("windows");
#endif
    return body;
}

QString FleetConnector::enrollErrorText(const FleetReply &r, const QString &base)
{
    if (r.pinRefused) {
        return tr("The server at %1 does not match the pin in the enrollment code, so nothing was sent. "
                  "Check the address, or ask your operator for a new code.").arg(base);
    }
    if (!r.netOk) {
        return r.error.isEmpty()
            ? tr("Could not reach %1").arg(base)
            : tr("Could not reach %1: %2").arg(base, r.error);
    }
    switch (r.status) {
    case 400:
        return tr("The server did not get an enrollment code. Enter the code and try again.");
    case 401:
        return tr("That code is wrong or has expired. Codes work once and expire after 15 minutes. "
                  "Ask your operator for a new one.");
    case 409:
        return tr("This RotorLab Server has no Fleet Server license, so it cannot enroll ground stations. "
                  "Ask your administrator.");
    case 429: {
        int wait = r.retryAfter;
        if (wait < 0) {
            // The server names the wait in its sentence: "Wait 42 seconds."
            static const QRegularExpression re(QStringLiteral("(\\d+)\\s*second"));
            const QRegularExpressionMatch m = re.match(r.error);
            if (m.hasMatch()) {
                wait = m.captured(1).toInt();
            }
        }
        return wait >= 0
            ? tr("Too many wrong codes from this computer. Wait %1 seconds, then try again.").arg(wait)
            : tr("Too many wrong codes from this computer. Wait a little, then try again.");
    }
    default:
        break;
    }
    return r.error.isEmpty()
        ? tr("The server could not enroll this station (HTTP %1).").arg(r.status)
        : tr("The server could not enroll this station (HTTP %1): %2").arg(r.status).arg(r.error);
}

void FleetConnector::enroll(const QString &address, const QString &code,
                            const QString &name)
{
    const EnrollCode parsed = parseEnrollCode(code);

    // The link form carries the address the pin belongs to, so it wins over
    // whatever is in the address field.
    const QString typedAddress = parsed.address.isEmpty() ? address : parsed.address;
#ifdef QGC_DAILY_BUILD
    const QString base = FleetClient::normalizeBaseUrl(typedAddress, true);
#else
    const QString base = FleetClient::normalizeBaseUrl(typedAddress);
#endif
    if (base.isEmpty()) {
        _setError(tr("That does not look like a server address."));
        return;
    }
    if (name.trimmed().isEmpty()) {
        // The server would answer 403 "a station name is required"; catching it
        // here keeps that from looking like a rejected code.
        _setError(tr("A name for this ground station is required."));
        return;
    }
    if (parsed.code.isEmpty()) {
        _setError(tr("Enter the enrollment code."));
        return;
    }
    if (parsed.hasPin && !isValidPin(parsed.pin)) {
        _setError(tr("The part after the dot is not a valid server pin. Copy the code again from the console."));
        return;
    }
    if (parsed.hasPin && !base.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        _setError(tr("This code carries a server pin, so the address must start with https://. "
                     "Nothing was sent."));
        return;
    }

    _setError({});
    _setBusy(true);
    _setState(Enrolling);

    // Enrolling starts a new server session and abandons any old request.
    // The pin is in force from the first byte: the client checks the server
    // key after the handshake and before the request is written.
    _client->reset();
    _client->setBaseUrl(base);
    _client->setToken({});
    _client->setPinnedSpkiSha256(parsed.pin);

    const QString pin = parsed.pin;
    _client->postUnauthenticated(QStringLiteral("/api/gc/v1/enroll"), enrollBody(parsed.code, name),
                                 [this, base, name, pin](const FleetReply &r) {
        _setBusy(false);
        _log(r.pinRefused ? QStringLiteral("enroll: refused before sending, the server key does not match the pin")
                          : QStringLiteral("enroll: HTTP %1").arg(r.status));

        if (!r.netOk || r.status != 200) {
            _setError(enrollErrorText(r, base));
            _setState(NotConnected);
            return;
        }

        const QString token = r.body.value(QStringLiteral("token")).toString();
        const int stationId = r.body.value(QStringLiteral("station_id")).toInt(0);
        if (token.isEmpty() || stationId <= 0) {
            _setError(tr("The server's reply was missing the station number or token. Try again with a new code."));
            _setState(NotConnected);
            return;
        }

        _logs->reset();
        _checklists->reset();
        _pilots.clear();
        _assets.clear();
        _rosterLoaded = false;
        FleetStore::resetStationState();
        FleetStore::setBaseUrl(base);
        FleetStore::setServerPin(pin);
        FleetStore::setToken(token);
        FleetStore::setStationName(name.trimmed().left(80));
        FleetStore::setStationId(stationId);
        _client->setToken(token);

        _log(QStringLiteral("enrolled as station %1").arg(stationId));
        emit identityChanged();
        _setState(FirstSync);
        _startFirstSync();
    });
}

void FleetConnector::disconnectFromServer()
{
    // Order matters: drop the in-flight replies before anything else, so none
    // of them can run against the half-cleared state below.
    _client->reset();
    _logs->reset();
    _checklists->reset();
    FleetStore::clearAll();
    _client->setBaseUrl({});
    _client->setToken({});
    _client->setPinnedSpkiSha256({});
    _lastReportAt = {};
    _setError({});
    _setStatus({});
    _setState(NotConnected);
    emit identityChanged();
}

void FleetConnector::refreshNow()
{
    if (!enrolled()) {
        return;
    }
    _fetchRoster();
}

// ---------------------------------------------------------------- sync

void FleetConnector::_startFirstSync()
{
    _fetchRoster(true);
}

void FleetConnector::_fetchRoster(bool duringFirstSync)
{
    if (!FleetStore::enrolled()) {
        return;
    }
    _client->get(QStringLiteral("/api/gc/v1/roster"), [this, duringFirstSync](const FleetReply &r) {
        if (!_handleCommon(r)) {
            return;
        }
        _applyRoster(r.body, true);
        if (_state != Connected) {
            _setState(Connected);
            _setError({});
        }
        if (duringFirstSync) {
            _evaluateCrewScreenDue();
            emit firstSyncComplete();
        }
    });
}

void FleetConnector::_rosterTick()
{
    _fetchRoster();
}

void FleetConnector::setRunContext(const QJsonObject &ctx)
{
    _runContext = ctx;
    emit runContextChanged();
}

// ---------------------------------------------------------------- roster

QString FleetConnector::rosterSourceText() const
{
    // Only the label changes; both lists render identically.
    return (_rosterSource == QLatin1String("rotorlab"))
        ? tr("RotorLab roster") : tr("RotorLab Server roster");
}

QStringList FleetConnector::recentSites() const { return FleetStore::recentSites(); }
QStringList FleetConnector::recentCrews() const { return FleetStore::recentCrews(); }

int FleetConnector::pilotId() const
{
    return _runContext.contains(QStringLiteral("pilot_id"))
        ? _runContext.value(QStringLiteral("pilot_id")).toInt(-1) : -1;
}
QString FleetConnector::pilotName() const { return _pilotName; }
int FleetConnector::assetId() const
{
    return _runContext.contains(QStringLiteral("asset_id"))
        ? _runContext.value(QStringLiteral("asset_id")).toInt(-1) : -1;
}
QString FleetConnector::site() const       { return _runContext.value(QStringLiteral("site")).toString(); }
QString FleetConnector::flightType() const { return _runContext.value(QStringLiteral("flight_type")).toString(); }
QString FleetConnector::flightTypeOther() const
{
    return _runContext.value(QStringLiteral("flight_type_other")).toString();
}
QString FleetConnector::crew() const       { return _runContext.value(QStringLiteral("crew")).toString(); }

void FleetConnector::refreshRoster()
{
    _fetchRoster();
}

void FleetConnector::_applyRoster(const QJsonObject &body, bool persist)
{
    _rosterSource = QStringLiteral("rotorlab");

    _pilots.clear();
    for (const QJsonValue &v : body.value(QStringLiteral("pilots")).toArray()) {
        const QJsonObject p = v.toObject();
        QVariantMap m;
        m[QStringLiteral("id")]       = p.value(QStringLiteral("id")).toInt();
        m[QStringLiteral("name")]     = p.value(QStringLiteral("name")).toString();
        m[QStringLiteral("currency")] = p.value(QStringLiteral("currency")).toString(QStringLiteral("unknown"));
        m[QStringLiteral("hours")]    = p.value(QStringLiteral("hours")).toDouble();
        QStringList lapsed;
        for (const QJsonValue &l : p.value(QStringLiteral("lapsed")).toArray()) {
            lapsed << l.toString();
        }
        m[QStringLiteral("lapsed")] = lapsed;
        _pilots.append(m);
    }

    _assets.clear();
    for (const QJsonValue &v : body.value(QStringLiteral("aircraft")).toArray()) {
        const QJsonObject a = v.toObject();
        QVariantMap m;
        m[QStringLiteral("id")]    = a.value(QStringLiteral("id")).toInt();
        m[QStringLiteral("name")]  = a.value(QStringLiteral("name")).toString();
        m[QStringLiteral("model")] = a.value(QStringLiteral("model")).toString();
        _assets.append(m);
    }
    if (body.contains(QStringLiteral("checklists"))) {
        _checklists->setTemplates(body.value(QStringLiteral("checklists")).toArray());
    }

    _rosterLoaded = true;
    if (persist) {
        FleetStore::setRosterJson(QJsonDocument(body).toJson(QJsonDocument::Compact));
    }
    emit rosterChanged();
}

// ---------------------------------------------------------------- crew screen

void FleetConnector::setCrew(int pilotId, const QString &pilotName, int assetId,
                             const QString &site, const QString &flightType,
                             const QString &crew)
{
    QJsonObject ctx;
    // A pilot id below zero means the crew did not choose one. Send nothing
    // rather than a guess: the flight is then held for assignment, which is an
    // honest gap. A wrong name looks correct and nobody ever goes back to it.
    if (pilotId >= 0) {
        ctx[QStringLiteral("pilot_id")] = pilotId;
        if (!pilotName.trimmed().isEmpty()) {
            ctx[QStringLiteral("pilot_name")] = pilotName.trimmed();
        }
    }
    if (assetId >= 0) {
        ctx[QStringLiteral("asset_id")] = assetId;
        for (const QVariant &assetValue : _assets) {
            const QVariantMap asset = assetValue.toMap();
            if (asset.value(QStringLiteral("id")).toInt() == assetId) {
                const QString name = asset.value(QStringLiteral("name")).toString();
                if (!name.isEmpty()) {
                    ctx[QStringLiteral("aircraft_name")] = name;
                }
                break;
            }
        }
    }
    if (!site.isEmpty())       ctx[QStringLiteral("site")]        = site;
    if (!flightType.isEmpty()) ctx[QStringLiteral("flight_type")] = flightType;
    if (!crew.isEmpty())       ctx[QStringLiteral("crew")]        = crew;

    _pilotName = (pilotId >= 0) ? pilotName : QString();
    _runContext = ctx;

    // Site, flight type, crew and aircraft are remembered - they are not
    // identity, and a crew flying ten times at one site should type it once.
    FleetStore::setLastSite(site);
    FleetStore::setLastFlightType(flightType);
    FleetStore::setLastCrew(crew);
    FleetStore::setLastAssetId(assetId);
    FleetStore::setCrewScreenShownAt(QDateTime::currentSecsSinceEpoch());

    _crewScreenPending = false;
    emit runContextChanged();
}

void FleetConnector::setFlightTypeOther(const QString &value)
{
    if (flightType() == QLatin1String("other") && !value.trimmed().isEmpty()) {
        _runContext.insert(QStringLiteral("flight_type_other"), value.trimmed());
    } else {
        _runContext.remove(QStringLiteral("flight_type_other"));
    }
    emit runContextChanged();
}

void FleetConnector::dismissCrewScreen()
{
    // Everything except identity survives a dismissal.
    QJsonObject ctx;
    const int    asset = FleetStore::lastAssetId();
    const QString s    = FleetStore::lastSite();
    const QString ft   = FleetStore::lastFlightType();
    const QString cw   = FleetStore::lastCrew();
    if (asset >= 0)     ctx[QStringLiteral("asset_id")]    = asset;
    if (!s.isEmpty())   ctx[QStringLiteral("site")]        = s;
    if (!ft.isEmpty())  ctx[QStringLiteral("flight_type")] = ft;
    if (!cw.isEmpty())  ctx[QStringLiteral("crew")]        = cw;

    _pilotName.clear();
    _runContext = ctx;      // no pilot_id, on purpose
    FleetStore::setCrewScreenShownAt(QDateTime::currentSecsSinceEpoch());
    _crewScreenPending = false;
    emit runContextChanged();
}

void FleetConnector::openCrewScreen()
{
    _crewScreenPending = true;
    emit runContextChanged();
}

// ---------------------------------------------------------------- checklists

QVariantList FleetConnector::preflightTemplates() const
{
    return _checklists->templates(QStringLiteral("preflight"));
}

QVariantList FleetConnector::postflightTemplates() const
{
    return _checklists->templates(QStringLiteral("postflight"));
}

int FleetConnector::outboxCount() const
{
    return _checklists->outboxCount();
}

// ------------------------------------------------------------- flight logs

QVariantList FleetConnector::flightLogs() const   { return _logs->model(); }
int  FleetConnector::flightLogsWaiting() const    { return _logs->waitingCount(); }

bool FleetConnector::collectLogsAutomatically() const
{
    return FleetStore::collectLogsAutomatically();
}

void FleetConnector::setCollectLogsAutomatically(bool on)
{
    if (on == FleetStore::collectLogsAutomatically()) {
        return;
    }
    FleetStore::setCollectLogsAutomatically(on);
    emit flightLogsChanged();
}

void FleetConnector::attachFlightLog(const QString &externalRef, const QString &filePath)
{
    _logs->attachFile(externalRef, filePath);
}

void FleetConnector::cancelFlightLogTransfer() { _logs->cancelCurrent(); }
void FleetConnector::retryFlightLog(const QString &ref)  { _logs->retry(ref); }
void FleetConnector::forgetFlightLog(const QString &ref) { _logs->forget(ref); }

QString FleetConnector::newChecklistRef(const QString &kind, int sysid) const
{
    return FleetChecklists::makeExternalRef(FleetStore::stationName(), kind, sysid);
}

QString FleetConnector::flightRefForChecklist(const QString &kind, int sysid)
{
    return _telemetry->flightRefForChecklist(kind, sysid, _runContext);
}

void FleetConnector::dismissChecklistOffer()
{
    _checklistOffer.clear();
    emit checklistsChanged();
}

void FleetConnector::fileChecklist(const QString &externalRef, const QString &flightRef,
                                   int templateId, const QString &kind,
                                   const QString &name, const QString &aircraft,
                                   double started, bool passed,
                                   const QVariantList &items, const QString &notes)
{
    QJsonObject run;
    run[QStringLiteral("external_ref")] = externalRef;
    run[QStringLiteral("flight_ref")] = flightRef;
    if (templateId > 0) {
        run[QStringLiteral("template_id")] = templateId;
    }
    run[QStringLiteral("kind")] = kind;
    run[QStringLiteral("name")] = name;
    run[QStringLiteral("completed_at")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    Q_UNUSED(started)
    Q_UNUSED(passed)
    Q_UNUSED(aircraft)
    Q_UNUSED(notes)

    QJsonArray jitems;
    for (const QVariant &v : items) {
        const QVariantMap m = v.toMap();
        QJsonObject j;
        j[QStringLiteral("text")] = m.value(QStringLiteral("text")).toString();
        QString result = m.value(QStringLiteral("result")).toString();
        if (result.isEmpty()) {
            result = m.value(QStringLiteral("ok")).toBool()
                ? QStringLiteral("pass") : QStringLiteral("fail");
        }
        j[QStringLiteral("result")] = result;
        j[QStringLiteral("note")] = m.value(QStringLiteral("note")).toString();
        jitems.append(j);
    }
    run[QStringLiteral("results")] = jitems;

    _checklists->file(run);
    _checklistOffer.clear();
    emit checklistsChanged();
}

void FleetConnector::_watchArmState()
{
    bool anyArmed = false;
    MultiVehicleManager *mvm = MultiVehicleManager::instance();
    if (QmlObjectListModel *list = mvm ? mvm->vehicles() : nullptr) {
        for (int i = 0; i < list->count(); ++i) {
            const Vehicle *v = qobject_cast<Vehicle *>((*list)[i]);
            if (v && v->armed()) {
                anyArmed = true;
                break;
            }
        }
    }
    if (anyArmed == _anyArmed) {
        return;
    }
    _anyArmed = anyArmed;

    if (anyArmed) {
        // A sortie has started; the pre-flight moment has passed.
        _postflightTimer.stop();
        if (_checklistOffer == QLatin1String("preflight")) {
            _checklistOffer.clear();
            emit checklistsChanged();
        }
    } else {
        _preflightOfferedThisSortie = false;    // ready to offer again next sortie
        _postflightTimer.start();
    }
}

void FleetConnector::_evaluateCrewScreenDue()
{
    if (!FleetStore::enrolled()) {
        return;
    }
    // Opens on every launch while a Fleet Server is connected. A ground
    // station is a shared laptop - two crews in a shift, three in a bad week -
    // and the whole point of pilot_id is that the record says which of them
    // flew. The session-crossed-midnight and ran-past-12-hours cases are
    // covered by the same rule, because the connector is created once per run.
    _crewScreenPending = true;
    emit runContextChanged();
}

void FleetConnector::_telemetryTick()
{
    if (!FleetStore::enrolled() || _suspended || _state == Revoked) {
        return;
    }
    const QJsonObject local = _telemetry->buildReport(_runContext, 500);
    const QJsonArray samples = local.value(QStringLiteral("samples")).toArray();
    _reportedVehicles = samples.size();

    _watchArmState();
    if (!_anyArmed && !_preflightOfferedThisSortie && _checklistOffer.isEmpty()
        && _reportedVehicles > 0 && _checklists->hasTemplates(QStringLiteral("preflight"))) {
        _preflightOfferedThisSortie = true;
        _checklistOffer = QStringLiteral("preflight");
        emit checklistsChanged();
    }

    if (!samples.isEmpty() && !FleetStore::enqueueReportSamples(samples)) {
        _setStatus(tr("The offline telemetry queue is full; new samples cannot be saved."));
        _log(QStringLiteral("telemetry queue reached its storage limit"));
        return;
    }
    if (_state != Connected || _telemetryInFlight) {
        return;
    }
    const QJsonArray batch = FleetStore::reportBatch(500);
    if (batch.isEmpty()) {
        return;
    }
    QJsonObject body;
    body.insert(QStringLiteral("samples"), batch);
    _telemetryInFlight = true;
    _client->post(QStringLiteral("/api/gc/v1/report"), body, [this, batch](const FleetReply &r) {
        _telemetryInFlight = false;
        if (r.netOk && r.status == 400) {
            FleetStore::acknowledgeReportSamples(batch.size());
            const QString reason = r.error.isEmpty()
                ? tr("The server rejected a telemetry batch; that batch was discarded.")
                : tr("The server rejected a telemetry batch; that batch was discarded. %1")
                      .arg(r.error);
            _setStatus(reason);
            _log(reason);
            return;
        }
        if (!_handleCommon(r)) {
            return;
        }
        const int taken = r.body.value(QStringLiteral("taken")).toInt(-1);
        const int skipped = r.body.value(QStringLiteral("skipped")).toInt(-1);
        const int refused = r.body.value(QStringLiteral("refused")).toInt(-1);
        const int acknowledged = taken + skipped + refused;
        if (taken < 0 || skipped < 0 || refused < 0 || acknowledged != batch.size()) {
            _log(QStringLiteral("invalid report acknowledgement from the Fleet Server"));
            return;
        }
        if (acknowledged > 0) {
            FleetStore::acknowledgeReportSamples(acknowledged);
        }
        if (refused > 0) {
            _log(QStringLiteral("the Fleet Server refused %1 telemetry sample(s)").arg(refused));
        }
        _lastReportAt = QDateTime::currentDateTimeUtc();
        _setError({});
        _setStatus({});
        if (_state != Connected) {
            _setState(Connected);
        }
        emit tickChanged();
        _flushFlightDetails();
    });
}

void FleetConnector::_flushFlightDetails()
{
    if (!FleetStore::enrolled() || _state != Connected) {
        return;
    }
    const QJsonArray pending = FleetStore::pendingFlightDetails();
    for (const QJsonValue &value : pending) {
        const QJsonObject item = value.toObject();
        const QString ref = item.value(QStringLiteral("flight_ref")).toString();
        if (ref.isEmpty()) {
            continue;
        }
        const QString path = QStringLiteral("/api/gc/v1/flights/%1")
            .arg(QString::fromUtf8(QUrl::toPercentEncoding(ref)));
        _client->put(path, item.value(QStringLiteral("details")).toObject(),
                     [this, ref](const FleetReply &reply) {
            if (reply.netOk && reply.status == 200) {
                FleetStore::completeFlightDetails(ref);
            } else if (reply.netOk && reply.status == 400) {
                FleetStore::completeFlightDetails(ref);
                const QString reason = tr("The server rejected flight details for %1: %2")
                                           .arg(ref, reply.error);
                _setStatus(reason);
                _log(reason);
            } else if (reply.status != 404) {
                _handleCommon(reply);
            }
        });
    }
}

bool FleetConnector::_handleCommon(const FleetReply &r)
{
    // A reply from a session that has been torn down must not touch the state
    // machine. Disconnect stops every timer, but a request already on the wire
    // still finishes - up to a timeout later - and without this it drags the
    // connector back out of NOT CONNECTED into UNREACHABLE with no credentials.
    // The same guard covers enrolling against a different server while an old
    // reply is still pending, which is exactly what a re-enrollment is.
    if (!FleetStore::enrolled() && _state != Enrolling) {
        return false;
    }

    if (!r.netOk) {
        // No HTTP answer at all: DNS, connect, timeout. Retry with backoff and
        // show one line - not a dialog. They are flying.
        _setStatus(r.error);
        if (_state != Unreachable) {
            _setState(Unreachable);
        }
        _scheduleReconnect();
        return false;
    }
    if (r.status == 401) {
        // An administrator revoked this station. Stop reporting, say so, and
        // do NOT retry in a loop.
        _setError(tr("This ground station was removed from the RotorLab Server."));
        _setState(Revoked);
        return false;
    }
    if (r.status == 409) {
        // The server has no Fleet Server license. Keep retrying without
        // blocking flight operations.
        _setStatus(r.error.isEmpty() ? tr("this RotorLab Server has no Fleet Server license") : r.error);
        if (_state != Unreachable) {
            _setState(Unreachable);
        }
        _scheduleReconnect();
        return false;
    }
    if (r.status >= 500) {
        _setStatus(tr("The RotorLab Server returned an error (%1).").arg(r.status));
        if (_state != Unreachable) {
            _setState(Unreachable);
        }
        _scheduleReconnect();
        return false;
    }
    if (r.status == 400) {
        // Malformed request - our bug. Log it loudly; it is not a connection
        // problem and must not send us into a reconnect loop.
        _log(QStringLiteral("400 from the Fleet Server: %1").arg(r.error));
        return false;
    }

    _setStatus({});
    return true;
}

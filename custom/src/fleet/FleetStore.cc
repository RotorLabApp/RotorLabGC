/****************************************************************************
 *
 * RotorLab GC Fleet Connector - persistence.
 *
 ****************************************************************************/

#include "FleetStore.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QSettings>
#include <QtCore/QStringList>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#endif

namespace {

constexpr auto kGroup      = "FleetConnector";
constexpr auto kBaseUrl    = "baseUrl";
constexpr auto kToken      = "token";           // DPAPI-sealed on Windows; plaintext elsewhere (see FleetStore.h)
constexpr auto kStationName = "stationName";
constexpr auto kStationId  = "stationId";
constexpr auto kServerPin  = "serverPin";       // lowercase hex SPKI SHA-256, or absent
constexpr auto kRoster     = "roster";
constexpr auto kTemplates  = "checklistTemplates";
constexpr auto kReportQueue = "reportQueue";
constexpr auto kSampleSequence = "sampleSequence";
constexpr auto kFlightStates = "flightStates";
constexpr auto kFlightDetails = "flightDetails";
constexpr auto kReportedFlightRefs = "reportedFlightRefs";
constexpr auto kLastSite   = "lastSite";
constexpr auto kLastType   = "lastFlightType";
constexpr auto kLastCrew   = "lastCrew";
constexpr auto kLastAsset  = "lastAssetId";
constexpr auto kRecentSites = "recentSites";
constexpr auto kRecentCrews = "recentCrews";
constexpr auto kCrewShown  = "crewScreenShownAt";
constexpr auto kOutbox     = "outbox";
constexpr auto kLogQueue   = "logQueue";
constexpr auto kCollectLogs = "collectLogs";

constexpr int kRecentMax = 8;
constexpr int kMaxQueuedSamples = 100000;
constexpr int kMaxReportedFlightRefs = 2048;

QVariant readValue(const char *key, const QVariant &fallback = {})
{
    QSettings s;
    s.beginGroup(QLatin1String(kGroup));
    return s.value(QLatin1String(key), fallback);
}

void writeValue(const char *key, const QVariant &value)
{
    QSettings s;
    s.beginGroup(QLatin1String(kGroup));
    s.setValue(QLatin1String(key), value);
    s.endGroup();
    // Flush now. The same lesson as the license record: a value that only
    // reaches disk on a clean exit is a value lost to the first crash, and
    // re-enrolling means a human generating another code.
    s.sync();
}

/// Push `value` to the front of a small most-recently-used list.
void noteRecent(const char *key, const QString &value)
{
    if (value.trimmed().isEmpty()) {
        return;
    }
    QStringList list = readValue(key).toStringList();
    list.removeAll(value);
    list.prepend(value);
    while (list.size() > kRecentMax) {
        list.removeLast();
    }
    writeValue(key, list);
}

} // namespace

// ---------------------------------------------------------------- token seal

QByteArray FleetStore::_seal(const QString &plain)
{
    const QByteArray raw = plain.toUtf8();
    if (raw.isEmpty()) {
        return {};
    }
#ifdef Q_OS_WIN
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(raw.constData()));
    in.cbData = static_cast<DWORD>(raw.size());

    DATA_BLOB out{};
    // CRYPTPROTECT_UI_FORBIDDEN: this runs on timers and must never block on a
    // prompt. User-scoped by default, so the ini is useless to another account.
    if (::CryptProtectData(&in, L"RotorLab GC Fleet Connector", nullptr, nullptr,
                           nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        const QByteArray sealed(reinterpret_cast<const char *>(out.pbData),
                                static_cast<int>(out.cbData));
        ::LocalFree(out.pbData);
        return sealed;
    }
    // Sealing failed. Storing the token in the clear instead would silently
    // downgrade the protection we promised, so store nothing and let the
    // operator re-enroll.
    return {};
#else
    // Not Windows: the token is stored as is (plaintext). On Android the
    // settings file is in app-private storage; on Linux and macOS only the
    // user account's file permissions protect it. Encrypting here without an
    // OS key store would only put the key beside the thing it encrypts.
    return raw;
#endif
}

QString FleetStore::_unseal(const QByteArray &sealed)
{
    if (sealed.isEmpty()) {
        return {};
    }
#ifdef Q_OS_WIN
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(sealed.constData()));
    in.cbData = static_cast<DWORD>(sealed.size());

    DATA_BLOB out{};
    if (::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                             CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        const QString plain = QString::fromUtf8(reinterpret_cast<const char *>(out.pbData),
                                                static_cast<int>(out.cbData));
        ::SecureZeroMemory(out.pbData, out.cbData);
        ::LocalFree(out.pbData);
        return plain;
    }
    return {};
#else
    return QString::fromUtf8(sealed);
#endif
}

// ---------------------------------------------------------------- identity

QString FleetStore::baseUrl()                       { return readValue(kBaseUrl).toString(); }
void    FleetStore::setBaseUrl(const QString &url)  { writeValue(kBaseUrl, url); }

QString FleetStore::token()
{
    return _unseal(readValue(kToken).toByteArray());
}

void FleetStore::setToken(const QString &token)
{
    writeValue(kToken, _seal(token));
}

QString FleetStore::stationName()                        { return readValue(kStationName).toString(); }
void    FleetStore::setStationName(const QString &name)  { writeValue(kStationName, name); }

int     FleetStore::stationId()                          { return readValue(kStationId, 0).toInt(); }
void    FleetStore::setStationId(int id)                 { writeValue(kStationId, id); }

QString FleetStore::serverPin()                          { return readValue(kServerPin).toString(); }
void    FleetStore::setServerPin(const QString &pin)     { writeValue(kServerPin, pin); }

bool    FleetStore::enrolled()                           { return !token().isEmpty(); }

// ---------------------------------------------------------------- caches

QByteArray FleetStore::rosterJson()                         { return readValue(kRoster).toByteArray(); }
void       FleetStore::setRosterJson(const QByteArray &j)   { writeValue(kRoster, j); }

QByteArray FleetStore::checklistTemplatesJson()                       { return readValue(kTemplates).toByteArray(); }
void       FleetStore::setChecklistTemplatesJson(const QByteArray &j) { writeValue(kTemplates, j); }

bool FleetStore::enqueueReportSamples(QJsonArray samples)
{
    QJsonArray queue = QJsonDocument::fromJson(readValue(kReportQueue).toByteArray()).array();
    if (queue.size() + samples.size() > kMaxQueuedSamples) {
        return false;
    }

    qint64 sequence = readValue(kSampleSequence, 0).toLongLong();
    for (int i = 0; i < samples.size(); ++i) {
        QJsonObject sample = samples.at(i).toObject();
        sample.insert(QStringLiteral("seq"), double(++sequence));
        queue.append(sample);
    }
    writeValue(kSampleSequence, sequence);
    writeValue(kReportQueue, QJsonDocument(queue).toJson(QJsonDocument::Compact));
    return true;
}

QJsonArray FleetStore::reportBatch(int maxSamples)
{
    const QJsonArray queue = QJsonDocument::fromJson(readValue(kReportQueue).toByteArray()).array();
    QJsonArray batch;
    const int count = qMin(qMax(maxSamples, 0), queue.size());
    for (int i = 0; i < count; ++i) {
        batch.append(queue.at(i));
    }
    return batch;
}

void FleetStore::acknowledgeReportSamples(int count)
{
    QJsonArray queue = QJsonDocument::fromJson(readValue(kReportQueue).toByteArray()).array();
    const int removeCount = qMin(qMax(count, 0), queue.size());
    for (int i = 0; i < removeCount; ++i) {
        queue.removeAt(0);
    }
    writeValue(kReportQueue, QJsonDocument(queue).toJson(QJsonDocument::Compact));
}

QJsonObject FleetStore::flightState(int sysid)
{
    const QJsonObject states =
        QJsonDocument::fromJson(readValue(kFlightStates).toByteArray()).object();
    return states.value(QString::number(sysid)).toObject();
}

void FleetStore::setFlightState(int sysid, const QJsonObject &state)
{
    QJsonObject states =
        QJsonDocument::fromJson(readValue(kFlightStates).toByteArray()).object();
    states.insert(QString::number(sysid), state);
    writeValue(kFlightStates, QJsonDocument(states).toJson(QJsonDocument::Compact));
}

QJsonArray FleetStore::pendingFlightDetails()
{
    return QJsonDocument::fromJson(readValue(kFlightDetails).toByteArray()).array();
}

void FleetStore::enqueueFlightDetails(const QJsonObject &details)
{
    QJsonArray queue = pendingFlightDetails();
    const QString ref = details.value(QStringLiteral("flight_ref")).toString();
    if (ref.isEmpty()) {
        return;
    }
    for (const QJsonValue &value : queue) {
        if (value.toObject().value(QStringLiteral("flight_ref")).toString() == ref) {
            return;
        }
    }
    queue.append(details);
    writeValue(kFlightDetails, QJsonDocument(queue).toJson(QJsonDocument::Compact));
}

void FleetStore::completeFlightDetails(const QString &flightRef)
{
    QJsonArray queue = pendingFlightDetails();
    for (int i = queue.size() - 1; i >= 0; --i) {
        if (queue.at(i).toObject().value(QStringLiteral("flight_ref")).toString() == flightRef) {
            queue.removeAt(i);
        }
    }
    writeValue(kFlightDetails, QJsonDocument(queue).toJson(QJsonDocument::Compact));
}

bool FleetStore::flightRefReported(const QString &flightRef)
{
    return QJsonDocument::fromJson(readValue(kReportedFlightRefs).toByteArray())
        .array().contains(QJsonValue(flightRef));
}

void FleetStore::markFlightRefsReported(const QStringList &flightRefs)
{
    QJsonArray refs =
        QJsonDocument::fromJson(readValue(kReportedFlightRefs).toByteArray()).array();
    for (const QString &ref : flightRefs) {
        if (!ref.isEmpty() && !refs.contains(QJsonValue(ref))) {
            refs.append(ref);
        }
    }
    while (refs.size() > kMaxReportedFlightRefs) {
        refs.removeAt(0);
    }
    writeValue(kReportedFlightRefs, QJsonDocument(refs).toJson(QJsonDocument::Compact));
}

void FleetStore::resetStationState()
{
    for (const char *key : {kRoster, kTemplates, kReportQueue, kSampleSequence,
                            kFlightStates, kFlightDetails, kReportedFlightRefs, kOutbox,
                            kLogQueue}) {
        writeValue(key, {});
    }
    setLastAssetId(-1);
}

QByteArray FleetStore::outboxJson()                         { return readValue(kOutbox).toByteArray(); }
void       FleetStore::setOutboxJson(const QByteArray &j)   { writeValue(kOutbox, j); }

QByteArray FleetStore::logQueueJson()                       { return readValue(kLogQueue).toByteArray(); }
void       FleetStore::setLogQueueJson(const QByteArray &j) { writeValue(kLogQueue, j); }

bool FleetStore::collectLogsAutomatically()             { return readValue(kCollectLogs, true).toBool(); }
void FleetStore::setCollectLogsAutomatically(bool on)   { writeValue(kCollectLogs, on); }

// ---------------------------------------------------------------- run context

QString FleetStore::lastSite()                          { return readValue(kLastSite).toString(); }
void    FleetStore::setLastSite(const QString &site)    { writeValue(kLastSite, site); noteRecent(kRecentSites, site); }

QString FleetStore::lastFlightType()                    { return readValue(kLastType).toString(); }
void    FleetStore::setLastFlightType(const QString &t) { writeValue(kLastType, t); }

QString FleetStore::lastCrew()                          { return readValue(kLastCrew).toString(); }
void    FleetStore::setLastCrew(const QString &crew)    { writeValue(kLastCrew, crew); noteRecent(kRecentCrews, crew); }

int     FleetStore::lastAssetId()                       { return readValue(kLastAsset, -1).toInt(); }
void    FleetStore::setLastAssetId(int id)              { writeValue(kLastAsset, id); }

QStringList FleetStore::recentSites()                   { return readValue(kRecentSites).toStringList(); }
void        FleetStore::noteSite(const QString &site)   { noteRecent(kRecentSites, site); }
QStringList FleetStore::recentCrews()                   { return readValue(kRecentCrews).toStringList(); }
void        FleetStore::noteCrew(const QString &crew)   { noteRecent(kRecentCrews, crew); }

qint64 FleetStore::crewScreenShownAt()                  { return readValue(kCrewShown, 0).toLongLong(); }
void   FleetStore::setCrewScreenShownAt(qint64 epoch)   { writeValue(kCrewShown, epoch); }

// ---------------------------------------------------------------- teardown

void FleetStore::clearAll()
{
    QSettings s;
    s.beginGroup(QLatin1String(kGroup));
    s.remove(QString());        // the whole group
    s.endGroup();
    s.sync();
}

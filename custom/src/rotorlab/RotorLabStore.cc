/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - persistence.
 *
 ****************************************************************************/

#include "RotorLabStore.h"
#include "Credential.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtCore/QVariant>

namespace {

constexpr auto kGroup       = "RotorLabConnector";
constexpr auto kEnabled     = "enabled";
constexpr auto kKeyLabel    = "keyLabel";
constexpr auto kPilots      = "pilots";
constexpr auto kAircraft    = "aircraft";
constexpr auto kTemplates   = "templates";
constexpr auto kAirframeMap = "airframeMap";
constexpr auto kReadiness   = "readiness";
constexpr auto kAutoFile    = "fileFlights";
constexpr auto kUploadLogs  = "uploadLogs";
constexpr auto kAskCrew     = "askWhoIsFlying";
constexpr auto kJurisdiction = "jurisdiction";
constexpr auto kCategory    = "operatingCategory";
constexpr auto kVisualMode  = "visualMode";
constexpr auto kLastSite    = "lastSite";
constexpr auto kLastType    = "lastFlightType";
constexpr auto kLastTypeOther = "lastFlightTypeOther";
constexpr auto kPilotNotices  = "filingPilotNotices";
constexpr auto kOtherNotices  = "filingOtherNotices";
constexpr auto kLastCrew    = "lastCrew";
constexpr auto kInstallId   = "installId";

/// The one spelling of the credential-store account name.
constexpr auto kKeyAccount  = "Device Key";

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
    // Flush now. A value that only reaches disk on a clean exit is a value
    // lost to the first crash.
    s.sync();
}

} // namespace

// ------------------------------------------------------------------ connection

bool    RotorLabStore::enabled()                 { return readValue(kEnabled, false).toBool(); }
void    RotorLabStore::setEnabled(bool on)       { writeValue(kEnabled, on); }

QString RotorLabStore::baseUrl()
{
#ifdef QGC_DAILY_BUILD
    // Development builds only, so the connector can be exercised against a
    // local RotorLab. This whole branch is compiled out of a stable build:
    // a released ground station has exactly one address it can talk to, and
    // no environment variable, settings file or hand edit changes that.
    const QByteArray override = qgetenv("ROTORLAB_BASE_URL");
    if (!override.isEmpty()) {
        return QString::fromUtf8(override);
    }
#endif
    return QStringLiteral("https://app.rotorlab.app");
}

QString RotorLabStore::keyAccount()
{
#ifdef QGC_UNITTEST_BUILD
    if (QCoreApplication::applicationName().endsWith(QLatin1String("_unittest"))) {
        return QStringLiteral("Unit test Device Key %1").arg(QCoreApplication::applicationPid());
    }
#endif
    return QLatin1String(kKeyAccount);
}

QString RotorLabStore::keyLabel()                { return readValue(kKeyLabel).toString(); }
void    RotorLabStore::setKeyLabel(const QString &l) { writeValue(kKeyLabel, l); }

// ---------------------------------------------------------------------- caches

QByteArray RotorLabStore::pilotsJson()                        { return readValue(kPilots).toByteArray(); }
void       RotorLabStore::setPilotsJson(const QByteArray &j)  { writeValue(kPilots, j); }

QByteArray RotorLabStore::aircraftJson()                       { return readValue(kAircraft).toByteArray(); }
void       RotorLabStore::setAircraftJson(const QByteArray &j) { writeValue(kAircraft, j); }

QByteArray RotorLabStore::templatesJson()                       { return readValue(kTemplates).toByteArray(); }
void       RotorLabStore::setTemplatesJson(const QByteArray &j) { writeValue(kTemplates, j); }

QByteArray RotorLabStore::readinessJson()                        { return readValue(kReadiness).toByteArray(); }
void       RotorLabStore::setReadinessJson(const QByteArray &j)  { writeValue(kReadiness, j); }

QByteArray RotorLabStore::airframeMapJson()                       { return readValue(kAirframeMap).toByteArray(); }
void       RotorLabStore::setAirframeMapJson(const QByteArray &j) { writeValue(kAirframeMap, j); }

// ------------------------------------------------------------------- behaviour

bool RotorLabStore::fileFlightsAutomatically()        { return readValue(kAutoFile, true).toBool(); }
void RotorLabStore::setFileFlightsAutomatically(bool on) { writeValue(kAutoFile, on); }

bool RotorLabStore::uploadLogs()                      { return readValue(kUploadLogs, true).toBool(); }
void RotorLabStore::setUploadLogs(bool on)            { writeValue(kUploadLogs, on); }

bool RotorLabStore::askWhoIsFlying()                  { return readValue(kAskCrew, true).toBool(); }
void RotorLabStore::setAskWhoIsFlying(bool on)        { writeValue(kAskCrew, on); }

// ---------------------------------------------------------------- compliance

QString RotorLabStore::jurisdiction()                       { return readValue(kJurisdiction).toString(); }
void    RotorLabStore::setJurisdiction(const QString &c)    { writeValue(kJurisdiction, c); }

QString RotorLabStore::operatingCategory()                  { return readValue(kCategory).toString(); }
void    RotorLabStore::setOperatingCategory(const QString &c) { writeValue(kCategory, c); }

QString RotorLabStore::visualMode()                         { return readValue(kVisualMode).toString(); }
void    RotorLabStore::setVisualMode(const QString &m)      { writeValue(kVisualMode, m); }

// --------------------------------------------------------------- run context

QString RotorLabStore::lastSite()                       { return readValue(kLastSite).toString(); }
void    RotorLabStore::setLastSite(const QString &s)    { writeValue(kLastSite, s); }

QString RotorLabStore::lastFlightType()                 { return readValue(kLastType).toString(); }
void    RotorLabStore::setLastFlightType(const QString &t) { writeValue(kLastType, t); }
QString RotorLabStore::lastFlightTypeOther()            { return readValue(kLastTypeOther).toString(); }
void    RotorLabStore::setLastFlightTypeOther(const QString &t) { writeValue(kLastTypeOther, t); }
QStringList RotorLabStore::filingPilotNotices()           { return readValue(kPilotNotices).toStringList(); }
void RotorLabStore::setFilingPilotNotices(const QStringList &n) { writeValue(kPilotNotices, n); }
QStringList RotorLabStore::filingOtherNotices()           { return readValue(kOtherNotices).toStringList(); }
void RotorLabStore::setFilingOtherNotices(const QStringList &n) { writeValue(kOtherNotices, n); }

QString RotorLabStore::lastCrew()                       { return readValue(kLastCrew).toString(); }
void    RotorLabStore::setLastCrew(const QString &c)    { writeValue(kLastCrew, c); }

// ------------------------------------------------------------------- teardown

bool RotorLabStore::configured()
{
    return !Credential::load(keyAccount()).isEmpty();
}

QString RotorLabStore::installId()
{
    static const QRegularExpression valid(QStringLiteral("^[0-9a-f]{8}$"));
    const QString stored = readValue(kInstallId).toString();
    if (valid.match(stored).hasMatch()) {
        return stored;
    }
    // Random, never derived from the machine or the user.
    const QString made = QStringLiteral("%1").arg(QRandomGenerator::system()->generate(),
                                                  8, 16, QLatin1Char('0'));
    writeValue(kInstallId, made);
    return made;
}

QString RotorLabStore::formatExternalRef(const QString &installId, const QDate &dayUtc,
                                         const QString &uuid12)
{
    return QStringLiteral("gc-%1-%2-%3")
        .arg(installId, dayUtc.toString(QStringLiteral("yyyy-MM-dd")), uuid12);
}

void RotorLabStore::clearAll()
{
    Credential::erase(keyAccount());

    // The install id is not about the key: it stays, so references minted
    // after a reconnect still read as this install's.
    const QString install = readValue(kInstallId).toString();

    QSettings s;
    s.beginGroup(QLatin1String(kGroup));
    s.remove(QString());        // the whole group
    s.endGroup();
    s.sync();
    if (!install.isEmpty()) {
        writeValue(kInstallId, install);
    }
    // The spool is deliberately untouched. A Disconnect that silently binned
    // four unfiled flights would be unforgivable.
}

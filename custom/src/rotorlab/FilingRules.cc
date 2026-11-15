/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - filing rules.
 *
 ****************************************************************************/

#include "FilingRules.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>

namespace RotorLabFiling {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("RotorLabFiling", text);
}

/// The server's line() refuses anything Python's str.isprintable() rejects.
/// Walk code points (a surrogate pair is one), turn every Unicode space,
/// line or paragraph separator into U+0020, and drop control, format,
/// surrogate, private-use and unassigned code points.
QList<char32_t> printableCodePoints(const QString &text)
{
    QList<char32_t> out;
    const auto ucs = text.toUcs4();
    out.reserve(ucs.size());
    for (const auto u : ucs) {
        const char32_t cp = static_cast<char32_t>(u);
        if (QChar::isSpace(cp)) {
            out.append(U' ');
            continue;
        }
        switch (QChar::category(cp)) {
        case QChar::Other_Control:
        case QChar::Other_Format:
        case QChar::Other_Surrogate:
        case QChar::Other_PrivateUse:
        case QChar::Other_NotAssigned:
            continue;
        default:
            out.append(cp);
        }
    }
    return out;
}

QString fromCodePoints(const QList<char32_t> &cps)
{
    return QString::fromUcs4(cps.constData(), cps.size());
}

} // namespace

QStringList flightTypeCodes()
{
    return { QStringLiteral("inspection"),  QStringLiteral("survey"),
             QStringLiteral("training"),    QStringLiteral("test"),
             QStringLiteral("commercial"),  QStringLiteral("recreational"),
             QStringLiteral("proficiency"), QStringLiteral("maintenance"),
             QStringLiteral("public_safety"), QStringLiteral("other") };
}

QString flightTypeLabel(const QString &code)
{
    if (code == QLatin1String("inspection"))    return tr("Inspection");
    if (code == QLatin1String("survey"))        return tr("Survey");
    if (code == QLatin1String("training"))      return tr("Training");
    if (code == QLatin1String("test"))          return tr("Test");
    if (code == QLatin1String("commercial"))    return tr("Commercial");
    if (code == QLatin1String("recreational"))  return tr("Recreational");
    if (code == QLatin1String("proficiency"))   return tr("Proficiency");
    if (code == QLatin1String("maintenance"))   return tr("Maintenance");
    if (code == QLatin1String("public_safety")) return tr("Public safety");
    if (code == QLatin1String("other"))         return tr("Other");
    return code;
}

QString flightTypeOtherFallback()
{
    // Sent to the server as data, not shown as a sentence, so it stays in
    // English like the codes it travels with.
    return QStringLiteral("Not specified");
}

QString cleanFlightTypeOther(const QString &text)
{
    // One space between words, none at the ends, and the 80 counted in code
    // points as the server counts them, so a surrogate pair is never split.
    QList<char32_t> out;
    for (const char32_t cp : printableCodePoints(text)) {
        if (cp == U' ' && (out.isEmpty() || out.last() == U' ')) {
            continue;
        }
        out.append(cp);
    }
    if (out.size() > kFlightTypeOtherMax) {
        out.resize(kFlightTypeOtherMax);
    }
    while (!out.isEmpty() && out.last() == U' ') {
        out.removeLast();
    }
    return fromCodePoints(out);
}

FlightType normalizeFlightType(const QString &type, const QString &other,
                               bool forSending)
{
    // Matched as the server's _kind_words does: case, spaces and hyphens do
    // not matter, so "Public safety" and "public-safety" are public_safety.
    static const QRegularExpression joiners(QStringLiteral("[\\s-]+"),
                                            QRegularExpression::UseUnicodePropertiesOption);
    const QString t = fromCodePoints(printableCodePoints(type)).trimmed().toLower()
                          .replace(joiners, QStringLiteral("_"));
    if (t.isEmpty()) {
        return {};
    }
    if (t == QLatin1String("other")) {
        QString o = cleanFlightTypeOther(other);
        if (o.isEmpty() && forSending) {
            o = flightTypeOtherFallback();
        }
        return { t, o };
    }
    if (flightTypeCodes().contains(t)) {
        return { t, {} };
    }
    if (t == QLatin1String("mapping")) {
        return { QStringLiteral("survey"), {} };
    }
    // patrol, search, delivery, and anything else no longer on the list:
    // keep the crew's word rather than guess a category for it.
    QString o = cleanFlightTypeOther(type);
    if (o.isEmpty() && forSending) {
        o = flightTypeOtherFallback();
    }
    return { QStringLiteral("other"), o };
}

bool requeueAfterTypeFix(int lastStatus, bool flightTypeChanged)
{
    return flightTypeChanged && (lastStatus == 400 || lastStatus == 0);
}

bool normalizeFlightBody(QJsonObject &body)
{
    const QString typeKey  = QStringLiteral("flight_type");
    const QString otherKey = QStringLiteral("flight_type_other");

    const QJsonObject before = body;
    const FlightType f = normalizeFlightType(body.value(typeKey).toString(),
                                             body.value(otherKey).toString(),
                                             /*forSending*/ true);
    if (f.type.isEmpty()) {
        body.remove(typeKey);
    } else {
        body[typeKey] = f.type;
    }
    // Never with any type but other: the server refuses that combination.
    if (f.other.isEmpty()) {
        body.remove(otherKey);
    } else {
        body[otherKey] = f.other;
    }
    return body != before;
}

QString clipServerText(const QString &text)
{
    const QString t = text.trimmed();
    const auto ucs = t.toUcs4();
    if (ucs.size() <= kServerTextMax) {
        return t;
    }
    return QString::fromUcs4(reinterpret_cast<const char32_t *>(ucs.constData()),
                             kServerTextMax - 1).trimmed()
           + QChar(0x2026);
}

bool versionAtLeast(const QString &version, int major, int minor, int patch)
{
    static const QRegularExpression re(QStringLiteral("^\\s*[vV]?(\\d+)(?:\\.(\\d+))?(?:\\.(\\d+))?"));
    const QRegularExpressionMatch m = re.match(version);
    if (!m.hasMatch()) {
        return false;
    }
    const int have[3] = { m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt() };
    const int want[3] = { major, minor, patch };
    for (int i = 0; i < 3; ++i) {
        if (have[i] != want[i]) {
            return have[i] > want[i];
        }
    }
    return true;
}

bool serverNamesPilots(const QString &version)
{
    return versionAtLeast(version, 3, 5, 2);
}

QString noPilotFallback()
{
    return tr("This flight filed with no pilot named. Name the pilot on the flight in RotorLab.");
}

QString noPilotNeutral()
{
    return tr("This station filed this flight with no pilot named.");
}

QString pilotNotice(const QJsonObject &reply, bool sentPilotId, bool namesPilots)
{
    if (!namesPilots) {
        // An older server sends neither field and does not list the flight
        // for naming; say only what this station did.
        return sentPilotId ? QString() : noPilotNeutral();
    }
    const QString note = clipServerText(reply.value(QStringLiteral("pilot_note")).toString());
    if (reply.contains(QStringLiteral("pilot_named"))) {
        if (reply.value(QStringLiteral("pilot_named")).toBool(true)) {
            return {};
        }
        return note.isEmpty() ? noPilotFallback() : note;
    }
    if (!note.isEmpty()) {
        return note;
    }
    return sentPilotId ? QString() : noPilotFallback();
}

QString flightNoticePrefix(const QString &takeoffIso, const QString &aircraft,
                           const QDate &today)
{
    QStringList parts;
    const QDateTime at = QDateTime::fromString(takeoffIso, Qt::ISODate);
    if (at.isValid()) {
        const QDateTime local = at.toLocalTime();
        parts << local.toString(local.date() == today ? QStringLiteral("HH:mm")
                                                      : QStringLiteral("d MMM HH:mm"));
    }
    const QString name = aircraft.trimmed();
    if (!name.isEmpty()) {
        parts << name;
    }
    return parts.join(QStringLiteral(", "));
}

QStringList parsePackSerials(const QString &text)
{
    // Every Unicode space is already U+0020 here, so it separates like a
    // comma; nothing the server's line() would refuse is left in a serial.
    static const QRegularExpression separators(QStringLiteral("[,; ]+"));
    QStringList out;
    const QString cleaned = fromCodePoints(printableCodePoints(text));
    for (const QString &raw : cleaned.split(separators, Qt::SkipEmptyParts)) {
        const QString serial = raw.trimmed();
        if (serial.isEmpty() || serial.toUcs4().size() > kPackSerialMax
            || out.contains(serial)) {
            continue;
        }
        out << serial;
        if (out.size() == kPackSerialsMax) {
            break;
        }
    }
    return out;
}

qint64 maxLogBytesFrom(const QJsonObject &usageReply)
{
    const double published = usageReply.value(QStringLiteral("max_file_bytes")).toDouble(0);
    return published >= 1 ? qint64(published) : kMaxLogBytes;
}

QString megabytesText(qint64 bytes)
{
    const qint64 mb = (qMax<qint64>(0, bytes) + 1024 * 1024 - 1) / (1024 * 1024);
    return tr("%1 MB").arg(mb);
}

QString logTooLargeText(qint64 bytes, qint64 maxBytes, const QString &filePath)
{
    QString s = tr("This log is %1 and RotorLab takes up to %2 for one file, so it was not sent.")
                    .arg(megabytesText(bytes), megabytesText(maxBytes));
    if (!filePath.isEmpty()) {
        s += QLatin1Char(' ') + tr("Attach it to the flight in RotorLab by hand: %1").arg(filePath);
    }
    return s;
}

QJsonObject retryFlightBody(const QJsonObject &stored, const QJsonObject &fresh)
{
    QJsonObject out = stored;

    auto missing = [&stored](const QString &key) {
        const QJsonValue v = stored.value(key);
        return v.isUndefined() || v.isNull()
               || (v.isString() && v.toString().trimmed().isEmpty());
    };
    auto fill = [&](const char *k) {
        const QString key = QLatin1String(k);
        if (missing(key) && fresh.contains(key)) {
            out[key] = fresh.value(key);
            return true;
        }
        return false;
    };

    fill("pilot_id");
    for (const char *k : { "site", "crew", "jurisdiction", "operating_category", "visual_mode" }) {
        fill(k);
    }
    // The other text belongs to the type it came with: only filled together.
    if (fill("flight_type")) {
        out.remove(QStringLiteral("flight_type_other"));
        if (fresh.contains(QStringLiteral("flight_type_other"))) {
            out[QStringLiteral("flight_type_other")] = fresh.value(QStringLiteral("flight_type_other"));
        }
    }

    // The aircraft: a stored registry id is never replaced. A stored name
    // gains the registry id when one is known now; nothing stored takes
    // whatever is known now.
    if (missing(QStringLiteral("asset_id"))) {
        if (fresh.contains(QStringLiteral("asset_id"))) {
            out[QStringLiteral("asset_id")] = fresh.value(QStringLiteral("asset_id"));
            out.remove(QStringLiteral("asset_name"));
        } else {
            fill("asset_name");
        }
    }

    // Never a new reference: the same external_ref is what makes it the same
    // flight in RotorLab.
    out[QStringLiteral("external_ref")] = stored.value(QStringLiteral("external_ref"));
    // flight_type_other only with other, and the type on the published list.
    normalizeFlightBody(out);
    return out;
}

} // namespace RotorLabFiling

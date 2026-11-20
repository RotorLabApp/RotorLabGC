/****************************************************************************
 *
 * RotorLab GC - the recording queue, persisted (G95).
 *
 ****************************************************************************/

#include "RecordingStore.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSettings>
#include <QtCore/QUuid>

#include <memory>

#include "QGCLoggingCategory.h"

QGC_LOGGING_CATEGORY(VideoRecordLog, "gcs.custom.video.record")

namespace {

std::unique_ptr<QSettings> openSettings(const QString &ini)
{
    if (ini.isEmpty()) {
        return std::make_unique<QSettings>();
    }
    return std::make_unique<QSettings>(ini, QSettings::IniFormat);
}

} // namespace

RecordingStore::RecordingStore(const QString &iniPath)
    : _ini(iniPath)
{
    reload();
}

void RecordingStore::reload()
{
    _entries.clear();
    auto s = openSettings(_ini);
    s->beginGroup(QLatin1String(kGroup));
    const QByteArray json = s->value(QStringLiteral("queue")).toByteArray();
    s->endGroup();
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    for (const QJsonValue &v : doc.array()) {
        if (v.isObject() && !v.toObject().value(QStringLiteral("id")).toString().isEmpty()) {
            _entries.append(v.toObject());
        }
    }
}

void RecordingStore::_save() const
{
    QJsonArray array;
    for (const QJsonObject &e : _entries) {
        array.append(e);
    }
    auto s = openSettings(_ini);
    s->beginGroup(QLatin1String(kGroup));
    s->setValue(QStringLiteral("queue"), QJsonDocument(array).toJson(QJsonDocument::Compact));
    s->endGroup();
    s->sync();
}

QString RecordingStore::newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonObject RecordingStore::entry(const QString &id) const
{
    for (const QJsonObject &e : _entries) {
        if (e.value(QStringLiteral("id")).toString() == id) {
            return e;
        }
    }
    return {};
}

QString RecordingStore::upsert(QJsonObject entry)
{
    QString id = entry.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        id = newId();
        entry[QStringLiteral("id")] = id;
    }
    bool replaced = false;
    for (QJsonObject &e : _entries) {
        if (e.value(QStringLiteral("id")).toString() == id) {
            e = entry;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        _entries.append(entry);
    }
    _save();
    return id;
}

void RecordingStore::remove(const QString &id)
{
    for (qsizetype i = 0; i < _entries.size(); ++i) {
        if (_entries.at(i).value(QStringLiteral("id")).toString() == id) {
            _entries.removeAt(i);
            _save();
            return;
        }
    }
}

QSet<QString> RecordingStore::names() const
{
    QSet<QString> out;
    for (const QJsonObject &e : _entries) {
        out.insert(e.value(QStringLiteral("name")).toString());
    }
    return out;
}

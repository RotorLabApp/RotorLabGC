/****************************************************************************
 *
 * RotorLab GC Fleet Connector - checklists.
 *
 ****************************************************************************/

#include "FleetChecklists.h"
#include "FleetClient.h"
#include "FleetStore.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QUuid>
#include <QtCore/QUrl>
#include <QtCore/QVariantMap>

#include "QGCLoggingCategory.h"

QGC_LOGGING_CATEGORY(FleetChecklistsLog, "gcs.custom.fleet.checklists")

namespace {
constexpr int kOutboxRetryMs = 60 * 1000;
constexpr int kOutboxMax     = 200;
}

FleetChecklists::FleetChecklists(FleetClient *client, QObject *parent)
    : QObject(parent)
    , _client(client)
{
    _outboxTimer.setInterval(kOutboxRetryMs);
    connect(&_outboxTimer, &QTimer::timeout, this, &FleetChecklists::_retryOutbox);
    _outboxTimer.start();
    loadCache();
}

void FleetChecklists::loadCache()
{
    const QByteArray raw = FleetStore::checklistTemplatesJson();
    if (raw.isEmpty()) {
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject o = doc.object();
    _source    = o.value(QStringLiteral("source")).toString();
    _templates = o.value(QStringLiteral("templates")).toArray();
    emit templatesChanged();
}

void FleetChecklists::setTemplates(const QJsonArray &templates)
{
    _source = templates.isEmpty() ? QStringLiteral("none") : QStringLiteral("rotorlab");
    _templates = templates;
    QJsonObject cache;
    cache.insert(QStringLiteral("source"), _source);
    cache.insert(QStringLiteral("templates"), _templates);
    FleetStore::setChecklistTemplatesJson(
        QJsonDocument(cache).toJson(QJsonDocument::Compact));
    emit templatesChanged();
}

void FleetChecklists::reset()
{
    _source.clear();
    _templates = {};
    FleetStore::setChecklistTemplatesJson({});
    FleetStore::setOutboxJson({});
    emit templatesChanged();
    emit outboxChanged();
}

bool FleetChecklists::hasTemplates(const QString &kind) const
{
    return !templates(kind).isEmpty();
}

QVariantList FleetChecklists::templates(const QString &kind) const
{
    QVariantList out;
    if (_source == QLatin1String("none")) {
        return out;
    }
    for (const QJsonValue &v : _templates) {
        const QJsonObject t = v.toObject();
        if (t.value(QStringLiteral("kind")).toString() != kind) {
            continue;
        }
        // A template switched off upstream is not offered.
        if (t.value(QStringLiteral("active")).toInt(1) == 0) {
            continue;
        }
        QVariantMap m;
        m[QStringLiteral("id")]   = t.value(QStringLiteral("id")).toInt();
        m[QStringLiteral("kind")] = t.value(QStringLiteral("kind")).toString();
        m[QStringLiteral("name")] = t.value(QStringLiteral("name")).toString();
        QStringList items;
        for (const QJsonValue &i : t.value(QStringLiteral("items")).toArray()) {
            items << i.toString();
        }
        m[QStringLiteral("items")] = items;
        out.append(m);
    }
    return out;
}

QString FleetChecklists::makeExternalRef(const QString &stationName, const QString &kind, int sysid)
{
    QString station = stationName.isEmpty() ? QStringLiteral("gc") : stationName;
    station.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), QStringLiteral("-"));
    return QStringLiteral("%1-%2-%3-%4")
        .arg(station.left(24), kind, QUuid::createUuid().toString(QUuid::Id128))
        .arg(sysid);
}

void FleetChecklists::file(const QJsonObject &run)
{
    _send(run, false);
}

void FleetChecklists::_send(const QJsonObject &run, bool fromOutbox)
{
    const QString ref = run.value(QStringLiteral("external_ref")).toString();
    const QString flightRef = run.value(QStringLiteral("flight_ref")).toString();
    if (ref.isEmpty() || flightRef.isEmpty()) {
        emit logMessage(QStringLiteral("refusing to file a checklist without its run references"));
        return;
    }
    if (!_client || !FleetStore::enrolled()) {
        if (!fromOutbox) {
            _enqueue(run);
        }
        return;
    }

    QJsonObject body;
    for (const QString &field : {QStringLiteral("external_ref"), QStringLiteral("template_id"),
                                 QStringLiteral("kind"), QStringLiteral("name"),
                                 QStringLiteral("completed_at"), QStringLiteral("results")}) {
        if (run.contains(field)) {
            body.insert(field, run.value(field));
        }
    }
    const QString path = QStringLiteral("/api/gc/v1/flights/%1/checklists")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(flightRef)));
    _client->post(path, body,
                  [this, run, ref, fromOutbox](const FleetReply &r) {
        if (r.netOk && r.status >= 200 && r.status < 300) {
            emit logMessage(QStringLiteral("checklist %1 filed").arg(ref));
            if (fromOutbox) {
                _dropFromOutbox(ref);
            }
            for (const QJsonValue &w : r.body.value(QStringLiteral("warnings")).toArray()) {
                emit logMessage(QStringLiteral("Fleet Server warning: %1").arg(w.toString()));
            }
            return;
        }
        if (r.status == 400) {
            // Our bug, not a transport problem. Queuing it would retry a
            // malformed request for ever.
            emit logMessage(QStringLiteral("the Fleet Server refused checklist %1: %2")
                                .arg(ref, r.error));
            if (fromOutbox) {
                _dropFromOutbox(ref);
            }
            return;
        }
        // Anything else - unreachable, not started, 5xx - is worth another go.
        // A checklist is a record, not a live picture: it is still true later.
        if (!fromOutbox) {
            _enqueue(run);
        }
    });
}

void FleetChecklists::_enqueue(const QJsonObject &run)
{
    QJsonArray box = QJsonDocument::fromJson(FleetStore::outboxJson()).array();
    const QString ref = run.value(QStringLiteral("external_ref")).toString();

    // Replace any earlier attempt at the same run rather than stacking copies:
    // the reference is the identity, and the newest answers are the right ones.
    for (int i = 0; i < box.size(); ++i) {
        if (box.at(i).toObject().value(QStringLiteral("external_ref")).toString() == ref) {
            box.removeAt(i);
            break;
        }
    }
    box.append(run);
    while (box.size() > kOutboxMax) {
        box.removeAt(0);
    }
    FleetStore::setOutboxJson(QJsonDocument(box).toJson(QJsonDocument::Compact));
    emit logMessage(QStringLiteral("checklist %1 queued; it will be filed when the link is back").arg(ref));
    emit outboxChanged();
}

void FleetChecklists::_dropFromOutbox(const QString &externalRef)
{
    QJsonArray box = QJsonDocument::fromJson(FleetStore::outboxJson()).array();
    for (int i = 0; i < box.size(); ++i) {
        if (box.at(i).toObject().value(QStringLiteral("external_ref")).toString() == externalRef) {
            box.removeAt(i);
            FleetStore::setOutboxJson(QJsonDocument(box).toJson(QJsonDocument::Compact));
            emit outboxChanged();
            return;
        }
    }
}

int FleetChecklists::outboxCount() const
{
    return QJsonDocument::fromJson(FleetStore::outboxJson()).array().size();
}

void FleetChecklists::_retryOutbox()
{
    if (!FleetStore::enrolled()) {
        return;
    }
    const QJsonArray box = QJsonDocument::fromJson(FleetStore::outboxJson()).array();
    for (const QJsonValue &v : box) {
        // Same external_ref as the first attempt, so this corrects in place
        // rather than filing a second copy.
        _send(v.toObject(), true);
    }
}

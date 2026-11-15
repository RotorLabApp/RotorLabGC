/****************************************************************************
 *
 * RotorLab GC Fleet Connector - checklists.
 *
 * Templates come from the customer's RotorLab organisation, through the Fleet
 * Server, to us. We never call RotorLab. Completed runs go back the same way.
 *
 * A checklist is NEVER a gate. A crew that skips one flies with a gap in the
 * record, which is their call to make. Nothing here blocks a launch, and a
 * failed run is filed exactly like a passing one - a crew that finds a cracked
 * arm should record it, and that record is worth more than a clean one.
 *
 * The field that decides whether this works is `external_ref`. It is generated
 * ONCE, when the crew opens the checklist, stored with the run, and reused for
 * the first send and every retry for ever. Same ref again corrects the run in
 * place; a new ref for the same run files a second copy of something that
 * happened once. Regenerating it on retry is the most likely bug in this
 * whole area because it only shows up when the link is flaky - which is not
 * when anyone is testing.
 *
 * Templates and the outbox are both cached to disk: a crew must be able to run
 * a checklist with the link down, and a completed run is still true an hour
 * later, so unlike telemetry it is queued rather than dropped.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>

class FleetClient;

class FleetChecklists : public QObject
{
    Q_OBJECT

public:
    explicit FleetChecklists(FleetClient *client, QObject *parent = nullptr);

    /// Templates the crew should run, from cache. Rows:
    /// {id, kind, name, items[]}. Only `active` ones, so a template switched
    /// off in RotorLab disappears here.
    QVariantList templates(const QString &kind) const;

    /// "rotorlab" | "local" | "none". With "none" we offer nothing at all -
    /// no button, no empty list, no placeholder, and certainly not a checklist
    /// nobody wrote.
    QString source() const { return _source; }
    bool    hasTemplates(const QString &kind) const;

    void loadCache();
    void setTemplates(const QJsonArray &templates);
    void reset();

    /// Generate the reference for a run. Called once when the crew opens the
    /// checklist; the caller stores it and uses it for every send after.
    static QString makeExternalRef(const QString &stationName, const QString &kind, int sysid);

    /// File a completed run. Queued to the outbox if it cannot be sent now.
    void file(const QJsonObject &run);

    int  outboxCount() const;

signals:
    void templatesChanged();
    void outboxChanged();
    void logMessage(const QString &line);

private slots:
    void _retryOutbox();

private:
    void _send(const QJsonObject &run, bool fromOutbox);
    void _enqueue(const QJsonObject &run);
    void _dropFromOutbox(const QString &externalRef);

    FleetClient *_client = nullptr;
    QJsonArray   _templates;
    QString      _source;
    QTimer       _outboxTimer;      ///< 60 s
};

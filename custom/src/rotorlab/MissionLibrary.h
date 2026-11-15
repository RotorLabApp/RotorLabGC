/****************************************************************************
 *
 * RotorLab GC RotorLab Connector - missions planned in RotorLab.app.
 *
 * READ ONLY, and deliberately so. The GC pulls a mission's native QGC `.plan`
 * and hands it to the Plan view; it never writes a mission back, never edits
 * one, and never uploads a plan to RotorLab. Planning happens in RotorLab, and
 * a plan that round-trips through two editors is a plan nobody can vouch for.
 *
 * Nothing here can hold up a launch, in keeping with the rest of the connector.
 * A mission list that fails to load, a plan that will not download, an empty
 * library - all of them leave the Plan view exactly as it was, and the crew can
 * still plan by hand and fly.
 *
 * The server answers with an envelope - { id, name, owner_id, plan } - and the
 * `.plan` QGC loads is the INNER object. It is written straight from the JSON
 * DOM, which keeps every key that was parsed, including ones this build knows
 * nothing about. What must never happen is a round trip through QGC's own
 * typed mission model: that keeps only what it models, and the crew would fly
 * the difference.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVariantList>

class RotorLabClient;

class MissionLibrary : public QObject
{
    Q_OBJECT

public:
    explicit MissionLibrary(RotorLabClient *client, QObject *parent = nullptr);

    /// Ask the server for the mission list. `pilotId` >= 0 narrows it to one
    /// pilot's assignments - the crew screen's pilot, when one is set.
    void fetch(int pilotId);

    /// id / name / detail, for the settings page list.
    QVariantList missions() const;
    bool loaded() const  { return _loaded; }
    bool busy() const    { return _busy; }
    QString lastError() const { return _lastError; }

    /// Download one mission's `.plan` and write it to disk. Emits planReady()
    /// with the file path on success, or failed() with a sentence to show.
    void download(int missionId);

    /// Where downloaded plans are kept.
    static QString planDir();

signals:
    void changed();
    void logMessage(const QString &line);
    /// A `.plan` is on disk and ready for PlanMasterController::loadFromFile().
    void planReady(const QString &filePath, const QString &missionName);
    void failed(const QString &why);

private:
    QString _nameFor(int missionId) const;

    RotorLabClient *_client = nullptr;
    QJsonArray _missions;
    bool    _loaded = false;
    bool    _busy = false;
    QString _lastError;
};

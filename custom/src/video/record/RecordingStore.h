/****************************************************************************
 *
 * RotorLab GC - the recording queue, persisted (G95).
 *
 * One JSON object per recorded part, in the QSettings group "VideoRecord"
 * (key "queue"), so a part survives a restart until the Fleet Server has
 * confirmed it, and stays listed after that until the operator deletes it.
 *
 * Each entry holds the part's path, size, SHA-256 at capture, its time map
 * sidecar's path, size and hash, the metadata for the registration body, the
 * Fleet server and station it was captured under, and its upload state.
 * Never the station token.
 *
 * GUI thread only.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QString>

class RecordingStore
{
public:
    /// `iniPath` empty: the application's settings. Tests pass a file.
    explicit RecordingStore(const QString &iniPath = QString());

    QList<QJsonObject> entries() const { return _entries; }
    int count() const { return int(_entries.size()); }

    /// The entry with this id, or an empty object.
    QJsonObject entry(const QString &id) const;
    /// Add or replace (by "id"; one is assigned when missing). Saved at once.
    QString upsert(QJsonObject entry);
    /// Forget an entry (its files are the caller's business). Saved at once.
    void remove(const QString &id);

    /// Video names already queued (recovery skips these).
    QSet<QString> names() const;

    void reload();

    static QString newId();

    static constexpr const char *kGroup = "VideoRecord";

private:
    void _save() const;

    QString _ini;
    QList<QJsonObject> _entries;
};

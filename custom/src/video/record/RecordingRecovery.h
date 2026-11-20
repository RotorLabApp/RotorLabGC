/****************************************************************************
 *
 * RotorLab GC - recordings left open by a crash or a power cut (G95).
 *
 * Runs at start-up, before any new recording, on a worker thread. It looks in
 * the recordings folder for time map sidecars whose video is not in the queue:
 *
 * - With an `end` line (closed, but the queue never heard): the file is hashed
 *   as found and queued. If that hash is not the one in the `end` line, the
 *   entry is parked: the file changed after capture.
 * - Without one (left open): the video is hashed exactly as it is on disk and
 *   never trimmed, repaired or changed. The sidecar gets one appended
 *   `recovered` line with the file's size and hash, the last complete
 *   fragment's end offset and its receive time. The part is queued with
 *   "recovered": true and ended_at = that receive time. The hash at capture
 *   for a recovered part is the hash of the file as found; the sidecar's
 *   fragment chain shows which prefix is intact.
 *
 * Nothing is deleted.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QString>

#include <atomic>

class RecordingRecovery
{
public:
    /// New queue entries for parts in `folder` not named in `knownVideoNames`.
    static QList<QJsonObject> scan(const QString &folder, const QSet<QString> &knownVideoNames);

    /// Recover one part from its sidecar. Empty when it is not a recording.
    static QJsonObject recoverPart(const QString &sidecarPath);

    /// SHA-256 (lowercase hex) of a file read in 1 MB blocks, never whole.
    /// Empty when it cannot be read or `cancel` was set.
    static QByteArray hashFile(const QString &path, qint64 *size = nullptr,
                               const std::atomic_bool *cancel = nullptr);
};

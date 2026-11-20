/****************************************************************************
 *
 * RotorLab GC - the recording's time map sidecar (G95).
 *
 * `<video>.timemap.jsonl`: JSON lines, one object per line, opened append-only
 * and never rewritten, with its own running SHA-256 (the hash of the file as
 * written). Line types, each with a "type" field:
 *
 *   header      once, first: station, camera, codec, size, source, GC version
 *               and commit, part, flight ref, vehicle, started_at
 *   fragment    one per media fragment: seq, file offset and length, source
 *               time, receive times, keyframe, the fragment hash chain, and
 *               the latest vehicle time and position with their age
 *   gap         units lost between two receive times (never hidden)
 *   arm/disarm  a vehicle armed or disarmed, with its flight ref
 *   end         last line of a part closed normally: ended_at, size, sha256
 *   recovered   added on the next start when a part was left open by a crash
 *
 * Values the station did not have are left out, never written as zero.
 * Nothing here is signed; the hashes show integrity of the bytes as written,
 * not who wrote them.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

class TimeMapWriter
{
public:
    static constexpr int kVersion = 1;

    /// True when `fps` is a measured rate Fleet accepts (1 to 240). Anything
    /// else is unknown: registered as fps null, never as a nominal rate.
    static bool fpsKnown(double fps) { return fps >= 1.0 && fps <= 240.0; }

    /// The fps fields for an end or recovered line: the measured rate and
    /// "fps_measured": true, or only "fps_measured": false when a part was too
    /// short to measure.
    static void addFps(QJsonObject &line, double measuredFps)
    {
        if (fpsKnown(measuredFps)) {
            line[QStringLiteral("fps")] = measuredFps;
            line[QStringLiteral("fps_measured")] = true;
        } else {
            line[QStringLiteral("fps_measured")] = false;
        }
    }

    TimeMapWriter();
    ~TimeMapWriter();

    /// Create `path` (append-only). Fails if it already exists with content.
    bool open(const QString &path);
    bool isOpen() const { return _file.isOpen(); }
    QString path() const { return _file.fileName(); }
    QString errorString() const { return _error; }

    /// One JSON line. Returns false on a write error.
    bool writeLine(const QJsonObject &line);

    /// Push written lines to the OS (after each fragment).
    bool flush();
    /// Push to the disk (fsync / FlushFileBuffers).
    bool sync();

    /// Sync and close; returns the hash of every byte written (lowercase hex).
    QByteArray close();

    qint64 size() const { return _size; }

    /// The chain value for the first fragment: SHA-256 of the init segment.
    static QByteArray chainStart(const QByteArray &initSegment);
    /// SHA-256(previous chain (32 raw bytes) || fragment bytes).
    static QByteArray chainNext(const QByteArray &previousChain, const QByteArray &fragment);

    /// fsync for an open QFile (FlushFileBuffers on Windows).
    static bool syncFile(QFile &file);

private:
    QFile _file;
    QCryptographicHash _hash;
    qint64 _size = 0;
    QString _error;
};

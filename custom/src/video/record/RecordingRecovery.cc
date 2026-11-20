/****************************************************************************
 *
 * RotorLab GC - recordings left open by a crash or a power cut (G95).
 *
 ****************************************************************************/

#include "RecordingRecovery.h"
#include "TimeMapWriter.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QObject>
#include <QtCore/QTimeZone>

#include "QGCLoggingCategory.h"

Q_DECLARE_LOGGING_CATEGORY(VideoRecordLog)

namespace {

constexpr qint64 kBlock = 1024 * 1024;
constexpr qint64 kMaxSidecarBytes = 256LL * 1024 * 1024;

QString isoUtc(qint64 utcMs, bool withMs = false)
{
    return QDateTime::fromMSecsSinceEpoch(utcMs, QTimeZone::UTC)
        .toString(withMs ? Qt::ISODateWithMs : Qt::ISODate);
}

} // namespace

QByteArray RecordingRecovery::hashFile(const QString &path, qint64 *size, const std::atomic_bool *cancel)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    qint64 total = 0;
    QByteArray block;
    while (!f.atEnd()) {
        if (cancel && cancel->load()) {
            return {};
        }
        block = f.read(kBlock);
        if (block.isEmpty()) {
            if (f.error() != QFileDevice::NoError) {
                return {};
            }
            break;
        }
        h.addData(block);
        total += block.size();
    }
    if (size) {
        *size = total;
    }
    return h.result().toHex();
}

QJsonObject RecordingRecovery::recoverPart(const QString &sidecarPath)
{
    QFile side(sidecarPath);
    if (!side.open(QIODevice::ReadOnly) || side.size() > kMaxSidecarBytes) {
        return {};
    }
    const QByteArray content = side.readAll();
    side.close();

    QJsonObject header;
    QJsonObject endLine;
    QJsonObject recoveredLine;
    QJsonObject lastFragment;
    QSet<QString> refs;
    qint64 samples = 0;
    qint64 duration = 0;
    QList<QJsonObject> fragments;
    for (const QByteArray &raw : content.split('\n')) {
        if (raw.trimmed().isEmpty()) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isObject()) {
            continue;                   // a line cut by the crash
        }
        const QJsonObject line = doc.object();
        const QString type = line.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("header") && header.isEmpty()) {
            header = line;
            const QString ref = line.value(QStringLiteral("flight_ref")).toString();
            if (!ref.isEmpty()) {
                refs.insert(ref);
            }
        } else if (type == QLatin1String("fragment")) {
            fragments.append(line);
        } else if (type == QLatin1String("arm") || type == QLatin1String("disarm")) {
            const QString ref = line.value(QStringLiteral("flight_ref")).toString();
            if (!ref.isEmpty()) {
                refs.insert(ref);
            }
        } else if (type == QLatin1String("end")) {
            endLine = line;
        } else if (type == QLatin1String("recovered")) {
            recoveredLine = line;
        }
    }
    if (header.value(QStringLiteral("kind")).toString() != QLatin1String("video-timemap")) {
        return {};
    }
    const QString videoName = header.value(QStringLiteral("video")).toString();
    if (videoName.isEmpty() || videoName.contains(QLatin1Char('/')) || videoName.contains(QLatin1Char('\\'))) {
        return {};
    }
    const QString videoPath = QFileInfo(sidecarPath).dir().filePath(videoName);
    if (!QFileInfo::exists(videoPath)) {
        return {};
    }

    qint64 size = 0;
    const QByteArray sha = hashFile(videoPath, &size);
    if (sha.isEmpty()) {
        return {};
    }

    // The last fragment wholly inside the file as found.
    for (const QJsonObject &f : std::as_const(fragments)) {
        const qint64 end = f.value(QStringLiteral("offset")).toInteger() + f.value(QStringLiteral("length")).toInteger();
        if (end <= size) {
            lastFragment = f;
            samples += f.value(QStringLiteral("samples")).toInteger();
            duration += f.value(QStringLiteral("duration")).toInteger();
        }
    }

    const qint64 startedMs = header.value(QStringLiteral("started_utc_ms")).toInteger();
    QJsonObject entry;
    entry[QStringLiteral("name")] = videoName;
    entry[QStringLiteral("path")] = videoPath;
    entry[QStringLiteral("size")] = size;
    entry[QStringLiteral("sha256")] = QString::fromLatin1(sha);
    entry[QStringLiteral("sidecar_name")] = QFileInfo(sidecarPath).fileName();
    entry[QStringLiteral("sidecar_path")] = sidecarPath;
    entry[QStringLiteral("camera")] = header.value(QStringLiteral("camera")).toString();
    entry[QStringLiteral("codec")] = header.value(QStringLiteral("codec")).toString();
    entry[QStringLiteral("width")] = header.value(QStringLiteral("width")).toInt();
    entry[QStringLiteral("height")] = header.value(QStringLiteral("height")).toInt();
    const double fps = (duration > 0 && samples > 1)
        ? qRound(double(samples) * 90000.0 / double(duration) * 100.0) / 100.0 : 0.0;
    entry[QStringLiteral("fps")] = fps;
    entry[QStringLiteral("started_at")] = isoUtc(startedMs);
    entry[QStringLiteral("part")] = header.value(QStringLiteral("part")).toInt();
    entry[QStringLiteral("flight_ref")] = (refs.size() == 1) ? *refs.constBegin() : QString();
    entry[QStringLiteral("sysid")] = header.value(QStringLiteral("sysid")).toInt();
    entry[QStringLiteral("base_url")] = header.value(QStringLiteral("fleet_server")).toString();
    entry[QStringLiteral("station_id")] = header.value(QStringLiteral("station_id")).toInt();

    if (!endLine.isEmpty()) {
        // Closed normally; only the queue missed it.
        entry[QStringLiteral("ended_at")] = isoUtc(endLine.value(QStringLiteral("ended_utc_ms")).toInteger());
        entry[QStringLiteral("recovered")] = false;
        if (endLine.value(QStringLiteral("sha256")).toString() != QString::fromLatin1(sha)
            || endLine.value(QStringLiteral("size")).toInteger() != size) {
            entry[QStringLiteral("state")] = QStringLiteral("parked");
            entry[QStringLiteral("detail")] = QObject::tr("The file changed on this computer after capture.");
            entry[QStringLiteral("changed")] = true;
        }
    } else {
        qint64 endedMs = startedMs;
        if (!lastFragment.isEmpty()) {
            endedMs = lastFragment.value(QStringLiteral("recv_utc_ms")).toInteger();
        }
        if (recoveredLine.isEmpty()) {
            QJsonObject line;
            line[QStringLiteral("type")] = QStringLiteral("recovered");
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            line[QStringLiteral("found_at")] = isoUtc(now, true);
            line[QStringLiteral("file_size")] = size;
            line[QStringLiteral("sha256")] = QString::fromLatin1(sha);
            if (!lastFragment.isEmpty()) {
                line[QStringLiteral("last_complete_seq")] = lastFragment.value(QStringLiteral("seq")).toInteger();
                line[QStringLiteral("last_complete_offset")] =
                    lastFragment.value(QStringLiteral("offset")).toInteger()
                    + lastFragment.value(QStringLiteral("length")).toInteger();
                line[QStringLiteral("last_complete_chain")] = lastFragment.value(QStringLiteral("chain")).toString();
            }
            line[QStringLiteral("ended_at")] = isoUtc(endedMs, true);
            line[QStringLiteral("ended_utc_ms")] = endedMs;
            TimeMapWriter::addFps(line, fps);

            QFile append(sidecarPath);
            if (append.open(QIODevice::WriteOnly | QIODevice::Append)) {
                QByteArray bytes;
                if (!content.isEmpty() && !content.endsWith('\n')) {
                    bytes.append('\n');         // end the line the crash cut, without changing it
                }
                bytes.append(QJsonDocument(line).toJson(QJsonDocument::Compact));
                bytes.append('\n');
                (void) append.write(bytes);
                (void) TimeMapWriter::syncFile(append);
                append.close();
            }
        } else {
            endedMs = recoveredLine.value(QStringLiteral("ended_utc_ms")).toInteger();
        }
        entry[QStringLiteral("ended_at")] = isoUtc(endedMs);
        entry[QStringLiteral("recovered")] = true;
    }

    qint64 sideSize = 0;
    const QByteArray sideSha = hashFile(sidecarPath, &sideSize);
    entry[QStringLiteral("sidecar_size")] = sideSize;
    entry[QStringLiteral("sidecar_sha256")] = QString::fromLatin1(sideSha);
    qCInfo(VideoRecordLog) << "recording found at start-up:" << videoName << size << "bytes sha256" << sha
                           << (entry.value(QStringLiteral("recovered")).toBool() ? "recovered" : "closed");
    return entry;
}

QList<QJsonObject> RecordingRecovery::scan(const QString &folder, const QSet<QString> &knownVideoNames)
{
    QList<QJsonObject> out;
    QDir dir(folder);
    if (folder.isEmpty() || !dir.exists()) {
        return out;
    }
    const QStringList sidecars = dir.entryList({QStringLiteral("*.timemap.jsonl")}, QDir::Files, QDir::Name);
    for (const QString &name : sidecars) {
        QString video = name;
        video.chop(int(qstrlen(".timemap.jsonl")));
        video += QStringLiteral(".mp4");
        if (knownVideoNames.contains(video)) {
            continue;
        }
        const QJsonObject entry = recoverPart(dir.filePath(name));
        if (!entry.isEmpty() && !knownVideoNames.contains(entry.value(QStringLiteral("name")).toString())) {
            out.append(entry);
        }
    }
    return out;
}

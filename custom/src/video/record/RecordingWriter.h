/****************************************************************************
 *
 * RotorLab GC - local video recording, the writer (G95).
 *
 * Consumes the receiver's packet tap (VideoPacketTap.h) and writes what the
 * camera sent, never decoded or re-encoded, as fragmented MP4:
 *
 *   <folder>/<station>-<camera>-<UTC start>-pNN.mp4
 *   <folder>/<station>-<camera>-<UTC start>-pNN.timemap.jsonl
 *
 * - Append-only: the MP4 is created new and opened WriteOnly | Append, never
 *   seeked or rewritten, so the running SHA-256 over every byte handed to it
 *   is the file's hash. No moov rewrite at stop.
 * - Fragments of about 1 s (Fmp4Muxer), flush() after each, fsync about
 *   every 5 s and at stop.
 * - A recording starts on a keyframe. A new part starts at the first
 *   keyframe after the size limit (15.5 GB, under the 16 GB contract limit),
 *   and on a codec, parameter set or generation change.
 * - The disk is checked before each part and every 10 s; under 2 GB free the
 *   part is closed and recording stops. Nothing is ever deleted to make room.
 * - A discontinuity from the tap is a `gap` line in the time map.
 *
 * Runs on its own thread (VideoRecorder owns it). Nothing here can stop,
 * delay or gate the receiver, the display or a flight: the tap drops units
 * for a slow consumer and resyncs at a keyframe.
 *
 * Tests drive handleItem() directly on their own thread.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QString>

#include <atomic>
#include <functional>
#include <memory>

#include "Fmp4Muxer.h"
#include "TimeMapWriter.h"
#include "VehicleClock.h"
#include "VideoPacketTap.h"

struct RecordingConfig
{
    QString folder;                         ///< where parts go (created when needed)
    QString station;                        ///< station name, for file names and the header
    int     stationId = 0;                  ///< Fleet station id at capture, 0 when not enrolled
    QString baseUrl;                        ///< Fleet server enrolled at capture, empty when none
    QString camera = QStringLiteral("main");
    QString gcVersion;
    QString gcCommit;
    qint64  partLimitBytes = 15872LL * 1024 * 1024;      ///< 15.5 GiB
    qint64  minFreeBytes = 2LL * 1024 * 1024 * 1024;               ///< 2 GiB
    qint64  fragmentMs = 1000;
    qint64  fsyncMs = 5000;
    qint64  diskCheckMs = 10000;
    /// Free bytes on the volume holding a folder; -1 unknown. Default: QStorageInfo.
    std::function<qint64(const QString &)> freeBytes;
    /// Latest vehicle values for a system id. Default: none known.
    std::function<VehicleSample(int)> vehicle;
};

class RecordingWriter : public QObject
{
    Q_OBJECT

public:
    enum StopReason {
        StopNone = 0,
        StopRequested,      ///< recording turned off, or the trigger ended
        StopSourceStopped,  ///< the video source stopped
        StopDiskLow,        ///< under the free-space limit
        StopWriteError,     ///< the file could not be written
        StopMuxError,       ///< the stream could not be put in MP4
        StopNoFolder,       ///< no folder to save video in
    };
    Q_ENUM(StopReason)

    explicit RecordingWriter(const RecordingConfig &config, QObject *parent = nullptr);
    ~RecordingWriter() override;

    /// The operator-facing sentence for a stop reason ("Recording stopped: ...").
    static QString stopText(int reason);

    /// File-name safe: letters, digits, '-' and '_' only, at most 40 characters.
    static QString safeName(const QString &text);

    // --- control: call on the writer's thread (VideoRecorder queues these) ---

    /// Start (true) or stop (false) recording. Starting clears a disk-low stop.
    Q_INVOKABLE void setWanted(bool wanted);
    /// The vehicle the time map follows, and its current flight ref.
    Q_INVOKABLE void setVehicle(int sysid, const QString &name, const QString &flightRef);
    /// An `arm` or `disarm` line (kept for the next part when none is open).
    Q_INVOKABLE void mark(const QString &type, int sysid, const QString &flightRef);
    /// The Fleet server, station id and station name bound to new parts.
    Q_INVOKABLE void setBinding(const QString &baseUrl, int stationId, const QString &station);
    Q_INVOKABLE void setSource(const QString &redactedSource);
    /// Start reading from a tap consumer (wakes this object's thread).
    void attachConsumer(std::shared_ptr<VideoPacketConsumer> consumer);
    /// Stop reading; returns the consumer so the caller can detach it.
    std::shared_ptr<VideoPacketConsumer> detachConsumer();
    /// Close any open part (GC closing).
    Q_INVOKABLE void shutdown();

    /// One tap item. The consumer path calls this; so do tests.
    void handleItem(const VideoTapItem &item);

    bool    partOpen() const { return _file.isOpen(); }
    bool    wanted() const { return _wanted; }
    QString currentPartPath() const { return _file.isOpen() ? _file.fileName() : QString(); }

signals:
    void partOpened(const QString &path, qint64 startedUtcMs);
    /// About once a second while a part is open.
    void progress(qint64 partBytes, qint64 sessionStartedUtcMs);
    /// A part is complete on disk; `entry` is its RecordingStore entry.
    void partClosed(const QJsonObject &entry);
    /// Recording stopped by itself (not StopRequested).
    void stopped(int reason, const QString &text);

private slots:
    void _drain();

private:
    void _onEvent(const VideoTapEvent &event);
    void _onPacket(const VideoTapPacket &packet);
    bool _openPart(const VideoTapPacket &first);
    void _closePart(int reason);
    void _halt(int reason);
    void _feed(const VideoTapPacket &packet);
    void _writeMarks();
    void _onInit(const QByteArray &bytes);
    void _onFragment(const QByteArray &bytes, const Fmp4FragmentInfo &info);
    bool _appendToFile(const QByteArray &bytes);
    qint64 _free(const QString &folder) const;

    RecordingConfig _config;
    QString _source;
    int     _sysid = 0;
    QString _vehicleName;
    QString _flightRef;

    bool    _wanted = false;
    bool    _halted = false;            ///< stopped by disk, write or mux error until the next start
    bool    _sessionActive = false;
    qint64  _sessionStartUtcMs = 0;
    int     _partNo = 0;

    // The open part.
    Fmp4Muxer *_muxer = nullptr;
    QFile   _file;
    QCryptographicHash _fileHash{QCryptographicHash::Sha256};
    TimeMapWriter _timeMap;
    QString _partName;
    QString _sidecarName;
    QByteArray _pendingInit;            ///< init segment, held until the file exists
    QByteArray _chain;
    QByteArray _initSha;
    qint64  _partBytes = 0;
    quint64 _partGeneration = 0;
    VideoTapCodec _partCodec = VideoTapCodec::H264;
    qint64  _partStartUtcMs = 0;
    qint64  _lastRecvUtcMs = 0;
    qint64  _lastRecvNs = 0;
    qint64  _lastSyncNs = 0;
    qint64  _lastDiskCheckNs = 0;
    qint64  _lastProgressNs = 0;
    qint64  _firstDts = -1;
    qint64  _lastDts = -1;
    int     _units = 0;
    quint64 _fragments = 0;
    QSet<QString> _partRefs;
    bool    _writeFailed = false;
    bool    _unitsInPart = false;
    bool    _hasRtp = false;
    qint64  _rtpUnwrapped = 0;
    quint32 _rtpLast = 0;
    QHash<quint64, VehicleSample> _fragVehicle;     ///< by the fragment's first tap index
    QList<QJsonObject> _pendingMarks;

    std::shared_ptr<VideoPacketConsumer> _consumer;
    std::atomic_bool _drainQueued{false};
};

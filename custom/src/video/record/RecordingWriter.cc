/****************************************************************************
 *
 * RotorLab GC - local video recording, the writer (G95).
 *
 ****************************************************************************/

#include "RecordingWriter.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QStorageInfo>
#include <QtCore/QTimeZone>
#include <QtCore/QUrl>

#include "QGCLoggingCategory.h"

Q_DECLARE_LOGGING_CATEGORY(VideoRecordLog)

namespace {

constexpr qint64 kNsPerMs = 1000000;
constexpr int kDrainBatch = 64;

QString isoUtc(qint64 utcMs, bool withMs = false)
{
    return QDateTime::fromMSecsSinceEpoch(utcMs, QTimeZone::UTC)
        .toString(withMs ? Qt::ISODateWithMs : Qt::ISODate);
}

QString stampUtc(qint64 utcMs)
{
    return QDateTime::fromMSecsSinceEpoch(utcMs, QTimeZone::UTC).toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
}

QString sourceType(const QString &source)
{
    const QString scheme = QUrl(source).scheme().toLower();
    return scheme.isEmpty() ? QStringLiteral("unknown") : scheme;
}

} // namespace

RecordingWriter::RecordingWriter(const RecordingConfig &config, QObject *parent)
    : QObject(parent)
    , _config(config)
    , _muxer(new Fmp4Muxer(this))
{
    connect(_muxer, &Fmp4Muxer::initSegment, this, &RecordingWriter::_onInit, Qt::DirectConnection);
    connect(_muxer, &Fmp4Muxer::fragment, this, &RecordingWriter::_onFragment, Qt::DirectConnection);
}

RecordingWriter::~RecordingWriter()
{
    (void) detachConsumer();
    if (partOpen()) {
        _closePart(StopRequested);
    }
}

QString RecordingWriter::stopText(int reason)
{
    switch (reason) {
    case StopSourceStopped:
        return QObject::tr("Recording stopped: the video source stopped.");
    case StopDiskLow:
        return QObject::tr("Recording stopped: the disk is nearly full.");
    case StopWriteError:
        return QObject::tr("Recording stopped: the recording could not be written to disk.");
    case StopMuxError:
        return QObject::tr("Recording stopped: this video stream could not be saved as MP4.");
    case StopNoFolder:
        return QObject::tr("Recording stopped: there is no folder to save video in. Set the application load and save path in General settings.");
    default:
        return QString();
    }
}

QString RecordingWriter::safeName(const QString &text)
{
    QString out;
    for (const QChar c : text) {
        const ushort u = c.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9')
                        || u == '-' || u == '_';
        out += ok ? c : QChar(QLatin1Char('_'));
        if (out.size() >= 40) {
            break;
        }
    }
    return out.isEmpty() ? QStringLiteral("station") : out;
}

qint64 RecordingWriter::_free(const QString &folder) const
{
    if (_config.freeBytes) {
        return _config.freeBytes(folder);
    }
    QStorageInfo info(folder);
    return info.isValid() ? info.bytesAvailable() : -1;
}

// ----------------------------------------------------------------- control

void RecordingWriter::setWanted(bool wanted)
{
    if (wanted && !_wanted) {
        _halted = false;                // a new start clears a disk-low stop
    }
    _wanted = wanted;
    if (!wanted) {
        if (partOpen()) {
            _closePart(StopRequested);
        }
        _sessionActive = false;
        _pendingMarks.clear();
    }
}

void RecordingWriter::setVehicle(int sysid, const QString &name, const QString &flightRef)
{
    _sysid = sysid;
    _vehicleName = name;
    _flightRef = flightRef;
    if (partOpen() && !flightRef.isEmpty()) {
        _partRefs.insert(flightRef);
    }
}

void RecordingWriter::mark(const QString &type, int sysid, const QString &flightRef)
{
    QJsonObject line;
    line[QStringLiteral("type")] = type;
    line[QStringLiteral("sysid")] = sysid;
    if (!flightRef.isEmpty()) {
        line[QStringLiteral("flight_ref")] = flightRef;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    line[QStringLiteral("utc_ms")] = now;
    line[QStringLiteral("at")] = isoUtc(now, true);
    line[QStringLiteral("steady_ns")] = VehicleClock::steadyNowNs();
    if (partOpen()) {
        if (!flightRef.isEmpty()) {
            _partRefs.insert(flightRef);
        }
        if (!_timeMap.writeLine(line)) {
            _writeFailed = true;
        }
    } else if (_wanted) {
        _pendingMarks.append(line);
        if (_pendingMarks.size() > 64) {
            _pendingMarks.removeFirst();
        }
    }
}

void RecordingWriter::setBinding(const QString &baseUrl, int stationId, const QString &station)
{
    _config.baseUrl = baseUrl;
    _config.stationId = stationId;
    _config.station = station;
}

void RecordingWriter::setSource(const QString &redactedSource)
{
    _source = redactedSource;
}

void RecordingWriter::attachConsumer(std::shared_ptr<VideoPacketConsumer> consumer)
{
    (void) detachConsumer();
    _consumer = std::move(consumer);
    if (!_consumer) {
        return;
    }
    _consumer->setWakeup([this]() {
        // On the receiver's thread: post once, return at once.
        if (!_drainQueued.exchange(true)) {
            QMetaObject::invokeMethod(this, &RecordingWriter::_drain, Qt::QueuedConnection);
        }
    });
    _drainQueued = true;
    QMetaObject::invokeMethod(this, &RecordingWriter::_drain, Qt::QueuedConnection);
}

std::shared_ptr<VideoPacketConsumer> RecordingWriter::detachConsumer()
{
    std::shared_ptr<VideoPacketConsumer> c = std::move(_consumer);
    _consumer.reset();
    if (c) {
        c->setWakeup({});
    }
    return c;
}

void RecordingWriter::shutdown()
{
    (void) detachConsumer();
    setWanted(false);
}

void RecordingWriter::_drain()
{
    _drainQueued = false;
    if (!_consumer) {
        return;
    }
    VideoTapItem item;
    int n = 0;
    while (n < kDrainBatch && _consumer && _consumer->tryPop(item)) {
        handleItem(item);
        ++n;
    }
    if (n == kDrainBatch && _consumer && !_drainQueued.exchange(true)) {
        // More may be waiting: come back after other events (control calls) run.
        QMetaObject::invokeMethod(this, &RecordingWriter::_drain, Qt::QueuedConnection);
    }
}

// ------------------------------------------------------------------- items

void RecordingWriter::handleItem(const VideoTapItem &item)
{
    if (const VideoTapEvent *event = std::get_if<VideoTapEvent>(&item)) {
        _onEvent(*event);
    } else if (const VideoTapPacket *packet = std::get_if<VideoTapPacket>(&item)) {
        _onPacket(*packet);
    }
}

void RecordingWriter::_onEvent(const VideoTapEvent &event)
{
    if (!event.source.isEmpty()) {
        _source = event.source;
    }
    switch (event.type) {
    case VideoTapEvent::Type::StreamStopped: {
        const bool wasRecording = partOpen();
        if (wasRecording) {
            _closePart(StopSourceStopped);
        }
        if (_sessionActive && _wanted) {
            qCInfo(VideoRecordLog) << "video source stopped; recording waits for it";
            emit stopped(StopSourceStopped, stopText(StopSourceStopped));
        }
        _sessionActive = false;
        break;
    }
    case VideoTapEvent::Type::StreamStarted:
    case VideoTapEvent::Type::SourceChanged:
    case VideoTapEvent::Type::SsrcChanged:
        // A new generation: this part ends; the next keyframe starts the next part.
        if (partOpen() && event.generation != _partGeneration) {
            _closePart(StopNone);
        }
        break;
    }
}

void RecordingWriter::_onPacket(const VideoTapPacket &p)
{
    if (!_wanted || _halted) {
        return;
    }

    if (partOpen()) {
        const bool newGeneration = p.generation != _partGeneration;
        if (newGeneration && !p.keyFrame) {
            _closePart(StopNone);       // wait for this generation's keyframe
            return;
        }
        if (p.keyFrame) {
            const Fmp4Muxer::Codec codec = (p.codec == VideoTapCodec::H265) ? Fmp4Muxer::Codec::H265
                                                                              : Fmp4Muxer::Codec::H264;
            if (newGeneration || _muxer->needsNewInit(codec, p.parameterSets)
                || _partBytes + _muxer->pendingBytes() >= _config.partLimitBytes) {
                _closePart(StopNone);
            }
        }
    }

    if (!partOpen()) {
        if (!p.keyFrame || p.parameterSets.isEmpty()) {
            return;                     // a part starts on a keyframe with its parameter sets
        }
        if (!_sessionActive) {
            _sessionActive = true;
            _sessionStartUtcMs = p.receiveUtcMs > 0 ? p.receiveUtcMs : QDateTime::currentMSecsSinceEpoch();
            _partNo = 0;
        }
        if (!_openPart(p)) {
            return;
        }
    } else if (p.discontinuity && _unitsInPart) {
        QJsonObject gap;
        gap[QStringLiteral("type")] = QStringLiteral("gap");
        gap[QStringLiteral("before_recv_utc_ms")] = _lastRecvUtcMs;
        gap[QStringLiteral("before_recv_steady_ns")] = _lastRecvNs;
        gap[QStringLiteral("after_recv_utc_ms")] = p.receiveUtcMs;
        gap[QStringLiteral("after_recv_steady_ns")] = p.receiveSteadyNs;
        if (p.droppedBefore > 0) {
            gap[QStringLiteral("dropped_by_gc")] = qint64(p.droppedBefore);
        }
        if (!_timeMap.writeLine(gap)) {
            _writeFailed = true;
        }
    }

    _feed(p);
    if (!partOpen()) {
        return;
    }
    if (_writeFailed) {
        _halt(StopWriteError);
        return;
    }

    // Disk check every 10 s of received video.
    if (p.receiveSteadyNs - _lastDiskCheckNs >= _config.diskCheckMs * kNsPerMs) {
        _lastDiskCheckNs = p.receiveSteadyNs;
        const qint64 free = _free(_config.folder);
        if (free >= 0 && free < _config.minFreeBytes) {
            _halt(StopDiskLow);
            return;
        }
    }
    if (p.receiveSteadyNs - _lastProgressNs >= 1000 * kNsPerMs) {
        _lastProgressNs = p.receiveSteadyNs;
        emit progress(_partBytes + _muxer->pendingBytes(), _sessionStartUtcMs);
    }
}

void RecordingWriter::_feed(const VideoTapPacket &p)
{
    Fmp4Muxer::Unit u;
    u.data = p.data;
    u.keyFrame = p.keyFrame;
    u.receiveSteadyNs = p.receiveSteadyNs;
    u.receiveUtcMs = p.receiveUtcMs;
    u.index = p.index;
    if (p.hasRtpTimestamp) {
        if (!_hasRtp) {
            _hasRtp = true;
            _rtpUnwrapped = qint64(p.rtpTimestamp);
        } else {
            _rtpUnwrapped += qint64(qint32(p.rtpTimestamp - _rtpLast));
        }
        _rtpLast = p.rtpTimestamp;
        u.pts90k = u.dts90k = _rtpUnwrapped;
        u.sourceTime90k = _rtpUnwrapped;
    } else {
        u.pts90k = p.pts90k;
        u.dts90k = p.dts90k;
        u.sourceTime90k = p.dts90k >= 0 ? p.dts90k : p.pts90k;
    }

    const quint64 before = _muxer->currentFragmentFirstIndex();
    if (!_muxer->write(u)) {
        if (!_muxer->errorString().isEmpty()) {
            qCWarning(VideoRecordLog) << "recording muxer error:" << _muxer->errorString();
            _halt(StopMuxError);
        }
        return;
    }
    if (_writeFailed) {
        return;
    }
    const quint64 now = _muxer->currentFragmentFirstIndex();
    if (now == p.index && (now != before || !_unitsInPart)) {
        // This unit starts a fragment: the vehicle values as of its arrival.
        _fragVehicle.insert(p.index, _config.vehicle ? _config.vehicle(_sysid) : VehicleSample());
    }
    _unitsInPart = true;
    _lastRecvUtcMs = p.receiveUtcMs;
    _lastRecvNs = p.receiveSteadyNs;
    _units += 1;
    const qint64 dts = _muxer->lastDts90k();
    if (_firstDts < 0) {
        _firstDts = dts;
    }
    _lastDts = dts;
}

// ------------------------------------------------------------------- parts

bool RecordingWriter::_openPart(const VideoTapPacket &first)
{
    if (_config.folder.isEmpty() || !QDir().mkpath(_config.folder)) {
        _halt(StopNoFolder);
        return false;
    }
    const qint64 free = _free(_config.folder);
    if (free >= 0 && free < _config.minFreeBytes) {
        _halt(StopDiskLow);
        return false;
    }

    Fmp4Muxer::Config mc;
    mc.codec = (first.codec == VideoTapCodec::H265) ? Fmp4Muxer::Codec::H265 : Fmp4Muxer::Codec::H264;
    mc.parameterSets = first.parameterSets;
    mc.fragment90k = _config.fragmentMs * 90;
    mc.cutAtKeyframes = true;
    _pendingInit.clear();
    if (!_muxer->open(mc)) {
        qCWarning(VideoRecordLog) << "cannot start a recording part:" << _muxer->errorString();
        _halt(StopMuxError);
        return false;
    }

    QDir dir(_config.folder);
    const QString base = QStringLiteral("%1-%2-%3")
        .arg(safeName(_config.station), safeName(_config.camera), stampUtc(_sessionStartUtcMs));
    QString name;
    QString sidecar;
    do {
        ++_partNo;
        name = QStringLiteral("%1-p%2.mp4").arg(base).arg(_partNo, 2, 10, QLatin1Char('0'));
        sidecar = QStringLiteral("%1-p%2.timemap.jsonl").arg(base).arg(_partNo, 2, 10, QLatin1Char('0'));
    } while ((dir.exists(name) || dir.exists(sidecar)) && _partNo < 999);

    _file.setFileName(dir.filePath(name));
    // A new file, appended to only: never an existing one, never a seek back.
    if (!_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::NewOnly)) {
        qCWarning(VideoRecordLog) << "cannot create" << name << _file.errorString();
        _muxer->abort();
        _halt(StopWriteError);
        return false;
    }
    if (!_timeMap.open(dir.filePath(sidecar))) {
        qCWarning(VideoRecordLog) << "cannot create" << sidecar << _timeMap.errorString();
        _file.close();
        _muxer->abort();
        _halt(StopWriteError);
        return false;
    }

    _partName = name;
    _sidecarName = sidecar;
    _fileHash.reset();
    _partBytes = 0;
    _partGeneration = first.generation;
    _partCodec = first.codec;
    _partStartUtcMs = first.receiveUtcMs > 0 ? first.receiveUtcMs : QDateTime::currentMSecsSinceEpoch();
    _lastSyncNs = first.receiveSteadyNs;
    _lastDiskCheckNs = first.receiveSteadyNs;
    _lastProgressNs = 0;
    _firstDts = -1;
    _lastDts = -1;
    _units = 0;
    _fragments = 0;
    _writeFailed = false;
    _unitsInPart = false;
    _hasRtp = false;
    _fragVehicle.clear();
    _partRefs.clear();
    if (!_flightRef.isEmpty()) {
        _partRefs.insert(_flightRef);
    }

    _initSha = QCryptographicHash::hash(_pendingInit, QCryptographicHash::Sha256).toHex();
    _chain = TimeMapWriter::chainStart(_pendingInit);
    if (!_appendToFile(_pendingInit)) {
        _writeFailed = true;
    }
    _pendingInit.clear();

    const VideoCodecInfo info = _muxer->codecInfo();
    QJsonObject header;
    header[QStringLiteral("type")] = QStringLiteral("header");
    header[QStringLiteral("kind")] = QStringLiteral("video-timemap");
    header[QStringLiteral("v")] = TimeMapWriter::kVersion;
    header[QStringLiteral("video")] = name;
    header[QStringLiteral("station")] = _config.station;
    if (_config.stationId > 0) {
        header[QStringLiteral("station_id")] = _config.stationId;
    }
    if (!_config.baseUrl.isEmpty()) {
        header[QStringLiteral("fleet_server")] = _config.baseUrl;   // the server this part is bound to
    }
    header[QStringLiteral("camera")] = _config.camera;
    header[QStringLiteral("codec")] = info.codecString;
    if (info.valid) {
        header[QStringLiteral("width")] = info.width;
        header[QStringLiteral("height")] = info.height;
    }
    header[QStringLiteral("source_type")] = sourceType(_source);
    header[QStringLiteral("source")] = _source;     // credentials already removed by the receiver
    header[QStringLiteral("gc_version")] = _config.gcVersion;
    header[QStringLiteral("gc_commit")] = _config.gcCommit;
    header[QStringLiteral("part")] = _partNo;
    header[QStringLiteral("flight_ref")] = _flightRef;
    if (_sysid > 0) {
        header[QStringLiteral("sysid")] = _sysid;
    }
    if (!_vehicleName.isEmpty()) {
        header[QStringLiteral("vehicle")] = _vehicleName;
    }
    header[QStringLiteral("started_at")] = isoUtc(_partStartUtcMs, true);
    header[QStringLiteral("started_utc_ms")] = _partStartUtcMs;
    header[QStringLiteral("init_sha256")] = QString::fromLatin1(_initSha);
    header[QStringLiteral("time_base")] = 90000;
    header[QStringLiteral("clock")] = QStringLiteral("recv_utc_ms is this station's clock at the socket; "
                                                     "recv_steady_ns its monotonic clock");
    if (!_timeMap.writeLine(header)) {
        _writeFailed = true;
    }
    _writeMarks();

    qCInfo(VideoRecordLog) << "recording part started:" << name << info.codecString
                           << info.width << "x" << info.height;
    emit partOpened(_file.fileName(), _sessionStartUtcMs);
    return true;
}

void RecordingWriter::_writeMarks()
{
    for (const QJsonObject &line : std::as_const(_pendingMarks)) {
        const QString ref = line.value(QStringLiteral("flight_ref")).toString();
        if (!ref.isEmpty()) {
            _partRefs.insert(ref);
        }
        if (!_timeMap.writeLine(line)) {
            _writeFailed = true;
        }
    }
    _pendingMarks.clear();
}

bool RecordingWriter::_appendToFile(const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return true;
    }
    if (_file.write(bytes) != bytes.size()) {
        qCWarning(VideoRecordLog) << "recording write failed:" << _file.errorString();
        return false;
    }
    _fileHash.addData(bytes);
    _partBytes += bytes.size();
    return _file.flush();
}

void RecordingWriter::_onInit(const QByteArray &bytes)
{
    _pendingInit = bytes;
}

void RecordingWriter::_onFragment(const QByteArray &bytes, const Fmp4FragmentInfo &info)
{
    if (!_file.isOpen()) {
        return;
    }
    if (!_appendToFile(bytes)) {
        _writeFailed = true;
        return;
    }
    _fragments += 1;
    _chain = TimeMapWriter::chainNext(_chain, bytes);

    QJsonObject line;
    line[QStringLiteral("type")] = QStringLiteral("fragment");
    line[QStringLiteral("seq")] = qint64(info.seq);
    line[QStringLiteral("offset")] = info.offset;
    line[QStringLiteral("length")] = info.length;
    line[QStringLiteral("samples")] = info.samples;
    line[QStringLiteral("chain")] = QString::fromLatin1(_chain.toHex());
    if (info.samples > 0) {
        line[QStringLiteral("keyframe")] = info.startsWithKeyframe;
        line[QStringLiteral("pts")] = info.pts90k;
        line[QStringLiteral("dts")] = info.dts90k;
        line[QStringLiteral("duration")] = info.duration90k;
        if (_hasRtp) {
            line[QStringLiteral("rtp")] = info.sourceTime90k;
        } else if (info.sourceTime90k >= 0) {
            line[QStringLiteral("src_time")] = info.sourceTime90k;
        }
        line[QStringLiteral("recv_utc_ms")] = info.receiveUtcMs;
        line[QStringLiteral("recv_steady_ns")] = info.receiveSteadyNs;

        const auto it = _fragVehicle.constFind(info.firstIndex);
        if (it != _fragVehicle.constEnd()) {
            const VehicleSample &s = it.value();
            if (s.hasBoot) {
                line[QStringLiteral("boot_ms")] = s.bootMs;
            }
            if (s.hasGpsUtc) {
                line[QStringLiteral("gps_utc_us")] = s.gpsUtcUs;
            }
            if (s.hasBoot || s.hasGpsUtc) {
                line[QStringLiteral("time_age_ms")] = (info.receiveSteadyNs - s.timeSteadyNs) / kNsPerMs;
            }
            if (s.hasPosition) {
                line[QStringLiteral("lat")] = s.lat;
                line[QStringLiteral("lon")] = s.lon;
                if (s.hasAlt) {
                    line[QStringLiteral("alt_m")] = s.altM;
                }
                line[QStringLiteral("age_ms")] = (info.receiveSteadyNs - s.posSteadyNs) / kNsPerMs;
            }
            _fragVehicle.erase(it);
        }
    }
    if (!_timeMap.writeLine(line) || !_timeMap.flush()) {
        _writeFailed = true;
        return;
    }

    // fsync about every 5 s: a power cut loses at most about that much.
    if (info.receiveSteadyNs - _lastSyncNs >= _config.fsyncMs * kNsPerMs) {
        _lastSyncNs = info.receiveSteadyNs;
        if (!TimeMapWriter::syncFile(_file) || !_timeMap.sync()) {
            _writeFailed = true;
        }
    }
}

void RecordingWriter::_halt(int reason)
{
    if (partOpen()) {
        _closePart(reason);
    }
    _halted = true;
    _sessionActive = false;
    qCWarning(VideoRecordLog) << "recording stopped:" << stopText(reason);
    emit stopped(reason, stopText(reason));
}

void RecordingWriter::_closePart(int reason)
{
    if (!_file.isOpen()) {
        return;
    }
    if (_muxer->isOpen()) {
        if (!_muxer->finish()) {
            qCWarning(VideoRecordLog) << "recording muxer error at stop:" << _muxer->errorString();
        }
    }
    (void) TimeMapWriter::syncFile(_file);
    _file.close();
    const QString sha = QString::fromLatin1(_fileHash.result().toHex());
    const qint64 endedUtcMs = _lastRecvUtcMs > 0 ? _lastRecvUtcMs : QDateTime::currentMSecsSinceEpoch();

    double fps = 0.0;
    if (_units > 1 && _lastDts > _firstDts) {
        fps = qRound(double(_units - 1) * 90000.0 / double(_lastDts - _firstDts) * 100.0) / 100.0;
    }

    QJsonObject end;
    end[QStringLiteral("type")] = QStringLiteral("end");
    end[QStringLiteral("ended_at")] = isoUtc(endedUtcMs, true);
    end[QStringLiteral("ended_utc_ms")] = endedUtcMs;
    end[QStringLiteral("size")] = _partBytes;
    end[QStringLiteral("sha256")] = sha;
    end[QStringLiteral("fragments")] = qint64(_fragments);
    end[QStringLiteral("chain")] = QString::fromLatin1(_chain.toHex());
    TimeMapWriter::addFps(end, fps);
    switch (reason) {
    case StopRequested:     end[QStringLiteral("reason")] = QStringLiteral("stopped"); break;
    case StopSourceStopped: end[QStringLiteral("reason")] = QStringLiteral("source_stopped"); break;
    case StopDiskLow:       end[QStringLiteral("reason")] = QStringLiteral("disk_low"); break;
    case StopWriteError:    end[QStringLiteral("reason")] = QStringLiteral("write_error"); break;
    case StopMuxError:      end[QStringLiteral("reason")] = QStringLiteral("mux_error"); break;
    default:                end[QStringLiteral("reason")] = QStringLiteral("next_part"); break;
    }
    (void) _timeMap.writeLine(end);
    const QString sidecarPath = _timeMap.path();
    const qint64 sidecarSize = _timeMap.size();
    const QString sidecarSha = QString::fromLatin1(_timeMap.close());

    const VideoCodecInfo info = _muxer->codecInfo();
    QString flightRef;
    if (_partRefs.size() == 1) {
        flightRef = *_partRefs.constBegin();
    }

    QJsonObject entry;
    entry[QStringLiteral("name")] = _partName;
    entry[QStringLiteral("path")] = _file.fileName();
    entry[QStringLiteral("size")] = _partBytes;
    entry[QStringLiteral("sha256")] = sha;
    entry[QStringLiteral("sidecar_name")] = _sidecarName;
    entry[QStringLiteral("sidecar_path")] = sidecarPath;
    entry[QStringLiteral("sidecar_size")] = sidecarSize;
    entry[QStringLiteral("sidecar_sha256")] = sidecarSha;
    entry[QStringLiteral("camera")] = _config.camera;
    entry[QStringLiteral("codec")] = info.codecString;
    entry[QStringLiteral("width")] = info.width;
    entry[QStringLiteral("height")] = info.height;
    entry[QStringLiteral("fps")] = fps;
    entry[QStringLiteral("started_at")] = isoUtc(_partStartUtcMs);
    entry[QStringLiteral("ended_at")] = isoUtc(endedUtcMs);
    entry[QStringLiteral("part")] = _partNo;
    entry[QStringLiteral("flight_ref")] = flightRef;
    entry[QStringLiteral("sysid")] = _sysid;
    entry[QStringLiteral("base_url")] = _config.baseUrl;
    entry[QStringLiteral("station_id")] = _config.stationId;
    entry[QStringLiteral("recovered")] = false;

    qCInfo(VideoRecordLog) << "recording part closed:" << _partName << _partBytes << "bytes sha256" << sha;
    _partName.clear();
    _sidecarName.clear();
    _fragVehicle.clear();
    emit partClosed(entry);
}

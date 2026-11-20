/****************************************************************************
 *
 * RotorLab GC - recorded video to the Fleet Server (G95, contract S21).
 *
 * Mirrors FleetLogs: one upload at a time, a persisted queue (RecordingStore),
 * never in the telemetry loop, nothing modal, and it never deletes a file.
 *
 * For each part, in order:
 *   1. Register the video: POST /api/gc/v1/flights/<ref>/videos, or
 *      POST /api/gc/v1/stations/self/videos when no single flight ref is
 *      known. This is when Fleet writes the fingerprint to its audit chain,
 *      so it happens as soon as Fleet is reachable.
 *   2. Register its time map the same way, kind "video-timemap", part_of.
 *   3. Upload the video in 8 MB chunks, PUT /api/gc/v1/uploads/<id> with
 *      Content-Range and X-Chunk-SHA256, streamed from disk.
 *   4. Upload the time map.
 *   5. Done only on a final 200 whose sha256 and size equal the local ones.
 *
 * A chunk taken answers 200 {"offset": n} (or 204 with Upload-Offset); the
 * last one answers 200 {"sha256", "size"} on a whole-file match, or 400 with
 * Upload-Offset: 0 on a mismatch (RotorLab's GC connector API, "Recorded video").
 * Resume after any failure or restart with HEAD /uploads/<id> (Upload-Offset).
 * 409 with Upload-Offset: continue from there. 400 on a chunk: resend it;
 * three in a row: re-hash the local file and park it if it changed. A final
 * mismatch (400 to 0, or a 200 whose hash or size differs) restarts from 0
 * once, then parks. 401 stops every upload and keeps
 * everything. 413 parks. Network errors, 5xx, 503 and a 409 license refusal
 * back off 1, 2, 5, 15, 30 s, then every 5 min; when the server sends
 * Retry-After (Server 2.0 sends 30 on 503) that wait is used instead.
 * 20 attempts per part, then it parks with "Try again".
 *
 * A server whose body-size cap is below the chunk size answers 413 on the
 * chunk, or (more often, as in the S20/S21 end-to-end run) closes the
 * connection while the body is still being written, so Qt reports only
 * "Connection closed", "Unable to write" or "Unknown error". Both get one
 * plain sentence (the server refused this upload chunk as too large), never
 * the raw Qt text. A 413 on a chunk parks at once; a close during the body
 * backs off as usual, and the kMaxEarlyCloses-th close on the same chunk
 * parks the recording with "Try again" instead of retrying forever. The file
 * stays on disk either way.
 *
 * After the final 200 (S21): GET /api/gc/v1/uploads/<id> every few
 * minutes, backing off to hourly, for the server's state (uploading, stored,
 * handed_over), the flight_ref once Fleet attaches the recording to a flight,
 * and records_receipt {received_at, statement, signature} once Records has
 * confirmed it. The receipt is kept as received in
 * `<file>.records-receipt.json` next to the recording and never changed. It
 * stops when both the video and its time map are handed_over with a receipt,
 * or after kRecordsMaxPolls (then "Check again" restarts it). GC shows the
 * receipt; it does not check the signature and gates nothing on it.
 *
 * Each part is bound to the Fleet server and station enrolled when it was
 * captured, and goes only there. After a disconnect or an enrollment
 * elsewhere it waits for the operator's "Send to this server".
 *
 * The station token is read at the start of each upload, set only through
 * HttpClient's one header path, and never stored, logged or put in a URL.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QTimer>

#include <functional>

#include "net/HttpClient.h"

class RecordingStore;
class QNetworkReply;

class VideoUploader : public QObject
{
    Q_OBJECT

public:
    /// Where the uploader reads the enrollment. Defaults use FleetStore and
    /// FleetConnector; tests give their own.
    struct Environment
    {
        std::function<bool()>    canUpload;     ///< enrolled, not suspended, not revoked, upload on
        std::function<QString()> baseUrl;       ///< the enrolled server now
        std::function<int()>     stationId;     ///< the enrolled station now
        std::function<QString()> token;         ///< read at the start of each upload
        std::function<QString()> pin;           ///< the enrollment's server key pin
    };

    static constexpr qint64 kChunkBytes = 8LL * 1024 * 1024;
    static constexpr int kMaxAttempts = 20;
    /// Closes during the body of the same chunk before the recording parks.
    static constexpr int kMaxEarlyCloses = 3;
    /// Status checks after the final 200 before it stops asking (about 3 days
    /// at the hourly cap); the operator's "Check again" starts it over.
    static constexpr int kRecordsMaxPolls = 72;

    VideoUploader(RecordingStore *store, HttpClient *client, const Environment &env,
                  QObject *parent = nullptr);
    ~VideoUploader() override;

    static Environment fleetEnvironment(const std::function<bool()> &uploadEnabled);

    /// Tests: smaller chunks, and backoff seconds counted in `msPerSecond` ms.
    void setChunkBytes(qint64 bytes) { _chunk = bytes; }
    void setBackoffUnitMs(int msPerSecond) { _unitMs = msPerSecond; }
    void setSweepMs(int ms);

    /// Look for work now.
    void kick();
    /// Fleet went away (revoked, disconnected): drop the request in flight,
    /// keep every file and every entry.
    void stopAll();
    /// Fleet is reachable and enrolled again (clears a 401 stop).
    void resume();

    /// Operator actions.
    void retry(const QString &id);
    void sendToThisServer(const QString &id);
    /// Ask the Fleet Server about a stored recording again after the status
    /// checks gave up (kRecordsMaxPolls, or the server no longer listed it).
    void checkRecords(const QString &id);

    bool    busy() const { return _busy; }
    bool    revoked() const { return _revoked; }
    QString currentId() const { return _currentId; }
    /// One line for the settings page when uploads as a whole are stopped.
    QString statusText() const;

    /// Paths for the routes (public for tests).
    static QString registerPath(const QString &flightRef);
    static QString uploadPath(const QString &uploadId);

    /// Where the Records receipt for `filePath` is kept.
    static QString receiptPath(const QString &filePath);
    /// True when this done entry still waits for the server's status or
    /// Records' receipt (and has not given up asking).
    static bool wantsRecordsStatus(const QJsonObject &e);

signals:
    void changed();

private:
    enum class Kind { Video, Sidecar };

    void _sweep();
    bool _eligible(QJsonObject &e, bool *bindingChanged);
    void _step();
    void _register(Kind kind);
    void _head(Kind kind);
    void _sendChunk(Kind kind);
    void _verifyThen(Kind kind, std::function<void()> next);
    void _onChunkReply(Kind kind, qint64 offset, qint64 length, const HttpReply &r);
    void _onEarlyClose(Kind kind, qint64 offset, const HttpReply &r);
    bool _closedEarly(const HttpReply &r) const;
    QString _chunkTooLargeText() const;
    void _chunkTaken();
    void _done(Kind kind, const HttpReply &r);
    void _restartOrPark(Kind kind, const QString &why);
    bool _common(const HttpReply &r);
    void _backoff(const QString &why, int retryAfter);
    void _park(const QString &why, bool retryable);
    void _finish();
    void _save();
    void _configureClient();
    void _startRecordsPolling();
    void _pollRecords(Kind kind);
    void _onRecordsReply(Kind kind, const HttpReply &r);
    void _keepReceipt(Kind kind, const QJsonObject &receipt);
    void _pollLater(int retryAfter);
    void _recordsDetail();
    static bool _kindFinished(const QJsonObject &e, const QString &prefix);

    QJsonObject &_cur() { return _entry; }
    QString _k(Kind kind, const char *field) const;
    QString _path(Kind kind) const;
    qint64  _size(Kind kind) const;
    QString _sha(Kind kind) const;

    RecordingStore *_store = nullptr;
    QPointer<HttpClient> _client;
    Environment _env;
    QTimer _timer;
    QTimer _wake;

    bool    _busy = false;
    bool    _revoked = false;
    quint64 _session = 0;               ///< bumped by stopAll(); stale callbacks check it
    QString _currentId;
    QJsonObject _entry;                 ///< working copy of the current entry
    bool    _verified = false;          ///< file re-hashed in this upload session
    bool    _needHead = true;           ///< ask the server for the offset before the next chunk
    int     _consecutive400 = 0;
    bool    _answered = false;          ///< the server gave an HTTP answer in this upload pass
    QPointer<QNetworkReply> _reply;

    qint64  _chunk = kChunkBytes;
    int     _unitMs = 1000;
};

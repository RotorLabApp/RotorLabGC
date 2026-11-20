#include "FFmpegReceiverTest.h"
#include "FFmpegFrameBridge.h"
#include "FFmpegReceiver.h"
#include "RtpDepacketizer.h"
#include "VideoPacketTap.h"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtMultimedia/QVideoFrame>
#include <QtMultimediaQuick/private/qquickvideooutput_p.h>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QUdpSocket>
#include <QtQuick/QQuickItem>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <cstring>
#include <limits>
#include <memory>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

quint16 freeUdpPort()
{
    QUdpSocket probe;
    if (!probe.bind(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const quint16 port = probe.localPort();
    probe.close();
    return port;
}

QByteArray rtpHeader(quint16 seq, quint32 ts, bool marker, quint32 ssrc = 0x0a0b0c0d)
{
    QByteArray p(12, 0);
    p[0] = char(0x80);
    p[1] = char((marker ? 0x80 : 0) | 96);
    p[2] = char(seq >> 8);
    p[3] = char(seq);
    p[4] = char(ts >> 24);
    p[5] = char(ts >> 16);
    p[6] = char(ts >> 8);
    p[7] = char(ts);
    p[8] = char(ssrc >> 24); p[9] = char(ssrc >> 16); p[10] = char(ssrc >> 8); p[11] = char(ssrc);
    return p;
}

struct Rng
{
    quint32 s;
    quint32 next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<quint32>(n)); }
};

QByteArray randomBytes(Rng &rng, int n)
{
    QByteArray b(n, Qt::Uninitialized);
    for (int i = 0; i < n; ++i) {
        b[i] = char(rng.next());
    }
    return b;
}

// Hang guard only: every wait below ends on the signal it waits for, never on the clock
constexpr int kHangGuardMs = 60000;

/// Pops items into items (if given) until an event of this type arrives. False only at the hang guard.
bool waitForEvent(VideoPacketConsumer &consumer, VideoTapEvent::Type type, QList<VideoTapItem> *items = nullptr,
                  VideoTapEvent::StopReason *reason = nullptr)
{
    VideoTapItem item;
    while (consumer.waitPop(item, kHangGuardMs)) {
        if (items) {
            items->append(item);
        }
        if (std::holds_alternative<VideoTapEvent>(item) && (std::get<VideoTapEvent>(item).type == type)) {
            if (reason) {
                *reason = std::get<VideoTapEvent>(item).stopReason;
            }
            return true;
        }
    }
    return false;
}

AVFrame *makeFrame(AVPixelFormat format, int width, int height)
{
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        return nullptr;
    }
    frame->format = format;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    for (int plane = 0; (plane < AV_NUM_DATA_POINTERS) && frame->data[plane]; ++plane) {
        const int rows = (plane == 0) ? height : (height + 1) / 2;
        for (int y = 0; y < rows; ++y) {
            std::memset(frame->data[plane] + static_cast<qsizetype>(y) * frame->linesize[plane], 0x40 + plane, frame->linesize[plane]);
        }
    }
    return frame;
}

} // namespace

void FFmpegReceiverTest::_sinkOnlyFromVideoOutput()
{
    QCOMPARE(FFmpegReceiver::createVideoSink(nullptr), nullptr);

    QQuickItem plainItem;
    plainItem.setObjectName(QStringLiteral("videoContent"));
    QCOMPARE(FFmpegReceiver::createVideoSink(&plainItem), nullptr);

    QQuickVideoOutput videoOutput;
    QVERIFY(videoOutput.videoSink() != nullptr);
    QCOMPARE(FFmpegReceiver::createVideoSink(&videoOutput), static_cast<void*>(videoOutput.videoSink()));
}

void FFmpegReceiverTest::_librariesMatch()
{
    // GC runs on Qt's own FFmpeg with the majors of the headers it was built with
    QVERIFY2(FFmpegReceiver::librariesMatchHeaders(), qPrintable(FFmpegReceiver::libraryVersions()));
    QVERIFY(FFmpegReceiver::libraryVersions().startsWith(QStringLiteral("avcodec ")));
}

void FFmpegReceiverTest::_badUriRefused()
{
    for (const QString &uri : { QStringLiteral("udp://camera.local:5600"), QStringLiteral("http://192.0.2.1/x"),
                                QStringLiteral("tcp://192.0.2.1:5600?listen=1"), QString() }) {
        FFmpegReceiver receiver;
        receiver.setName(QStringLiteral("videoContent"));
        receiver.setUri(uri);
        QSignalSpy spy(&receiver, &VideoReceiver::onStartComplete);
        QVERIFY(spy.isValid());
        receiver.start(3);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(qvariant_cast<VideoReceiver::STATUS>(spy.at(0).at(0)), VideoReceiver::STATUS_INVALID_URL);
        QSignalSpy stopSpy(&receiver, &VideoReceiver::onStopComplete);
        receiver.stop();
        QCOMPARE(qvariant_cast<VideoReceiver::STATUS>(stopSpy.at(0).at(0)), VideoReceiver::STATUS_INVALID_STATE);
    }
}

void FFmpegReceiverTest::_frameBridgeOddSizes()
{
    struct Case { AVPixelFormat av; QVideoFrameFormat::PixelFormat qt; int w; int h; };
    const QList<Case> cases = {
        { AV_PIX_FMT_YUV420P, QVideoFrameFormat::Format_YUV420P, 33, 17 },
        { AV_PIX_FMT_YUVJ420P, QVideoFrameFormat::Format_YUV420P, 2, 2 },
        { AV_PIX_FMT_NV12, QVideoFrameFormat::Format_NV12, 35, 19 },
        { AV_PIX_FMT_YUV420P, QVideoFrameFormat::Format_YUV420P, 1280, 720 },
    };
    for (const Case &c : cases) {
        AVFrame *frame = makeFrame(c.av, c.w, c.h);
        QVERIFY(frame);
        QVideoFrame out = FFmpegFrameBridge::toVideoFrame(frame);
        av_frame_free(&frame);
        QVERIFY2(out.isValid(), qPrintable(QStringLiteral("%1x%2").arg(c.w).arg(c.h)));
        QCOMPARE(out.size(), QSize(c.w, c.h));
        QCOMPARE(out.pixelFormat(), c.qt);
        QVERIFY(out.map(QVideoFrame::ReadOnly));
        QCOMPARE(int(out.bits(0)[0]), 0x40);
        QCOMPARE(int(out.bits(1)[0]), 0x41);
        out.unmap();
    }

    // A pixel format the bridge does not handle, and no picture at all
    AVFrame *rgb = makeFrame(AV_PIX_FMT_RGB24, 16, 16);
    QVERIFY(rgb);
    QVERIFY(!FFmpegFrameBridge::toVideoFrame(rgb).isValid());
    av_frame_free(&rgb);
    QVERIFY(!FFmpegFrameBridge::toVideoFrame(nullptr).isValid());
    AVFrame *empty = av_frame_alloc();
    QVERIFY(!FFmpegFrameBridge::toVideoFrame(empty).isValid());
    av_frame_free(&empty);
}

void FFmpegReceiverTest::_frameBridgeRefusesShortPlanes()
{
    AVFrame *frame = makeFrame(AV_PIX_FMT_YUV420P, 64, 32);
    QVERIFY(frame);
    const int saved = frame->linesize[1];
    frame->linesize[1] = 8;                 // shorter than a chroma row (32 bytes)
    QVERIFY(!FFmpegFrameBridge::toVideoFrame(frame).isValid());
    frame->linesize[1] = saved;
    uint8_t *const plane = frame->data[2];
    frame->data[2] = nullptr;               // missing plane
    QVERIFY(!FFmpegFrameBridge::toVideoFrame(frame).isValid());
    frame->data[2] = plane;
    QVERIFY(FFmpegFrameBridge::toVideoFrame(frame).isValid());
    av_frame_free(&frame);
}

void FFmpegReceiverTest::_hostileUdpInput()
{
    const quint16 port = freeUdpPort();
    QVERIFY(port != 0);

    FFmpegReceiver receiver;
    receiver.setName(QStringLiteral("videoContent"));
    receiver.setUri(QStringLiteral("udp://127.0.0.1:%1").arg(port));
    QSignalSpy startSpy(&receiver, &VideoReceiver::onStartComplete);
    std::shared_ptr<VideoPacketConsumer> started = receiver.packetTap()->attach(1);
    receiver.start(3);
    QCOMPARE(qvariant_cast<VideoReceiver::STATUS>(startSpy.at(0).at(0)), VideoReceiver::STATUS_OK);
    // StreamStarted is published once the worker's socket is bound
    QVERIFY(waitForEvent(*started, VideoTapEvent::Type::StreamStarted));
    receiver.packetTap()->detach(started);

    QUdpSocket sender;
    const QHostAddress to(QHostAddress::LocalHost);
    Rng rng{ 0x5eed5eedu };
    quint16 seq = 1;
    quint32 ts = 0;
    auto send = [&](const QByteArray &datagram) {
        (void) sender.writeDatagram(datagram, to, port);
    };

    for (int round = 0; round < 4; ++round) {
        // Random bytes of every size up to a typical MTU
        for (int i = 0; i < 100; ++i) {
            send(randomBytes(rng, rng.below(1500)));
        }
        // Believable RTP headers with random payloads, flags and lengths
        for (int i = 0; i < 100; ++i) {
            QByteArray p = rtpHeader(seq++, ts, rng.below(3) == 0) + randomBytes(rng, 1 + rng.below(1400));
            p[0] = char(0x80 | (rng.next() & 0x3f));
            send(p);
            if (rng.below(4) == 0) {
                ts += 3000;
            }
        }
        // "Key frames" of garbage: FU-A IDR fragments that reach the H.264 decoder
        for (int f = 0; f < 5; ++f) {
            send(rtpHeader(seq++, ts, false) + QByteArray("\x7c\x85", 2) + randomBytes(rng, 1200));
            send(rtpHeader(seq++, ts, false) + QByteArray("\x7c\x05", 2) + randomBytes(rng, 1200));
            send(rtpHeader(seq++, ts, true) + QByteArray("\x7c\x45", 2) + randomBytes(rng, 1200));
            ts += 3000;
            // Parameter sets of garbage too
            send(rtpHeader(seq++, ts, false) + QByteArray("\x67", 1) + randomBytes(rng, 40));
            send(rtpHeader(seq++, ts, false) + QByteArray("\x68", 1) + randomBytes(rng, 20));
            send(rtpHeader(seq++, ts, true) + QByteArray("\x65", 1) + randomBytes(rng, 900));
            ts += 3000;
        }
        // The largest datagram loopback carries (may be refused by the OS; that is fine too)
        send(rtpHeader(seq++, ts, true) + QByteArray("\x65", 1) + randomBytes(rng, 65507 - 13));
        QTest::qWait(50);
    }

    QTRY_VERIFY_WITH_TIMEOUT(receiver.counters().packets > 0, kHangGuardMs);
    QTRY_VERIFY_WITH_TIMEOUT(receiver.counters().malformedPackets + receiver.counters().refusedPackets > 0, kHangGuardMs);

    QElapsedTimer timer;
    timer.start();
    receiver.stop();
    QVERIFY2(timer.elapsed() < 4500,   // well under FFmpeg's own 5 s I/O timeout, with room for a loaded runner
        qPrintable(QString::number(timer.elapsed())));

    const FFmpegReceiver::Counters c = receiver.counters();
    qInfo().noquote() << QStringLiteral("hostile UDP: packets=%1 malformed=%2 refused=%3 dropped_access_units=%4 decode_errors=%5 decoded=%6")
                             .arg(c.packets).arg(c.malformedPackets).arg(c.refusedPackets).arg(c.droppedAccessUnits).arg(c.decodeErrors).arg(c.decoded);
    QCOMPARE(c.shown, quint64(0));   // no sink: nothing is set anywhere
}

void FFmpegReceiverTest::_stopIsPrompt()
{
    // UDP (GC's own socket) and MPEG-TS over UDP (libavformat, blocked waiting for data):
    // stop must not wait for FFmpeg's 5 s timeout
    for (const QString &scheme : { QStringLiteral("udp"), QStringLiteral("mpegts"), QStringLiteral("tcp") }) {
        const quint16 port = freeUdpPort();
        QVERIFY(port != 0);
        FFmpegReceiver receiver;
        receiver.setName(QStringLiteral("videoContent"));
        receiver.setUri(QStringLiteral("%1://127.0.0.1:%2").arg(scheme).arg(port));
        receiver.start(3);
        QTest::qWait(400);
        QElapsedTimer timer;
        timer.start();
        receiver.stop();
        QVERIFY2(timer.elapsed() < 4500,   // well under FFmpeg's own 5 s I/O timeout, with room for a loaded runner
        qPrintable(scheme + QStringLiteral(" took %1 ms").arg(timer.elapsed())));
        QCOMPARE(receiver.counters().decoded, quint64(0));
    }
}

void FFmpegReceiverTest::_packetTapOnLoopback()
{
    const quint16 port = freeUdpPort();
    QVERIFY(port != 0);

    FFmpegReceiver receiver;
    receiver.setName(QStringLiteral("videoContent"));
    receiver.setUri(QStringLiteral("udp://127.0.0.1:%1").arg(port));
    VideoPacketTap *const tap = receiver.packetTap();
    QVERIFY(tap);
    QVERIFY(!tap->hasConsumers());
    std::shared_ptr<VideoPacketConsumer> consumer = tap->attach(100);
    QVERIFY(consumer);

    receiver.start(3);
    // StreamStarted is published once the socket is bound: send only then
    QList<VideoTapItem> firstItems;
    QVERIFY(waitForEvent(*consumer, VideoTapEvent::Type::StreamStarted, &firstItems));

    const QByteArray sps("\x67\x4d\x00\x1f", 4);
    const QByteArray pps("\x68\xee\x3c\x80", 4);
    const QByteArray idr("\x65\x88\x84\x00\x10", 5);
    const QByteArray pslice("\x41\x9a\x02", 3);
    const QByteArray sc("\x00\x00\x00\x01", 4);
    QByteArray stap("\x78", 1);
    for (const QByteArray &n : { sps, pps }) {
        stap.append(char(0));
        stap.append(char(n.size()));
        stap.append(n);
    }

    QUdpSocket sender;
    const QHostAddress to(QHostAddress::LocalHost);
    quint16 seq = 100;
    for (quint32 frame = 0; frame < 10; ++frame) {
        const quint32 ts = 90000 + frame * 3000;
        if ((frame % 5) == 0) {
            (void) sender.writeDatagram(rtpHeader(seq++, ts, false) + stap, to, port);
            (void) sender.writeDatagram(rtpHeader(seq++, ts, true) + idr, to, port);
        } else {
            (void) sender.writeDatagram(rtpHeader(seq++, ts, true) + pslice, to, port);
        }
        QTest::qWait(5);
    }

    QList<VideoTapPacket> packets;
    QList<VideoTapEvent> events;
    for (const VideoTapItem &item : std::as_const(firstItems)) {
        events.append(std::get<VideoTapEvent>(item));
    }
    while (packets.size() < 10) {
        VideoTapItem item;
        if (!consumer->waitPop(item, kHangGuardMs)) {
            break;
        }
        if (std::holds_alternative<VideoTapPacket>(item)) {
            packets.append(std::get<VideoTapPacket>(item));
        } else {
            events.append(std::get<VideoTapEvent>(item));
        }
    }
    QCOMPARE(packets.size(), 10);
    QVERIFY(!events.isEmpty());
    QVERIFY(events.first().type == VideoTapEvent::Type::StreamStarted);
    QVERIFY(events.first().codecKnown);
    QVERIFY(events.first().codec == VideoTapCodec::H264);
    for (int i = 0; i < packets.size(); ++i) {
        const VideoTapPacket &p = packets.at(i);
        QCOMPARE(p.rtpTimestamp, quint32(90000 + i * 3000));
        QVERIFY(p.hasRtpTimestamp);
        QCOMPARE(p.ssrc, quint32(0x0a0b0c0d));
        QCOMPARE(p.keyFrame, (i % 5) == 0);
        QCOMPARE(p.parameterSets, sc + sps + sc + pps);
        QCOMPARE(p.data, ((i % 5) == 0) ? (sc + sps + sc + pps + sc + idr) : (sc + pslice));
        QCOMPARE(p.generation, events.first().generation);
        QVERIFY(p.receiveSteadyNs > 0);
        QCOMPARE(p.pts90k, qint64(p.rtpTimestamp));
        QCOMPARE(p.dts90k, p.pts90k);
        QCOMPARE(p.discontinuity, i == 0);
        QVERIFY(qAbs(p.receiveUtcMs - QDateTime::currentMSecsSinceEpoch()) < 60000);
        QVERIFY(p.receiveUtcMs >= events.first().utcAnchorMs);
        if (i > 0) {
            QVERIFY(p.index > packets.at(i - 1).index);
        }
    }

    // Another sender after the first went quiet: SsrcChanged, then its packets on a new stream id
    QTest::qWait(700);
    (void) sender.writeDatagram(rtpHeader(5000, 1000, false, 0x01020304) + stap, to, port);
    (void) sender.writeDatagram(rtpHeader(5001, 1000, true, 0x01020304) + idr, to, port);
    VideoTapEvent ssrcEvent;
    bool gotEvent = false;
    bool gotPacket = false;
    while (!(gotEvent && gotPacket)) {
        VideoTapItem item;
        if (!consumer->waitPop(item, kHangGuardMs)) {
            break;
        }
        if (std::holds_alternative<VideoTapEvent>(item)) {
            const VideoTapEvent &e = std::get<VideoTapEvent>(item);
            if (e.type == VideoTapEvent::Type::SsrcChanged) {
                ssrcEvent = e;
                gotEvent = true;
            }
        } else {
            const VideoTapPacket &p = std::get<VideoTapPacket>(item);
            QVERIFY(gotEvent);     // the event comes first
            QCOMPARE(p.ssrc, quint32(0x01020304));
            QCOMPARE(p.generation, ssrcEvent.generation);
            QVERIFY(p.generation != packets.first().generation);
            QVERIFY(p.discontinuity);
            gotPacket = true;
        }
    }
    QVERIFY(gotEvent && gotPacket);
    QCOMPARE(ssrcEvent.ssrc, quint32(0x01020304));

    // Detach while streaming, then stop: StreamStopped reaches a consumer still attached
    std::shared_ptr<VideoPacketConsumer> second = tap->attach(10);
    tap->detach(consumer);
    receiver.stop();
    bool stopped = false;
    VideoTapItem item;
    while (second->tryPop(item)) {
        if (std::holds_alternative<VideoTapEvent>(item) && (std::get<VideoTapEvent>(item).type == VideoTapEvent::Type::StreamStopped)) {
            stopped = true;
        }
    }
    QVERIFY(stopped);
}

// The source-stopped signal for recording and relay: no packets for the receiver's timeout
// gives StreamStopped(Timeout); packets coming back give StreamStarted and a new generation
// whose first unit is a discontinuity; a lost packet marks the next unit too.
void FFmpegReceiverTest::_packetTapTimeoutAndGenerations()
{
    const quint16 port = freeUdpPort();
    QVERIFY(port != 0);

    FFmpegReceiver receiver;
    receiver.setName(QStringLiteral("videoContent"));
    receiver.setUri(QStringLiteral("udp://127.0.0.1:%1").arg(port));
    std::shared_ptr<VideoPacketConsumer> consumer = receiver.packetTap()->attach(100);
    receiver.start(1);   // 1 s timeout
    QList<VideoTapItem> items;
    QVERIFY(waitForEvent(*consumer, VideoTapEvent::Type::StreamStarted, &items));   // socket bound

    const QByteArray idr("\x65\x88\x84\x00\x10", 5);
    const QByteArray pslice("\x41\x9a\x02", 3);
    QUdpSocket sender;
    const QHostAddress to(QHostAddress::LocalHost);
    auto burst = [&](quint16 seq, quint32 ts) {
        (void) sender.writeDatagram(rtpHeader(seq, ts, true) + idr, to, port);
        (void) sender.writeDatagram(rtpHeader(seq + 1, ts + 3000, true) + pslice, to, port);
        (void) sender.writeDatagram(rtpHeader(seq + 3, ts + 9000, true) + idr, to, port);   // seq + 2 lost
    };

    burst(10, 0);
    // Wait for the receiver's own timeout report (sent after 1 s without packets), then resume
    VideoTapEvent::StopReason reason = VideoTapEvent::StopReason::Stopped;
    QVERIFY(waitForEvent(*consumer, VideoTapEvent::Type::StreamStopped, &items, &reason));
    QVERIFY(reason == VideoTapEvent::StopReason::Timeout);
    burst(20, 90000);       // the same sender comes back
    QVERIFY(waitForEvent(*consumer, VideoTapEvent::Type::StreamStarted, &items));
    int packets = 0;
    while (packets < 3) {
        VideoTapItem item;
        if (!consumer->waitPop(item, kHangGuardMs)) {
            break;
        }
        packets += std::holds_alternative<VideoTapPacket>(item) ? 1 : 0;
        items.append(item);
    }
    receiver.stop();
    VideoTapItem item;
    while (consumer->tryPop(item)) {
        items.append(item);
    }

    // StreamStarted, 3 units, StreamStopped(Timeout), StreamStarted, 3 units, StreamStopped(Stopped)
    QStringList shape;
    for (const VideoTapItem &i : std::as_const(items)) {
        if (std::holds_alternative<VideoTapPacket>(i)) {
            const VideoTapPacket &p = std::get<VideoTapPacket>(i);
            shape << QStringLiteral("P%1%2").arg(p.generation).arg(p.discontinuity ? QStringLiteral("d") : QString());
        } else {
            const VideoTapEvent &e = std::get<VideoTapEvent>(i);
            QString name;
            switch (e.type) {
            case VideoTapEvent::Type::StreamStarted: name = QStringLiteral("Start"); break;
            case VideoTapEvent::Type::StreamStopped:
                name = (e.stopReason == VideoTapEvent::StopReason::Timeout) ? QStringLiteral("StopTimeout") : QStringLiteral("Stop");
                break;
            case VideoTapEvent::Type::SourceChanged: name = QStringLiteral("Source"); break;
            case VideoTapEvent::Type::SsrcChanged: name = QStringLiteral("Ssrc"); break;
            }
            shape << QStringLiteral("%1%2").arg(name).arg(e.generation);
            if (e.type != VideoTapEvent::Type::StreamStopped) {
                QVERIFY(e.utcAnchorMs > 0);
                QVERIFY(e.steadyAnchorNs > 0);
            }
        }
    }
    const QString got = shape.join(QLatin1Char(' '));
    QCOMPARE(got, QStringLiteral("Start1 P1d P1 P1d StopTimeout1 Start2 P2d P2 P2d Stop2"));
}

void FFmpegReceiverTest::_udpPortInUse()
{
    // m4080: another program holds the UDP video port (for example a second GC on 5600)
    const quint16 port = freeUdpPort();
    QVERIFY(port != 0);
    auto holder = std::make_unique<QUdpSocket>();
    QVERIFY2(holder->bind(QHostAddress::LocalHost, port, QUdpSocket::DontShareAddress), qPrintable(holder->errorString()));

    FFmpegReceiver receiver;
    receiver.setName(QStringLiteral("videoContent"));
    receiver.setUri(QStringLiteral("udp://127.0.0.1:%1").arg(port));
    QSignalSpy statusSpy(&receiver, &VideoReceiver::receiveStatusChanged);
    QSignalSpy startSpy(&receiver, &VideoReceiver::onStartComplete);
    std::shared_ptr<VideoPacketConsumer> started = receiver.packetTap()->attach(16);
    QCOMPARE(receiver.receiveStatus(), VideoReceiver::RECEIVE_STATUS_OK);

    receiver.start(3);
    QCOMPARE(qvariant_cast<VideoReceiver::STATUS>(startSpy.at(0).at(0)), VideoReceiver::STATUS_OK);

    // Told once, with the port named
    QTRY_COMPARE_WITH_TIMEOUT(statusSpy.count(), 1, kHangGuardMs);
    QCOMPARE(receiver.receiveStatus(), VideoReceiver::RECEIVE_STATUS_PORT_IN_USE);
    const QString expected = QStringLiteral("Video is not received: UDP port %1 is in use by another program on this computer. "
                                            "Close it or choose another port in Video settings.").arg(port);
    QCOMPARE(receiver.receiveStatusText(), expected);
    QCOMPARE(receiver.receiveStatusText(), FFmpegReceiver::portInUseText(port));
    QCOMPARE(qvariant_cast<VideoReceiver::RECEIVE_STATUS>(statusSpy.at(0).at(0)), VideoReceiver::RECEIVE_STATUS_PORT_IN_USE);
    QCOMPARE(statusSpy.at(0).at(1).toString(), expected);

    // Still retrying, and not told again: several retries fit in this wait (250, 500, 1000 ms)
    QTest::qWait(FFmpegReceiver::kBindRetryFirstMs * 7);
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(receiver.receiveStatus(), VideoReceiver::RECEIVE_STATUS_PORT_IN_USE);
    QCOMPARE(started->stats().events, quint64(0));   // the worker never bound, so no stream started

    // The port is released: the receiver binds on a retry and the text clears
    holder->close();
    holder.reset();
    QVERIFY(waitForEvent(*started, VideoTapEvent::Type::StreamStarted));
    QTRY_COMPARE_WITH_TIMEOUT(statusSpy.count(), 2, kHangGuardMs);
    QCOMPARE(receiver.receiveStatus(), VideoReceiver::RECEIVE_STATUS_OK);
    QVERIFY(receiver.receiveStatusText().isEmpty());
    QCOMPARE(qvariant_cast<VideoReceiver::RECEIVE_STATUS>(statusSpy.at(1).at(0)), VideoReceiver::RECEIVE_STATUS_OK);

    // And it receives on that port
    QUdpSocket sender;
    (void) sender.writeDatagram(rtpHeader(1, 0, true) + QByteArray("e", 1) + QByteArray(100, 'x'), QHostAddress(QHostAddress::LocalHost), port);
    QTRY_VERIFY_WITH_TIMEOUT(receiver.counters().packets > 0, kHangGuardMs);

    receiver.packetTap()->detach(started);
    receiver.stop();
    QCOMPARE(receiver.receiveStatus(), VideoReceiver::RECEIVE_STATUS_OK);
    QCOMPARE(statusSpy.count(), 2);
}

void FFmpegReceiverTest::_uriChangeWhileBinding()
{
    // G88 M1: the worker never reads _uri. setUri on the GUI thread while the worker is still
    // in its bind loop (the port is held, so it retries for as long as the test wants) must not
    // reach the worker: the stream it then starts is reported with the URI it was started on.
    const quint16 port = freeUdpPort();
    QVERIFY(port != 0);
    auto holder = std::make_unique<QUdpSocket>();
    QVERIFY2(holder->bind(QHostAddress::LocalHost, port, QUdpSocket::DontShareAddress), qPrintable(holder->errorString()));

    const QString startedUri = QStringLiteral("udp://127.0.0.1:%1").arg(port);
    FFmpegReceiver receiver;
    receiver.setName(QStringLiteral("videoContent"));
    receiver.setUri(startedUri);
    QSignalSpy statusSpy(&receiver, &VideoReceiver::receiveStatusChanged);
    std::shared_ptr<VideoPacketConsumer> consumer = receiver.packetTap()->attach(16);
    receiver.start(3);

    // The worker is in its bind loop once it reports the port in use
    QTRY_COMPARE_WITH_TIMEOUT(statusSpy.count(), 1, kHangGuardMs);
    QCOMPARE(receiver.receiveStatus(), VideoReceiver::RECEIVE_STATUS_PORT_IN_USE);
    const quint16 otherPort = (port < 65000) ? quint16(port + 1) : quint16(1024);
    for (int i = 0; i < 50; ++i) {
        receiver.setUri(QStringLiteral("udp://127.0.0.1:%1").arg((i % 2) ? port : otherPort));
    }
    receiver.setUri(QStringLiteral("udp://127.0.0.1:%1").arg(otherPort));

    holder->close();
    holder.reset();
    QList<VideoTapItem> items;
    QVERIFY(waitForEvent(*consumer, VideoTapEvent::Type::StreamStarted, &items));
    QVERIFY(!items.isEmpty());
    const VideoTapEvent started = std::get<VideoTapEvent>(items.last());
    QCOMPARE(started.source, FFmpegSource::logText(startedUri));

    receiver.packetTap()->detach(consumer);
    receiver.stop();
}

void FFmpegReceiverTest::_avformatPacketBound()
{
    // G88 M3: RTSP, MPEG-TS and TCP refuse what the UDP depacketizer refuses
    QCOMPARE(FFmpegReceiver::kMaxAvformatPacketBytes, RtpDepacketizer::kDefaultMaxAccessUnitBytes);
    QVERIFY(FFmpegReceiver::avformatPacketAllowed(0));
    QVERIFY(FFmpegReceiver::avformatPacketAllowed(1));
    QVERIFY(FFmpegReceiver::avformatPacketAllowed(RtpDepacketizer::kDefaultMaxAccessUnitBytes));
    QVERIFY(!FFmpegReceiver::avformatPacketAllowed(RtpDepacketizer::kDefaultMaxAccessUnitBytes + 1));
    QVERIFY(!FFmpegReceiver::avformatPacketAllowed(std::numeric_limits<int>::max()));
    QVERIFY(!FFmpegReceiver::avformatPacketAllowed(-1));
    // Every unit either path lets through fits in the smallest tap byte budget
    QVERIFY(qint64(FFmpegReceiver::kMaxAvformatPacketBytes) < VideoPacketTap::kMinMaxBytes);
}

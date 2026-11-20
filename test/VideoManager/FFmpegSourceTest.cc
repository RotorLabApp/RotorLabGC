#include "FFmpegSourceTest.h"
#include "FFmpegSource.h"

#include <QtTest/QTest>

namespace {

QByteArray option(const FFmpegSource &s, const QByteArray &key)
{
    for (const auto &kv : s.formatOptions) {
        if (kv.first == key) {
            return kv.second;
        }
    }
    return QByteArray();
}

} // namespace

void FFmpegSourceTest::_udpH264()
{
    const FFmpegSource s = FFmpegSource::fromUri(QStringLiteral("udp://0.0.0.0:5600"));
    QVERIFY2(s.isValid(), qPrintable(s.error));
    QVERIFY(s.kind == FFmpegSource::Kind::Rtp);
    QCOMPARE(s.codecName, QStringLiteral("h264"));
    QCOMPARE(s.host, QStringLiteral("0.0.0.0"));
    QCOMPARE(s.port, 5600);
    // GC's own socket: nothing goes to libavformat
    QVERIFY(s.url.isEmpty());
    QVERIFY(s.formatOptions.isEmpty());
}

void FFmpegSourceTest::_udp265()
{
    const FFmpegSource s = FFmpegSource::fromUri(QStringLiteral("udp265://192.168.42.10:5601"));
    QVERIFY(s.isValid());
    QVERIFY(s.kind == FFmpegSource::Kind::Rtp);
    QCOMPARE(s.codecName, QStringLiteral("hevc"));
    QCOMPARE(s.host, QStringLiteral("192.168.42.10"));
    QCOMPARE(s.port, 5601);
}

void FFmpegSourceTest::_udpPortOnly()
{
    // VideoManager writes "udp://" + the setting, and the setting may be a bare port
    for (const QString &uri : { QStringLiteral("udp://5600"), QStringLiteral("udp://:5600") }) {
        const FFmpegSource s = FFmpegSource::fromUri(uri);
        QVERIFY2(s.isValid(), qPrintable(uri));
        QCOMPARE(s.host, QStringLiteral("0.0.0.0"));
        QCOMPARE(s.port, 5600);
    }
}

void FFmpegSourceTest::_udpIpv6()
{
    const FFmpegSource s = FFmpegSource::fromUri(QStringLiteral("udp://[::1]:5600"));
    QVERIFY(s.isValid());
    QCOMPARE(s.host, QStringLiteral("::1"));
    QVERIFY(!FFmpegSource::fromUri(QStringLiteral("udp://::1:5600")).isValid());
}

void FFmpegSourceTest::_rtsp()
{
    const QString uri = QStringLiteral("rtsp://192.168.42.1:8554/live");
    const FFmpegSource s = FFmpegSource::fromUri(uri);
    QVERIFY2(s.isValid(), qPrintable(s.error));
    QVERIFY(s.kind == FFmpegSource::Kind::Avformat);
    QCOMPARE(s.url, uri.toLatin1());
    QCOMPARE(s.inputFormat, QByteArray("rtsp"));
    QCOMPARE(s.host, QStringLiteral("192.168.42.1"));
    QCOMPARE(s.port, 8554);
    QCOMPARE(option(s, "protocol_whitelist"), QByteArray("tcp,udp,rtp"));
    QCOMPARE(option(s, "fflags"), QByteArray("nobuffer"));
    QCOMPARE(option(s, "probesize"), QByteArray("32768"));
    QCOMPARE(option(s, "analyzeduration"), QByteArray("0"));
    QCOMPARE(option(s, "max_delay"), QByteArray("0"));
    QCOMPARE(option(s, "timeout"), QByteArray("5000000"));
    QCOMPARE(option(s, "max_streams"), QByteArray("8"));

    // Credentials go to FFmpeg (the camera needs them), host names are allowed
    const FFmpegSource c = FFmpegSource::fromUri(QStringLiteral("rtsp://pilot:pw@camera.local:554/stream1"));
    QVERIFY2(c.isValid(), qPrintable(c.error));
    QCOMPARE(c.host, QStringLiteral("camera.local"));
    QCOMPARE(c.url, QByteArray("rtsp://pilot:pw@camera.local:554/stream1"));
}

void FFmpegSourceTest::_rtspDefaultPortAndTls()
{
    const FFmpegSource s = FFmpegSource::fromUri(QStringLiteral("rtsp://192.168.43.1/fpv_stream"));
    QVERIFY(s.isValid());
    QCOMPARE(s.port, 554);
    const FFmpegSource v6 = FFmpegSource::fromUri(QStringLiteral("rtsp://[fd00::1]:8554/x"));
    QVERIFY2(v6.isValid(), qPrintable(v6.error));
    QCOMPARE(v6.host, QStringLiteral("fd00::1"));
    const FFmpegSource tls = FFmpegSource::fromUri(QStringLiteral("rtsps://192.168.43.1:322/x"));
    QVERIFY(tls.isValid());
    QVERIFY(option(tls, "protocol_whitelist").contains("tls"));
    QVERIFY(!option(s, "protocol_whitelist").contains("tls"));
}

void FFmpegSourceTest::_mpegts()
{
    const FFmpegSource s = FFmpegSource::fromUri(QStringLiteral("mpegts://0.0.0.0:5600"));
    QVERIFY2(s.isValid(), qPrintable(s.error));
    QVERIFY(s.kind == FFmpegSource::Kind::Avformat);
    QCOMPARE(s.url, QByteArray("udp://0.0.0.0:5600"));
    QCOMPARE(s.inputFormat, QByteArray("mpegts"));
    QCOMPARE(option(s, "protocol_whitelist"), QByteArray("udp"));
    QCOMPARE(option(s, "buffer_size"), QByteArray("4194304"));
    QCOMPARE(option(s, "overrun_nonfatal"), QByteArray("1"));
    QCOMPARE(FFmpegSource::fromUri(QStringLiteral("mpegts://5600")).url, QByteArray("udp://0.0.0.0:5600"));
    QCOMPARE(FFmpegSource::fromUri(QStringLiteral("mpegts://[::]:5600")).url, QByteArray("udp://[::]:5600"));
    QVERIFY(!FFmpegSource::fromUri(QStringLiteral("mpegts://camera.local:5600")).isValid());
}

void FFmpegSourceTest::_tcp()
{
    const FFmpegSource s = FFmpegSource::fromUri(QStringLiteral("tcp://192.168.1.2:5600"));
    QVERIFY2(s.isValid(), qPrintable(s.error));
    QCOMPARE(s.url, QByteArray("tcp://192.168.1.2:5600"));
    QCOMPARE(s.inputFormat, QByteArray("mpegts"));
    QCOMPARE(option(s, "protocol_whitelist"), QByteArray("tcp"));
    QVERIFY(FFmpegSource::fromUri(QStringLiteral("tcp://camera-1.local:5600")).isValid());
    // A TCP source connects to the camera: it needs its address
    QVERIFY(!FFmpegSource::fromUri(QStringLiteral("tcp://5600")).isValid());
    QVERIFY(!FFmpegSource::fromUri(QStringLiteral("tcp://:5600")).isValid());
}

void FFmpegSourceTest::_lowLatencyReorderQueue()
{
    QCOMPARE(option(FFmpegSource::fromUri(QStringLiteral("rtsp://192.0.2.1/x"), true), "reorder_queue_size"), QByteArray("0"));
    QCOMPARE(option(FFmpegSource::fromUri(QStringLiteral("rtsp://192.0.2.1/x"), false), "reorder_queue_size"), QByteArray("4"));
}

void FFmpegSourceTest::_noProcessWideWhitelist()
{
    // Design: the whitelist is an option on this one input, never QT_FFMPEG_PROTOCOL_WHITELIST
    const QByteArray before = qgetenv("QT_FFMPEG_PROTOCOL_WHITELIST");
    for (const QString &uri : { QStringLiteral("rtsp://192.0.2.1/x"), QStringLiteral("rtsps://192.0.2.1/x"),
                                QStringLiteral("mpegts://0.0.0.0:5600"), QStringLiteral("tcp://192.0.2.1:5600") }) {
        const FFmpegSource s = FFmpegSource::fromUri(uri);
        QVERIFY2(s.isValid(), qPrintable(uri));
        QCOMPARE(qgetenv("QT_FFMPEG_PROTOCOL_WHITELIST"), before);
        // Never local files, web requests or data URLs from network input
        const QByteArray wl = option(s, "protocol_whitelist");
        QVERIFY(!wl.isEmpty());
        for (const char *forbidden : { "file", "http", "data", "concat", "pipe", "subfile" }) {
            QVERIFY2(!wl.contains(forbidden), forbidden);
        }
    }
}

void FFmpegSourceTest::_refusedSchemes()
{
    for (const QString &uri : { QStringLiteral("http://example.com/x"), QStringLiteral("file:///etc/passwd"),
                                QStringLiteral("rtmp://192.0.2.1/live"), QStringLiteral("concat://a|b"),
                                QStringLiteral("data://text/plain") }) {
        const FFmpegSource s = FFmpegSource::fromUri(uri);
        QVERIFY2(!s.isValid(), qPrintable(uri));
        QVERIFY(s.error.contains(QStringLiteral("unknown video scheme")));
        QVERIFY(s.url.isEmpty());
    }
}

void FFmpegSourceTest::_badUris()
{
    const QStringList bad = {
        QString(), QStringLiteral("   "), QStringLiteral("0.0.0.0:5600"), QStringLiteral("udp://"),
        QStringLiteral("udp://0.0.0.0:"), QStringLiteral("udp://0.0.0.0:abc"), QStringLiteral("udp://0.0.0.0:70000"),
        QStringLiteral("udp://0.0.0.0:0"), QStringLiteral("udp://0.0.0.0:+5600"), QStringLiteral("udp://0.0.0.0:-1"),
        QStringLiteral("udp://camera.local:5600"), QStringLiteral("udp://0.0.0.0:5600/path"),
        // FFmpeg reads options from a URL's query: never let a URI add any
        QStringLiteral("mpegts://0.0.0.0:5600?localaddr=10.0.0.1"), QStringLiteral("tcp://192.0.2.1:5600?listen=1"),
        QStringLiteral("udp://0.0.0.0:5600?reuse=1"),
        QStringLiteral("tcp://192.0.2.1:5600/x"), QStringLiteral("rtsp://"), QStringLiteral("rtsp:///live"),
        QStringLiteral("rtsp://192.0.2.1:99999/x"), QStringLiteral("rtsp://bad_host!:554/x"),
        QStringLiteral("rtsp://192.0.2.1/live stream"), QStringLiteral("rtsp://192.0.2.1/\x01live"),
        QStringLiteral("rtsp://192.0.2.1/caf") + QChar(0xe9),
        QStringLiteral("rtsp://") + QString(3000, QLatin1Char('a')),
    };
    for (const QString &uri : bad) {
        const FFmpegSource s = FFmpegSource::fromUri(uri);
        QVERIFY2(!s.isValid(), qPrintable(uri.left(80)));
        QVERIFY(!s.error.isEmpty());
        QVERIFY(s.url.isEmpty());
        QVERIFY(s.formatOptions.isEmpty());
    }
}

void FFmpegSourceTest::_credentialsNeverInErrors()
{
    const QString secret = QStringLiteral("hunter2");
    for (const QString &uri : { QStringLiteral("udp://pilot:hunter2@0.0.0.0:5600"), QStringLiteral("rtsp://pilot:hunter2@bad_host!:554/live"),
                                QStringLiteral("udp265://pilot:hunter2@:x"), QStringLiteral("tcp://pilot:hunter2@192.0.2.1:5600"),
                                QStringLiteral("mpegts://pilot:hunter2@0.0.0.0:5600"), QStringLiteral("rtsp://pilot:hunter2@192.0.2.1:0/x") }) {
        const FFmpegSource s = FFmpegSource::fromUri(uri);
        QVERIFY2(!s.isValid(), qPrintable(uri));
        QVERIFY2(!s.error.contains(secret), qPrintable(s.error));
        QVERIFY(!FFmpegSource::logText(uri).contains(secret));
    }
}

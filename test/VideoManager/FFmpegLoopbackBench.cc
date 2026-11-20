#include "FFmpegLoopbackBench.h"
#include "FFmpegReceiver.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtMultimedia/QVideoFrame>
#include <QtMultimedia/QVideoSink>
#include <QtGui/QGuiApplication>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickView>
#include <QtQuick/QSGRendererInterface>
#include <QtTest/QTest>

#include <memory>

#ifdef Q_OS_LINUX
#include <sys/resource.h>
#endif
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {

double cpuSeconds()
{
#ifdef Q_OS_LINUX
    rusage u{};
    (void) getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
#elif defined(Q_OS_WIN)
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return -1.0;
    }
    auto ticks = [](const FILETIME &f) { return (static_cast<quint64>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return (ticks(kernel) + ticks(user)) / 1.0e7;   // 100 ns units
#else
    return -1.0;
#endif
}

qint64 rssKb()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS pmc{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<qint64>(pmc.WorkingSetSize / 1024);   // working set
    }
#endif
#ifdef Q_OS_LINUX
    QFile f(QStringLiteral("/proc/self/status"));
    if (f.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : f.readAll().split('\n')) {
            if (line.startsWith("VmRSS:")) {
                return line.mid(6).trimmed().split(' ').first().toLongLong();
            }
        }
    }
#endif
    return -1;
}

// The Fly view's video item reduced to what the receiver needs: a VideoOutput named
// "videoContent" (G83 checks the type) filling a 1280x720 window.
const char *kWindowQml = R"(
import QtQuick
import QtMultimedia
Rectangle {
    width: 1280; height: 720; color: "black"
    VideoOutput { objectName: "videoContent"; anchors.fill: parent; fillMode: VideoOutput.PreserveAspectFit }
}
)";

} // namespace

void FFmpegLoopbackBench::_receive()
{
    const int port = qEnvironmentVariableIntValue("QGC_G88_PORT") > 0 ? qEnvironmentVariableIntValue("QGC_G88_PORT") : 5600;
    const int seconds = qEnvironmentVariableIntValue("QGC_G88_SECONDS") > 0 ? qEnvironmentVariableIntValue("QGC_G88_SECONDS") : 60;
    const bool windowed = qEnvironmentVariableIntValue("QGC_G88_WINDOW") > 0;

    // Windowed: a real QQuickWindow with a VideoOutput, so frameSwapped gives socket to swap.
    std::unique_ptr<QQuickView> view;
    QTemporaryDir qmlDir;
    QQuickItem *videoItem = nullptr;
    QVideoSink plainSink;
    QVideoSink *sink = &plainSink;
    quint64 windowSwaps = 0;
    if (windowed) {
        QVERIFY(qmlDir.isValid());
        const QString qmlPath = qmlDir.filePath(QStringLiteral("G88BenchWindow.qml"));
        QFile qml(qmlPath);
        QVERIFY(qml.open(QIODevice::WriteOnly));
        (void) qml.write(kWindowQml);
        qml.close();
        view = std::make_unique<QQuickView>();
        view->setResizeMode(QQuickView::SizeRootObjectToView);
        view->setSource(QUrl::fromLocalFile(qmlPath));
        QVERIFY2(view->status() == QQuickView::Ready, qPrintable(view->errors().isEmpty() ? QString() : view->errors().first().toString()));
        videoItem = view->rootObject()->findChild<QQuickItem*>(QStringLiteral("videoContent"));
        QVERIFY(videoItem);
        (void) connect(view.get(), &QQuickWindow::frameSwapped, this, [&windowSwaps]() { ++windowSwaps; }, Qt::DirectConnection);
        view->resize(1280, 720);
        view->show();
        QVERIFY(QTest::qWaitForWindowExposed(view.get()));
        sink = static_cast<QVideoSink*>(FFmpegReceiver::createVideoSink(videoItem));
        QVERIFY2(sink, "the VideoOutput gave no video sink");
        qInfo() << "G88BENCH window: graphics api" << view->rendererInterface()->graphicsApi() << "platform" << QGuiApplication::platformName();
    }

    quint64 sinkFrames = 0;
    QElapsedTimer sinceStart;
    qint64 firstFrameMs = -1;
    (void) connect(sink, &QVideoSink::videoFrameChanged, this, [&](const QVideoFrame &frame) {
        if (frame.isValid()) {
            ++sinkFrames;
            if (firstFrameMs < 0) {
                firstFrameMs = sinceStart.elapsed();
            }
        }
    });

    FFmpegReceiver receiver;
    receiver.setName(QStringLiteral("videoContent"));
    receiver.setUri(QStringLiteral("udp://127.0.0.1:%1").arg(port));
    receiver.setLowLatency(true);
    if (videoItem) {
        receiver.setWidget(videoItem);
    }
    (void) connect(&receiver, &VideoReceiver::onStartComplete, &receiver, [&](VideoReceiver::STATUS status) {
        if (status == VideoReceiver::STATUS_OK) {
            receiver.startDecoding(sink);
        }
    });

    qInfo() << "G88BENCH libraries:" << FFmpegReceiver::libraryVersions();
    const double cpu0 = cpuSeconds();
    const qint64 rss0 = rssKb();
    qint64 rssAt60 = -1;
    sinceStart.start();
    receiver.start(3);
    qInfo().noquote() << "G88BENCH input own RTP depacketizer," << (windowed ? "windowed" : "no window");

    while (sinceStart.elapsed() < seconds * 1000LL) {
        // A real event loop, as GC's GUI thread: QTest::qWait sleeps between event passes
        // (10 ms, more with Windows timer resolution), which delayed the queued frame delivery.
        QEventLoop loop;
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        (void) loop.exec();
        if ((rssAt60 < 0) && (sinceStart.elapsed() >= 60000)) {
            rssAt60 = rssKb();
        }
    }
    const double wall = sinceStart.elapsed() / 1000.0;
    const double cpu1 = cpuSeconds();
    const qint64 rss1 = rssKb();
    receiver.stop();

    const FFmpegReceiver::Counters c = receiver.counters();
    qInfo().noquote() << QStringLiteral("G88BENCH packets=%1 decoded=%2 shown=%3 superseded=%4 convert_failed=%5 decode_errors=%6 corrupt=%7 rtp_missed=%8 reopens=%9")
                             .arg(c.packets).arg(c.decoded).arg(c.shown).arg(c.superseded).arg(c.convertFailed).arg(c.decodeErrors)
                             .arg(c.corruptPackets).arg(c.rtpMissed).arg(c.reopens);
    qInfo().noquote() << QStringLiteral("G88BENCH corrupt_frames=%1 dropped_access_units=%2 swaps=%3 window_swaps=%4 replaced_on_sink=%5 malformed=%6 refused=%7")
                             .arg(c.corruptFrames).arg(c.droppedAccessUnits).arg(c.swaps).arg(windowSwaps).arg(c.replacedOnSink)
                             .arg(c.malformedPackets).arg(c.refusedPackets);
    // Reorder window: packets put back in order (no loss), late and duplicate packets, and the
    // added latency (wait of the packets held behind a gap; in-order packets never wait)
    qInfo().noquote() << QStringLiteral("G88BENCH reorder recovered=%1 late=%2 duplicates=%3 held=%4 hold_avg_ms=%5 hold_max_ms=%6 rtp_missed=%7")
                             .arg(c.reorderRecovered).arg(c.rtpLate).arg(c.rtpDuplicates).arg(c.reorderHeld)
                             .arg((c.reorderHeld > 0) ? c.reorderHoldNsTotal / 1.0e6 / static_cast<double>(c.reorderHeld) : 0.0, 0, 'f', 3)
                             .arg(c.reorderHoldNsMax / 1.0e6, 0, 'f', 3).arg(c.rtpMissed);
    qInfo().noquote() << QStringLiteral("G88BENCH sink_frames=%1 first_frame_ms=%2 wall_s=%3 cpu_s=%4 cores=%5 rss_kb_start=%6 rss_kb_60s=%7 rss_kb_end=%8 csv=%9")
                             .arg(sinkFrames).arg(firstFrameMs).arg(wall, 0, 'f', 1).arg(cpu1 - cpu0, 0, 'f', 2)
                             .arg((cpu0 >= 0) ? (cpu1 - cpu0) / wall : -1.0, 0, 'f', 3).arg(rss0).arg(rssAt60).arg(rss1)
                             .arg(receiver.statsFilePrefix());

    QVERIFY2(c.shown > 0, "no frame reached the video sink: is the sender running?");
}

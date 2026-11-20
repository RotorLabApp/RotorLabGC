/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "AudioOutputTest.h"
#include "AudioOutput.h"
#include "Fact.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QSemaphore>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

// G104: a speech engine whose setup hangs (a leftover speech-dispatcher that
// stops accepting connections) must not block init(), the event loop, say(),
// or shutdown. The hook below stands in for the hung plugin connect. Engine
// "none" is used so no real speech engine is loaded.

void AudioOutputTest::_testInitDoesNotBlockOnStuckEngine()
{
    if (!AudioOutput::kEngineOnOwnThread) {
        QSKIP("Speech engine runs on its own thread on desktop Linux only (speech-dispatcher)");
    }
    static QSemaphore gate; // static: outlives the speech thread
    Fact mutedFact(0, QStringLiteral("audioMuted"), FactMetaData::valueTypeBool);
    mutedFact.setRawValue(false);

    AudioOutput *const audio = new AudioOutput();
    audio->_engineName = QStringLiteral("none");
    audio->_beforeEngineCreate = []() { (void) gate.tryAcquire(1, 30000); };
    QSignalSpy readySpy(audio, &AudioOutput::engineReady);

    QElapsedTimer timer;
    timer.start();
    audio->init(&mutedFact);
    QVERIFY2(timer.elapsed() < 500, qPrintable(QStringLiteral("init took %1 ms").arg(timer.elapsed())));
    QVERIFY(!audio->isEngineReady());

    // The main thread keeps running its event loop while the engine is stuck.
    timer.restart();
    QTest::qWait(300);
    QVERIFY(timer.elapsed() < 2000);
    QVERIFY(!audio->isEngineReady());
    QCOMPARE(readySpy.count(), 0);

    // Speech before the engine is ready is dropped at once, not queued behind the hang.
    timer.restart();
    audio->say(QStringLiteral("Takeoff"));
    audio->setMuted(true);
    audio->setMuted(false);
    QVERIFY(timer.elapsed() < 100);

    // The engine comes up once the stuck connect returns.
    gate.release();
    QVERIFY(readySpy.wait(10000));
    QCOMPARE(readySpy.count(), 1);
    QCOMPARE(readySpy.at(0).at(0).toBool(), false); // engine "none" cannot speak
    QVERIFY(audio->isEngineReady());

    timer.restart();
    delete audio;
    QVERIFY(timer.elapsed() < 2000);
}

void AudioOutputTest::_testShutdownDoesNotWaitOnStuckEngine()
{
    if (!AudioOutput::kEngineOnOwnThread) {
        QSKIP("Speech engine runs on its own thread on desktop Linux only (speech-dispatcher)");
    }
    static QSemaphore gate;   // static: outlives the speech thread left behind
    static QSemaphore entered;
    Fact mutedFact(0, QStringLiteral("audioMuted"), FactMetaData::valueTypeBool);

    AudioOutput *const audio = new AudioOutput();
    audio->_engineName = QStringLiteral("none");
    audio->_beforeEngineCreate = []() {
        entered.release();
        (void) gate.tryAcquire(1, 30000);
    };
    audio->init(&mutedFact);
    QVERIFY(entered.tryAcquire(1, 5000));

    // Shutdown while the engine is still stuck: bounded by the 2 s stop wait.
    QElapsedTimer timer;
    timer.start();
    delete audio;
    const qint64 elapsed = timer.elapsed();
    QVERIFY2(elapsed < 3500, qPrintable(QStringLiteral("shutdown took %1 ms").arg(elapsed)));

    // Let the left-behind thread return; it sees the interruption and touches nothing.
    gate.release();
    QTest::qWait(200);
}

void AudioOutputTest::_testSpokenReplacements()
{
    QString result = AudioOutput::_fixTextMessageForAudio(QStringLiteral("-10.5m, -10.5m. -10.5 m"));
    QCOMPARE(result, QStringLiteral("negative 10 point 5 meters, negative 10 point 5 meters. negative 10 point 5  meters"));
    result = AudioOutput::_fixTextMessageForAudio(QStringLiteral("-10m -10 m"));
    QCOMPARE(result, QStringLiteral("negative 10 meters negative 10  meters"));
    result = AudioOutput::_fixTextMessageForAudio(QStringLiteral("foo -10m -10 m bar"));
    QCOMPARE(result, QStringLiteral("foo negative 10 meters negative 10  meters bar"));
    result = AudioOutput::_fixTextMessageForAudio(QStringLiteral("-foom"));
    QCOMPARE(result, QStringLiteral("-foom"));
    result = AudioOutput::_fixTextMessageForAudio(QStringLiteral("10 moo"));
    QCOMPARE(result, QStringLiteral("10 moo"));
    result = AudioOutput::_fixTextMessageForAudio(QStringLiteral("10moo"));
    QCOMPARE(result, QStringLiteral("10moo"));
    result = AudioOutput::_fixTextMessageForAudio(QStringLiteral("10ms"));
    QCOMPARE(result, QStringLiteral("10ms"));
    result = AudioOutput::_fixTextMessageForAudio(QStringLiteral("1000ms"));
    QCOMPARE(result, QStringLiteral("1 second"));
    result = AudioOutput::_fixTextMessageForAudio(QStringLiteral("1001ms"));
    QCOMPARE(result, QStringLiteral("1 second and 1 millisecond"));
}

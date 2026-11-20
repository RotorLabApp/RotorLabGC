/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "AudioOutput.h"
#include "Fact.h"
#include "QGCLoggingCategory.h"
#include "QGCApplication.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QMutexLocker>
#include <QtCore/QThread>
#include <QtCore/qapplicationstatic.h>
#include <QtTextToSpeech/QTextToSpeech>

QGC_LOGGING_CATEGORY(AudioOutputLog, "qgc.audio.audiooutput");
// qt.speech.tts.flite
// qt.speech.tts.android

const QHash<QString, QString> AudioOutput::_textHash = {
    { "ERR",            "error" },
    { "POSCTL",         "Position Control" },
    { "ALTCTL",         "Altitude Control" },
    { "AUTO_RTL",       "auto return to launch" },
    { "RTL",            "return To launch" },
    { "ACCEL",          "accelerometer" },
    { "RC_MAP_MODE_SW", "RC mode switch" },
    { "REJ",            "rejected" },
    { "WP",             "waypoint" },
    { "CMD",            "command" },
    { "COMPID",         "component eye dee" },
    { "PARAMS",         "parameters" },
    { "ID",             "I.D." },
    { "ADSB",           "A.D.S.B." },
    { "EKF",            "E.K.F." },
    { "PREARM",         "pre arm" },
    { "PITOT",          "pee toe" },
    { "SERVOX_FUNCTION","Servo X Function" },
};

Q_APPLICATION_STATIC(AudioOutput, _audioOutput);

AudioOutput::AudioOutput(QObject *parent)
    : QObject(parent)
{
    // qCDebug(AudioOutputLog) << this;
}

AudioOutput::~AudioOutput()
{
    _shutdownEngineThread();
    // qCDebug(AudioOutputLog) << this;
}

AudioOutput *AudioOutput::instance()
{
    return _audioOutput();
}

void AudioOutput::init(Fact *mutedFact)
{
    Q_CHECK_PTR(mutedFact);

    if (_initialized) {
        return;
    }

    (void) connect(mutedFact, &Fact::valueChanged, this, [this](QVariant value) {
        setMuted(value.toBool());
    });
    _muted = mutedFact->rawValue().toBool();
    _initialized = true;

    // G104: selecting the engine loads its plugin, and on Linux the
    // speech-dispatcher plugin connects to the speechd socket. A leftover
    // speech-dispatcher that stops accepting connections held that connect,
    // and with it the first main window, for about five minutes. The engine
    // is therefore created on its own thread on desktop Linux; init() returns
    // at once and speech before the engine is ready is dropped. Other
    // platforms keep the engine on the main thread as before (Windows and
    // Android speech plugins are not exposed to speechd).
    if (kEngineOnOwnThread) {
        _engineThread = new QThread();
        _engineThread->setObjectName(QStringLiteral("QGC_TextToSpeech"));
        _engineContext = new QObject();
        _engineContext->moveToThread(_engineThread);
        (void) connect(_engineThread, &QThread::finished, _engineContext, &QObject::deleteLater);
        _engineThread->start(QThread::LowPriority);

        (void) QMetaObject::invokeMethod(_engineContext, [this, guard = _guard]() { _createEngine(guard); }, Qt::QueuedConnection);
    } else {
        _engineContext = this;
        _createEngine(_guard);
    }

    qCDebug(AudioOutputLog) << "AudioOutput initialized with muted state:" << _muted << "engine on own thread:" << kEngineOnOwnThread;
}

void AudioOutput::_createEngine(const std::shared_ptr<Guard> &guard)
{
    // Copy everything the blocking calls need, under the guard, before making
    // them. After a blocking call this AudioOutput is touched only under the
    // guard and only while it is alive, so a thread left behind at shutdown
    // (see _shutdownEngineThread) never reads or writes a destroyed AudioOutput.
    QObject *context = nullptr;
    QString engineName;
    std::function<void()> beforeEngineCreate;
    bool mutedAtStart = false;
    {
        const QMutexLocker lock(&guard->mutex);
        if (!guard->alive) {
            return;
        }
        Q_ASSERT(!_engineThread || (QThread::currentThread() == _engineThread));
        context = _engineContext;
        engineName = _engineName;
        beforeEngineCreate = _beforeEngineCreate;
        mutedAtStart = _muted;
    }

    if (beforeEngineCreate) {
        beforeEngineCreate();
    }

    if (engineName.isEmpty() && QTextToSpeech::availableEngines().isEmpty()) {
        qCWarning(AudioOutputLog) << "No available QTextToSpeech engines found.";
        const QMutexLocker lock(&guard->mutex);
        if (!guard->alive) {
            return;
        }
        _canSpeak = false;
        _engineReady = true;
        emit engineReady(false);
        return;
    }

    // Autoselect engine by priority (empty name). This, and the locale calls
    // below, are the calls that can block.
    QTextToSpeech *const engine = new QTextToSpeech(engineName, context);
    // A plugin that failed to load still reports its declared capabilities,
    // so a failed engine counts as unable to speak.
    const bool engineOk = !engine->engine().isEmpty() && (engine->state() != QTextToSpeech::Error);
    if (!engineOk) {
        qCWarning(AudioOutputLog) << "Failed to set the TTS engine:" << engine->errorString();
    } else {
        qCDebug(AudioOutputLog) << "TTS Engine set to:" << engine->engine();
        const QLocale defaultLocale = QLocale("en_US");
        if (engine->availableLocales().contains(defaultLocale)) {
            engine->setLocale(defaultLocale);
        }
    }

    if (AudioOutputLog().isDebugEnabled()) {
        (void) connect(engine, &QTextToSpeech::stateChanged, engine, [](QTextToSpeech::State state) {
            qCDebug(AudioOutputLog) << "TTS State changed to:" << state;
        });
        (void) connect(engine, &QTextToSpeech::errorOccurred, engine, [](QTextToSpeech::ErrorReason reason, const QString &errorString) {
            qCDebug(AudioOutputLog) << "TTS Error occurred. Reason:" << reason << ", Message:" << errorString;
        });
        (void) connect(engine, &QTextToSpeech::localeChanged, engine, [](const QLocale &locale) {
            qCDebug(AudioOutputLog) << "TTS Locale change to:" << locale;
        });
        (void) connect(engine, &QTextToSpeech::volumeChanged, engine, [](double volume) {
            qCDebug(AudioOutputLog) << "TTS Volume changed to:" << volume;
        });
        (void) connect(engine, &QTextToSpeech::sayingWord, engine, [](const QString &word, qsizetype id, qsizetype start, qsizetype length) {
            qCDebug(AudioOutputLog) << "TTS Saying:" << word << "ID:" << id << "Start:" << start << "Length:" << length;
        });
    }

    engine->setVolume(mutedAtStart ? 0.0 : 1.0);
    const bool canSpeak = engineOk && engine->engineCapabilities().testFlag(QTextToSpeech::Capability::Speak);

    const QMutexLocker lock(&guard->mutex);
    if (!guard->alive) {
        return;
    }

    // The engine outlives this AudioOutput only on the leave-behind path, so
    // its queue counter is reached through the guard as well.
    (void) connect(engine, &QTextToSpeech::aboutToSynthesize, engine, [this, guard](qsizetype id) {
        const QMutexLocker synthLock(&guard->mutex);
        if (!guard->alive) {
            return;
        }
        qCDebug(AudioOutputLog) << "TTS About To Synthesize ID:" << id;
        _textQueueSize--;
        qCDebug(AudioOutputLog) << "Queue Size:" << _textQueueSize;
    }, Qt::DirectConnection);

    if (_muted != mutedAtStart) {
        engine->setVolume(_muted ? 0.0 : 1.0);
    }
    _engine = engine;
    _canSpeak = canSpeak;
    _engineReady = true;

    emit engineReady(canSpeak);
}

void AudioOutput::_shutdownEngineThread()
{
    {
        // After this, the speech thread never touches this AudioOutput again.
        const QMutexLocker lock(&_guard->mutex);
        _guard->alive = false;
    }

    if (!_engineThread) {
        return;
    }

    _engineReady = false;
    _engineThread->requestInterruption();
    _engineThread->quit();
    if (_engineThread->wait(kEngineThreadStopWaitMs)) {
        delete _engineThread;
    } else {
        // The engine is still stuck in its plugin (for example a speechd
        // connect). Do not hold up shutdown for it: leave the thread to the
        // operating system at process exit rather than destroy a running QThread.
        qCWarning(AudioOutputLog) << "Speech engine thread did not stop in" << kEngineThreadStopWaitMs << "ms; leaving it to process exit.";
    }
    _engineThread = nullptr;
    _engineContext = nullptr;
    _engine = nullptr;
}

void AudioOutput::setMuted(bool muted)
{
    if (_muted.exchange(muted) != muted) {
        if (_engineReady) {
            QTextToSpeech *const engine = _engine;
            (void) QMetaObject::invokeMethod(engine, [engine, muted]() { engine->setVolume(muted ? 0.0 : 1.0); }, Qt::QueuedConnection);
        }
        qCDebug(AudioOutputLog) << "AudioOutput muted state set to:" << muted;
    }
}

void AudioOutput::say(const QString &text, TextMods textMods)
{
    if (!_initialized) {
        if (!qgcApp()->runningUnitTests()) {
            qCWarning(AudioOutputLog) << "AudioOutput not initialized. Call init() before using say().";
        }
        return;
    }

    if (_muted) {
        return;
    }

    if (!_engineReady) {
        qCDebug(AudioOutputLog) << "Speech engine not ready yet, dropped:" << text;
        return;
    }

    if (!_canSpeak) {
        qCWarning(AudioOutputLog) << "Speech Not Supported:" << text;
        return;
    }

    QTextToSpeech *const engine = _engine;

    if (_textQueueSize >= kMaxTextQueueSize) {
        (void) QMetaObject::invokeMethod(engine, [engine]() { engine->stop(QTextToSpeech::BoundaryHint::Default); }, Qt::QueuedConnection);
        _textQueueSize = 0;
        qCWarning(AudioOutputLog) << "Text queue exceeded maximum size. Stopped current speech.";
    }

    QString outText = _fixTextMessageForAudio(text);

    if (textMods.testFlag(TextMod::Translate)) {
        outText = tr("%1").arg(outText);
    }

    // Queued so a busy speech thread never blocks the caller.
    _textQueueSize++;
    if (!QMetaObject::invokeMethod(engine, [engine, outText]() { (void) engine->enqueue(outText); }, Qt::QueuedConnection)) {
        _textQueueSize--;
        qCWarning(AudioOutputLog) << "Failed to invoke Enqueue method.";
    }
}

QString AudioOutput::_fixTextMessageForAudio(const QString &string)
{
    QString result = string;
    result = _replaceAbbreviations(result);
    result = _replaceNegativeSigns(result);
    result = _replaceDecimalPoints(result);
    result = _replaceMeters(result);
    result = _convertMilliseconds(result);
    return result;
}

QString AudioOutput::_replaceAbbreviations(const QString &input)
{
    QString output = input;

    const QStringList wordList = input.split(' ', Qt::SkipEmptyParts);
    for (const QString &word : wordList) {
        const QString upperWord = word.toUpper();
        if (_textHash.contains(upperWord)) {
            (void) output.replace(word, _textHash.value(upperWord));
        }
    }

    return output;
}

QString AudioOutput::_replaceNegativeSigns(const QString &input)
{
    static const QRegularExpression negNumRegex(QStringLiteral("-\\s*(?=\\d)"));
    Q_ASSERT(negNumRegex.isValid());

    QString output = input;
    (void) output.replace(negNumRegex, "negative ");
    return output;
}

QString AudioOutput::_replaceDecimalPoints(const QString &input)
{
    static const QRegularExpression realNumRegex(QStringLiteral("([0-9]+)(\\.)([0-9]+)"));
    Q_ASSERT(realNumRegex.isValid());

    QString output = input;
    QRegularExpressionMatch realNumRegexMatch = realNumRegex.match(output);
    while (realNumRegexMatch.hasMatch()) {
        if (!realNumRegexMatch.captured(2).isNull()) {
            (void) output.replace(realNumRegexMatch.capturedStart(2), realNumRegexMatch.capturedEnd(2) - realNumRegexMatch.capturedStart(2), QStringLiteral(" point "));
        }
        realNumRegexMatch = realNumRegex.match(output);
    }

    return output;
}

QString AudioOutput::_replaceMeters(const QString &input)
{
    static const QRegularExpression realNumMeterRegex(QStringLiteral("[0-9]*\\.?[0-9]\\s?(m)([^A-Za-z]|$)"));
    Q_ASSERT(realNumMeterRegex.isValid());

    QString output = input;
    QRegularExpressionMatch realNumMeterRegexMatch = realNumMeterRegex.match(output);
    while (realNumMeterRegexMatch.hasMatch()) {
        if (!realNumMeterRegexMatch.captured(1).isNull()) {
            (void) output.replace(realNumMeterRegexMatch.capturedStart(1), realNumMeterRegexMatch.capturedEnd(1) - realNumMeterRegexMatch.capturedStart(1), QStringLiteral(" meters"));
        }
        realNumMeterRegexMatch = realNumMeterRegex.match(output);
    }

    return output;
}

QString AudioOutput::_convertMilliseconds(const QString &input)
{
    QString result = input;

    QString match;
    int number;
    if (_getMillisecondString(input, match, number) && (number >= 1000)) {
        QString newNumber;
        if (number < 60000) {
            const int seconds = number / 1000;
            const int ms = number - (seconds * 1000);
            newNumber = QStringLiteral("%1 second%2").arg(seconds).arg(seconds > 1 ? "s" : "");
            if (ms > 0) {
                (void) newNumber.append(QStringLiteral(" and %1 millisecond").arg(ms));
            }
        } else {
            const int minutes = number / 60000;
            const int seconds = (number - (minutes * 60000)) / 1000;
            newNumber = QStringLiteral("%1 minute%2").arg(minutes).arg(minutes > 1 ? "s" : "");
            if (seconds > 0) {
                (void) newNumber.append(QStringLiteral(" and %1 second%2").arg(seconds).arg(seconds > 1 ? "s" : ""));
            }
        }
        (void) result.replace(match, newNumber);
    }

    return result;
}

bool AudioOutput::_getMillisecondString(const QString &string, QString &match, int &number)
{
    static const QRegularExpression msRegex("((?<number>[0-9]+)ms)");
    Q_ASSERT(msRegex.isValid());

    bool result = false;

    QRegularExpressionMatch regexpMatch = msRegex.match(string);
    if (regexpMatch.hasMatch()) {
        match = regexpMatch.captured(0);
        const QString numberStr = regexpMatch.captured("number");
        number = numberStr.toInt();
        result = true;
    }

    return result;
}

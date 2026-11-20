/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QLoggingCategory>
#include <QtCore/QMutex>
#include <QtCore/QObject>

#include <atomic>
#include <functional>
#include <memory>

class QTextToSpeech;
class QThread;
class Fact;
class AudioOutputTest;

Q_DECLARE_LOGGING_CATEGORY(AudioOutputLog)

/// The AudioOutput class provides functionality for audio output using text-to-speech.
class AudioOutput : public QObject
{
    Q_OBJECT

    friend AudioOutputTest;

public:
    /// Enumeration for text modification options.
    enum class TextMod {
        None = 0,
        Translate = 1 << 0,
    };
    Q_FLAG(TextMod)
    Q_DECLARE_FLAGS(TextMods, TextMod)

    /// Constructs an AudioOutput object.
    ///     @param parent The parent QObject.
    explicit AudioOutput(QObject *parent = nullptr);

    /// Destructor for the AudioOutput class.
    ~AudioOutput();

    /// Gets the singleton instance of AudioOutput.
    ///     @return The singleton instance.
    static AudioOutput *instance();

    /// Initialize the Singleton
    void init(Fact *mutedFact);

    /// Checks if the audio output is muted.
    ///     @return True if muted, false otherwise.
    bool isMuted() const { return _muted; }

    /// Sets the mute state of the audio output.
    ///     @param enable True to mute, false to unmute.
    void setMuted(bool muted);

    /// Reads the specified text with optional text modifications.
    ///     @param text The text to be read.
    ///     @param textMods The text modifications to apply.
    void say(const QString &text, TextMods textMods = TextMod::None);

    /// True once the speech engine has been created on its own thread.
    bool isEngineReady() const { return _engineReady; }

signals:
    /// Emitted once, from the speech thread, when engine setup finishes.
    ///     @param canSpeak True if the selected engine can speak.
    void engineReady(bool canSpeak);

private:
    /// Shared with the speech thread. alive is cleared at shutdown under
    /// mutex; after that the thread never touches this AudioOutput.
    struct Guard {
        QMutex mutex;
        bool alive = true;
    };
    const std::shared_ptr<Guard> _guard = std::make_shared<Guard>();

    void _createEngine(const std::shared_ptr<Guard> &guard);
    void _shutdownEngineThread();

    /// The speech engine and its plugin (for example speech-dispatcher on Linux)
    /// live on their own thread. Connecting to a stuck speech-dispatcher can
    /// block for minutes (G104); it must never block the main window.
    QThread *_engineThread = nullptr;
    QObject *_engineContext = nullptr;  ///< Lives on _engineThread; parent of _engine
    QTextToSpeech *_engine = nullptr;   ///< Set on the speech thread before _engineReady
    std::atomic_bool _engineReady = false;
    std::atomic_bool _canSpeak = false;
    QAtomicInteger<qsizetype> _textQueueSize = 0;
    bool _initialized = false;
    std::atomic_bool _muted = false;

    /// Engine name passed to QTextToSpeech; empty selects by priority.
    QString _engineName;
    /// Test hook: runs on the speech thread before the engine is created.
    std::function<void()> _beforeEngineCreate;

    static constexpr unsigned long kEngineThreadStopWaitMs = 2000;
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    static constexpr bool kEngineOnOwnThread = true;
#else
    static constexpr bool kEngineOnOwnThread = false;
#endif

    static const QHash<QString, QString> _textHash;

    static constexpr qsizetype kMaxTextQueueSize = 20;

    /// Fixes text messages for audio output.
    ///     @param string The text message to fix.
    ///     @return The fixed text message.
    static QString _fixTextMessageForAudio(const QString &string);

    /// Replaces predefined abbreviations with their corresponding full forms.
    ///     @param input The input string containing abbreviations.
    ///     @return A string with abbreviations replaced by their full forms.
    static QString _replaceAbbreviations(const QString &input);

    /// Replaces negative signs with the word "negative".
    static QString _replaceNegativeSigns(const QString &input);

    /// Replaces decimal points with the word "point".
    static QString _replaceDecimalPoints(const QString &input);

    /// Replaces "m" (meters) with the word "meters" following numbers.
    static QString _replaceMeters(const QString &input);

    /// Converts millisecond values to a more readable format (seconds and minutes).
    static QString _convertMilliseconds(const QString &input);

    /// Extracts a millisecond value from the given string.
    ///     @param string The string to extract from.
    ///     @param match The extracted millisecond string.
    ///     @param number The extracted number.
    ///     @return True if extraction is successful, false otherwise.
    static bool _getMillisecondString(const QString &string, QString &match, int &number);

};
Q_DECLARE_OPERATORS_FOR_FLAGS(AudioOutput::TextMods)

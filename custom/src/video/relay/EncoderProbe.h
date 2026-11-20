/****************************************************************************
 *
 * RotorLab GC - G109: which H.264 encoder opens on this computer, and what
 * the forward-quality drop-down may offer because of it.
 *
 * Qt 6.8.3's FFmpeg has no software H.264 encoder of its own (no libx264).
 * What it has: h264_mf (Windows Media Foundation: the hardware encoder when
 * there is one, else Microsoft's own encoder) and h264_mediacodec (Android).
 * Nothing is assumed from the name: a candidate passes only when
 * avcodec_open2 succeeds at 640x360 and ten test frames give output.
 *
 ****************************************************************************/

#pragma once

#include "RelayTypes.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QList>
#include <QtCore/QString>

struct AVCodecContext;

struct EncoderChoice
{
    QString name;               ///< FFmpeg encoder name
    QString options;            ///< "key=value,key=value" given to avcodec_open2
    bool hardware = false;      ///< asked for a hardware encoder
    bool ok = false;            ///< opened and produced output
    double msPerFrame = 0;      ///< probe encode time per 640x360 frame
    QString detail;             ///< why it failed, for the log

    QString label() const;      ///< for the log and the settings page
};

struct EncoderProbeReport
{
    QList<EncoderChoice> tried;
    EncoderChoice chosen;       ///< the first that passed; ok == false when none did
};

class EncoderProbe
{
public:
    /// The candidates for this platform, best first.
    static QList<EncoderChoice> candidates();

    /// Probe every candidate (stops at the first that passes). Blocking: a
    /// few hundred milliseconds at most; run it off the GUI thread.
    static EncoderProbeReport run(int width = 640, int height = 360, int frames = 10);

    static EncoderChoice probeOne(EncoderChoice candidate, int width, int height, int frames);

    /// Open `choice` for the given output. Null with `error` set when it does
    /// not open. The caller frees it with avcodec_free_context.
    static AVCodecContext *openEncoder(const EncoderChoice &choice, int width, int height,
                                       int fps, int bitRate, QString *error);

    /// True when this build can encode at all (FFmpeg receiver built).
    static bool buildHasEncoders();
};

/// What the drop-down offers for a camera codec and a probe result.
struct QualityOffer
{
    Q_DECLARE_TR_FUNCTIONS(QualityOffer)

public:
    bool relayAllowed = true;
    QList<RelayQuality> levels;     ///< in drop-down order
    QString reason;                 ///< why some levels (or all) are missing; empty when all are offered

    bool offers(RelayQuality q) const { return levels.contains(q); }
    /// The lowest level offered (the default setting).
    RelayQuality lowest() const { return levels.isEmpty() ? RelayQuality::Full : levels.first(); }

    /// `codecKnown` false (no stream yet) is treated as H.264.
    static QualityOffer compute(VideoTapCodec codec, bool codecKnown, bool encoderOk);

    static QString noEncoderText();
    static QString fullNeedsH264Text();
    static QString needsH264Text();
    static QString levelText(RelayQuality q);
};

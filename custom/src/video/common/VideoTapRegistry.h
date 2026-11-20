/****************************************************************************
 *
 * RotorLab GC - where G95 and G109 find the receiver's packet tap.
 *
 * CustomPlugin::createVideoReceiver() notes every receiver it creates here;
 * the recorder and the relay ask for the main stream's tap ("videoContent").
 * Null when that receiver has no tap (GStreamer, Qt Multimedia) or does not
 * exist yet. GUI thread only.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QString>

class VideoPacketTap;
class VideoReceiver;

class VideoTapRegistry
{
public:
    static void noteReceiver(VideoReceiver *receiver);
    /// The tap of the receiver with this name, or null.
    static VideoPacketTap *tap(const QString &receiverName = QStringLiteral("videoContent"));
};

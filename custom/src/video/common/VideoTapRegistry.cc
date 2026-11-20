/****************************************************************************
 *
 * RotorLab GC - where G95 and G109 find the receiver's packet tap.
 *
 ****************************************************************************/

#include "VideoTapRegistry.h"

#include <QtCore/QList>
#include <QtCore/QPointer>

#include "VideoReceiver.h"

namespace {

QList<QPointer<VideoReceiver>> &receivers()
{
    static QList<QPointer<VideoReceiver>> list;
    return list;
}

} // namespace

void VideoTapRegistry::noteReceiver(VideoReceiver *receiver)
{
    if (receiver) {
        receivers().append(QPointer<VideoReceiver>(receiver));
    }
}

VideoPacketTap *VideoTapRegistry::tap(const QString &receiverName)
{
    for (const QPointer<VideoReceiver> &r : std::as_const(receivers())) {
        if (r && r->name() == receiverName) {
            return r->packetTap();
        }
    }
    return nullptr;
}

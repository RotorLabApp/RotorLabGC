/****************************************************************************
 *
 * RotorLab GC - G109: relay worker.
 *
 ****************************************************************************/

#include "RelayPipeline.h"

#include <QtCore/QMutexLocker>

#include <algorithm>

RelayPipeline::RelayPipeline(QObject *parent)
    : QObject(parent)
    , _waker(std::make_shared<Waker>())
{
    _waker->target = this;
    _fragmenter.setOutput([this](const LiveFragment &f) { emit fragmentReady(f); });
}

RelayPipeline::~RelayPipeline()
{
    _detach();
    QMutexLocker locker(&_waker->mutex);
    _waker->target = nullptr;
}

void RelayPipeline::probe()
{
    emit probeDone(EncoderProbe::run());
}

void RelayPipeline::_detach()
{
    if (_consumer) {
        _consumer->setWakeup({});
        // A consumer the tap already let go of (the receiver and its tap were
        // destroyed) must not be handed back to that tap.
        if (_tap && _consumer->attached()) {
            _tap->detach(_consumer);
        }
    }
    _consumer.reset();
    _tap = nullptr;
}

void RelayPipeline::start(VideoPacketTap *tap, int quality, const EncoderChoice &encoder)
{
    _detach();
    _fragmenter.reset();
    setQuality(quality, encoder);
    if (!tap) {
        return;
    }
    _consumer = tap->attach(kTapCapacity);
    if (!_consumer) {
        qCWarning(VideoRelayLog) << "the video tap has no room for another consumer; live relay cannot start";
        return;
    }
    _tap = tap;
    const std::shared_ptr<Waker> waker = _waker;
    // Runs on the receiver's thread: post and return at once, at most one post pending.
    _consumer->setWakeup([waker]() {
        if (waker->scheduled.exchange(true)) {
            return;
        }
        QMutexLocker locker(&waker->mutex);
        if (waker->target) {
            QMetaObject::invokeMethod(waker->target, "_drain", Qt::QueuedConnection);
        }
    });
    _waker->scheduled = false;
    QMetaObject::invokeMethod(this, "_drain", Qt::QueuedConnection);
}

void RelayPipeline::setQuality(int quality, const EncoderChoice &encoder)
{
    const RelayQuality q = RelayQuality(qBound(0, quality, 2));
    if (_encoder) {
        _encoder->flush();
    }
    _encoder.reset();
    _quality = q;
    _choice = encoder;
    _overloadSent = false;
    _encoderFailSent = false;
    if (q != RelayQuality::Full) {
        _encoder = std::make_unique<RelayEncoder>(encoder, q, [this](const RelayUnit &u) { _onEncoded(u); });
        _encoder->setRestarted([this]() {
            // The content measure starts again with the new encoder's output.
            _contentBytes = 0;
            _contentDuration90k = 0;
            _contentLastPts = -1;
            emit encoderRestarted();
        });
    }
    // A new quality is a new picture: new init, start at a keyframe.
    _fragmenter.reset();
    _pass = Pass::All;
    _passWaitKey = false;
    _contentBytes = 0;
    _contentDuration90k = 0;
    _contentLastPts = -1;
}

void RelayPipeline::setPass(int pass)
{
    const Pass p = Pass(std::clamp(pass, int(Pass::All), int(Pass::Nothing)));
    if (p == _pass) {
        return;
    }
    const Pass was = _pass;
    _pass = p;
    if (p != Pass::All) {
        // Over the cap: what is being built holds the frames that were too
        // much. Drop it; the next fragment starts at a keyframe.
        _fragmenter.resync();
    }
    if (p == Pass::All && was != Pass::All) {
        // Delta frames refer to frames that were not sent: start again at a keyframe.
        _passWaitKey = true;
        if (_encoder) {
            _encoder->requestKeyframe();
        }
    } else {
        _passWaitKey = false;
    }
}

void RelayPipeline::_onEncoded(const RelayUnit &u)
{
    // The guard's content measure: every unit the encoder made.
    const qint64 bytes = u.data.size();
    qint64 step = 0;
    if (u.pts90k >= 0 && _contentLastPts >= 0 && u.pts90k > _contentLastPts) {
        step = std::min<qint64>(u.pts90k - _contentLastPts, VideoRelayLimits::kMaxSampleGap90k);
    }
    if (u.pts90k >= 0) {
        _contentLastPts = u.pts90k;
    }
    _contentBytes += bytes;
    _contentDuration90k += step;
    if (_contentDuration90k >= 45000) {
        emit contentMeasured(_contentBytes, _contentDuration90k);
        _contentBytes = 0;
        _contentDuration90k = 0;
    }

    switch (_pass) {
    case Pass::Nothing:
        return;
    case Pass::KeyframesOnly:
        if (!u.keyFrame) {
            return;
        }
        break;
    case Pass::All:
        if (_passWaitKey) {
            if (!u.keyFrame) {
                return;
            }
            _passWaitKey = false;
        }
        break;
    }
    _fragmenter.push(u);
}

void RelayPipeline::stop()
{
    _detach();
    if (_encoder) {
        _encoder->flush();
    }
    _encoder.reset();
    _fragmenter.reset();
}

void RelayPipeline::requestKeyframe()
{
    if (_encoder) {
        _encoder->requestKeyframe();
    }
}

void RelayPipeline::_drain()
{
    _waker->scheduled = false;
    if (!_consumer) {
        return;
    }
    VideoTapItem item;
    // Bounded per pass, so a stop or quality change queued behind us is not starved.
    for (int i = 0; i < 64 && _consumer && _consumer->tryPop(item); ++i) {
        if (const VideoTapPacket *p = std::get_if<VideoTapPacket>(&item)) {
            _handle(*p);
        } else if (const VideoTapEvent *e = std::get_if<VideoTapEvent>(&item)) {
            _handle(*e);
        }
    }
    if (_consumer && _consumer->stats().queued > 0 && !_waker->scheduled.exchange(true)) {
        QMetaObject::invokeMethod(this, "_drain", Qt::QueuedConnection);
    }
}

void RelayPipeline::_handle(const VideoTapPacket &p)
{
    if (int(p.codec) != _lastCodec) {
        _lastCodec = int(p.codec);
        emit codecSeen(_lastCodec);
    }
    const RelayUnit u = RelayUnit::fromTap(p);
    if (_encoder) {
        _encoder->push(u);
        if (_encoder->overloaded() && !_overloadSent) {
            _overloadSent = true;
            emit encoderOverloaded();
        }
        if (!_encoder->lastError().isEmpty() && _encoder->stats().framesEncoded == 0
            && _encoder->stats().framesDecoded > 30 && !_encoderFailSent) {
            _encoderFailSent = true;
            emit encoderFailed(_encoder->lastError());
        }
    } else {
        // Full: the units as received. The fragmenter refuses anything but H.264.
        // Full has no cap, so the guard never filters it; the same path keeps
        // the filter's behavior one thing.
        _onEncoded(u);
    }
}

void RelayPipeline::_handle(const VideoTapEvent &e)
{
    switch (e.type) {
    case VideoTapEvent::Type::StreamStarted:
    case VideoTapEvent::Type::SourceChanged:
    case VideoTapEvent::Type::SsrcChanged:
        if (e.codecKnown) {
            _lastCodec = int(e.codec);
        }
        emit streamStarted(int(e.codec), e.codecKnown);
        break;
    case VideoTapEvent::Type::StreamStopped:
        if (_encoder) {
            _encoder->flush();
        }
        _fragmenter.flush();
        emit streamStopped();
        break;
    }
}

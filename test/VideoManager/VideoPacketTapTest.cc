#include "VideoPacketTapTest.h"
#include "VideoPacketTap.h"
#include "RtpDepacketizer.h"

#include <QtCore/QElapsedTimer>
#include <QtTest/QTest>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <thread>

namespace {

VideoTapPacket packet(bool key, quint32 n)
{
    VideoTapPacket p;
    p.keyFrame = key;
    p.data = QByteArray("\x00\x00\x00\x01", 4) + QByteArray::number(n);
    p.rtpTimestamp = n;
    p.hasRtpTimestamp = true;
    return p;
}

VideoTapEvent tapEvent(VideoTapEvent::Type type)
{
    VideoTapEvent e;
    e.type = type;
    return e;
}

bool isPacket(const VideoTapItem &item)
{
    return std::holds_alternative<VideoTapPacket>(item);
}

const VideoTapPacket &asPacket(const VideoTapItem &item)
{
    return std::get<VideoTapPacket>(item);
}

QByteArray bytes(std::initializer_list<int> b)
{
    QByteArray out;
    for (int v : b) {
        out.append(char(v));
    }
    return out;
}

} // namespace

void VideoPacketTapTest::_noConsumerIsNoOp()
{
    VideoPacketTap tap;
    QVERIFY(!tap.hasConsumers());
    QElapsedTimer timer;
    timer.start();
    for (quint32 i = 0; i < 100000; ++i) {
        tap.publishPacket(packet((i % 30) == 0, i));
    }
    tap.publishEvent(tapEvent(VideoTapEvent::Type::StreamStarted));
    qInfo() << "100000 publishes without a consumer:" << timer.elapsed() << "ms";

    // Nothing was counted while nobody listened
    std::shared_ptr<VideoPacketConsumer> c = tap.attach();
    tap.publishPacket(packet(true, 7));
    VideoTapItem item;
    QVERIFY(c->tryPop(item));
    QCOMPARE(asPacket(item).index, quint64(1));
}

void VideoPacketTapTest::_orderAndEvents()
{
    VideoPacketTap tap;
    std::shared_ptr<VideoPacketConsumer> c = tap.attach(1000);
    QVERIFY(c);
    QCOMPARE(tap.consumerCount(), 1);

    tap.publishEvent(tapEvent(VideoTapEvent::Type::StreamStarted));
    for (quint32 i = 0; i < 100; ++i) {
        tap.publishPacket(packet(i == 0, i));
    }
    tap.publishEvent(tapEvent(VideoTapEvent::Type::StreamStopped));

    VideoTapItem item;
    QVERIFY(c->tryPop(item));
    QVERIFY(!isPacket(item));
    QVERIFY(std::get<VideoTapEvent>(item).type == VideoTapEvent::Type::StreamStarted);
    quint64 lastIndex = 0;
    for (quint32 i = 0; i < 100; ++i) {
        QVERIFY(c->tryPop(item));
        QVERIFY(isPacket(item));
        const VideoTapPacket &p = asPacket(item);
        QCOMPARE(p.rtpTimestamp, i);
        QCOMPARE(p.droppedBefore, quint64(0));
        QCOMPARE(p.keyFrame, i == 0);
        QCOMPARE(p.index, lastIndex + 1);
        lastIndex = p.index;
    }
    QVERIFY(c->tryPop(item));
    QVERIFY(std::get<VideoTapEvent>(item).type == VideoTapEvent::Type::StreamStopped);
    QVERIFY(!c->tryPop(item));
    QCOMPARE(c->stats().packets, quint64(100));
    QCOMPARE(c->stats().events, quint64(2));
    QCOMPARE(c->stats().dropped, quint64(0));
}

void VideoPacketTapTest::_startsAtKeyframe()
{
    VideoPacketTap tap;
    std::shared_ptr<VideoPacketConsumer> c = tap.attach(10);
    tap.publishPacket(packet(false, 1));
    tap.publishPacket(packet(false, 2));
    tap.publishPacket(packet(true, 3));
    tap.publishPacket(packet(false, 4));

    VideoTapItem item;
    QVERIFY(c->tryPop(item));
    QVERIFY(asPacket(item).keyFrame);
    QCOMPARE(asPacket(item).rtpTimestamp, quint32(3));
    QCOMPARE(asPacket(item).droppedBefore, quint64(0));
    QVERIFY(c->tryPop(item));
    QCOMPARE(asPacket(item).rtpTimestamp, quint32(4));
    QVERIFY(!c->tryPop(item));
    QCOMPARE(c->stats().dropped, quint64(0));   // joining mid-GOP is not a loss
    QCOMPARE(c->stats().resyncs, quint64(0));
}

void VideoPacketTapTest::_keyframeResyncAfterDrop()
{
    VideoPacketTap tap;
    std::shared_ptr<VideoPacketConsumer> c = tap.attach(4);
    tap.publishPacket(packet(true, 1));
    tap.publishPacket(packet(false, 2));
    tap.publishPacket(packet(false, 3));
    tap.publishPacket(packet(false, 4));
    tap.publishPacket(packet(false, 5));    // full: dropped
    tap.publishPacket(packet(false, 6));    // dropped
    QCOMPARE(c->stats().queued, 4);
    QCOMPARE(c->stats().dropped, quint64(2));

    VideoTapItem item;
    QVERIFY(c->tryPop(item));
    QCOMPARE(asPacket(item).rtpTimestamp, quint32(1));
    QVERIFY(c->tryPop(item));
    QCOMPARE(asPacket(item).rtpTimestamp, quint32(2));

    tap.publishPacket(packet(false, 7));    // room again, but no keyframe yet: dropped
    tap.publishPacket(packet(true, 8));     // resync here
    tap.publishPacket(packet(false, 9));
    QCOMPARE(c->stats().dropped, quint64(3));
    QCOMPARE(c->stats().resyncs, quint64(1));

    QVERIFY(c->tryPop(item));
    QCOMPARE(asPacket(item).rtpTimestamp, quint32(3));
    QVERIFY(c->tryPop(item));
    QCOMPARE(asPacket(item).rtpTimestamp, quint32(4));
    QVERIFY(c->tryPop(item));
    QCOMPARE(asPacket(item).rtpTimestamp, quint32(8));
    QVERIFY(asPacket(item).keyFrame);
    QCOMPARE(asPacket(item).droppedBefore, quint64(3));
    QVERIFY(asPacket(item).discontinuity);
    QCOMPARE(asPacket(item).index, quint64(8));   // the tap's counter shows the gap too
    QVERIFY(c->tryPop(item));
    QCOMPARE(asPacket(item).rtpTimestamp, quint32(9));
    QCOMPARE(asPacket(item).droppedBefore, quint64(0));
    QVERIFY(!asPacket(item).discontinuity);
    QVERIFY(!c->tryPop(item));
}

void VideoPacketTapTest::_eventsAfterFullQueue()
{
    VideoPacketTap tap;
    std::shared_ptr<VideoPacketConsumer> c = tap.attach(2);
    tap.publishPacket(packet(true, 1));
    tap.publishPacket(packet(false, 2));
    // Stream changes reach a full queue (event reserve)
    tap.publishEvent(tapEvent(VideoTapEvent::Type::SsrcChanged));
    QCOMPARE(c->stats().events, quint64(1));
    // and the new stream starts at its keyframe
    VideoTapItem item;
    QVERIFY(c->tryPop(item));
    QVERIFY(c->tryPop(item));
    tap.publishPacket(packet(false, 3));
    tap.publishPacket(packet(true, 4));
    QVERIFY(c->tryPop(item));
    QVERIFY(std::get<VideoTapEvent>(item).type == VideoTapEvent::Type::SsrcChanged);
    QVERIFY(c->tryPop(item));
    QCOMPARE(asPacket(item).rtpTimestamp, quint32(4));

    // The event reserve is bounded too
    for (int i = 0; i < VideoPacketConsumer::kEventReserve + 10; ++i) {
        tap.publishEvent(tapEvent(VideoTapEvent::Type::StreamStarted));
    }
    QVERIFY(c->stats().queued <= c->capacity() + VideoPacketConsumer::kEventReserve);
    QVERIFY(c->stats().droppedEvents > 0);
}

void VideoPacketTapTest::_slowConsumerDoesNotBlock()
{
    // Deterministic: the slow consumer takes one unit and then blocks until every unit has
    // been published. If publishing waited on that consumer, it could never finish (the test
    // then fails at the hang guard instead of hanging). No wall-clock threshold.
    constexpr auto kHangGuard = std::chrono::seconds(60);
    constexpr quint32 kPackets = 3000;

    VideoPacketTap tap;
    std::shared_ptr<VideoPacketConsumer> slow = tap.attach(30);
    std::shared_ptr<VideoPacketConsumer> stuck = tap.attach(10);    // never reads at all
    std::shared_ptr<VideoPacketConsumer> fast = tap.attach(VideoPacketTap::kMaxCapacity);

    std::promise<void> slowHasOne;
    std::promise<void> publishedAll;
    std::shared_future<void> release = publishedAll.get_future().share();
    std::atomic_bool slowBlockedCheck{false};
    quint64 slowReceived = 0;
    bool slowOrdered = true;
    std::thread slowThread([&]() {
        quint64 last = 0;
        VideoTapItem item;
        if (slow->waitPop(item, 60000)) {
            last = asPacket(item).index;
            ++slowReceived;
        }
        slowHasOne.set_value();
        // Blocked, as a stalled disk or network would be, until the publisher is done
        slowBlockedCheck = (release.wait_for(kHangGuard) == std::future_status::ready);
        while (slow->tryPop(item)) {
            const VideoTapPacket &p = asPacket(item);
            slowOrdered = slowOrdered && (p.index > last) && ((p.droppedBefore == 0) || p.keyFrame);
            last = p.index;
            ++slowReceived;
        }
    });

    std::promise<void> publisherDone;
    std::future<void> publisherFinished = publisherDone.get_future();
    std::future<void> slowStarted = slowHasOne.get_future();
    std::thread publisher([&]() {
        // The first unit, then (once the slow consumer holds it and blocks) all the others
        tap.publishPacket(packet(true, 0));
        (void) slowStarted.wait_for(kHangGuard);
        for (quint32 i = 1; i < kPackets; ++i) {
            tap.publishPacket(packet((i % 30) == 0, i));
        }
        publisherDone.set_value();
    });

    // The fast consumer reads on this thread until it has every unit (bounded by the guard)
    quint64 fastReceived = 0;
    bool fastOrdered = true;
    quint64 last = 0;
    VideoTapItem item;
    while ((fastReceived < kPackets) && fast->waitPop(item, 60000)) {
        const VideoTapPacket &p = asPacket(item);
        fastOrdered = fastOrdered && (p.index > last);
        last = p.index;
        ++fastReceived;
    }

    const bool finished = (publisherFinished.wait_for(kHangGuard) == std::future_status::ready);
    publishedAll.set_value();   // release the slow consumer either way
    publisher.join();
    slowThread.join();

    QVERIFY2(finished, "publishing waited on the blocked consumer");
    QVERIFY(slowBlockedCheck.load());
    QCOMPARE(fastReceived, quint64(kPackets));
    QVERIFY(fastOrdered);
    // The slow consumer held unit 0, then got units 1 to 30 queued; the rest were dropped
    QVERIFY(slow->stats().dropped > 0);
    QVERIFY(slowOrdered);
    QCOMPARE(slowReceived, quint64(31));
    QCOMPARE(stuck->stats().queued, 10);
    QCOMPARE(stuck->stats().dropped, quint64(kPackets - 10));
}

void VideoPacketTapTest::_attachDetachWhileStreaming()
{
    // Generous bounds: a loaded CI runner may starve any of these threads for seconds
    constexpr int kWaitMs = 30000;

    VideoPacketTap tap;
    std::atomic_bool stop{false};
    std::atomic<quint64> published{0};
    std::shared_ptr<VideoPacketConsumer> waiting;
    std::atomic_bool waitReturned{false};
    std::thread producer;
    std::thread waiter;

    // Whatever happens below (a failed check returns early), the threads are stopped and
    // joined before the locals they use go away: never std::terminate on a joinable thread
    struct Cleanup
    {
        std::function<void()> run;
        ~Cleanup() { run(); }
    } cleanup{ [&]() {
        stop = true;
        if (waiting) {
            tap.detach(waiting);
        }
        if (waiter.joinable()) {
            waiter.join();
        }
        if (producer.joinable()) {
            producer.join();
        }
    } };

    producer = std::thread([&]() {
        quint32 n = 0;
        while (!stop.load()) {
            if ((n % 100) == 0) {
                tap.publishEvent(tapEvent(VideoTapEvent::Type::StreamStarted));
            }
            tap.publishPacket(packet((n % 10) == 0, n));
            ++n;
            published = n;
            if ((n % 64) == 0) {
                std::this_thread::yield();
            }
        }
    });

    // A consumer blocked in waitPop is woken by its detach. First see it receive something.
    waiting = tap.attach(1);
    {
        VideoTapItem item;
        QVERIFY2(waiting->waitPop(item, kWaitMs), "the publisher reached no consumer");
    }
    waiter = std::thread([&]() {
        VideoTapItem item;
        while (waiting->waitPop(item, kWaitMs)) {
        }
        waitReturned = true;
    });

    for (int round = 0; round < 200; ++round) {
        std::shared_ptr<VideoPacketConsumer> c = tap.attach(16);
        QVERIFY(c);
        bool firstPacket = true;
        quint64 last = 0;
        for (int i = 0; i < 20; ++i) {
            VideoTapItem item;
            if (c->tryPop(item) && isPacket(item)) {
                const VideoTapPacket &p = asPacket(item);
                if (firstPacket) {
                    QVERIFY(p.keyFrame);
                    firstPacket = false;
                }
                QVERIFY(p.index > last);
                last = p.index;
            }
        }
        tap.detach(c);
        QVERIFY(!c->attached());
        VideoTapItem item;
        while (c->tryPop(item)) {
        }
        const quint64 before = c->stats().packets;
        std::this_thread::yield();
        QCOMPARE(c->stats().packets, before);   // nothing more arrives once detached
    }

    // Detach wakes the blocked consumer at once (well before its own long timeout)
    QElapsedTimer timer;
    timer.start();
    tap.detach(waiting);
    waiter.join();
    QVERIFY(waitReturned.load());
    QVERIFY2(timer.elapsed() < kWaitMs, qPrintable(QString::number(timer.elapsed())));
    stop = true;
    producer.join();
    QVERIFY(published.load() > 0);
    QCOMPARE(tap.consumerCount(), 0);
}

void VideoPacketTapTest::_releasedConsumerIsDropped()
{
    VideoPacketTap tap;
    std::shared_ptr<VideoPacketConsumer> c = tap.attach();
    QCOMPARE(tap.consumerCount(), 1);
    c.reset();
    tap.publishPacket(packet(true, 1));
    QCOMPARE(tap.consumerCount(), 0);
}

void VideoPacketTapTest::_dataIsCopy()
{
    VideoPacketTap tap;
    std::shared_ptr<VideoPacketConsumer> a = tap.attach();
    std::shared_ptr<VideoPacketConsumer> b = tap.attach();
    QByteArray source = QByteArrayLiteral("receiver");
    VideoTapPacket p;
    p.keyFrame = true;
    p.data = source;
    tap.publishPacket(p);
    source[0] = 'X';    // the publisher's own buffer changes afterwards
    p.data[0] = 'Y';

    VideoTapItem ia;
    VideoTapItem ib;
    QVERIFY(a->tryPop(ia));
    QVERIFY(b->tryPop(ib));
    VideoTapPacket pa = asPacket(ia);
    QCOMPARE(pa.data, QByteArrayLiteral("receiver"));
    pa.data[0] = 'Z';   // one consumer writing its copy does not touch the other's
    QCOMPARE(asPacket(ib).data, QByteArrayLiteral("receiver"));
}

void VideoPacketTapTest::_parameterSets()
{
    const QByteArray sc4 = bytes({ 0, 0, 0, 1 });
    const QByteArray sc3 = bytes({ 0, 0, 1 });
    const QByteArray sps = bytes({ 0x67, 0x4d, 0x00, 0x1f });
    const QByteArray sps2 = bytes({ 0x67, 0x64, 0x00, 0x28 });
    const QByteArray pps = bytes({ 0x68, 0xee, 0x3c, 0x80 });
    const QByteArray idr = bytes({ 0x65, 0x88, 0x84, 0x10 });   // a NAL unit never ends in a zero byte (rbsp stop bit)

    VideoParameterSets h264(VideoTapCodec::H264);
    QVERIFY(h264.isEmpty());
    QVERIFY(h264.scanAnnexB(sc4 + sps + sc3 + pps + sc4 + idr));
    QCOMPARE(h264.annexB(), sc4 + sps + sc4 + pps);
    QVERIFY(!h264.scanAnnexB(sc4 + sps + sc4 + pps));   // same again: no change
    QVERIFY(h264.addNal(sps2.constData(), int(sps2.size())));
    QCOMPARE(h264.annexB(), sc4 + sps2 + sc4 + pps);
    QVERIFY(!h264.addNal(idr.constData(), int(idr.size())));
    QVERIFY(!h264.scanAnnexB(QByteArray()));
    QVERIFY(!h264.scanAnnexB(bytes({ 0, 0, 0, 0, 1 })));
    // Too large, or the forbidden bit: not kept
    QByteArray big = sps + QByteArray(VideoParameterSets::kMaxNalBytes, 'x');
    QVERIFY(!h264.addNal(big.constData(), int(big.size())));
    const QByteArray forbidden = bytes({ 0xe7, 0x00 });
    QVERIFY(!h264.addNal(forbidden.constData(), int(forbidden.size())));

    // split: the NAL units one by one, for avcC/hvcC records
    const QList<QByteArray> nals = VideoParameterSets::split(sc4 + sps + sc3 + pps + sc4 + idr);
    QCOMPARE(nals.size(), 3);
    QCOMPARE(nals.at(0), sps);
    QCOMPARE(nals.at(1), pps);
    QCOMPARE(nals.at(2), idr);
    QVERIFY(VideoParameterSets::split(QByteArray()).isEmpty());
    QVERIFY(VideoParameterSets::split(bytes({ 0, 0, 1 })).isEmpty());

    VideoParameterSets h265(VideoTapCodec::H265);
    const QByteArray vps = bytes({ 0x40, 0x01, 0x0c });
    const QByteArray hsps = bytes({ 0x42, 0x01, 0x01 });
    const QByteArray hpps = bytes({ 0x44, 0x01, 0xc1 });
    QVERIFY(h265.scanAnnexB(sc4 + hpps + sc4 + hsps + sc4 + vps + sc4 + bytes({ 0x26, 0x01, 0xaf })));
    QCOMPARE(h265.annexB(), sc4 + vps + sc4 + hsps + sc4 + hpps);   // always VPS, SPS, PPS
    h265.setCodec(VideoTapCodec::H264);
    QVERIFY(h265.isEmpty());
}

void VideoPacketTapTest::_limits()
{
    VideoPacketTap tap;
    QCOMPARE(tap.attach(0)->capacity(), 1);
    QCOMPARE(tap.attach(1000000)->capacity(), VideoPacketTap::kMaxCapacity);
    QList<std::shared_ptr<VideoPacketConsumer>> keep;
    while (std::shared_ptr<VideoPacketConsumer> c = tap.attach()) {
        keep.append(c);
        QVERIFY(keep.size() <= VideoPacketTap::kMaxConsumers);
    }
    QVERIFY(tap.consumerCount() <= VideoPacketTap::kMaxConsumers);
}

// Security review ------------------------------------------------------------------------

namespace {

/// Runs a wakeup that blocks inside its first call, then calls clear (setWakeup({}) or
/// detach) on another thread. clear must not return while that call runs, and the old wakeup
/// must never run once clear has returned (M2). Deterministic: the wakeup is held by a gate,
/// not by the clock; the short poll below only gives a broken clear the chance to return early.
void checkClearWaitsForWakeup(const std::function<void(VideoPacketTap &, const std::shared_ptr<VideoPacketConsumer> &)> &clear)
{
    constexpr auto kHangGuard = std::chrono::seconds(60);

    VideoPacketTap tap;
    std::shared_ptr<VideoPacketConsumer> c = tap.attach(16);
    QVERIFY(c);

    std::promise<void> inWakeup;
    std::future<void> inWakeupFuture = inWakeup.get_future();
    std::promise<void> releaseWakeup;
    std::shared_future<void> release = releaseWakeup.get_future().share();
    std::atomic_bool first{true};
    std::atomic_bool clearReturned{false};
    std::atomic_bool ranAfterClear{false};
    std::atomic<int> calls{0};
    c->setWakeup([&]() {
        calls++;
        if (clearReturned.load()) {
            ranAfterClear = true;
        }
        if (first.exchange(false)) {
            inWakeup.set_value();
            (void) release.wait_for(kHangGuard);
        }
        if (clearReturned.load()) {
            ranAfterClear = true;   // still running when clear had returned
        }
    });

    std::thread publisher([&]() { tap.publishPacket(packet(true, 1)); });
    const bool wakeupStarted = (inWakeupFuture.wait_for(kHangGuard) == std::future_status::ready);

    std::promise<void> clearStarting;
    std::future<void> clearStartingFuture = clearStarting.get_future();
    std::thread clearer([&]() {
        clearStarting.set_value();
        clear(tap, c);
        clearReturned = true;
    });
    (void) clearStartingFuture.wait_for(kHangGuard);
    QElapsedTimer timer;
    timer.start();
    while (!clearReturned.load() && (timer.elapsed() < 200)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool returnedWhileRunning = clearReturned.load();
    releaseWakeup.set_value();
    clearer.join();
    publisher.join();

    QVERIFY(wakeupStarted);
    QVERIFY2(!returnedWhileRunning, "clear returned while the old wakeup was still running");
    QVERIFY(clearReturned.load());
    QVERIFY(!ranAfterClear.load());

    // And no call starts afterwards, whether or not the consumer is still attached
    const int before = calls.load();
    for (quint32 i = 2; i < 50; ++i) {
        tap.publishPacket(packet(true, i));
    }
    QCOMPARE(calls.load(), before);
    tap.detach(c);
}

} // namespace

void VideoPacketTapTest::_detachWaitsForRunningWakeup()
{
    checkClearWaitsForWakeup([](VideoPacketTap &tap, const std::shared_ptr<VideoPacketConsumer> &c) { tap.detach(c); });
}

void VideoPacketTapTest::_setWakeupWaitsForRunningWakeup()
{
    checkClearWaitsForWakeup([](VideoPacketTap &, const std::shared_ptr<VideoPacketConsumer> &c) { c->setWakeup({}); });
}

void VideoPacketTapTest::_publishDetachDestroyConcurrently()
{
    // The G95 writer's pattern: the wakeup captures a raw pointer to its owner, the owner clears
    // the wakeup (or detaches) and is then deleted at once, while the receiver publishes on
    // another thread. A wakeup that ran after the clear returned would touch a deleted owner:
    // the owner is poisoned before it is deleted, so that is counted here (and ASan reports the
    // use after free). The outcome does not depend on timing: with the wake lock it is always 0.
    constexpr int kRounds = 300;
    constexpr int kWaitMs = 30000;
    static constexpr quint32 kAlive = 0x600dcafe;

    struct Owner
    {
        std::atomic<quint32> magic{kAlive};
        std::atomic<int> wakes{0};
    };

    VideoPacketTap tap;
    std::atomic_bool stop{false};
    std::atomic<int> violations{0};
    std::thread publisher;
    struct Cleanup
    {
        std::function<void()> run;
        ~Cleanup() { run(); }
    } cleanup{ [&]() {
        stop = true;
        if (publisher.joinable()) {
            publisher.join();
        }
    } };

    publisher = std::thread([&]() {
        quint32 n = 0;
        while (!stop.load()) {
            if ((n % 50) == 0) {
                tap.publishEvent(tapEvent(VideoTapEvent::Type::StreamStarted));
            }
            tap.publishPacket(packet((n % 2) == 0, n));
            ++n;
        }
    });

    int reached = 0;
    for (int round = 0; round < kRounds; ++round) {
        Owner *owner = new Owner;
        std::shared_ptr<VideoPacketConsumer> c = tap.attach(4);
        QVERIFY(c);
        c->setWakeup([owner, &violations]() {
            if (owner->magic.load() != kAlive) {
                violations++;
            }
            owner->wakes++;
        });
        // Let the publisher reach this consumer (bounded by the hang guard, not timed)
        QElapsedTimer timer;
        timer.start();
        while ((owner->wakes.load() == 0) && (timer.elapsed() < kWaitMs)) {
            std::this_thread::yield();
        }
        if (owner->wakes.load() > 0) {
            ++reached;
        }
        VideoTapItem item;
        while (c->tryPop(item)) {
        }
        switch (round % 3) {
        case 0:
            c->setWakeup({});
            break;
        case 1:
            tap.detach(c);
            break;
        default:
            c->setWakeup({});
            tap.detach(c);
            break;
        }
        // From here no call to the old wakeup may run: poison and delete the owner at once
        owner->magic = 0;
        delete owner;
        tap.detach(c);      // safe after a detach as well
        c.reset();
    }
    stop = true;
    publisher.join();

    QCOMPARE(violations.load(), 0);
    QCOMPARE(reached, kRounds);
    QCOMPARE(tap.consumerCount(), 0);
}

void VideoPacketTapTest::_byteBudgetBoundsQueue()
{
    // M3: a hostile sender makes every access unit as large as the receiver accepts (8 MiB,
    // the bound on the UDP and RTSP/TCP paths) and the consumer never reads. The queue stays
    // inside its byte budget although its packet capacity alone would allow 3000 x 8 MiB.
    VideoPacketTap tap;
    std::shared_ptr<VideoPacketConsumer> c = tap.attach(VideoPacketTap::kMaxCapacity);
    QVERIFY(c);
    QCOMPARE(c->maxBytes(), VideoPacketTap::kDefaultMaxBytes);

    const QByteArray unit(RtpDepacketizer::kDefaultMaxAccessUnitBytes, 'k');
    const QByteArray parameterSets(64, 'p');
    const qint64 perUnit = static_cast<qint64>(unit.size()) + static_cast<qint64>(parameterSets.size());
    const int fits = static_cast<int>(VideoPacketTap::kDefaultMaxBytes / perUnit);
    constexpr int kUnits = 100;
    QVERIFY((fits > 0) && (fits < kUnits));

    auto bigUnit = [&](bool key, quint32 n) {
        VideoTapPacket p;
        p.keyFrame = key;
        p.data = unit;      // shared bytes: the test itself stays small
        p.parameterSets = parameterSets;
        p.rtpTimestamp = n;
        return p;
    };

    for (int i = 0; i < kUnits; ++i) {
        tap.publishPacket(bigUnit(true, quint32(i)));
        QVERIFY(c->stats().queuedBytes <= VideoPacketTap::kDefaultMaxBytes);
    }
    VideoPacketConsumer::Stats s = c->stats();
    QCOMPARE(s.queued, fits);
    QCOMPARE(s.queuedBytes, perUnit * fits);
    QCOMPARE(s.packets, quint64(fits));
    QCOMPARE(s.dropped, quint64(kUnits - fits));
    QCOMPARE(VideoPacketConsumer::packetBytes(bigUnit(true, 0)), perUnit);

    // Room again after one pop, but the consumer waits for a keyframe, as after a count drop
    VideoTapItem item;
    QVERIFY(c->tryPop(item));
    QCOMPARE(c->stats().queuedBytes, perUnit * (fits - 1));
    tap.publishPacket(bigUnit(false, 1000));
    QCOMPARE(c->stats().queued, fits - 1);
    QCOMPARE(c->stats().dropped, quint64(kUnits - fits + 1));

    // The next keyframe resyncs, marked as a discontinuity with the units lost before it
    tap.publishPacket(bigUnit(true, 1001));
    s = c->stats();
    QCOMPARE(s.queued, fits);
    QCOMPARE(s.resyncs, quint64(1));
    VideoTapPacket lastPacket;
    int popped = 0;
    while (c->tryPop(item)) {
        lastPacket = asPacket(item);
        ++popped;
    }
    QCOMPARE(popped, fits);
    QVERIFY(lastPacket.keyFrame);
    QVERIFY(lastPacket.discontinuity);
    QCOMPARE(lastPacket.rtpTimestamp, quint32(1001));
    QCOMPARE(lastPacket.droppedBefore, quint64(kUnits - fits + 1));
    QCOMPARE(c->stats().queuedBytes, qint64(0));
    QCOMPARE(c->stats().queued, 0);

    // waitPop releases bytes the same way
    tap.publishPacket(bigUnit(true, 1002));
    QCOMPARE(c->stats().queuedBytes, perUnit);
    QVERIFY(c->waitPop(item, 1000));
    QCOMPARE(c->stats().queuedBytes, qint64(0));
}

void VideoPacketTapTest::_byteBudgetLimits()
{
    // The smallest budget always holds one largest unit with the largest parameter sets
    QVERIFY(VideoPacketTap::kMinMaxBytes >= qint64(RtpDepacketizer::kDefaultMaxAccessUnitBytes) + qint64(3 * (VideoParameterSets::kMaxNalBytes + 4)));
    QVERIFY(VideoPacketTap::kMinMaxBytes <= VideoPacketTap::kDefaultMaxBytes);
    QVERIFY(VideoPacketTap::kDefaultMaxBytes <= VideoPacketTap::kMaxMaxBytes);

    VideoPacketTap tap;
    QCOMPARE(tap.attach(10, 0)->maxBytes(), VideoPacketTap::kMinMaxBytes);
    QCOMPARE(tap.attach(10, -1)->maxBytes(), VideoPacketTap::kMinMaxBytes);
    QCOMPARE(tap.attach(10, qint64(1) << 40)->maxBytes(), VideoPacketTap::kMaxMaxBytes);
    QCOMPARE(tap.attach(10, 32LL * 1024 * 1024)->maxBytes(), 32LL * 1024 * 1024);

    std::shared_ptr<VideoPacketConsumer> smallest = tap.attach(100, VideoPacketTap::kMinMaxBytes);
    const QByteArray unit(RtpDepacketizer::kDefaultMaxAccessUnitBytes, 'k');
    for (quint32 i = 0; i < 3; ++i) {
        VideoTapPacket p;
        p.keyFrame = true;
        p.data = unit;
        tap.publishPacket(p);
    }
    QCOMPARE(smallest->stats().queued, int(VideoPacketTap::kMinMaxBytes / unit.size()));
    QVERIFY(smallest->stats().queuedBytes <= VideoPacketTap::kMinMaxBytes);

    // Small units are still bounded by the packet count
    std::shared_ptr<VideoPacketConsumer> counted = tap.attach(5);
    for (quint32 i = 0; i < 20; ++i) {
        tap.publishPacket(packet(true, i));
    }
    QCOMPARE(counted->stats().queued, 5);
}

void VideoPacketTapTest::_attachedFalseAfterTapDestroyed()
{
    // S12: a consumer outlives its tap (the receiver was destroyed first). attached() turns
    // false, so an owner that guards on it never calls detach on the dead tap, and the
    // consumer itself stays usable.
    auto tap = std::make_unique<VideoPacketTap>();
    std::shared_ptr<VideoPacketConsumer> c = tap->attach(16);
    std::atomic<int> calls{0};
    c->setWakeup([&calls]() { calls++; });
    tap->publishPacket(packet(true, 1));
    QCOMPARE(calls.load(), 1);
    QVERIFY(c->attached());

    tap.reset();
    QVERIFY(!c->attached());
    c->setWakeup({});
    VideoTapItem item;
    QVERIFY(c->tryPop(item));                   // what was queued is still there
    QVERIFY(!c->waitPop(item, 60000));          // and a detached consumer does not wait
    QCOMPARE(calls.load(), 1);
}

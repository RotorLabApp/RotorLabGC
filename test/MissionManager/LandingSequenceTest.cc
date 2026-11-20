/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

// RotorLab GC 5.1.2 (G75). Uses the fixed-wing and VTOL landing plans of the G61 corpus
// (:/unittest/plan-corpus/, saved by stock GC 5.1.1), adds a waypoint after the landing,
// turns useDoChangeSpeed on and off, and checks the sequence numbers the way
// MissionController::_convertToMissionItems builds the upload: each visual item appends its
// items in order, so the numbers must run 0, 1, 2, ... without a gap or a repeat.

#include "LandingSequenceTest.h"
#include "LandingComplexItem.h"
#include "MissionController.h"
#include "MissionItem.h"
#include "PlanMasterController.h"
#include "QmlObjectListModel.h"
#include "VisualMissionItem.h"

#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkProxyFactory>
#include <QtTest/QTest>

namespace {

class DeadLocalProxyFactory : public QNetworkProxyFactory
{
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery&) override
    {
        // Discard port on loopback: terrain queries are refused at once, nothing leaves the machine.
        return { QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 9) };
    }
};

// Builds the mission items as the upload does and checks the numbering. Returns a failure text or "".
QString checkSequence(QmlObjectListModel* visualItems, LandingComplexItem* land, VisualMissionItem* after)
{
    QObject parent;
    QList<MissionItem*> all;
    for (int i = 0; i < visualItems->count(); i++) {
        VisualMissionItem* item = visualItems->value<VisualMissionItem*>(i);
        const int first = all.count();
        item->appendMissionItems(all, &parent);
        if (i == 0) {
            continue;   // mission settings: the planned home, sequence 0
        }
        const int emitted = all.count() - first;
        if (emitted > 0 && all.last()->sequenceNumber() != item->lastSequenceNumber()) {
            return QStringLiteral("item %1 (%2): lastSequenceNumber %3, but its last emitted item is %4 (%5 items)")
                .arg(i).arg(item->commandName()).arg(item->lastSequenceNumber()).arg(all.last()->sequenceNumber()).arg(emitted);
        }
    }
    for (int i = 0; i < all.count(); i++) {
        if (all[i]->sequenceNumber() != i) {
            return QStringLiteral("mission item %1 has sequence number %2 (command %3)").arg(i).arg(all[i]->sequenceNumber()).arg(all[i]->command());
        }
    }
    if (after->sequenceNumber() != land->lastSequenceNumber() + 1) {
        return QStringLiteral("item after the landing has sequence %1, landing ends at %2").arg(after->sequenceNumber()).arg(land->lastSequenceNumber());
    }
    return QString();
}

} // namespace

void LandingSequenceTest::init()
{
    UnitTest::init();
    QNetworkProxyFactory::setApplicationProxyFactory(new DeadLocalProxyFactory);
}

void LandingSequenceTest::cleanup()
{
    QNetworkProxyFactory::setApplicationProxyFactory(nullptr);
    UnitTest::cleanup();
}

void LandingSequenceTest::_sequenceMatchesEmittedItems_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::newRow("fixed wing") << QStringLiteral("ardupilot-plane-fw-landing.plan");
    QTest::newRow("vtol")       << QStringLiteral("ardupilot-vtol-vtol-landing.plan");
}

void LandingSequenceTest::_sequenceMatchesEmittedItems()
{
    QFETCH(QString, fileName);

    PlanMasterController pmc;
    pmc.setFlyView(false);
    pmc.start();
    pmc.loadFromFile(QStringLiteral(":/unittest/plan-corpus/") + fileName);
    QTest::qWait(300);

    MissionController* mc = pmc.missionController();
    QmlObjectListModel* visualItems = mc->visualItems();

    LandingComplexItem* land = nullptr;
    for (int i = 0; i < visualItems->count() && !land; i++) {
        land = visualItems->value<LandingComplexItem*>(i);
    }
    QVERIFY(land);
    QVERIFY(!land->useDoChangeSpeed()->rawValue().toBool());   // the corpus files have it off

    // Another item after the landing (a go-around waypoint, as a user may add).
    VisualMissionItem* after = mc->insertSimpleMissionItem(land->coordinate().atDistanceAndAzimuth(300, 0), -1);
    QVERIFY(after);
    QTest::qWait(100);

    // Off: the stock case, must already hold.
    QString failure = checkSequence(visualItems, land, after);
    QVERIFY2(failure.isEmpty(), qPrintable(QStringLiteral("useDoChangeSpeed off: ") + failure));

    // On: one more item (DO_CHANGE_SPEED) in the landing.
    land->useDoChangeSpeed()->setRawValue(true);
    QTest::qWait(100);
    failure = checkSequence(visualItems, land, after);
    QVERIFY2(failure.isEmpty(), qPrintable(QStringLiteral("useDoChangeSpeed on: ") + failure));

    // Off again: the numbers move back.
    land->useDoChangeSpeed()->setRawValue(false);
    QTest::qWait(100);
    failure = checkSequence(visualItems, land, after);
    QVERIFY2(failure.isEmpty(), qPrintable(QStringLiteral("useDoChangeSpeed off again: ") + failure));
}

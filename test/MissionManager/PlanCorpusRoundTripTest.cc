/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

// RotorLab GC 5.1.2 (G70, G71). The corpus files are the G61 plan corpus, saved by stock
// GC 5.1.1 through its own save path (see test/Custom/plan-corpus). They are compiled in
// through UnitTest.qrc as :/unittest/plan-corpus/<file>.
//
// Network: every request GC makes while these plans load (terrain queries) is sent to a dead
// local port (127.0.0.1:9) by an application proxy factory, so nothing leaves the machine.
// With no terrain reply, GC must not change any saved value (plan contract 5.5, offline).

#include "PlanCorpusRoundTripTest.h"
#include "MissionController.h"
#include "PlanMasterController.h"
#include "QGCMapPolygon.h"
#include "QmlObjectListModel.h"
#include "SimpleMissionItem.h"
#include "StructureScanComplexItem.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkProxyFactory>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace {

class DeadLocalProxyFactory : public QNetworkProxyFactory
{
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery&) override
    {
        // Discard port on loopback: refused at once, no DNS lookup of the target host.
        return { QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 9) };
    }
};

const QStringList kCorpus = {
    QStringLiteral("ardupilot-copter-alt-amsl.plan"),
    QStringLiteral("ardupilot-copter-alt-calc-above-terrain.plan"),     // synthetic terrain (100 m)
    QStringLiteral("ardupilot-copter-alt-mixed.plan"),
    QStringLiteral("ardupilot-copter-alt-relative.plan"),
    QStringLiteral("ardupilot-copter-alt-terrain-frame.plan"),
    QStringLiteral("ardupilot-copter-fence-rally.plan"),
    QStringLiteral("ardupilot-copter-spline.plan"),
    QStringLiteral("ardupilot-copter-structure.plan"),                  // G71
    QStringLiteral("ardupilot-copter-survey-custom-camera.plan"),
    QStringLiteral("ardupilot-copter-survey-follow-terrain.plan"),      // synthetic terrain (100 m)
    QStringLiteral("ardupilot-plane-fw-landing.plan"),
    QStringLiteral("ardupilot-vtol-vtol-landing.plan"),
    QStringLiteral("px4-copter-alt-amsl.plan"),
    QStringLiteral("px4-copter-corridor.plan"),
    QStringLiteral("px4-copter-fence-rally.plan"),
    QStringLiteral("px4-copter-survey-sony-a6000.plan"),
    QStringLiteral("wp300-px4-copter-lawnmower.plan"),
};

QString corpusPath(const QString& fileName)
{
    return QStringLiteral(":/unittest/plan-corpus/") + fileName;
}

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// First line that differs, for a readable failure message.
QString firstDifference(const QByteArray& expected, const QByteArray& actual)
{
    const QList<QByteArray> e = expected.split('\n');
    const QList<QByteArray> a = actual.split('\n');
    for (int i = 0; i < qMax(e.count(), a.count()); i++) {
        const QByteArray el = e.value(i).trimmed();
        const QByteArray al = a.value(i).trimmed();
        if (el != al) {
            return QStringLiteral("line %1: expected '%2', got '%3'").arg(i + 1).arg(QString::fromUtf8(el), QString::fromUtf8(al));
        }
    }
    return QStringLiteral("line endings differ");
}

} // namespace

// RotorLab: the dead proxy covers the whole test case, not each row. Terrain
// requests still queued when a row ended could start between rows without it
// and reach the real elevation server, so a later row saw real terrain (CI run
// 44167460910, px4-copter-survey-sony-a6000). Same pattern as Plan3DTest.
void PlanCorpusRoundTripTest::initTestCase()
{
    QNetworkProxyFactory::setApplicationProxyFactory(new DeadLocalProxyFactory);
}

void PlanCorpusRoundTripTest::cleanupTestCase()
{
    QNetworkProxyFactory::setApplicationProxyFactory(nullptr);   // deletes ours
}

void PlanCorpusRoundTripTest::init()
{
    UnitTest::init();
}

void PlanCorpusRoundTripTest::cleanup()
{
    UnitTest::cleanup();
}

PlanMasterController* PlanCorpusRoundTripTest::_load(const QString& fileName)
{
    PlanMasterController* pmc = new PlanMasterController(this);
    pmc->setFlyView(false);
    pmc->start();
    pmc->loadFromFile(corpusPath(fileName));
    QTest::qWait(500);  // let GC's recalc timers and the (refused) terrain queries run
    return pmc;
}

void PlanCorpusRoundTripTest::_roundTripIsIdentity_data()
{
    QTest::addColumn<QString>("fileName");
    for (const QString& fileName : kCorpus) {
        QTest::newRow(qPrintable(fileName)) << fileName;
    }
}

// G70 and G71: load then save gives the corpus file back byte for byte. The plan contract
// (PLAN-CONTRACT.md 5.5) lists the fields GC may rewrite on its own load and save; offline and
// with GC's own files, none of them changes, so the whole file must be identical.
void PlanCorpusRoundTripTest::_roundTripIsIdentity()
{
    QFETCH(QString, fileName);

    QByteArray expected = readAll(corpusPath(fileName));
    QVERIFY2(!expected.isEmpty(), qPrintable(fileName));
#ifndef Q_OS_WIN
    expected.replace("\r\n", "\n");    // the corpus was written on Windows (CRLF); GC writes LF elsewhere
#endif

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString savePath = tempDir.filePath(fileName);

    PlanMasterController* pmc = _load(fileName);
    pmc->saveToFile(savePath);
    const QByteArray actual = readAll(savePath);
    delete pmc;

    QVERIFY2(actual == expected, qPrintable(fileName + QStringLiteral(": ") + firstDifference(expected, actual)));
}

// G70: AMSLAltAboveTerrain is read from its own key. In the synthetic-terrain file (100 m terrain,
// 40 m above it) the waypoints hold 140; before the fix the load put Altitude (40) there.
void PlanCorpusRoundTripTest::_amslAltAboveTerrainLoadedFromItsOwnKey()
{
    PlanMasterController* pmc = _load(QStringLiteral("ardupilot-copter-alt-calc-above-terrain.plan"));
    QmlObjectListModel* items = pmc->missionController()->visualItems();

    int waypoints = 0;
    for (int i = 0; i < items->count(); i++) {
        SimpleMissionItem* item = items->value<SimpleMissionItem*>(i);
        if (!item || item->altitudeMode() != QGroundControlQmlGlobal::AltitudeModeCalcAboveTerrain) {
            continue;
        }
        waypoints++;
        QCOMPARE(item->altitude()->rawValue().toDouble(), 40.0);
        QCOMPARE(item->amslAltAboveTerrain()->rawValue().toDouble(), 140.0);
    }
    QCOMPARE(waypoints, 4);

    // A saved null stays NaN (the takeoff in the same file), it does not become the altitude (30).
    SimpleMissionItem* takeoff = items->value<SimpleMissionItem*>(1);
    QVERIFY(takeoff);
    QCOMPARE(takeoff->altitude()->rawValue().toDouble(), 30.0);
    QVERIFY(qIsNaN(takeoff->amslAltAboveTerrain()->rawValue().toDouble()));

    delete pmc;
}

// G71: a structure scan loads (a Debug build used to abort in beginInsertRows) and keeps its footprint.
void PlanCorpusRoundTripTest::_structureScanLoads()
{
    PlanMasterController* pmc = _load(QStringLiteral("ardupilot-copter-structure.plan"));
    QmlObjectListModel* items = pmc->missionController()->visualItems();

    StructureScanComplexItem* scan = nullptr;
    for (int i = 0; i < items->count() && !scan; i++) {
        scan = items->value<StructureScanComplexItem*>(i);
    }
    QVERIFY(scan);
    QCOMPARE(scan->structurePolygon()->count(), 4);
    QVERIFY(scan->flightPolygon()->count() >= 3);
    delete pmc;
}

// G71: inserting an empty list is a no-op: no row signals (beginInsertRows(n, n-1) asserts in Qt
// Debug builds), no count change, and the model does not become dirty.
void PlanCorpusRoundTripTest::_emptyInsertIsNoOp()
{
    QmlObjectListModel model;
    QObject a, b;
    model.append(&a);
    model.setDirty(false);

    QSignalSpy aboutToInsert(&model, &QAbstractItemModel::rowsAboutToBeInserted);
    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy countChanged(&model, &QmlObjectListModel::countChanged);

    model.append(QList<QObject*>());
    model.insert(0, QList<QObject*>());

    QCOMPARE(model.count(), 1);
    QCOMPARE(aboutToInsert.count(), 0);
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(countChanged.count(), 0);
    QVERIFY(!model.dirty());

    // A non-empty list still inserts as before.
    model.append(QList<QObject*>{ &b });
    QCOMPARE(model.count(), 2);
    QCOMPARE(inserted.count(), 1);
    QVERIFY(model.dirty());

    model.clear();
}

/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

// RotorLab GC 5.2.0 (G86). ArduPilot keeps only the breach return point's latitude and longitude
// (MissionItemProtocol_Fence.cpp, AC_PolyFence_loader.cpp), ArduCopter and Rover never go to it,
// ArduPlane flies to it at FENCE_RET_ALT, and PX4 skips it. The editor's model must say the
// height is not used, keep the height it read, and save the file unchanged.
//
// Network: terrain queries made while a plan loads go to a dead local port (127.0.0.1:9).

#include "GeoFenceBreachReturnTest.h"
#include "GeoFenceController.h"
#include "PlanMasterController.h"

#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkProxyFactory>
#include <QtTest/QTest>

namespace {

class DeadLocalProxyFactory : public QNetworkProxyFactory
{
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery&) override
    {
        return { QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 9) };
    }
};

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeAll(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

} // namespace

void GeoFenceBreachReturnTest::init()
{
    UnitTest::init();
    QNetworkProxyFactory::setApplicationProxyFactory(new DeadLocalProxyFactory);
}

void GeoFenceBreachReturnTest::cleanup()
{
    QNetworkProxyFactory::setApplicationProxyFactory(nullptr);
    UnitTest::cleanup();
}

PlanMasterController* GeoFenceBreachReturnTest::_load(const QString& path)
{
    PlanMasterController* pmc = new PlanMasterController(this);
    pmc->setFlyView(false);
    pmc->start();
    pmc->loadFromFile(path);
    QTest::qWait(500);
    return pmc;
}

void GeoFenceBreachReturnTest::_heightUsage_data()
{
    QTest::addColumn<QString>("corpusFile");
    QTest::addColumn<QByteArray>("vehicleTypeLine");    // replaces "vehicleType": 2, when not empty
    QTest::addColumn<QString>("label");
    QTest::addColumn<QString>("noteStart");

    QTest::newRow("ArduPilot Copter") << QStringLiteral("ardupilot-copter-fence-rally.plan") << QByteArray()
        << QStringLiteral("Height not used by ArduPilot Copter")
        << QStringLiteral("ArduPilot keeps only the position. ArduCopter does not fly to this point: its fence action (FENCE_ACTION) decides what it does.");
    QTest::newRow("ArduPilot Plane") << QStringLiteral("ardupilot-copter-fence-rally.plan") << QByteArray("\"vehicleType\": 1,")
        << QStringLiteral("Height not used by ArduPilot Plane")
        << QStringLiteral("ArduPilot keeps only the position. With a Guided fence action, ArduPlane flies here at its fence return altitude (FENCE_RET_ALT) above home, not at this height.");
    QTest::newRow("ArduPilot Rover") << QStringLiteral("ardupilot-copter-fence-rally.plan") << QByteArray("\"vehicleType\": 10,")
        << QStringLiteral("Height not used by ArduPilot Rover")
        << QStringLiteral("ArduPilot keeps only the position. ArduPilot Rover does not drive to this point: its fence action (FENCE_ACTION) decides what it does.");
    // The PX4 corpus file has no breach return; the ArduPilot file is loaded as PX4 to cover it.
    QTest::newRow("PX4") << QStringLiteral("ardupilot-copter-fence-rally.plan") << QByteArray("PX4")
        << QStringLiteral("Height not used by PX4")
        << QStringLiteral("PX4 skips this point when it loads the fence");
}

void GeoFenceBreachReturnTest::_heightUsage()
{
    QFETCH(QString, corpusFile);
    QFETCH(QByteArray, vehicleTypeLine);
    QFETCH(QString, label);
    QFETCH(QString, noteStart);

    QByteArray input = readAll(QStringLiteral(":/unittest/plan-corpus/") + corpusFile);
    QVERIFY(!input.isEmpty());
    if (vehicleTypeLine == "PX4") {
        QVERIFY(input.contains("\"firmwareType\": 3,"));
        input.replace("\"firmwareType\": 3,", "\"firmwareType\": 12,");
    } else if (!vehicleTypeLine.isEmpty()) {
        QVERIFY(input.contains("\"vehicleType\": 2,"));
        input.replace("\"vehicleType\": 2,", vehicleTypeLine);
    }

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString inPath = tempDir.filePath(QStringLiteral("in.plan"));
    const QString outPath = tempDir.filePath(QStringLiteral("out.plan"));
    QVERIFY(writeAll(inPath, input));

    PlanMasterController* pmc = _load(inPath);
    GeoFenceController* fence = pmc->geoFenceController();

    // The editor's model: height not used, with the matching text.
    QCOMPARE(fence->breachReturnAltitudeUsed(), false);
    QCOMPARE(fence->breachReturnAltitudeLabel(), label);
    QVERIFY2(fence->breachReturnAltitudeNote().startsWith(noteStart), qPrintable(fence->breachReturnAltitudeNote()));
    QVERIFY(!fence->breachReturnAltitudeNote().contains(QChar(0x2014)));

    // The height read from the file is kept.
    QVERIFY(fence->breachReturnPoint().isValid());
    QCOMPARE(fence->breachReturnAltitude()->rawValue().toDouble(), 35.0);

    // Round trip: the saved file is the file that was loaded.
    pmc->saveToFile(outPath);
    const QByteArray output = readAll(outPath);
    delete pmc;
    QVERIFY(!output.isEmpty());
    QByteArray expected = input;
#ifndef Q_OS_WIN
    expected.replace("\r\n", "\n");
#endif
    QVERIFY2(output == expected, qPrintable(corpusFile + QStringLiteral(" as ") + label));
}

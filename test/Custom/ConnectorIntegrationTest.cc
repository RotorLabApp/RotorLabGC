#include "ConnectorIntegrationTest.h"

#include "Vehicle.h"
#include "fleet/FleetClient.h"
#include "fleet/FleetConnector.h"
#include "fleet/FleetStore.h"
#include "license/LicenseManager.h"
#include "rotorlab/FlightSpool.h"
#include "rotorlab/RotorLabConnector.h"

#include <QtCore/QDateTime>
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrl>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

void ConnectorIntegrationTest::initTestCase()
{
#ifndef QGC_DAILY_BUILD
    QFAIL("Local integration tests require a daily build.");
#endif
    QCoreApplication::setApplicationName(QStringLiteral("RotorLabGC_integration_%1_unittest")
                                            .arg(QCoreApplication::applicationPid()));
    QStandardPaths::setTestModeEnabled(true);
    QFile configFile(qEnvironmentVariable("ROTORLAB_GC_INTEGRATION_CONFIG"));
    QVERIFY2(configFile.open(QIODevice::ReadOnly), "Set ROTORLAB_GC_INTEGRATION_CONFIG to a local test configuration.");
    QJsonParseError error;
    _config = QJsonDocument::fromJson(configFile.readAll(), &error).object();
    QCOMPARE(error.error, QJsonParseError::NoError);
    for (const QString &name : {QStringLiteral("fleet_url"), QStringLiteral("records_url"),
                                QStringLiteral("records_only_url"),
                                QStringLiteral("rotorlab_url"), QStringLiteral("control_url")}) {
        const QUrl url(_config.value(name).toString());
        QVERIFY(url.isValid());
        QCOMPARE(url.host(), QStringLiteral("127.0.0.1"));
        QCOMPARE(url.scheme(), QStringLiteral("http"));
    }
    qputenv("ROTORLAB_BASE_URL", _config.value(QStringLiteral("rotorlab_url")).toString().toUtf8());
    RotorLabConnector::instance()->setEnabled(false);
    FleetConnector::instance()->disconnectFromServer();
}

void ConnectorIntegrationTest::cleanupTestCase()
{
    RotorLabConnector::instance()->setEnabled(false);
    RotorLabConnector::instance()->disconnectFromRotorLab();
    FleetConnector::instance()->disconnectFromServer();
}

void ConnectorIntegrationTest::_licenseLifecycle()
{
    LicenseManager license;
    license.activate(_config.value(QStringLiteral("gc_license_key")).toString());
    QTRY_VERIFY_WITH_TIMEOUT(!license.busy(), 20000);
    QVERIFY2(license.activated(), qPrintable(license.statusText()));
    license.recheck();
    QTRY_VERIFY_WITH_TIMEOUT(!license.busy(), 20000);
    QVERIFY2(license.activated(), qPrintable(license.statusText()));
    license.deactivate();
    QTRY_VERIFY_WITH_TIMEOUT(!license.busy(), 20000);
    QVERIFY2(!license.activated(), qPrintable(license.statusText()));
}

QJsonObject ConnectorIntegrationTest::_control(const QString &action, const QJsonObject &body)
{
    HttpClient client(HttpClient::TlsPolicy::PublicCa);
    client.setBaseUrl(_config.value(QStringLiteral("control_url")).toString());
    client.setToken(_config.value(QStringLiteral("control_token")).toString());
    bool done = false;
    HttpReply reply;
    client.post(QStringLiteral("/") + action, body, [&](const HttpReply &result) {
        reply = result;
        done = true;
    }, 60000);
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 65000) {
        QTest::qWait(20);
    }
    if (!done || !reply.netOk || reply.status != 200) {
        QTest::qFail(qPrintable(QStringLiteral("Test coordinator %1: HTTP %2: %3")
                                   .arg(action).arg(reply.status).arg(reply.error)), __FILE__, __LINE__);
        return {};
    }
    return reply.body;
}

void ConnectorIntegrationTest::_flyFleet(const QString &stage, bool records)
{
    const QJsonObject enrollment = _control(QStringLiteral("enrollment"),
                                            {{QStringLiteral("server"), records ? QStringLiteral("records") : QStringLiteral("fleet")},
                                             {QStringLiteral("stage"), stage}});
    QVERIFY(!enrollment.isEmpty());
    FleetConnector *const fleet = FleetConnector::instance();
    fleet->disconnectFromServer();
    fleet->enroll(_config.value(records ? QStringLiteral("records_url") : QStringLiteral("fleet_url")).toString(),
                  enrollment.value(QStringLiteral("code")).toString(),
                  QStringLiteral("GC integration ") + stage);
    QTRY_COMPARE_WITH_TIMEOUT(fleet->state(), FleetConnector::Connected, 20000);
    QVERIFY(fleet->rosterLoaded());
    QVERIFY(!fleet->pilots().isEmpty());
    QVERIFY(!fleet->assets().isEmpty());
    fleet->setCollectLogsAutomatically(false);
    fleet->setCrew(_config.value(QStringLiteral("pilot_id")).toInt(),
                   QStringLiteral("Local demo pilot"), _config.value(QStringLiteral("asset_id")).toInt(),
                   stage, QStringLiteral("training"), QStringLiteral("Local simulated crew"));
    _connectMockLink();
    QVERIFY(_vehicle);
    _vehicle->setArmed(true, false);
    QTRY_VERIFY_WITH_TIMEOUT(_vehicle->armed(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(FleetStore::flightState(_vehicle->id()).value(QStringLiteral("active")).toBool(), 5000);
    const QString ref = FleetStore::flightState(_vehicle->id()).value(QStringLiteral("ref")).toString();
    QVERIFY(!ref.isEmpty());
    QTest::qWait(12000);
    _vehicle->setArmed(false, false);
    QTRY_VERIFY_WITH_TIMEOUT(!_vehicle->armed(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(FleetStore::flightState(_vehicle->id()).value(QStringLiteral("completed")).toBool(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(FleetStore::pendingFlightDetails().isEmpty(), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(!fleet->flightLogs().isEmpty(), 5000);
    fleet->attachFlightLog(ref, enrollment.value(QStringLiteral("log_path")).toString());

    QVERIFY(!fleet->preflightTemplates().isEmpty());
    const QVariantMap checklist = fleet->preflightTemplates().first().toMap();
    const QVariantList items{QVariantMap{{QStringLiteral("text"), QStringLiteral("Local test inspection")},
                                         {QStringLiteral("ok"), true},
                                         {QStringLiteral("result"), QStringLiteral("pass")}}};
    fleet->fileChecklist(ref + QStringLiteral("-pre"), ref, checklist.value(QStringLiteral("id")).toInt(),
                         QStringLiteral("preflight"), checklist.value(QStringLiteral("name")).toString(),
                         QStringLiteral("Demo aircraft"), QDateTime::currentSecsSinceEpoch(),
                         true, items, QStringLiteral("Local integration test"));
    QTRY_COMPARE_WITH_TIMEOUT(fleet->outboxCount(), 0, 20000);
    const QJsonObject evidence = _control(QStringLiteral("verify-fleet"),
                                         {{QStringLiteral("flight_ref"), ref}, {QStringLiteral("stage"), stage}});
    QVERIFY2(evidence.value(QStringLiteral("passed")).toBool(), qPrintable(QString::fromUtf8(QJsonDocument(evidence).toJson())));
    _disconnectMockLink();
    fleet->disconnectFromServer();
}

void ConnectorIntegrationTest::_fleetToRotorLab()
{
    QVERIFY(_control(QStringLiteral("unlink-records")).contains(QStringLiteral("linked")));
    _flyFleet(QStringLiteral("gc-fleet-rotorlab"));
}

void ConnectorIntegrationTest::_recordsToRotorLab()
{
    _flyFleet(QStringLiteral("gc-records-rotorlab"), true);
}

void ConnectorIntegrationTest::_recordsOnlyRefusesGroundStations()
{
    FleetClient client;
    client.setBaseUrl(_config.value(QStringLiteral("records_only_url")).toString());
    bool done = false;
    HttpReply reply;
    client.postUnauthenticated(QStringLiteral("/api/gc/v1/enroll"),
                               {{QStringLiteral("code"), QStringLiteral("records-only-test")},
                                {QStringLiteral("label"), QStringLiteral("Local GC")}},
                               [&](const HttpReply &result) { reply = result; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
    QVERIFY(reply.netOk);
    QCOMPARE(reply.status, 409);
    QVERIFY(!reply.body.contains(QStringLiteral("token")));
    FleetConnector *const fleet = FleetConnector::instance();
    fleet->enroll(_config.value(QStringLiteral("records_only_url")).toString(),
                  QStringLiteral("records-only-test"), QStringLiteral("Local GC"));
    QTRY_VERIFY_WITH_TIMEOUT(!fleet->busy(), 20000);
    QCOMPARE(fleet->state(), FleetConnector::NotConnected);
    QVERIFY(!fleet->enrolled());
    QVERIFY(fleet->lastError().contains(QStringLiteral("Fleet Server license")));
}

void ConnectorIntegrationTest::_directRotorLab()
{
    QTest::qWait(31000);
    RotorLabConnector *const rotorlab = RotorLabConnector::instance();
    rotorlab->setEnabled(true);
    rotorlab->setFileFlightsAutomatically(true);
    rotorlab->setUploadLogs(false);
    rotorlab->setKeyAndVerify(_config.value(QStringLiteral("device_key")).toString());
    QTRY_COMPARE_WITH_TIMEOUT(rotorlab->state(), RotorLabConnector::Connected, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(rotorlab->rosterLoaded(), 20000);
    QVERIFY(!rotorlab->assets().isEmpty());
    rotorlab->setCrew(_config.value(QStringLiteral("pilot_id")).toInt(),
                      QStringLiteral("Local demo pilot"), _config.value(QStringLiteral("asset_id")).toInt(),
                      QStringLiteral("gc-direct-rotorlab"), QStringLiteral("training"), QStringLiteral("Local simulated crew"));
    FlightSpool *const spool = rotorlab->findChild<FlightSpool *>();
    QVERIFY(spool);
    QSignalSpy filed(spool, &FlightSpool::flightFiled);
    _connectMockLink();
    QVERIFY(_vehicle);
    _vehicle->setArmed(true, false);
    QTRY_VERIFY_WITH_TIMEOUT(_vehicle->armed(), 10000);
    QTest::qWait(12000);
    _vehicle->setArmed(false, false);
    QTRY_VERIFY_WITH_TIMEOUT(!_vehicle->armed(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!filed.isEmpty(), 45000);
    const QString ref = filed.first().first().toString();
    QVERIFY(!ref.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(rotorlab->queuePending(), 0, 30000);
    QCOMPARE(rotorlab->queueNeedsAttention(), 0);
    spool->addLog(ref + QStringLiteral("-log"), _config.value(QStringLiteral("log_path")).toString(),
                  QStringLiteral("local-demo.bin"), ref);
    QVERIFY(!rotorlab->postflightTemplates().isEmpty());
    const QVariantMap checklist = rotorlab->postflightTemplates().first().toMap();
    const QVariantList items{QVariantMap{{QStringLiteral("text"), QStringLiteral("Local test inspection")},
                                         {QStringLiteral("ok"), true},
                                         {QStringLiteral("result"), QStringLiteral("pass")}}};
    rotorlab->fileChecklist(ref + QStringLiteral("-post"), checklist.value(QStringLiteral("id")).toInt(),
                            QStringLiteral("postflight"), QStringLiteral("Demo aircraft"),
                            QDateTime::currentSecsSinceEpoch(), true, items, QStringLiteral("Local integration test"));
    rotorlab->retryQueueNow();
    QTRY_COMPARE_WITH_TIMEOUT(rotorlab->queuePending(), 0, 30000);
    QCOMPARE(rotorlab->queueNeedsAttention(), 0);
    const QJsonObject evidence = _control(QStringLiteral("verify-direct"), {{QStringLiteral("flight_ref"), ref}});
    QVERIFY2(evidence.value(QStringLiteral("passed")).toBool(), qPrintable(QString::fromUtf8(QJsonDocument(evidence).toJson())));
    rotorlab->setEnabled(false);
    rotorlab->disconnectFromRotorLab();
    _disconnectMockLink();
}

void ConnectorIntegrationTest::_fleetToRecordsToRotorLab()
{
    QVERIFY(_control(QStringLiteral("link-records")).value(QStringLiteral("linked")).toBool());
    _flyFleet(QStringLiteral("gc-fleet-records-rotorlab"));
}

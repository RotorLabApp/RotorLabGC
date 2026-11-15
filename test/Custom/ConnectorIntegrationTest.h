#pragma once

#include "UnitTest.h"

#include <QtCore/QJsonObject>

class ConnectorIntegrationTest : public UnitTest
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void _licenseLifecycle();
    void _fleetToRotorLab();
    void _recordsToRotorLab();
    void _recordsOnlyRefusesGroundStations();
    void _directRotorLab();
    void _fleetToRecordsToRotorLab();

private:
    void _flyFleet(const QString &stage, bool records = false);
    QJsonObject _control(const QString &action, const QJsonObject &body = {});
    QJsonObject _config;
};

/****************************************************************************
 *
 * RotorLab GC Fleet Connector - unified-server telemetry mapping.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QString>

class Vehicle;

class FleetTelemetry : public QObject
{
    Q_OBJECT

public:
    explicit FleetTelemetry(QObject *parent = nullptr);

    QJsonObject buildReport(const QJsonObject &runContext, int maxSamples);
    QString flightRefForChecklist(const QString &kind, int sysid,
                                  const QJsonObject &runContext);

signals:
    void flightEnded(const QJsonObject &flight);

private slots:
    void _onVehicleAdded(Vehicle *vehicle);
    void _onVehicleRemoved(Vehicle *vehicle);

private:
    void _watch(Vehicle *vehicle);

    QHash<int, QString> _uidBySysid;
};

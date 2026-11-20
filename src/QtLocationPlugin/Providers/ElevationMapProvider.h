/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "MapProvider.h"

class ElevationProvider : public MapProvider
{
protected:
    ElevationProvider(const QString &mapName, const QString &referrer, const QString &imageFormat, quint32 averageSize,
                      QGeoMapType::MapStyle mapType)
        : MapProvider(
            mapName,
            referrer,
            imageFormat,
            averageSize,
            mapType) {}

public:
    bool isElevationProvider() const final { return true; }
    virtual QByteArray serialize(const QByteArray &image) const = 0;
};

/// https://spacedata.copernicus.eu/collections/copernicus-digital-elevation-model
class CopernicusElevationProvider : public ElevationProvider
{
public:
    CopernicusElevationProvider()
        : ElevationProvider(
            kProviderKey,
            kProviderURL,
            QStringLiteral("bin"),
            kAvgElevSize,
            QGeoMapType::TerrainMap) {}

    int long2tileX(double lon, int z) const final;
    int lat2tileY(double lat, int z) const final;

    QGCTileSet getTileCount(int zoom, double topleftLon,
                            double topleftLat, double bottomRightLon,
                            double bottomRightLat) const final;

    QByteArray serialize(const QByteArray &image) const final;

    static constexpr const char *kProviderKey = "Copernicus";
    static constexpr const char *kProviderNotice = "© Airbus Defence and Space GmbH";
    static constexpr const char *kProviderURL = "https://terrain-ce.suite.auterion.com";
    static constexpr quint32 kAvgElevSize = 2786;
    /// Part of the tile cache key. GC 5.2.0 (G79): tiles cached by older versions can hold 0 m for a null height,
    /// so the key changed and those entries are never found again. Change it whenever the tile format or meaning changes.
    static constexpr const char *kTileCacheVersion = "-v2";

private:
    QString _getURL(int x, int y, int zoom) const final;

    const QString _mapUrl = QString(kProviderURL) + QStringLiteral("/api/v1/carpet?points=%1,%2,%3,%4");
};

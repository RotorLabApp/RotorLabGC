/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "TerrainTileTest.h"
#include "TerrainTile.h"
#include "TerrainTileCopernicus.h"
#include "TerrainQuery.h"
#include "QGCMapUrlEngine.h"
#include "ElevationMapProvider.h"
#include "QGCTileCacheWorker.h"

#include <QtCore/QJsonValue>
#include <QtCore/QMetaObject>
#include <QtCore/QString>
#include <QtCore/QtNumeric>
#include <QtPositioning/QGeoCoordinate>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace {

// Synthetic Copernicus-shaped terrain tile reply (no network). The tile's south west corner is
// (10.0, 20.0), its north east corner (10.01, 20.01), and the grid is 2 rows (latitude) by 3 columns (longitude).
QByteArray tileJson(const QString &row0, const QString &row1,
                    const QString &stats = QStringLiteral("\"min\": -3, \"max\": 120, \"avg\": 25.5"))
{
    return QStringLiteral(
        "{\"status\": \"success\", \"data\": {"
        "\"bounds\": {\"sw\": [10.0, 20.0], \"ne\": [10.01, 20.01]},"
        "\"stats\": {%1},"
        "\"carpet\": [[%2], [%3]]}}").arg(stats, row0, row1).toUtf8();
}

// Center of the cell at (row, col) of the 2 by 3 tile above
QGeoCoordinate cellCenter(int row, int col)
{
    return QGeoCoordinate(10.0 + ((row + 0.5) * 0.005), 20.0 + ((col + 0.5) * (0.01 / 3.0)));
}

}

void TerrainTileTest::_testCopernicusRoundsToNearest()
{
    // Truncation would give 12, 12, 0, -2, 2, 99
    const QByteArray bytes = TerrainTileCopernicus::serializeFromData(tileJson(QStringLiteral("12.7, 12.4, -0.6"), QStringLiteral("-2.5, 2.5, 99.5")));
    QVERIFY(!bytes.isEmpty());

    const TerrainTile tile(bytes);
    QVERIFY(tile.isValid());
    QCOMPARE(tile.elevation(cellCenter(0, 0)), 13.0);
    QCOMPARE(tile.elevation(cellCenter(0, 1)), 12.0);
    QCOMPARE(tile.elevation(cellCenter(0, 2)), -1.0);
    QCOMPARE(tile.elevation(cellCenter(1, 0)), -3.0);   // half rounds away from zero
    QCOMPARE(tile.elevation(cellCenter(1, 1)), 3.0);
    QCOMPARE(tile.elevation(cellCenter(1, 2)), 100.0);
}

void TerrainTileTest::_testCopernicusNullIsUnknown()
{
    // A null cell must come back as unknown (NaN), never as 0 m of known ground
    const QByteArray bytes = TerrainTileCopernicus::serializeFromData(tileJson(QStringLiteral("5.0, null, 7.0"), QStringLiteral("null, 0, 0.4")));
    QVERIFY(!bytes.isEmpty());

    const TerrainTile tile(bytes);
    QVERIFY(tile.isValid());
    QCOMPARE(tile.elevation(cellCenter(0, 0)), 5.0);
    QVERIFY(qIsNaN(tile.elevation(cellCenter(0, 1))));
    QCOMPARE(tile.elevation(cellCenter(0, 2)), 7.0);
    QVERIFY(qIsNaN(tile.elevation(cellCenter(1, 0))));
    QCOMPARE(tile.elevation(cellCenter(1, 1)), 0.0);    // a real 0 stays 0
    QCOMPARE(tile.elevation(cellCenter(1, 2)), 0.0);    // 0.4 rounds to 0
}

void TerrainTileTest::_testCopernicusBadRowsAndValues()
{
    // Values that are not numbers (string, bool, object) are unknown too
    const QByteArray bytes = TerrainTileCopernicus::serializeFromData(tileJson(QStringLiteral("\"7\", true, {}"), QStringLiteral("1, 2, 3")));
    QVERIFY(!bytes.isEmpty());
    const TerrainTile tile(bytes);
    QVERIFY(tile.isValid());
    QVERIFY(qIsNaN(tile.elevation(cellCenter(0, 0))));
    QVERIFY(qIsNaN(tile.elevation(cellCenter(0, 1))));
    QVERIFY(qIsNaN(tile.elevation(cellCenter(0, 2))));
    QCOMPARE(tile.elevation(cellCenter(1, 2)), 3.0);

    // A short row still rejects the whole tile
    QVERIFY(TerrainTileCopernicus::serializeFromData(tileJson(QStringLiteral("1, 2, 3"), QStringLiteral("1, 2"))).isEmpty());

    // Fractional min and max stats are rounded, not read as 0
    const QByteArray statsBytes = TerrainTileCopernicus::serializeFromData(
        tileJson(QStringLiteral("1, 2, 3"), QStringLiteral("4, 5, 6"), QStringLiteral("\"min\": 100.6, \"max\": 200.4, \"avg\": 150.0")));
    QVERIFY(!statsBytes.isEmpty());
    const TerrainTile statsTile(statsBytes);
    QCOMPARE(statsTile.minElevation(), 101.0);
    QCOMPARE(statsTile.maxElevation(), 200.0);
}

void TerrainTileTest::_testToTileElevationLimits()
{
    QCOMPARE(TerrainTileCopernicus::toTileElevation(0.49), static_cast<int16_t>(0));
    QCOMPARE(TerrainTileCopernicus::toTileElevation(-0.49), static_cast<int16_t>(0));
    QCOMPARE(TerrainTileCopernicus::toTileElevation(0.5), static_cast<int16_t>(1));
    QCOMPARE(TerrainTileCopernicus::toTileElevation(-0.5), static_cast<int16_t>(-1));
    QCOMPARE(TerrainTileCopernicus::toTileElevation(8848.6), static_cast<int16_t>(8849));
    QCOMPARE(TerrainTileCopernicus::toTileElevation(-430.4), static_cast<int16_t>(-430));
    QCOMPARE(TerrainTileCopernicus::toTileElevation(qQNaN()), TerrainTile::kUnknownElevation);
    QCOMPARE(TerrainTileCopernicus::toTileElevation(1e9), TerrainTile::kUnknownElevation);
    QCOMPARE(TerrainTileCopernicus::toTileElevation(-1e9), TerrainTile::kUnknownElevation);

    QVERIFY(qIsNaN(TerrainTileCopernicus::heightFromJson(QJsonValue())));
    QVERIFY(qIsNaN(TerrainTileCopernicus::heightFromJson(QJsonValue(QJsonValue::Null))));
    QVERIFY(qIsNaN(TerrainTileCopernicus::heightFromJson(QJsonValue(QStringLiteral("12")))));
    QCOMPARE(TerrainTileCopernicus::heightFromJson(QJsonValue(12.5)), 12.5);
}

void TerrainTileTest::_testCoordinateQueryUnknownHeightFails()
{
    TerrainAtCoordinateQuery query(false);
    QSignalSpy spy(&query, &TerrainAtCoordinateQuery::terrainDataReceived);

    query.signalTerrainData(true, QList<double>{ 10.0, 20.0 });
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toBool(), true);
    QCOMPARE(spy.at(0).at(1).value<QList<double>>(), (QList<double>{ 10.0, 20.0 }));

    // One unknown height: the whole answer is reported as not available, no 0 m is invented
    query.signalTerrainData(true, QList<double>{ 10.0, qQNaN() });
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toBool(), false);
    QVERIFY(spy.at(1).at(1).value<QList<double>>().isEmpty());
}

void TerrainTileTest::_testPathQueryUnknownHeightFails()
{
    TerrainPathQuery query(false);
    QSignalSpy spy(&query, &TerrainPathQuery::terrainDataReceived);

    const QList<double> good{ 5.0, 6.0, 7.0 };
    const QList<double> withUnknown{ 5.0, qQNaN(), 7.0 };

    QVERIFY(QMetaObject::invokeMethod(&query, "_pathHeights", Qt::DirectConnection,
                                      Q_ARG(bool, true), Q_ARG(double, 30.0), Q_ARG(double, 12.0), Q_ARG(QList<double>, good)));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toBool(), true);
    QCOMPARE(spy.at(0).at(1).value<TerrainPathQuery::PathHeightInfo_t>().heights.count(), 3);

    QVERIFY(QMetaObject::invokeMethod(&query, "_pathHeights", Qt::DirectConnection,
                                      Q_ARG(bool, true), Q_ARG(double, 30.0), Q_ARG(double, 12.0), Q_ARG(QList<double>, withUnknown)));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toBool(), false);
    QVERIFY(spy.at(1).at(1).value<TerrainPathQuery::PathHeightInfo_t>().heights.isEmpty());
}

void TerrainTileTest::_testOldCopernicusCacheKeyNotUsed()
{
    // Tile key an older GC used for Copernicus: provider hash from the plain name, zoom 1
    const QString type = QString::fromLatin1(CopernicusElevationProvider::kProviderKey);
    const int oldProviderHash = static_cast<int>(qHash(QStringView(type)) >> 1);
    const QString oldKey = QString::asprintf("%010d%08d%08d%03d", oldProviderHash, 1234, 5678, 1);

    const QString newKey = UrlFactory::getTileHash(type, 1234, 5678, 1);
    QVERIFY(newKey != oldKey);                                      // a tile cached under the old key is not found
    QVERIFY(UrlFactory::tileHashToType(newKey) == type);            // the new key still maps back to the provider
    QVERIFY(UrlFactory::tileHashToType(oldKey).isEmpty());          // the old key matches no provider

    // Other providers keep their keys
    for (const QString &other : UrlFactory::getProviderTypes()) {
        if (other != type) {
            const int hash = static_cast<int>(qHash(QStringView(other)) >> 1);
            QCOMPARE(UrlFactory::getTileHash(other, 1, 2, 3), QString::asprintf("%010d%08d%08d%03d", hash, 1, 2, 3));
        }
    }
}

namespace {

const QString kMigrationConnection = QStringLiteral("TerrainTileTestMigration");

// Tile cache schema as QGCCacheWorker::_createDB makes it, in a memory database. Set 1 is the default set, set 2 an offline set.
QSqlDatabase openMigrationDb()
{
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), kMigrationConnection);
    db.setDatabaseName(QStringLiteral(":memory:"));
    if (!db.open()) {
        return db;
    }
    QSqlQuery q(db);
    q.exec(QStringLiteral("CREATE TABLE Tiles (tileID INTEGER PRIMARY KEY NOT NULL, hash TEXT NOT NULL UNIQUE, format TEXT NOT NULL, tile BLOB NULL, size INTEGER, type INTEGER, date INTEGER DEFAULT 0)"));
    q.exec(QStringLiteral("CREATE TABLE TileSets (setID INTEGER PRIMARY KEY NOT NULL, name TEXT NOT NULL UNIQUE, numTiles INTEGER DEFAULT 0, defaultSet INTEGER DEFAULT 0)"));
    q.exec(QStringLiteral("CREATE TABLE SetTiles (setID INTEGER, tileID INTEGER)"));
    q.exec(QStringLiteral("CREATE TABLE TilesDownload (setID INTEGER, hash TEXT NOT NULL UNIQUE, type INTEGER, x INTEGER, y INTEGER, z INTEGER, state INTEGER DEFAULT 0)"));
    q.exec(QStringLiteral("INSERT INTO TileSets(setID, name, numTiles, defaultSet) VALUES(1, 'Default Tile Set', 0, 1)"));
    q.exec(QStringLiteral("INSERT INTO TileSets(setID, name, numTiles, defaultSet) VALUES(2, 'Offline terrain', 3, 0)"));
    return db;
}

QString legacyKey(const QString &type, int x, int y, int z)
{
    return QString::asprintf("%010d%08d%08d%03d", UrlFactory::legacyHashFromProviderType(type), x, y, z);
}

int scalar(QSqlDatabase &db, const QString &sql)
{
    QSqlQuery q(db);
    return (q.exec(sql) && q.next()) ? q.value(0).toInt() : -1;
}

}

void TerrainTileTest::_testMigrationMovesOldPendingRows()
{
    const QString type = QString::fromLatin1(CopernicusElevationProvider::kProviderKey);
    {
        QSqlDatabase db = openMigrationDb();
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        // Pending rows of set 2 under the old key: one waiting (state 0), one that was mid download (state 1)
        q.exec(QStringLiteral("INSERT INTO TilesDownload(setID, hash, type, x, y, z, state) VALUES(2, '%1', 5, 100, 200, 1, 0)").arg(legacyKey(type, 100, 200, 1)));
        q.exec(QStringLiteral("INSERT INTO TilesDownload(setID, hash, type, x, y, z, state) VALUES(2, '%1', 5, 101, 200, 1, 1)").arg(legacyKey(type, 101, 200, 1)));
        // A pending row of another provider must not change
        const QString other = UrlFactory::getProviderTypes().first() == type ? UrlFactory::getProviderTypes().last() : UrlFactory::getProviderTypes().first();
        const QString otherKey = legacyKey(other, 7, 8, 9);
        q.exec(QStringLiteral("INSERT INTO TilesDownload(setID, hash, type, x, y, z, state) VALUES(2, '%1', 6, 7, 8, 9, 0)").arg(otherKey));

        QCOMPARE(QGCCacheWorker::migrateCopernicusTileKeys(db), 2);

        // Both old rows are now under the new key, which resolves to the provider, and none is left under the old key
        QCOMPARE(scalar(db, QStringLiteral("SELECT COUNT(*) FROM TilesDownload WHERE hash = '%1'").arg(UrlFactory::getTileHash(type, 100, 200, 1))), 1);
        QCOMPARE(scalar(db, QStringLiteral("SELECT COUNT(*) FROM TilesDownload WHERE hash = '%1' AND state = 0").arg(UrlFactory::getTileHash(type, 101, 200, 1))), 1);
        QCOMPARE(scalar(db, QStringLiteral("SELECT COUNT(*) FROM TilesDownload WHERE hash = '%1'").arg(legacyKey(type, 100, 200, 1))), 0);
        QCOMPARE(scalar(db, QStringLiteral("SELECT COUNT(*) FROM TilesDownload WHERE hash = '%1'").arg(otherKey)), 1);
        QVERIFY(UrlFactory::tileHashToType(UrlFactory::getTileHash(type, 100, 200, 1)) == type);

        // Running it again changes nothing
        QCOMPARE(QGCCacheWorker::migrateCopernicusTileKeys(db), 0);
        QCOMPARE(scalar(db, QStringLiteral("SELECT COUNT(*) FROM TilesDownload")), 3);
        db.close();
    }
    QSqlDatabase::removeDatabase(kMigrationConnection);
}

void TerrainTileTest::_testMigrationOldFinishedSetBecomesIncomplete()
{
    const QString type = QString::fromLatin1(CopernicusElevationProvider::kProviderKey);
    {
        QSqlDatabase db = openMigrationDb();
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        // Finished offline set 2: 3 tiles, all downloaded under the old key. One old tile also sits in the default set.
        for (int i = 0; i < 3; i++) {
            q.exec(QStringLiteral("INSERT INTO Tiles(tileID, hash, format, size) VALUES(%1, '%2', 'bin', 100)").arg(10 + i).arg(legacyKey(type, 300 + i, 400, 1)));
            q.exec(QStringLiteral("INSERT INTO SetTiles(setID, tileID) VALUES(2, %1)").arg(10 + i));
        }
        q.exec(QStringLiteral("INSERT INTO Tiles(tileID, hash, format, size) VALUES(20, '%1', 'bin', 100)").arg(legacyKey(type, 500, 600, 1)));
        q.exec(QStringLiteral("INSERT INTO SetTiles(setID, tileID) VALUES(1, 20)"));
        // A tile of another provider in set 2 stays
        const QString other = UrlFactory::getProviderTypes().first() == type ? UrlFactory::getProviderTypes().last() : UrlFactory::getProviderTypes().first();
        q.exec(QStringLiteral("INSERT INTO Tiles(tileID, hash, format, size) VALUES(30, '%1', 'png', 100)").arg(legacyKey(other, 1, 2, 3)));
        q.exec(QStringLiteral("INSERT INTO SetTiles(setID, tileID) VALUES(2, 30)"));

        // Before: set 2 holds 3 Copernicus tiles and looks complete for them
        QCOMPARE(scalar(db, QStringLiteral("SELECT COUNT(*) FROM Tiles A INNER JOIN SetTiles B ON A.tileID = B.tileID WHERE B.setID = 2 AND A.type IS NOT 99")), 4);

        QCOMPARE(QGCCacheWorker::migrateCopernicusTileKeys(db), 4);

        // No old Copernicus tile or link is left, so nothing can read a possibly wrong height; other providers are untouched
        QCOMPARE(scalar(db, QStringLiteral("SELECT COUNT(*) FROM Tiles WHERE hash LIKE '%1%'").arg(QString::asprintf("%010d", UrlFactory::legacyHashFromProviderType(type)))), 0);
        QCOMPARE(scalar(db, QStringLiteral("SELECT COUNT(*) FROM Tiles")), 1);
        QCOMPARE(scalar(db, QStringLiteral("SELECT COUNT(*) FROM SetTiles")), 1);
        // The set's counts are true again: 1 saved tile of 3 expected, and 3 fresh downloads are pending under the new key
        QCOMPARE(scalar(db, QStringLiteral("SELECT COUNT(*) FROM Tiles A INNER JOIN SetTiles B ON A.tileID = B.tileID WHERE B.setID = 2")), 1);
        QCOMPARE(scalar(db, QStringLiteral("SELECT COUNT(*) FROM TilesDownload WHERE setID = 2 AND state = 0")), 3);
        QCOMPARE(scalar(db, QStringLiteral("SELECT COUNT(*) FROM TilesDownload WHERE hash = '%1' AND x = 301 AND y = 400 AND z = 1").arg(UrlFactory::getTileHash(type, 301, 400, 1))), 1);
        // The default set (a plain cache) gets no downloads
        QCOMPARE(scalar(db, QStringLiteral("SELECT COUNT(*) FROM TilesDownload WHERE setID = 1")), 0);

        QCOMPARE(QGCCacheWorker::migrateCopernicusTileKeys(db), 0);
        db.close();
    }
    QSqlDatabase::removeDatabase(kMigrationConnection);
}

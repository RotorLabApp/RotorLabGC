/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2, block (c)) - placement and leg colors.
 * See Plan3DTest.h for what is tested and which ground each test uses.
 *
 ****************************************************************************/

#include "Plan3DTest.h"

#ifdef RL_PLAN3D

#include "Plan3DBuilder.h"
#include "Plan3DColors.h"
#include "Plan3DGeometry.h"
#include "Plan3DGround.h"
#include "Plan3DScene.h"
#include "Plan3DMath.h"
#include "Plan3DSource.h"

#include "CameraCalc.h"
#include "Fact.h"
#include "MissionController.h"
#include "QmlObjectListModel.h"
#include "MultiVehicleManager.h"
#include "PlanMasterController.h"
#include "QGCMapPolygon.h"
#include "StructureScanComplexItem.h"
#include "LandingComplexItem.h"
#include "Vehicle.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkProxyFactory>
#include <QtPositioning/QGeoCoordinate>
#include <QtTest/QTest>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Plan3D;

namespace {

#ifndef RL_PLAN_CORPUS_DIR
#error "RL_PLAN_CORPUS_DIR must name test/Custom/plan-corpus"
#endif

constexpr double kTol = 1e-6;
constexpr double kSyntheticTerrainM = 100.0;    // CORPUS.md "Synthetic terrain": constant 100.0 m, NOT real terrain

QString corpusPath(const QString &fileName)
{
    return QDir(QStringLiteral(RL_PLAN_CORPUS_DIR)).filePath(fileName);
}

/// Every request goes to the discard port on loopback: refused locally,
/// nothing leaves the machine, no lookup of the target name.
class DeadLocalProxyFactory : public QNetworkProxyFactory
{
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &) override
    {
        return { QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 9) };
    }
};

GroundFn unknownGround()
{
    return [](double, double) { return kNaN; };
}

/// Test fixture: a constant ground. Not terrain.
GroundFn flatFixture(double amsl)
{
    return [amsl](double, double) { return amsl; };
}

double p7(const QJsonObject &item)
{
    const QJsonValue v = item[QStringLiteral("params")].toArray().at(6);
    return v.isNull() ? kNaN : v.toDouble();
}

double param(const QJsonObject &item, int i)
{
    const QJsonValue v = item[QStringLiteral("params")].toArray().at(i);
    return v.isNull() ? kNaN : v.toDouble();
}

/// The contract's section 2 table, spelled out independently of Plan3DMath.
double expectedAmsl(int frame, double param7, double H, double G)
{
    switch (frame) {
    case 0: case 5:     return param7;          // AMSL; mode 3 also flies params[6]
    case 3: case 6:     return H + param7;      // above launch
    case 10: case 11:   return G + param7;      // above terrain, shown by this product's terrain
    default:            return kNaN;
    }
}

bool isRouteCommand(int c)
{
    return c == 16 || c == 17 || c == 18 || c == 19 || c == 21 || c == 22 || c == 31 || c == 82 || c == 84 || c == 85;
}

QList<QJsonObject> missionItems(const QJsonObject &root)
{
    QList<QJsonObject> out;
    for (const QJsonValue &v : root[QStringLiteral("mission")].toObject()[QStringLiteral("items")].toArray()) {
        out.append(v.toObject());
    }
    return out;
}

QList<QJsonObject> transectItems(const QJsonObject &complex)
{
    QList<QJsonObject> out;
    for (const QJsonValue &v : complex[QStringLiteral("TransectStyleComplexItem")].toObject()[QStringLiteral("Items")].toArray()) {
        out.append(v.toObject());
    }
    return out;
}

const Marker *markerForSeq(const Scene &sc, int seq)
{
    for (const Marker &m : sc.markers) {
        if (m.seq == seq && (m.kind == MarkerKind::Waypoint || m.kind == MarkerKind::PatternPoint)) {
            return &m;
        }
    }
    return nullptr;
}

bool sameOrBothNaN(double a, double b)
{
    return (std::isnan(a) && std::isnan(b)) || std::abs(a - b) < kTol;
}

int countKind(const Scene &sc, LineKind k)
{
    int n = 0;
    for (const LineSeg &l : sc.lines) {
        n += l.kind == k ? 1 : 0;
    }
    return n;
}

int countMarkers(const Scene &sc, MarkerKind k)
{
    int n = 0;
    for (const Marker &m : sc.markers) {
        n += m.kind == k ? 1 : 0;
    }
    return n;
}

QList<int> complexCommands(const SourcePlan &plan)
{
    QList<int> out;
    for (const SourceItem &it : plan.items) {
        if (it.fromComplex) {
            out.append(it.command);
        }
    }
    return out;
}

SourceItem item(int seq, int command, int frame, double lat, double lon, double alt, int mode = 0)
{
    SourceItem s;
    s.seq = seq;
    s.visualIndex = seq;
    s.command = command;
    s.frame = frame;
    for (double &p : s.params) {
        p = 0;
    }
    s.params[3] = kNaN;
    s.params[4] = lat;
    s.params[5] = lon;
    s.params[6] = alt;
    s.altitudeMode = mode;
    return s;
}

/// A fixture plan at the corpus home: home 20 m AMSL, ArduPilot multirotor.
SourcePlan fixturePlan()
{
    SourcePlan p;
    p.homeLat = 37.7679;
    p.homeLon = -122.4934;
    p.homeAmsl = 20;
    p.arduPilot = true;
    p.multiRotor = true;
    return p;
}

/// A point `e` m east and `n` m north of the fixture home.
QPair<double, double> offset(double n, double e)
{
    LocalFrame f{37.7679, -122.4934, 0};
    return {f.latFromNorth(n), f.lonFromEast(e)};
}

/// Largest horizontal distance from a Path sample of `curved` (legs into
/// `visualIndex`) to the drawn line of the same legs in `straight`.
double maxOffStraight(const Scene &curved, const Scene &straight, int visualIndex)
{
    double worstD = 0;
    for (const LineSeg &c : curved.lines) {
        if (c.kind != LineKind::Path || c.visualIndex != visualIndex) {
            continue;
        }
        double nearest = 1e18;
        for (const LineSeg &s : straight.lines) {
            if (s.kind != LineKind::Path || s.visualIndex != visualIndex) {
                continue;
            }
            const double dx = s.b.x - s.a.x, dz = s.b.z - s.a.z;
            const double len2 = dx * dx + dz * dz;
            const double t = len2 > 0 ? std::clamp(((c.b.x - s.a.x) * dx + (c.b.z - s.a.z) * dz) / len2, 0.0, 1.0) : 0.0;
            nearest = std::min(nearest, std::hypot(c.b.x - (s.a.x + t * dx), c.b.z - (s.a.z + t * dz)));
        }
        worstD = std::max(worstD, nearest);
    }
    return worstD;
}

} // namespace

// ---------------------------------------------------------------------------

void Plan3DTest::initTestCase()
{
    _proxyFactory = new DeadLocalProxyFactory;
    QNetworkProxyFactory::setApplicationProxyFactory(_proxyFactory);
}

void Plan3DTest::cleanupTestCase()
{
    QNetworkProxyFactory::setApplicationProxyFactory(nullptr);   // deletes ours
    _proxyFactory = nullptr;
}

void Plan3DTest::init()
{
    UnitTest::init();
    _pmc = new PlanMasterController(this);
    _pmc->setFlyView(false);
    _pmc->start();
}

void Plan3DTest::cleanup()
{
    if (_pmc) {
        _pmc->setDirty(false);
    }
    delete _pmc;
    _pmc = nullptr;
    UnitTest::cleanup();
}

bool Plan3DTest::_load(const QString &fileName, SourcePlan &plan, QJsonObject &json)
{
    QFile f(corpusPath(fileName));
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "cannot open" << f.fileName();
        return false;
    }
    json = QJsonDocument::fromJson(f.readAll()).object();
    _pmc->loadFromFile(f.fileName());
    // Read at once: GC's terrain replies (refused offline) need the event
    // loop, so none can change params[6] or the home altitude before this.
    plan = sourceFromPlan(_pmc);
    return !plan.items.isEmpty();
}

bool Plan3DTest::_loadStructure(const QString &fileName, SourcePlan &plan, QJsonObject &json)
{
    QFile f(corpusPath(fileName));
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    json = QJsonDocument::fromJson(f.readAll()).object();

    // The file without its structure scan item, loaded through GC's own path.
    QJsonObject root = json;
    QJsonObject mission = root[QStringLiteral("mission")].toObject();
    QJsonArray rest;
    QJsonObject scanJson;
    int scanIndex = -1;
    const QJsonArray items = mission[QStringLiteral("items")].toArray();
    for (int i = 0; i < items.size(); i++) {
        const QJsonObject o = items[i].toObject();
        if (o[QStringLiteral("complexItemType")].toString() == QStringLiteral("StructureScan")) {
            scanJson = o;
            scanIndex = i;
        } else {
            rest.append(o);
        }
    }
    if (scanIndex < 0) {
        return false;
    }
    mission[QStringLiteral("items")] = rest;
    root[QStringLiteral("mission")] = mission;
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("structure-without-scan.plan"));
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        return false;
    }
    out.write(QJsonDocument(root).toJson());
    out.close();
    _pmc->loadFromFile(path);

    // The scan, rebuilt from the file's values through GC's API (one
    // appendVertices call, as the corpus harness and QGCMapPolygonVisuals do).
    MissionController *mc = _pmc->missionController();
    QList<QGeoCoordinate> vertices;
    for (const QJsonValue &v : scanJson[QStringLiteral("polygon")].toArray()) {
        vertices.append(QGeoCoordinate(v.toArray()[0].toDouble(), v.toArray()[1].toDouble()));
    }
    auto *scan = qobject_cast<StructureScanComplexItem *>(
        mc->insertComplexMissionItem(mc->structureScanComplexItemName(), vertices.first(), scanIndex + 1, true));
    if (!scan) {
        return false;
    }
    scan->structurePolygon()->appendVertices(vertices);
    QString error;
    if (!scan->cameraCalc()->load(scanJson[QStringLiteral("CameraCalc")].toObject(), false, error, false)) {
        qWarning() << "CameraCalc load" << error;
        return false;
    }
    scan->structureHeight()->setRawValue(scanJson[QStringLiteral("StructureHeight")].toDouble());
    scan->scanBottomAlt()->setRawValue(scanJson[QStringLiteral("ScanBottomAlt")].toDouble());
    scan->entranceAlt()->setRawValue(scanJson[QStringLiteral("EntranceAltitude")].toDouble());
    scan->startFromTop()->setRawValue(scanJson[QStringLiteral("StartFromTop")].toBool());
    scan->setWizardMode(false);
    QTest::qWait(50);

    // The rebuilt scan must save as the file holds it.
    QJsonArray saved;
    scan->save(saved);
    if (saved.size() != 1) {
        return false;
    }
    const QJsonObject s = saved[0].toObject();
    for (const char *key : {"EntranceAltitude", "ScanBottomAlt", "StructureHeight", "Layers", "StartFromTop", "GimbalPitch", "polygon", "CameraCalc", "version"}) {
        if (s[QLatin1String(key)] != scanJson[QLatin1String(key)]) {
            qWarning() << "structure rebuild differs from the file at" << key << s[QLatin1String(key)] << scanJson[QLatin1String(key)];
            return false;
        }
    }
    plan = sourceFromPlan(_pmc);
    return !plan.items.isEmpty();
}

void Plan3DTest::_commonCorpusChecks(const QString &fileName, const SourcePlan &plan, const QJsonObject &json,
                                     double groundM, bool syntheticTerrain)
{
    const QJsonObject mission = json[QStringLiteral("mission")].toObject();
    const QJsonArray homeArr = mission[QStringLiteral("plannedHomePosition")].toArray();
    const double H = homeArr[2].toDouble();
    QVERIFY2(std::abs(plan.homeAmsl - H) < kTol, qPrintable(fileName));
    if (syntheticTerrain) {
        QCOMPARE(H, kSyntheticTerrainM);    // the synthetic files took their launch altitude from the 100 m ground
    }
    QCOMPARE(plan.arduPilot, mission[QStringLiteral("firmwareType")].toInt() == 3);

    // ---- Section 2: every position item in the file is placed by its frame
    // and params[6] (A-1), on unknown ground and on the given ground.
    for (const bool known : {false, true}) {
        const double G = known ? groundM : kNaN;
        const Scene sc = Builder::build(plan, known ? flatFixture(groundM) : unknownGround());
        int checked = 0;
        auto checkItem = [&](const QJsonObject &it, MarkerKind kind) {
            const int command = it[QStringLiteral("command")].toInt();
            if (!isRouteCommand(command)) {
                return;
            }
            const int seq = it[QStringLiteral("doJumpId")].toInt();
            const Marker *m = markerForSeq(sc, seq);
            QVERIFY2(m, qPrintable(QStringLiteral("%1: no marker for seq %2").arg(fileName).arg(seq)));
            QCOMPARE(m->kind, kind);
            QCOMPARE(m->command, command);
            const double expected = expectedAmsl(it[QStringLiteral("frame")].toInt(), p7(it), H, G);
            QVERIFY2(sameOrBothNaN(m->amsl, expected),
                     qPrintable(QStringLiteral("%1 seq %2: amsl %3, expected %4 (frame %5, params[6] %6, Altitude %7)")
                                    .arg(fileName).arg(seq).arg(m->amsl).arg(expected).arg(it[QStringLiteral("frame")].toInt())
                                    .arg(p7(it)).arg(it[QStringLiteral("Altitude")].toDouble())));
            // A-11 and the other positions: drawn where the file puts them.
            const double lat = param(it, 4), lon = param(it, 5);
            const bool noPosition = lat == 0.0 && lon == 0.0;
            const Vec3 at = noPosition ? sc.frame.toScene(plan.homeLat, plan.homeLon, 0) : sc.frame.toScene(lat, lon, 0);
            QVERIFY(std::abs(m->pos.x - at.x) < 1e-3 && std::abs(m->pos.z - at.z) < 1e-3);
            // CL-4: unknown ground means a violet point and a dashed stem.
            // CL-1a: a touchdown in its contact span is ground contact, never red.
            const Clearance want = !known ? Clearance::Unknown
                                 : Builder::isLand(command) ? classifyContact(expected, G) : classify(expected, G);
            QCOMPARE(m->clearance, want);
            checked++;
        };
        for (const QJsonObject &it : missionItems(json)) {
            if (it[QStringLiteral("type")].toString() == QStringLiteral("SimpleItem")) {
                checkItem(it, MarkerKind::Waypoint);
            } else {
                for (const QJsonObject &t : transectItems(it)) {
                    checkItem(t, MarkerKind::PatternPoint);
                }
            }
        }
        QVERIFY2(checked > 0, qPrintable(fileName));

        // ---- Section 4 on unknown ground: nothing is red, nothing is "no strike" (CL-2).
        if (!known) {
            QCOMPARE(sc.stats.samplesStrike, 0);
            QCOMPARE(sc.stats.samplesNoStrike, 0);
            QVERIFY(sc.stats.samplesUnknown > 0);
            QCOMPARE(countKind(sc, LineKind::Stem), 0);
            QVERIFY(countKind(sc, LineKind::StemUnknown) > 0);
            for (const LineSeg &l : sc.lines) {
                if (l.kind == LineKind::Path || l.kind == LineKind::Unknown) {
                    QCOMPARE(l.clearance, Clearance::Unknown);
                }
            }
            continue;
        }

        // ---- Section 4 on known flat ground: every drawn route segment is
        // classed by the heights above ground of its two samples (CL-1, CL-3).
        // Under M it is red, or ground contact when CL-1a puts it in a
        // takeoff or landing contact span (never below -0.5 m).
        int contactSegs = 0;
        for (const LineSeg &l : sc.lines) {
            if (l.kind != LineKind::Path) {
                continue;
            }
            const double hagA = l.a.y + sc.frame.alt0 - groundM;
            const double hagB = l.b.y + sc.frame.alt0 - groundM;
            const double lo = std::min(hagA, hagB);
            const bool ok = lo >= kStrikeMarginM ? l.clearance == Clearance::NoStrike
                          : (l.clearance == Clearance::Strike
                             || (l.clearance == Clearance::GroundContact && lo >= -kContactBelowGroundM - kTol));
            QVERIFY2(ok, qPrintable(QStringLiteral("%1: segment HAG %2 / %3 classed %4").arg(fileName).arg(hagA).arg(hagB).arg(int(l.clearance))));
            contactSegs += l.clearance == Clearance::GroundContact ? 1 : 0;
        }
        QVERIFY(sc.stats.samplesNoStrike > 0);
        if (std::abs(groundM - H) < kTol) {
            // Question 1 (rev 5, CL-1a): on ground at launch level the
            // takeoffs and landings meet the ground by design. None of it is
            // red, and none of it is counted as a strike.
            QVERIFY2(sc.stats.samplesStrike == 0, qPrintable(QStringLiteral("%1: %2 red samples").arg(fileName).arg(sc.stats.samplesStrike)));
            QVERIFY2(sc.stats.samplesGroundContact > 0 && contactSegs > 0, qPrintable(fileName));
            // Route markers only: a rally point at 0 m above launch is red by
            // A-9 and is not a takeoff or a landing.
            for (const Marker &mk : sc.markers) {
                if (mk.kind != MarkerKind::Waypoint && mk.kind != MarkerKind::PatternPoint) {
                    continue;
                }
                QVERIFY2(mk.clearance != Clearance::Strike, qPrintable(QStringLiteral("%1: red marker %2").arg(fileName).arg(mk.label)));
            }
        }

        // ---- A-10: RTL legs are violet and end at the nearest of the rally
        // points and home (above launch), from the last position before RTL.
        bool hasRtl = false;
        double lastLat = plan.homeLat, lastLon = plan.homeLon;
        for (const QJsonObject &it : missionItems(json)) {
            const int c = it[QStringLiteral("command")].toInt();
            hasRtl |= c == 20;
            if (!hasRtl && isRouteCommand(c) && !(param(it, 4) == 0.0 && param(it, 5) == 0.0)) {
                lastLat = param(it, 4);
                lastLon = param(it, 5);
            }
        }
        QCOMPARE(sc.hasUnknownRtl, hasRtl);
        if (hasRtl) {
            QCOMPARE(countKind(sc, LineKind::Unknown), 2);
            const LineSeg *lastUnknown = nullptr;
            for (const LineSeg &l : sc.lines) {
                if (l.kind == LineKind::Unknown) {
                    QCOMPARE(l.clearance, Clearance::Unknown);
                    lastUnknown = &l;
                }
            }
            QVERIFY(lastUnknown);
            const LocalFrame &F = sc.frame;
            double destLat = plan.homeLat, destLon = plan.homeLon, destAmsl = H;
            double best = std::hypot(F.east(destLon) - F.east(lastLon), F.north(destLat) - F.north(lastLat));
            for (const QJsonValue &v : json[QStringLiteral("rallyPoints")].toObject()[QStringLiteral("points")].toArray()) {
                const QJsonArray r = v.toArray();
                const double d = std::hypot(F.east(r[1].toDouble()) - F.east(lastLon), F.north(r[0].toDouble()) - F.north(lastLat));
                if (d < best) {
                    best = d;
                    destLat = r[0].toDouble();
                    destLon = r[1].toDouble();
                    destAmsl = H + r[2].toDouble();
                }
            }
            const Vec3 dest = F.toScene(destLat, destLon, destAmsl);
            QVERIFY2(std::abs(lastUnknown->b.x - dest.x) < 1e-3 && std::abs(lastUnknown->b.z - dest.z) < 1e-3,
                     qPrintable(fileName + QStringLiteral(": RTL does not end at the nearest rally point or home")));
            QVERIFY(std::abs(lastUnknown->b.y - dest.y) < kTol);
        } else {
            QCOMPARE(countKind(sc, LineKind::Unknown), 0);
        }

        // ---- CL-7: every corpus navigation command is drawn (items of 100
        // and above are skipped by design and counted in the legend).
        for (const QString &w : sc.warnings) {
            QVERIFY2(!w.startsWith(QStringLiteral("Commands not drawn")), qPrintable(fileName + QStringLiteral(": ") + w));
            QVERIFY2(!w.contains(QStringLiteral("carries no position height")), qPrintable(fileName + QStringLiteral(": ") + w));
        }
        QCOMPARE(sc.warnings.filter(QStringLiteral("DO_JUMP item")).size(), 0);   // none in the corpus
    }
}

// ---------------------------------------------------------------------------
// Section 2 and section 4 rules on their own.

void Plan3DTest::_placeHeightByFrame()
{
    const double H = 20, G = 35, A = 40;
    // AMSL frames: params[6] as is, ground not needed.
    for (int frame : {0, 5}) {
        const Height h = placeHeight(frame, A, H, kNaN);
        QCOMPARE(h.datum, Datum::Amsl);
        QCOMPARE(h.amsl, A);
    }
    // Relative frames: H + params[6]; unknown without H (A-4).
    for (int frame : {3, 6}) {
        QCOMPARE(placeHeight(frame, A, H, kNaN).amsl, H + A);
        QVERIFY(!placeHeight(frame, A, kNaN, G).known());
    }
    // Terrain frames: G(p) + params[6] by this product's terrain; unknown without G (A-3, A-6).
    for (int frame : {10, 11}) {
        const Height h = placeHeight(frame, A, H, G);
        QCOMPARE(h.datum, Datum::Terrain);
        QCOMPARE(h.amsl, G + A);
        QCOMPARE(h.terrainHag, A);
        QVERIFY(!placeHeight(frame, A, H, kNaN).known());
    }
    // Mode 3 before terrain: params[6] null is unknown (A-5).
    QVERIFY(!placeHeight(0, kNaN, H, G).known());
    // Frames without a position height.
    for (int frame : {1, 2, 4, 7, 8, 9}) {
        QCOMPARE(placeHeight(frame, A, H, G).datum, Datum::None);
    }
}

void Plan3DTest::_modeAgreesWithFrame()
{
    struct Row { int mode; int frame; bool agrees; };
    const Row rows[] = {
        {1, 3, true}, {1, 6, true}, {1, 0, false}, {1, 10, false},
        {2, 0, true}, {2, 5, true}, {2, 3, false}, {2, 10, false},
        {3, 0, true}, {3, 5, true}, {3, 3, false}, {3, 10, false},
        {4, 10, true}, {4, 11, true}, {4, 0, false}, {4, 3, false},
        {0, 3, false}, {5, 0, false},
    };
    for (const Row &r : rows) {
        QVERIFY2(modeAgreesWithFrame(r.mode, r.frame) == r.agrees, qPrintable(QStringLiteral("mode %1 frame %2").arg(r.mode).arg(r.frame)));
    }
}

void Plan3DTest::_defaultModeWithoutTriple()
{
    // A-1: only frame 3 maps to mode 1; every other frame, 10 included, to mode 2.
    // Only frame 3: GC's MissionItem::relativeAltitude() is frame 3 only (MissionItem.h:98).
    QCOMPARE(defaultAltitudeModeForFrame(3), 1);
    for (int frame : {0, 5, 6, 10, 11, 2}) {
        QCOMPARE(defaultAltitudeModeForFrame(frame), 2);
    }
}

void Plan3DTest::_classifyThresholds()
{
    QCOMPARE(kStrikeMarginM, 2.0);
    QVERIFY(kSamplesPerLeg >= 24);
    QCOMPARE(classify(102.0, 100.0), Clearance::NoStrike);     // exactly M: no strike
    QCOMPARE(classify(101.999, 100.0), Clearance::Strike);
    QCOMPARE(classify(99.0, 100.0), Clearance::Strike);        // below ground is always red
    QCOMPARE(classify(kNaN, 100.0), Clearance::Unknown);       // height unknown
    QCOMPARE(classify(150.0, kNaN), Clearance::Unknown);       // ground unknown: never "no strike"
}

void Plan3DTest::_groundContactRule()
{
    // CL-1a (rev 5): inside a contact span, HAG from -0.5 m to under M is
    // ground contact; below -0.5 m is red; M and above is no strike.
    QCOMPARE(kContactBelowGroundM, 0.5);
    QCOMPARE(classifyLegSample(100.5, 100.0, true), Clearance::GroundContact);
    QCOMPARE(classifyLegSample(100.0, 100.0, true), Clearance::GroundContact);
    QCOMPARE(classifyLegSample(101.999, 100.0, true), Clearance::GroundContact);
    QCOMPARE(classifyLegSample(102.0, 100.0, true), Clearance::NoStrike);
    QCOMPARE(classifyLegSample(99.5, 100.0, true), Clearance::GroundContact);      // exactly -0.5 m
    QCOMPARE(classifyLegSample(99.499, 100.0, true), Clearance::Strike);           // just past -0.5 m
    QCOMPARE(classifyLegSample(90.0, 100.0, true), Clearance::Strike);
    QCOMPARE(classifyLegSample(kNaN, 100.0, true), Clearance::Unknown);
    QCOMPARE(classifyLegSample(100.5, kNaN, true), Clearance::Unknown);
    // Outside a span every sample keeps the strike rule.
    QCOMPARE(classifyLegSample(100.5, 100.0, false), Clearance::Strike);
    QCOMPARE(classifyLegSample(99.5, 100.0, false), Clearance::Strike);

    // The span itself. Takeoff: from the start up to the first sample at M.
    const QList<double> climb{0, 0.5, 1.5, 2.0, 5, 1.0, 10};
    QCOMPARE(contactSpan(climb, true, false), (QList<bool>{true, true, true, false, false, false, false}));
    // Landing: after the last sample at M or more, up to the touchdown.
    const QList<double> descent{30, 1.0, 20, 3, 1.9, 0.2, -0.4};
    QCOMPARE(contactSpan(descent, false, true), (QList<bool>{false, false, false, false, true, true, true}));
    // An unknown sample ends a span like a sample at M.
    QCOMPARE(contactSpan(QList<double>{0, kNaN, 1}, true, false), (QList<bool>{true, false, false}));
    QCOMPARE(contactSpan(QList<double>{1, kNaN, 1}, false, true), (QList<bool>{false, false, true}));
    // Not a takeoff or landing leg: no span at all.
    QCOMPARE(contactSpan(QList<double>{0, 1, 0}, false, false), (QList<bool>{false, false, false}));
}

void Plan3DTest::_colors()
{
    QCOMPARE(strikeColor(), QColor(QStringLiteral("#ff5544")));
    QCOMPARE(noStrikeColor(), QColor(QStringLiteral("#2dd4bf")));
    QCOMPARE(unknownColor(true), QColor(QStringLiteral("#b07cff")));
    QCOMPARE(unknownColor(false), QColor(QStringLiteral("#7c3aed")));
    QCOMPARE(clearanceColor(Clearance::Strike, true), strikeColor());
    QCOMPARE(clearanceColor(Clearance::NoStrike, false), noStrikeColor());
    QCOMPARE(clearanceColor(Clearance::Unknown, true), unknownColor(true));
    // CL-1a: neutral slate, never red.
    QCOMPARE(groundContactColor(true), QColor(QStringLiteral("#94a3b8")));
    QCOMPARE(groundContactColor(false), QColor(QStringLiteral("#64748b")));
    QCOMPARE(clearanceColor(Clearance::GroundContact, true), groundContactColor(true));
    QCOMPARE(clearanceColor(Clearance::GroundContact, false), groundContactColor(false));
}

void Plan3DTest::_splineMath()
{
    const Vec3 pts[] = {{0, 0, 0}, {100, 0, 0}, {100, 0, 100}, {0, 0, 100}};
    // Passes through its points.
    for (int leg = 0; leg < 3; leg++) {
        const Vec3 a = catmullRomLegPoint(pts, 4, leg, 0.0);
        const Vec3 b = catmullRomLegPoint(pts, 4, leg, 1.0);
        QVERIFY(std::abs(a.x - pts[leg].x) < kTol && std::abs(a.z - pts[leg].z) < kTol);
        QVERIFY(std::abs(b.x - pts[leg + 1].x) < kTol && std::abs(b.z - pts[leg + 1].z) < kTol);
    }
    // Curved: the middle leg bows outward.
    const Vec3 mid = catmullRomLegPoint(pts, 4, 1, 0.5);
    QVERIFY(mid.x > 100.5);
    // S-7: fewer than three points is straight.
    const Vec3 two = catmullRomLegPoint(pts, 2, 0, 0.5);
    QVERIFY(std::abs(two.x - 50) < kTol && std::abs(two.z) < kTol);
}

// ---------------------------------------------------------------------------
// Fixture plans.

void Plan3DTest::_fixtureModeFrameDisagreeDrawnByFrame()
{
    // Every frame against every mode: the height is always by frame and
    // params[6]; a disagreeing mode only adds a warning (A-1).
    const auto p = offset(100, 0);
    for (int frame : {0, 3, 10}) {
        for (int mode : {1, 2, 3, 4}) {
            SourcePlan plan = fixturePlan();
            plan.items << item(1, 16, frame, p.first, p.second, 40, mode);
            const Scene sc = Builder::build(plan, flatFixture(25));
            const Marker *m = markerForSeq(sc, 1);
            QVERIFY(m);
            QCOMPARE(m->amsl, expectedAmsl(frame, 40, 25, 25));   // relative from R = G(home) = 25 (A-0)
            const bool warned = std::any_of(sc.warnings.cbegin(), sc.warnings.cend(),
                                            [](const QString &w) { return w.startsWith(QStringLiteral("Item 1")); });
            QVERIFY2(warned == !modeAgreesWithFrame(mode, frame), qPrintable(QStringLiteral("frame %1 mode %2").arg(frame).arg(mode)));
            if (frame == 10 && mode == 2) {
                QVERIFY(sc.warnings.join(QString()).contains(QStringLiteral("labeled AMSL in GC")));
            }
        }
    }
}

void Plan3DTest::_fixtureNoHomeAltitudeUnknown()
{
    // A-4 (rev 3): without H and without ground at home every relative item
    // is unknown; AMSL items stay known.
    SourcePlan plan = fixturePlan();
    plan.homeAmsl = kNaN;
    const auto a = offset(100, 0), b = offset(200, 0);
    plan.items << item(1, 16, 3, a.first, a.second, 40, 1) << item(2, 16, 0, b.first, b.second, 80, 2);
    const LocalFrame f{37.7679, -122.4934, 0};
    const GroundFn holeAtHome = [f](double lat, double lon) { return std::hypot(f.east(lon), f.north(lat)) < 20 ? kNaN : 20.0; };
    const Scene sc = Builder::build(plan, holeAtHome);
    QVERIFY(!std::isfinite(markerForSeq(sc, 1)->amsl));
    QCOMPARE(markerForSeq(sc, 1)->clearance, Clearance::Unknown);
    QCOMPARE(markerForSeq(sc, 2)->amsl, 80.0);
    QVERIFY(sc.warnings.join(QString()).contains(QStringLiteral("heights above launch are unknown")));
    // The legs touching the unknown point are unknown, never "no strike".
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Path && l.visualIndex == 1) {
            QCOMPARE(l.clearance, Clearance::Unknown);
        }
    }
}

void Plan3DTest::_fixtureCalcAboveTerrainWithoutAmslUnknown()
{
    // A-5: a mode 3 item with params[6] null is unknown.
    SourcePlan plan = fixturePlan();
    const auto a = offset(100, 0);
    plan.items << item(1, 16, 0, a.first, a.second, kNaN, 3);
    const Scene sc = Builder::build(plan, flatFixture(20));
    QVERIFY(!std::isfinite(markerForSeq(sc, 1)->amsl));
    QVERIFY(sc.warnings.join(QString()).contains(QStringLiteral("has no height yet")));
    QCOMPARE(sc.stats.samplesNoStrike, 0);
}

void Plan3DTest::_fixtureLaunchReferenceFromGround()
{
    // Rev 3, A-0: relative heights from R = G(home) when the ground at home
    // is loaded, else H; A-4: unknown only when both are missing; A-9 and
    // A-11 use R too.
    SourcePlan plan = fixturePlan();            // H = 20
    const auto a = offset(100, 0), r = offset(-100, 0);
    plan.items << item(1, 22, 3, 0, 0, 30, 1) << item(2, 16, 3, a.first, a.second, 40, 1);
    plan.rally << SourcePoint{r.first, r.second, 15};

    // Ground at home loaded (41 m): R = 41.
    Scene sc = Builder::build(plan, flatFixture(41));
    QVERIFY(sc.launchRefFromGround);
    QCOMPARE(sc.launchRef, 41.0);
    QCOMPARE(markerForSeq(sc, 2)->amsl, 81.0);          // R + 40, not H + 40
    QCOMPARE(markerForSeq(sc, 1)->amsl, 71.0);          // A-11: takeoff to R + A
    for (const Marker &m : sc.markers) {
        if (m.kind == MarkerKind::Rally) {
            QCOMPARE(m.amsl, 56.0);                     // A-9: R + 15
        }
    }
    QCOMPARE(markerForSeq(sc, 2)->clearance, Clearance::NoStrike);

    // Ground at home not loaded, known elsewhere: R = H.
    const LocalFrame f{37.7679, -122.4934, 0};
    const GroundFn holeAtHome = [f](double lat, double lon) { return std::hypot(f.east(lon), f.north(lat)) < 20 ? kNaN : 41.0; };
    sc = Builder::build(plan, holeAtHome);
    QVERIFY(!sc.launchRefFromGround);
    QCOMPARE(sc.launchRef, 20.0);
    QCOMPARE(markerForSeq(sc, 2)->amsl, 60.0);
    QVERIFY(!sc.homeDatumNote);                         // A-0b needs both

    // Both missing: every relative height unknown (A-4).
    plan.homeAmsl = kNaN;
    sc = Builder::build(plan, holeAtHome);
    QVERIFY(!std::isfinite(sc.launchRef));
    QVERIFY(!std::isfinite(markerForSeq(sc, 2)->amsl));
    QVERIFY(sc.warnings.join(QString()).contains(QStringLiteral("heights above launch are unknown")));

    // H missing, ground at home loaded: R = G(home), no A-4.
    sc = Builder::build(plan, flatFixture(41));
    QCOMPARE(markerForSeq(sc, 2)->amsl, 81.0);
    QVERIFY(!sc.warnings.join(QString()).contains(QStringLiteral("heights above launch are unknown")));
}

void Plan3DTest::_fixtureHomeDatumNoteThreshold()
{
    // Rev 3, A-0b: the note shows when |H - G(home)| > 3 m, never at 3 m,
    // and is a note only: the colors are the same either way.
    SourcePlan plan = fixturePlan();            // H = 20
    const auto a = offset(100, 0);
    plan.items << item(1, 16, 3, a.first, a.second, 40, 1);
    struct Row { double ground; bool note; };
    for (const Row &r : {Row{20.0, false}, Row{23.0, false}, Row{17.0, false}, Row{23.01, true}, Row{16.99, true}, Row{41.0, true}}) {
        const Scene sc = Builder::build(plan, flatFixture(r.ground));
        QVERIFY2(sc.homeDatumNote == r.note, qPrintable(QStringLiteral("ground %1").arg(r.ground)));
        QCOMPARE(sc.warnings.filter(QStringLiteral("may be stale")).size(), r.note ? 1 : 0);
    }
    const Scene sc = Builder::build(plan, flatFixture(41));
    const QString note = sc.warnings.filter(QStringLiteral("may be stale")).value(0);
    QCOMPARE(note, QStringLiteral("The plan's home altitude (20.0 m) differs from the ground at home (41.0 m) by 21.0 m and may be stale; heights above launch are drawn from the ground at home."));
}

void Plan3DTest::_fixtureSampleSpacingLongFixedWingLeg()
{
    // Rev 4, CL-1: a 5 km fixed-wing leg is sampled so that no two
    // neighbors are more than S apart along the ground, both ends included,
    // and a 16 m wide fixture ridge is found (24 samples would fall at
    // 2996 m and 3204 m and miss it).
    SourcePlan plan = fixturePlan();
    plan.multiRotor = false;
    plan.fixedWing = true;
    const auto a = offset(0, 500), b = offset(0, 5500);
    plan.items << item(1, 16, 3, a.first, a.second, 60, 1) << item(2, 16, 3, b.first, b.second, 60, 1);
    for (const double cell : {kNaN, 30.0, 20.0, 50.0}) {
        const double S = sampleSpacingM(cell);
        QCOMPARE(S, std::isfinite(cell) ? std::min(15.0, cell / 2) : 15.0);
        const Scene sc = Builder::build(plan, flatFixture(20), cell);
        QCOMPARE(sc.sampleSpacingM, S);
        QList<const LineSeg *> leg;
        for (const LineSeg &l : sc.lines) {
            if (l.kind == LineKind::Path && l.visualIndex == 2) {
                leg.append(&l);
            }
        }
        QVERIFY(leg.size() >= int(std::ceil(5000.0 / S)));
        double widest = 0;
        for (const LineSeg *l : leg) {
            widest = std::max(widest, std::hypot(l->b.x - l->a.x, l->b.z - l->a.z));
        }
        QVERIFY2(widest <= S + 1e-6, qPrintable(QStringLiteral("cell %1: widest gap %2, S %3").arg(cell).arg(widest).arg(S)));
        QVERIFY(std::abs(leg.first()->a.x - 500) < 1e-6 && std::abs(leg.last()->b.x - 5500) < 1e-6);   // both ends
    }

    const LocalFrame f{37.7679, -122.4934, 0};
    const GroundFn ridge = [f](double, double lon) { return std::abs(f.east(lon) - 3100) < 8 ? 79.0 : 20.0; };
    const Scene sc = Builder::build(plan, ridge, 30.0);
    int strike = 0;
    for (const LineSeg &l : sc.lines) {
        strike += (l.kind == LineKind::Path && l.visualIndex == 2 && l.clearance == Clearance::Strike) ? 1 : 0;
    }
    QVERIFY(strike > 0);
}

void Plan3DTest::_wp300BuildTime()
{
    // Rev 4 sampling must keep the rebuild cheap on the 300-waypoint plan.
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(QStringLiteral("wp300-px4-copter-lawnmower.plan"), plan, json));
    QElapsedTimer t;
    t.start();
    int segs = 0;
    const int runs = 10;
    for (int i = 0; i < runs; i++) {
        segs = int(Builder::build(plan, flatFixture(20), 30.0).lines.size());
    }
    const double ms = double(t.nsecsElapsed()) / 1e6 / runs;
    qInfo() << "wp300 Builder::build (this build type):" << ms << "ms," << segs << "segments";
    QVERIFY(ms < 500);
}

void Plan3DTest::_fixtureSamplesPerLeg()
{
    // Section 4: kSamplesPerLeg samples on every straight leg.
    SourcePlan plan = fixturePlan();
    const auto a = offset(100, 0), b = offset(100, 100);
    plan.items << item(1, 16, 3, a.first, a.second, 40, 1) << item(2, 16, 3, b.first, b.second, 40, 1);
    const Scene sc = Builder::build(plan, flatFixture(20));
    QCOMPARE(countKind(sc, LineKind::Path), 2 * kSamplesPerLeg);
    QCOMPARE(sc.stats.samplesStrike + sc.stats.samplesNoStrike + sc.stats.samplesUnknown + sc.stats.samplesGroundContact, 2 * kSamplesPerLeg + 1);
}

void Plan3DTest::_fixtureRidgeStrikeMidLeg()
{
    // CL-1, CL-3: a fixture ridge 70 m AMSL under the middle of a leg flown
    // at 60 m AMSL is a strike in the middle only; both ends stay clear.
    SourcePlan plan = fixturePlan();
    const auto a = offset(100, -200), b = offset(100, 200);
    plan.items << item(1, 16, 3, a.first, a.second, 40, 1) << item(2, 16, 3, b.first, b.second, 40, 1);
    const LocalFrame f{37.7679, -122.4934, 0};
    // The ridge stays clear of home, so R = G(home) = 20 (A-0).
    const GroundFn ridge = [f](double lat, double lon) { return (std::abs(f.east(lon)) < 30 && f.north(lat) > 50) ? 70.0 : 20.0; };
    const Scene sc = Builder::build(plan, ridge);
    int strikeOnLeg = 0, clearOnLeg = 0;
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Path && l.visualIndex == 2) {
            if (l.clearance == Clearance::Strike) {
                strikeOnLeg++;
                QVERIFY(std::abs(l.a.x) < 50 || std::abs(l.b.x) < 50);
            } else {
                clearOnLeg++;
            }
        }
    }
    QVERIFY(strikeOnLeg > 0);
    QVERIFY(clearOnLeg > 0);
    QCOMPARE(markerForSeq(sc, 1)->clearance, Clearance::NoStrike);
    QCOMPARE(markerForSeq(sc, 2)->clearance, Clearance::NoStrike);
}

void Plan3DTest::_cl1aCopterTakeoffAndLanding()
{
    // CL-1a: a copter that takes off at launch (22, no position: A-11) and
    // lands (21) on known flat ground at launch level. Both ends meet the
    // ground by design: ground contact, never red, never counted.
    SourcePlan plan = fixturePlan();
    const auto a = offset(200, 0);
    plan.items << item(1, 22, 3, 0, 0, 30, 1) << item(2, 16, 3, a.first, a.second, 30, 1)
               << item(3, 21, 3, a.first, a.second, 0, 1);
    const Scene sc = Builder::build(plan, flatFixture(20));
    QCOMPARE(sc.stats.samplesStrike, 0);
    QVERIFY(sc.stats.samplesGroundContact > 0);
    QCOMPARE(markerForSeq(sc, 3)->clearance, Clearance::GroundContact);    // touchdown
    QCOMPARE(markerForSeq(sc, 1)->clearance, Clearance::NoStrike);         // top of the climb
    int contactUp = 0, contactDown = 0;
    for (const LineSeg &l : sc.lines) {
        if (l.kind != LineKind::Path) {
            continue;
        }
        QVERIFY(l.clearance != Clearance::Strike);
        const double lo = std::min(l.a.y, l.b.y) + sc.frame.alt0 - 20;
        if (l.clearance == Clearance::GroundContact) {
            QVERIFY(lo < kStrikeMarginM && lo >= -kContactBelowGroundM);
            contactUp += l.visualIndex == 1 ? 1 : 0;
            contactDown += l.visualIndex == 3 ? 1 : 0;
        } else {
            QVERIFY(lo >= kStrikeMarginM);
        }
    }
    QVERIFY(contactUp > 0);
    QVERIFY(contactDown > 0);
    // The cruise leg never touches the span.
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Path && l.visualIndex == 2) {
            QCOMPARE(l.clearance, Clearance::NoStrike);
        }
    }
}

void Plan3DTest::_cl1aBelowGroundEdge()
{
    // CL-1a: a touchdown down to 0.5 m below the loaded ground is ground
    // contact; past that it is red (CL-1a).
    const auto a = offset(200, 0);
    struct Row { double touchdownAmsl; Clearance want; };
    for (const Row &r : {Row{20.0, Clearance::GroundContact}, Row{19.5, Clearance::GroundContact},
                         Row{19.49, Clearance::Strike}, Row{18.0, Clearance::Strike}, Row{21.99, Clearance::GroundContact}}) {
        SourcePlan plan = fixturePlan();
        plan.items << item(1, 22, 3, 0, 0, 30, 1) << item(2, 16, 3, a.first, a.second, 30, 1)
                   << item(3, 21, 0, a.first, a.second, r.touchdownAmsl, 2);
        const Scene sc = Builder::build(plan, flatFixture(20));
        QVERIFY2(markerForSeq(sc, 3)->clearance == r.want, qPrintable(QStringLiteral("touchdown %1").arg(r.touchdownAmsl)));
        const LineSeg *last = nullptr;
        for (const LineSeg &l : sc.lines) {
            if (l.kind == LineKind::Path && l.visualIndex == 3) {
                last = &l;
            }
        }
        QVERIFY(last);
        QVERIFY2(last->clearance == r.want, qPrintable(QStringLiteral("touchdown %1: last segment %2").arg(r.touchdownAmsl).arg(int(last->clearance))));
        if (r.want == Clearance::Strike) {
            QVERIFY(sc.stats.samplesStrike > 0);
        } else {
            QCOMPARE(sc.stats.samplesStrike, 0);
        }
    }
}

void Plan3DTest::_cl1aMidLegDipStaysRed()
{
    // CL-1a: only the contact span at the ends is exempt. A ridge under the
    // middle of a landing glide or a takeoff climb is still red.
    const LocalFrame f{37.7679, -122.4934, 0};
    const GroundFn ridge = [f](double, double lon) { return std::abs(f.east(lon) - 200) < 8 ? 39.0 : 20.0; };
    const auto top = offset(0, 0), td = offset(0, 400);

    // Landing: a fixed-wing glide from 40 m above launch to a touchdown 400 m east.
    SourcePlan land = fixturePlan();
    land.multiRotor = false;
    land.fixedWing = true;
    land.items << item(1, 22, 3, 0, 0, 40, 1) << item(2, 16, 3, top.first, top.second + 1e-6, 40, 1)
               << item(3, 21, 3, td.first, td.second, 0, 1);
    Scene sc = Builder::build(land, ridge);
    int red = 0, contact = 0;
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Path && l.visualIndex == 3) {
            if (l.clearance == Clearance::Strike) {
                red++;
                QVERIFY(std::abs(l.a.x - 200) < 30 || std::abs(l.b.x - 200) < 30);   // at the ridge only
            }
            if (l.clearance == Clearance::GroundContact) {
                contact++;
                QVERIFY(l.a.x > 300 && l.b.x > 300);                              // at the touchdown only
            }
        }
    }
    QVERIFY(red > 0);
    QVERIFY(contact > 0);
    QCOMPARE(markerForSeq(sc, 3)->clearance, Clearance::GroundContact);

    // Takeoff: a climb with a position (a diagonal from home) over the same ridge.
    SourcePlan up = fixturePlan();
    up.arduPilot = false;
    up.items << item(1, 22, 3, td.first, td.second, 40, 1);
    sc = Builder::build(up, ridge);
    red = 0;
    contact = 0;
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Path && l.visualIndex == 1) {
            if (l.clearance == Clearance::Strike) {
                red++;
                QVERIFY(std::abs(l.a.x - 200) < 30 || std::abs(l.b.x - 200) < 30);
            }
            if (l.clearance == Clearance::GroundContact) {
                contact++;
                QVERIFY(l.a.x < 50 && l.b.x < 50);                                // leaving launch only
            }
        }
    }
    QVERIFY(red > 0);
    QVERIFY(contact > 0);
    QVERIFY(sc.stats.samplesStrike > 0);

    // A cruise leg dipping under 2 m is red end to end of the dip, as before.
    SourcePlan cruise = fixturePlan();
    const auto w1 = offset(0, 10);
    cruise.items << item(1, 22, 3, 0, 0, 20, 1) << item(2, 16, 3, w1.first, w1.second, 20, 1)
                 << item(3, 16, 3, td.first, td.second, 20, 1);
    sc = Builder::build(cruise, ridge);
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Path && l.visualIndex == 3) {
            QVERIFY(l.clearance != Clearance::GroundContact);
        }
    }
}

void Plan3DTest::_fixtureBreachReturnAtGround()
{
    // A-9 (rev 8): the breach return point is drawn at the ground under its
    // position, never strike colored, whatever its stored height, and the
    // stored height is kept as read. No stem, no 0 m warning.
    const auto bp = offset(150, 50);
    const LocalFrame f{37.7679, -122.4934, 0};
    const GroundFn slope = [f](double, double lon) { return 20.0 + 0.1 * f.east(lon); };   // 25 m under the point
    for (const double alt : {35.0, 0.0, -10.0, 1.0}) {
        SourcePlan plan = fixturePlan();
        const auto a = offset(100, 0);
        plan.items << item(1, 22, 3, 0, 0, 30, 1) << item(2, 16, 3, a.first, a.second, 30, 1);
        plan.breachReturn = SourcePoint{bp.first, bp.second, alt};
        const Scene sc = Builder::build(plan, slope);
        QCOMPARE(plan.breachReturn.alt, alt);      // stored height untouched
        QCOMPARE(countMarkers(sc, MarkerKind::BreachReturn), 1);
        for (const Marker &m : sc.markers) {
            if (m.kind == MarkerKind::BreachReturn) {
                QVERIFY(std::abs(m.pos.y + sc.frame.alt0 - 25.0) < 1e-6);
                QCOMPARE(m.clearance, Clearance::NoStrike);
                QVERIFY(!std::isfinite(m.amsl));
                QVERIFY(m.label.contains(QStringLiteral("height not used")));
                QCOMPARE(m.visualIndex, -1);
            }
        }
        QCOMPARE(countKind(sc, LineKind::Stem) + countKind(sc, LineKind::StemUnknown), 2);   // the two route points only
        QVERIFY(sc.warnings.filter(QStringLiteral("Breach return")).isEmpty());
    }
    // Unknown ground: still a position marker, never red or counted.
    SourcePlan plan = fixturePlan();
    plan.breachReturn = SourcePoint{bp.first, bp.second, 35.0};
    plan.items << item(1, 22, 3, 0, 0, 30, 1);
    const Scene sc = Builder::build(plan, unknownGround());
    for (const Marker &m : sc.markers) {
        if (m.kind == MarkerKind::BreachReturn) {
            QCOMPARE(m.clearance, Clearance::NoStrike);
            QCOMPARE(m.pos.y, 0.0);                // launch level, display only
        }
    }
}

void Plan3DTest::_fixtureUnknownGroundPatch()
{
    // A-3, CL-2: where the ground is not loaded the samples are unknown,
    // never counted as no strike.
    SourcePlan plan = fixturePlan();
    const auto a = offset(100, -200), b = offset(100, 200);
    plan.items << item(1, 16, 3, a.first, a.second, 40, 1) << item(2, 16, 3, b.first, b.second, 40, 1);
    const LocalFrame f{37.7679, -122.4934, 0};
    const GroundFn patch = [f](double, double lon) { return f.east(lon) > 50 ? kNaN : 20.0; };
    const Scene sc = Builder::build(plan, patch);
    int unknownSegs = 0;
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Path && (l.a.x > 50 || l.b.x > 50)) {
            QCOMPARE(l.clearance, Clearance::Unknown);
            unknownSegs++;
        }
    }
    QVERIFY(unknownSegs > 0);
    QCOMPARE(markerForSeq(sc, 2)->clearance, Clearance::Unknown);
    QVERIFY(sc.stats.pointsUnknownGround >= 1);
    QVERIFY(countKind(sc, LineKind::StemUnknown) >= 1);
}

void Plan3DTest::_fixtureTerrainFrameLegFollowsGround()
{
    // A-7: a leg into a terrain-frame point follows the ground at a height
    // interpolated between the two heights above ground.
    SourcePlan plan = fixturePlan();
    const auto a = offset(0, 100), b = offset(0, 300);
    plan.items << item(1, 16, 10, a.first, a.second, 40, 4) << item(2, 16, 10, b.first, b.second, 60, 4);
    const LocalFrame f{37.7679, -122.4934, 0};
    const GroundFn slope = [f](double, double lon) { return 20.0 + 0.1 * f.east(lon); };
    const Scene sc = Builder::build(plan, slope);
    QVERIFY(sc.hasTerrainFrame);
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Path && l.visualIndex == 2) {
            const double t = (l.b.x - 100.0) / 200.0;
            const double hag = 40.0 + 20.0 * t;
            QVERIFY(std::abs((l.b.y + sc.frame.alt0) - (20.0 + 0.1 * l.b.x + hag)) < 1e-3);
            QCOMPARE(l.clearance, Clearance::NoStrike);
        }
    }
}

void Plan3DTest::_fixtureRtlUnknownAndNeverRed()
{
    // A-10, CL-1: RTL is violet even over ground that would be a strike,
    // and it ends at home at the launch altitude.
    SourcePlan plan = fixturePlan();
    const auto a = offset(300, 0);
    plan.items << item(1, 22, 3, 0, 0, 30, 1) << item(2, 16, 3, a.first, a.second, 40, 1);
    SourceItem rtl = item(3, 20, 2, 0, 0, 0);
    plan.items << rtl;
    const Scene sc = Builder::build(plan, flatFixture(500));    // fixture ground far above the route
    QVERIFY(sc.hasUnknownRtl);
    QCOMPARE(countKind(sc, LineKind::Unknown), 2);
    for (const LineSeg &l : sc.lines) {
        if (l.visualIndex == 3) {
            QCOMPARE(l.kind, LineKind::Unknown);
            QCOMPARE(l.clearance, Clearance::Unknown);
        }
    }
    const LineSeg *last = nullptr;
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Unknown) {
            last = &l;
        }
    }
    QVERIFY(last);
    QVERIFY(std::abs(last->b.x) < 1e-6 && std::abs(last->b.z) < 1e-6 && std::abs(last->b.y) < 1e-6);

    // A plane does not descend at home: it ends over home.
    plan.multiRotor = false;
    plan.fixedWing = true;
    QCOMPARE(countKind(Builder::build(plan, flatFixture(20)), LineKind::Unknown), 1);
}

void Plan3DTest::_fixtureRtlHeightFromAircraft()
{
    // A-10: a return height read from the aircraft is drawn, and the legend
    // names it, but the legs stay violet and unclassed: never red, even
    // where they descend to the ground (review must-fix 2).
    SourcePlan plan = fixturePlan();
    const auto a = offset(300, 0);
    plan.items << item(1, 22, 3, 0, 0, 30, 1) << item(2, 16, 3, a.first, a.second, 40, 1) << item(3, 20, 2, 0, 0, 0);
    plan.rtlReturnRelM = 60;
    plan.rtlSource = QStringLiteral("fixture");
    const Scene sc = Builder::build(plan, flatFixture(20));
    QVERIFY(sc.hasUnknownRtl);
    QCOMPARE(countKind(sc, LineKind::Unknown), 3);     // climb, over home, down
    double top = -1e9;
    for (const LineSeg &l : sc.lines) {
        if (l.visualIndex == 3) {
            QCOMPARE(l.kind, LineKind::Unknown);
            QCOMPARE(l.clearance, Clearance::Unknown);
            top = std::max(top, l.b.y + sc.frame.alt0);
        }
    }
    QCOMPARE(top, 80.0);    // H 20 + 60
    QCOMPARE(sc.stats.samplesStrike, 0);     // the takeoff's ground contact is not red (CL-1a)
    QVERIFY(sc.warnings.join(QString()).contains(QStringLiteral("read from fixture")));
}

void Plan3DTest::_fixtureRtlToNearestRally()
{
    // A-10 with rally points: ArduPilot (RALLY_INCL_HOME) and PX4 return to
    // the nearest of the rally points and home. Violet in every case.
    SourcePlan plan = fixturePlan();
    const auto a = offset(400, 0), nearRally = offset(380, 20), farRally = offset(-500, 0);
    plan.items << item(1, 22, 3, 0, 0, 30, 1) << item(2, 16, 3, a.first, a.second, 40, 1) << item(3, 20, 2, 0, 0, 0);
    plan.rally << SourcePoint{farRally.first, farRally.second, 15} << SourcePoint{nearRally.first, nearRally.second, 10};
    Scene sc = Builder::build(plan, flatFixture(20));
    const LineSeg *last = nullptr;
    for (const LineSeg &l : sc.lines) {
        if (l.visualIndex == 3) {
            QCOMPARE(l.kind, LineKind::Unknown);
            last = &l;
        }
    }
    QVERIFY(last);
    const Vec3 dest = sc.frame.toScene(nearRally.first, nearRally.second, 20 + 10);
    QVERIFY(std::abs(last->b.x - dest.x) < 1e-3 && std::abs(last->b.z - dest.z) < 1e-3 && std::abs(last->b.y - dest.y) < 1e-6);
    QVERIFY(sc.warnings.join(QString()).contains(QStringLiteral("Rally 2")));

    // Home nearer than every rally point: home.
    plan.rally.clear();
    plan.rally << SourcePoint{farRally.first, farRally.second, 15};
    plan.items[1] = item(2, 16, 3, offset(50, 0).first, offset(50, 0).second, 40, 1);
    sc = Builder::build(plan, flatFixture(20));
    for (const LineSeg &l : sc.lines) {
        if (l.visualIndex == 3) {
            last = &l;
        }
    }
    QVERIFY(std::abs(last->b.x) < 1e-6 && std::abs(last->b.z) < 1e-6 && std::abs(last->b.y) < 1e-6);
}

void Plan3DTest::_fixtureSplineUnknownControlPoint()
{
    // CL-2 (review must-fix 3): a spline leg whose four control points
    // include one of unknown height is unknown, though both its ends are known.
    SourcePlan plan = fixturePlan();
    const auto a = offset(100, 0), b = offset(100, 100), c = offset(200, 100), d = offset(200, 300);
    plan.items << item(1, 16, 3, a.first, a.second, 40, 1) << item(2, 82, 3, b.first, b.second, 40, 1)
               << item(3, 82, 3, c.first, c.second, 40, 1) << item(4, 16, 10, d.first, d.second, 40, 4);
    const LocalFrame f{37.7679, -122.4934, 0};
    const GroundFn patch = [f](double, double lon) { return f.east(lon) > 250 ? kNaN : 20.0; };   // unknown only near item 4
    const Scene sc = Builder::build(plan, patch);
    QVERIFY(!std::isfinite(markerForSeq(sc, 4)->amsl));
    int into2 = 0, into3 = 0;
    for (const LineSeg &l : sc.lines) {
        if (l.kind != LineKind::Path) {
            continue;
        }
        if (l.visualIndex == 3) {          // control points 1, 2, 3, 4: item 4 unknown
            QCOMPARE(l.clearance, Clearance::Unknown);
            into3++;
        } else if (l.visualIndex == 2) {   // control points home, 1, 2, 3: all known
            QVERIFY(l.clearance != Clearance::Unknown);
            into2++;
        }
    }
    QCOMPARE(into2, kSamplesPerLeg);
    QCOMPARE(into3, kSamplesPerLeg);
}

void Plan3DTest::_fixtureDoJumpSkippedWarned()
{
    // Items of 100 and above (DO_JUMP among them) are not drawn; the legend
    // counts them and says loops are not checked.
    SourcePlan plan = fixturePlan();
    const auto a = offset(100, 0);
    SourceItem jump = item(3, 177, 2, 0, 0, 0);
    jump.params[0] = 1;
    jump.params[1] = 3;
    plan.items << item(1, 16, 3, a.first, a.second, 40, 1) << item(2, 206, 2, 0, 0, 0) << jump;
    const Scene sc = Builder::build(plan, flatFixture(20));
    QCOMPARE(sc.stats.skippedCommands, 2);
    const QString w = sc.warnings.join(QStringLiteral(" | "));
    QVERIFY(w.contains(QStringLiteral("2 items with commands of 100 or above")));
    QVERIFY(w.contains(QStringLiteral("are flown but not drawn or checked")));
    QVERIFY(w.contains(QStringLiteral("1 DO_JUMP item")));
}

void Plan3DTest::_planHomeKeptAfterGcTerrain()
{
    // A-0b compares the plan's own H. GC overwrites the home altitude when
    // its terrain query answers; the source keeps the plan's value, whether
    // it saw the plan before the overwrite or only after it (then from the file).
    for (const bool seenBefore : {true, false}) {
        delete _pmc;
        _pmc = new PlanMasterController(this);
        _pmc->setFlyView(false);
        _pmc->start();
        _pmc->loadFromFile(corpusPath(QStringLiteral("ardupilot-copter-alt-relative.plan")));
        if (seenBefore) {
            QCOMPARE(sourceFromPlan(_pmc).homeAmsl, 20.0);
        }
        // GC's own path: the settings item's terrain reply sets the home altitude.
        QObject *settings = _pmc->missionController()->visualItems()->get(0);
        QVERIFY(QMetaObject::invokeMethod(settings, "_terrainDataReceived", Qt::DirectConnection,
                                          Q_ARG(bool, true), Q_ARG(QList<double>, QList<double>{41.0})));
        QCOMPARE(_pmc->missionController()->plannedHomePosition().altitude(), 41.0);
        const SourcePlan plan = sourceFromPlan(_pmc);
        QVERIFY2(plan.homeAmsl == 20.0, qPrintable(QStringLiteral("seenBefore %1: homeAmsl %2, plan file '%3'")
                                                     .arg(seenBefore).arg(plan.homeAmsl).arg(_pmc->currentPlanFile())));
        const Scene sc = Builder::build(plan, flatFixture(41));
        QVERIFY(sc.homeDatumNote);
        _pmc->setDirty(false);
    }
}

void Plan3DTest::_fixtureSplineOnlyArduPilotMultirotor()
{
    // S-2, S-3: a spline leg curves only on ArduPilot multirotor; elsewhere
    // it is straight and warned.
    SourcePlan plan = fixturePlan();
    const auto a = offset(100, 0), b = offset(100, 100), c = offset(200, 100), d = offset(200, 0);
    plan.items << item(1, 16, 3, a.first, a.second, 40, 1) << item(2, 82, 3, b.first, b.second, 40, 1)
               << item(3, 82, 3, c.first, c.second, 40, 1) << item(4, 16, 3, d.first, d.second, 40, 1);
    const Scene curved = Builder::build(plan, flatFixture(20));
    QVERIFY(curved.hasSpline);
    plan.arduPilot = false;
    const Scene straight = Builder::build(plan, flatFixture(20));
    QVERIFY(!straight.hasSpline);
    QVERIFY(straight.warnings.join(QString()).contains(QStringLiteral("only ArduPilot multirotors fly as a curve")));
    QVERIFY(maxOffStraight(curved, straight, 3) > 0.5);
}

void Plan3DTest::_fixtureLoiterToAltTangentExit()
{
    // A landing pattern's NAV_LOITER_TO_ALT: a ring at its height, and the
    // route leaves the ring on the tangent toward the touchdown.
    SourcePlan plan = fixturePlan();
    plan.multiRotor = false;
    plan.fixedWing = true;
    const auto c = offset(0, 400), land = offset(0, 0);
    SourceItem loiter = item(1, 31, 3, c.first, c.second, 50, 0);
    loiter.params[1] = 75;      // clockwise
    loiter.fromComplex = true;
    SourceItem touch = item(2, 21, 3, land.first, land.second, 0, 0);
    touch.fromComplex = true;
    plan.items << loiter << touch;
    const Scene sc = Builder::build(plan, flatFixture(20));
    // The glide leg into the touchdown starts on the circle.
    const LineSeg *first = nullptr;
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Path && l.visualIndex == 2) {
            first = &l;
            break;
        }
    }
    QVERIFY(first);
    const double cx = 400, cz = 0;
    const double r = std::hypot(first->a.x - cx, first->a.z - cz);
    QVERIFY(std::abs(r - 75) < 0.05);
    // Tangent: the radius is at right angles to the line to the touchdown.
    const double dot = (first->a.x - cx) * (0 - first->a.x) + (first->a.z - cz) * (0 - first->a.z);
    QVERIFY(std::abs(dot) < 1.0);
    // Clockwise seen from above (north up), heading west to the touchdown:
    // it leaves from the south side of the circle.
    QVERIFY(-first->a.z < 0);
    // No line is drawn from the loiter centre out to the exit point.
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Path) {
            QVERIFY(!(std::abs(l.a.x - cx) < 1e-6 && std::abs(l.a.z - cz) < 1e-6));
        }
    }
    // The ring is drawn at the loiter height.
    int ring = 0;
    for (const LineSeg &l : sc.lines) {
        ring += (l.kind == LineKind::Path && l.visualIndex == 1 && std::abs(l.a.y - 50) < 1e-6) ? 1 : 0;
    }
    QVERIFY(ring >= 48);
}

void Plan3DTest::_rtlFromConnectedAircraft()
{
    // Plan3DSource reads the return height only from a connected aircraft's
    // own parameters (MockLink PX4: RTL_RETURN_ALT 30 m).
    QVERIFY(std::isnan(sourceFromPlan(_pmc).rtlReturnRelM));    // offline: no aircraft, no height
    delete _pmc;
    _pmc = nullptr;
    _connectMockLink(MAV_AUTOPILOT_PX4);
    _pmc = new PlanMasterController(this);
    _pmc->setFlyView(false);
    _pmc->start();
    QTest::qWait(100);
    const SourcePlan plan = sourceFromPlan(_pmc);
    QCOMPARE(plan.rtlReturnRelM, 30.0);
    QVERIFY(plan.rtlSource.contains(QStringLiteral("RTL_RETURN_ALT")));
    delete _pmc;
    _pmc = nullptr;
    _disconnectMockLink();
}

// ---------------------------------------------------------------------------
// The 17 corpus plans. Non-synthetic files carry no terrain, so their known
// ground is a flat test fixture at the 20 m launch altitude (not terrain).

void Plan3DTest::_corpus_wp300_px4_copter_lawnmower()
{
    const QString f = QStringLiteral("wp300-px4-copter-lawnmower.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(f, plan, json));
    _commonCorpusChecks(f, plan, json, 20, false);
    const Scene sc = Builder::build(plan, flatFixture(20));
    QCOMPARE(countMarkers(sc, MarkerKind::Waypoint), 301);    // takeoff + 300
    QVERIFY(!plan.arduPilot);
    // Question 1 (the 2 m touch points on wp300): the climb from launch
    // and the touchdown are ground contact, not red (CL-1a).
    QCOMPARE(sc.stats.samplesStrike, 0);
    QVERIFY(sc.stats.samplesGroundContact > 0);
}

void Plan3DTest::_corpus_px4_copter_survey_sony_a6000()
{
    const QString f = QStringLiteral("px4-copter-survey-sony-a6000.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(f, plan, json));
    _commonCorpusChecks(f, plan, json, 20, false);
    const Scene sc = Builder::build(plan, flatFixture(20));
    int transectNav = 0;
    for (const QJsonObject &it : missionItems(json)) {
        for (const QJsonObject &t : transectItems(it)) {
            transectNav += t[QStringLiteral("command")].toInt() == 16 ? 1 : 0;
        }
    }
    QVERIFY(transectNav > 0);
    QCOMPARE(countMarkers(sc, MarkerKind::PatternPoint), transectNav);
    QCOMPARE(plan.outlines.size(), 1);
    QVERIFY(plan.outlines[0].closed);
    QVERIFY(countKind(sc, LineKind::Outline) > 0);
}

void Plan3DTest::_corpus_ardupilot_copter_survey_custom_camera()
{
    const QString f = QStringLiteral("ardupilot-copter-survey-custom-camera.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(f, plan, json));
    _commonCorpusChecks(f, plan, json, 20, false);
    QCOMPARE(plan.outlines.size(), 1);
    QCOMPARE(plan.outlines[0].path.size(), 5);
}

void Plan3DTest::_corpus_SYNTHETIC_TERRAIN_ardupilot_copter_survey_follow_terrain()
{
    // SYNTHETIC TERRAIN: the file was saved on a constant 100 m ground; this
    // proves arithmetic only, nothing about real terrain.
    const QString f = QStringLiteral("ardupilot-copter-survey-follow-terrain.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(f, plan, json));
    _commonCorpusChecks(f, plan, json, kSyntheticTerrainM, true);
    // A-8: DistanceMode 3 writes frame 0 transect points at G + 50 = 150 AMSL.
    const Scene sc = Builder::build(plan, flatFixture(kSyntheticTerrainM));
    int n = 0;
    for (const Marker &m : sc.markers) {
        if (m.kind == MarkerKind::PatternPoint) {
            QCOMPARE(m.amsl, 150.0);
            QCOMPARE(m.amsl - m.ground, 50.0);
            n++;
        }
    }
    QVERIFY(n > 0);
}

void Plan3DTest::_corpus_px4_copter_corridor()
{
    const QString f = QStringLiteral("px4-copter-corridor.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(f, plan, json));
    _commonCorpusChecks(f, plan, json, 20, false);
    QCOMPARE(plan.outlines.size(), 1);
    QVERIFY(!plan.outlines[0].closed);     // a corridor is a polyline
    QCOMPARE(plan.outlines[0].path.size(), 4);
}

void Plan3DTest::_corpus_ardupilot_copter_structure()
{
    const QString f = QStringLiteral("ardupilot-copter-structure.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_loadStructure(f, plan, json));
    _commonCorpusChecks(f, plan, json, 20, false);
    // 1.8: (4 + 3) x 5 + 4 = 39 items; the RTL at sequence 41.
    int fromScan = 0;
    for (const SourceItem &it : plan.items) {
        if (it.fromComplex) {
            fromScan++;
            if (Builder::isRoutePoint(it.command)) {
                QCOMPARE(it.frame, 3);     // A-8: structure scan nav items are above launch
            }
        }
        if (it.command == 20) {
            QCOMPARE(it.seq, 41);
        }
    }
    QCOMPARE(fromScan, 39);
    const Scene sc = Builder::build(plan, flatFixture(20));
    double lo = 1e9, hi = -1e9;
    for (const Marker &m : sc.markers) {
        if (m.kind == MarkerKind::PatternPoint) {
            lo = std::min(lo, m.amsl);
            hi = std::max(hi, m.amsl);
        }
    }
    QVERIFY(lo >= 20 + 5 - kTol);      // not below the scan bottom
    QCOMPARE(hi, 20.0 + 40.0);         // the entrance altitude
    QCOMPARE(plan.outlines.size(), 1);

    // FW-4 (rev 7): on ArduPilot the scan aims with 195 at the structure
    // center (the mean of the corners), at the middle of the scanned band,
    // above launch; no 196. The view draws that ROI where GC sends it.
    double cLat = 0, cLon = 0;
    int nCorners = 0;
    for (const QJsonObject &it : missionItems(json)) {
        if (it[QStringLiteral("complexItemType")].toString() == QStringLiteral("StructureScan")) {
            for (const QJsonValue &v : it[QStringLiteral("polygon")].toArray()) {
                cLat += v.toArray()[0].toDouble();
                cLon += v.toArray()[1].toDouble();
                nCorners++;
            }
        }
    }
    QVERIFY(nCorners >= 3);
    cLat /= nCorners;
    cLon /= nCorners;
    const SourceItem *roi = nullptr;
    for (const SourceItem &it : plan.items) {
        QVERIFY(it.command != 196);
        if (it.fromComplex && it.command == 195) {
            QVERIFY(!roi);
            roi = &it;
        }
    }
    QVERIFY(roi);
    QCOMPARE(roi->frame, 3);
    QVERIFY(std::abs(roi->params[4] - cLat) < 1e-9 && std::abs(roi->params[5] - cLon) < 1e-9);
    const Marker *rm = nullptr;
    for (const Marker &m : sc.markers) {
        if (m.kind == MarkerKind::Roi && m.seq == roi->seq) {
            rm = &m;
        }
    }
    QVERIFY(rm);
    QCOMPARE(rm->kind, MarkerKind::Roi);
    QCOMPARE(rm->amsl, 20.0 + roi->params[6]);
    QVERIFY(rm->amsl > lo - kTol && rm->amsl < hi + kTol);     // inside the scanned band
    QVERIFY(rm->label.contains(QStringLiteral("structure center")));
    QCOMPARE(rm->clearance, Clearance::NoStrike);              // a look-at point is never red
    const Vec3 at = sc.frame.toScene(cLat, cLon, rm->amsl);
    QVERIFY(std::abs(rm->pos.x - at.x) < 1e-3 && std::abs(rm->pos.z - at.z) < 1e-3 && std::abs(rm->pos.y - at.y) < 1e-3);
}

void Plan3DTest::_corpus_ardupilot_plane_fw_landing()
{
    const QString f = QStringLiteral("ardupilot-plane-fw-landing.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(f, plan, json));
    _commonCorpusChecks(f, plan, json, 20, false);
    QVERIFY(plan.fixedWing);
    // 1.9 expansion: DO_LAND_START, NAV_LOITER_TO_ALT, NAV_LAND.
    QCOMPARE(complexCommands(plan), (QList<int>{189, 31, 21}));
    const Scene sc = Builder::build(plan, flatFixture(20));
    for (const Marker &m : sc.markers) {
        if (m.command == 31) {
            QCOMPARE(m.amsl, 70.0);     // approach 50 m above launch
        } else if (m.command == 21) {
            QCOMPARE(m.amsl, 20.0);     // touchdown at launch level
            QCOMPARE(m.clearance, Clearance::GroundContact);    // CL-1a, question 1
        } else if (m.command == 22) {
            QVERIFY(std::abs(m.pos.x) < 1e-6 && std::abs(m.pos.z) < 1e-6);    // A-11
        }
    }
}

void Plan3DTest::_corpus_ardupilot_vtol_vtol_landing()
{
    const QString f = QStringLiteral("ardupilot-vtol-vtol-landing.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(f, plan, json));
    _commonCorpusChecks(f, plan, json, 20, false);
    QVERIFY(plan.vtol);
    QCOMPARE(complexCommands(plan), (QList<int>{189, 31, 85}));
    const Scene sc = Builder::build(plan, flatFixture(20));
    bool takeoff84 = false, land85 = false;
    for (const Marker &m : sc.markers) {
        if (m.command == 84) {
            takeoff84 = true;
            QCOMPARE(m.amsl, 60.0);
            QVERIFY(std::abs(m.pos.x) < 1e-6 && std::abs(m.pos.z) < 1e-6);    // A-11: climbs at launch
        } else if (m.command == 85) {
            land85 = true;
            QCOMPARE(m.amsl, 20.0);
            QCOMPARE(m.clearance, Clearance::GroundContact);     // CL-1a
        }
    }
    QVERIFY(takeoff84 && land85);
    // NAV_VTOL_LAND: flown to over the point, then a vertical descent.
    bool vertical = false;
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Path && std::abs(l.a.x - l.b.x) < 1e-6 && std::abs(l.a.z - l.b.z) < 1e-6 && l.b.y < 1e-6 && l.b.y > -1e-6) {
            vertical = true;
        }
    }
    QVERIFY(vertical);
}

void Plan3DTest::_corpus_ardupilot_copter_alt_relative()
{
    const QString f = QStringLiteral("ardupilot-copter-alt-relative.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(f, plan, json));
    _commonCorpusChecks(f, plan, json, 20, false);
}

void Plan3DTest::_corpus_ardupilot_copter_alt_amsl()
{
    const QString f = QStringLiteral("ardupilot-copter-alt-amsl.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(f, plan, json));
    _commonCorpusChecks(f, plan, json, 20, false);
    const Scene sc = Builder::build(plan, unknownGround());
    for (const QJsonObject &it : missionItems(json)) {
        if (it[QStringLiteral("frame")].toInt() == 0) {
            QCOMPARE(markerForSeq(sc, it[QStringLiteral("doJumpId")].toInt())->amsl, p7(it));   // not H + params[6]
        }
    }
}

void Plan3DTest::_corpus_SYNTHETIC_TERRAIN_ardupilot_copter_alt_calc_above_terrain()
{
    // SYNTHETIC TERRAIN: constant 100 m; arithmetic only.
    const QString f = QStringLiteral("ardupilot-copter-alt-calc-above-terrain.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(f, plan, json));
    _commonCorpusChecks(f, plan, json, kSyntheticTerrainM, true);
    // Mode 3, frame 0: params[6] (140) flies, Altitude (40) is the height
    // above the planner's terrain and is never used as the height.
    const Scene sc = Builder::build(plan, flatFixture(kSyntheticTerrainM));
    int n = 0;
    for (const QJsonObject &it : missionItems(json)) {
        if (it[QStringLiteral("AltitudeMode")].toInt() == 3) {
            QCOMPARE(it[QStringLiteral("Altitude")].toDouble(), 40.0);
            const Marker *m = markerForSeq(sc, it[QStringLiteral("doJumpId")].toInt());
            QCOMPARE(m->amsl, 140.0);
            QCOMPARE(m->amsl - m->ground, 40.0);
            QCOMPARE(m->clearance, Clearance::NoStrike);
            n++;
        }
    }
    QCOMPARE(n, 4);
    QVERIFY(sc.warnings.filter(QStringLiteral("frame disagree")).isEmpty());
}

void Plan3DTest::_corpus_ardupilot_copter_alt_terrain_frame()
{
    const QString f = QStringLiteral("ardupilot-copter-alt-terrain-frame.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(f, plan, json));
    _commonCorpusChecks(f, plan, json, 20, false);
    QVERIFY(Builder::build(plan, unknownGround()).hasTerrainFrame);
}

void Plan3DTest::_corpus_ardupilot_copter_alt_mixed()
{
    const QString f = QStringLiteral("ardupilot-copter-alt-mixed.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(f, plan, json));
    _commonCorpusChecks(f, plan, json, 20, false);
    QSet<int> modes;
    for (const SourceItem &it : plan.items) {
        if (it.altitudeMode) {
            modes.insert(it.altitudeMode);
            QVERIFY(modeAgreesWithFrame(it.altitudeMode, it.frame));
        }
    }
    QCOMPARE(modes, (QSet<int>{1, 2, 4}));
    QVERIFY(Builder::build(plan, flatFixture(20)).warnings.filter(QStringLiteral("frame disagree")).isEmpty());
}

void Plan3DTest::_corpus_px4_copter_alt_amsl()
{
    const QString f = QStringLiteral("px4-copter-alt-amsl.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(f, plan, json));
    _commonCorpusChecks(f, plan, json, 20, false);
    QVERIFY(!plan.arduPilot);
}

void Plan3DTest::_corpus_ardupilot_copter_fence_rally()
{
    const QString f = QStringLiteral("ardupilot-copter-fence-rally.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(f, plan, json));
    _commonCorpusChecks(f, plan, json, 20, false);
    QCOMPARE(plan.fences.size(), 4);
    const Scene sc = Builder::build(plan, flatFixture(20));
    QVERIFY(countKind(sc, LineKind::FenceInclusion) > 0);
    QVERIFY(countKind(sc, LineKind::FenceExclusion) > 0);
    QCOMPARE(countMarkers(sc, MarkerKind::Rally), 3);
    QCOMPARE(countMarkers(sc, MarkerKind::BreachReturn), 1);
    for (const Marker &m : sc.markers) {
        if (m.kind == MarkerKind::BreachReturn) {
            // A-9 (rev 8): at the ground under its position, height not used.
            QVERIFY(!std::isfinite(m.amsl));
            QCOMPARE(m.ground, 20.0);
            QCOMPARE(m.pos.y + sc.frame.alt0, 20.0);
            QCOMPARE(m.clearance, Clearance::NoStrike);
            QVERIFY(m.label.contains(QStringLiteral("height not used")));
        } else if (m.kind == MarkerKind::Rally) {
            QCOMPARE(m.amsl, 20.0);            // 0 m above launch (B4), shown as such
        }
    }
    QCOMPARE(sc.warnings.filter(QStringLiteral("0 m above launch")).size(), 3);
}

void Plan3DTest::_corpus_px4_copter_fence_rally()
{
    const QString f = QStringLiteral("px4-copter-fence-rally.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(f, plan, json));
    _commonCorpusChecks(f, plan, json, 20, false);
    const Scene sc = Builder::build(plan, flatFixture(20));
    QCOMPARE(plan.fences.size(), 4);
    QCOMPARE(countMarkers(sc, MarkerKind::Rally), 2);
    QCOMPARE(countMarkers(sc, MarkerKind::BreachReturn), 0);
}

void Plan3DTest::_corpus_ardupilot_copter_spline()
{
    const QString f = QStringLiteral("ardupilot-copter-spline.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(f, plan, json));
    _commonCorpusChecks(f, plan, json, 20, false);
    int splines = 0;
    for (const SourceItem &it : plan.items) {
        if (it.command == 82) {
            splines++;
            QCOMPARE(it.params[3], 0.0);    // S-4
        }
    }
    QCOMPARE(splines, 4);
    QVERIFY(plan.arduPilot && plan.multiRotor);
    const Scene curved = Builder::build(plan, flatFixture(20));
    QVERIFY(curved.hasSpline);
    SourcePlan straightPlan = plan;
    straightPlan.multiRotor = false;
    const Scene straight = Builder::build(straightPlan, flatFixture(20));
    double maxDiff = 0;
    for (const SourceItem &it : plan.items) {
        if (it.command == 82) {
            maxDiff = std::max(maxDiff, maxOffStraight(curved, straight, it.visualIndex));
        }
    }
    QVERIFY(maxDiff > 1.0);
}

void Plan3DTest::_corpusAllSeventeenRender()
{
    // MG2 "every corpus plan renders": all 17 files, CL-7.
    const QStringList files = QDir(QStringLiteral(RL_PLAN_CORPUS_DIR)).entryList({QStringLiteral("*.plan")}, QDir::Files, QDir::Name);
    QCOMPARE(files.size(), 17);
    for (const QString &f : files) {
        if (f.contains(QStringLiteral("structure"))) {
            continue;   // covered by _corpus_ardupilot_copter_structure (G71)
        }
        delete _pmc;
        _pmc = new PlanMasterController(this);
        _pmc->setFlyView(false);
        _pmc->start();
        SourcePlan plan; QJsonObject json;
        QVERIFY2(_load(f, plan, json), qPrintable(f));
        const Scene sc = Builder::build(plan, unknownGround());
        QVERIFY2(sc.markers.size() > 1, qPrintable(f));
        QVERIFY2(countKind(sc, LineKind::Path) > 0, qPrintable(f));
        QVERIFY2(sc.warnings.filter(QStringLiteral("Commands not drawn")).isEmpty(), qPrintable(f + sc.warnings.join(QStringLiteral(" | "))));
        QVERIFY2(sc.warnings.filter(QStringLiteral("carries no position height")).isEmpty(), qPrintable(f + sc.warnings.join(QStringLiteral(" | "))));
        _pmc->setDirty(false);
    }
}

// ---------------------------------------------------------------------------
// G67 M1, contract revision 9: drawing limits. A malformed plan must build a
// bounded scene, never crash, never draw a part past a limit in the no
// strike color, and say so on the view.

namespace {

/// Every limit warning is also a legend warning, and its wording follows the
/// contract: US spelling, no em or en dash, never a word that reads as
/// permission (CL-5).
void checkLimitWording(const Scene &sc)
{
    QVERIFY(!sc.limitWarnings.isEmpty());
    for (const QString &w : sc.limitWarnings) {
        QVERIFY2(sc.warnings.contains(w), qPrintable(w));
        QVERIFY2(!w.contains(QChar(0x2014)) && !w.contains(QChar(0x2013)), qPrintable(w));
        for (const char *word : {"clear", "safe", "approv", "certif", " OK", "metre"}) {
            QVERIFY2(!w.contains(QLatin1String(word), Qt::CaseInsensitive), qPrintable(w));
        }
    }
}

bool allFinite(const Scene &sc)
{
    for (const LineSeg &l : sc.lines) {
        for (const Vec3 &v : {l.a, l.b}) {
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
                return false;
            }
        }
    }
    for (const Marker &m : sc.markers) {
        if (!std::isfinite(m.pos.x) || !std::isfinite(m.pos.y) || !std::isfinite(m.pos.z)) {
            return false;
        }
    }
    return true;
}

int countVisual(const Scene &sc, LineKind k, int visualIndex)
{
    int n = 0;
    for (const LineSeg &l : sc.lines) {
        n += (l.kind == k && l.visualIndex == visualIndex) ? 1 : 0;
    }
    return n;
}

/// No line or marker for `visualIndex` is in the no strike color.
bool neverNoStrike(const Scene &sc, int visualIndex)
{
    for (const LineSeg &l : sc.lines) {
        if (l.visualIndex == visualIndex && l.kind == LineKind::Path && l.clearance == Clearance::NoStrike) {
            return false;
        }
    }
    for (const Marker &m : sc.markers) {
        if (m.visualIndex == visualIndex && m.clearance == Clearance::NoStrike) {
            return false;
        }
    }
    return true;
}

} // namespace

void Plan3DTest::_boundsHugeLoiterRadius()
{
    // One loiter with radius 1e7 m (and a NAV_LOITER_TO_ALT with -1e7): no
    // ring, no tangent exit, no wider ground area, a warning; the route is
    // still drawn.
    SourcePlan plan = fixturePlan();
    const auto a = offset(200, 0), b = offset(400, 0), c = offset(600, 0);
    SourceItem loiter = item(1, 17, 3, a.first, a.second, 40, 1);
    loiter.params[2] = 1e7;
    SourceItem toAlt = item(2, 31, 3, b.first, b.second, 40, 1);
    toAlt.params[1] = -1e7;
    plan.items << loiter << toAlt << item(3, 16, 3, c.first, c.second, 40, 1);

    SourcePlan plain = plan;
    plain.items[0].params[2] = 0;
    plain.items[1].params[1] = 0;
    const GeoBounds wide = Builder::bounds(plan), normal = Builder::bounds(plain);
    QVERIFY(wide.valid());
    QCOMPARE(wide.north, normal.north);
    QCOMPARE(wide.east, normal.east);

    QElapsedTimer t;
    t.start();
    const Scene sc = Builder::build(plan, flatFixture(20), 2.0);   // S = 1 m, the worst case
    QVERIFY(t.elapsed() < 5000);
    QVERIFY(sc.lines.size() < 2000);
    QVERIFY(!sc.overSampleBudget);
    QCOMPARE(sc.limitWarnings.filter(QStringLiteral("loiter radius")).size(), 2);
    QCOMPARE(sc.limitWarnings.filter(QStringLiteral("10 km")).size(), 2);
    checkLimitWording(sc);
    // The legs are drawn and classed as usual: 200 m at S = 1 m.
    QCOMPARE(countVisual(sc, LineKind::Path, 3), 200);
    QCOMPARE(countKind(sc, LineKind::Unknown), 0);
    QCOMPARE(countMarkers(sc, MarkerKind::Waypoint), 3);
    QVERIFY(allFinite(sc));
}

void Plan3DTest::_boundsLoiterRingTooManyPoints()
{
    // A 5 km loiter (under the radius limit) on a 2 m ground cell (S = 1 m)
    // would need about 31 400 ring points: it is drawn with
    // kMaxLoiterRingPoints, violet, never classed, and the view says so.
    SourcePlan plan = fixturePlan();
    const auto a = offset(200, 0);
    SourceItem loiter = item(1, 17, 3, a.first, a.second, 40, 1);
    loiter.params[2] = 5000;
    plan.items << loiter;
    const Scene sc = Builder::build(plan, flatFixture(20), 2.0);
    QCOMPARE(countVisual(sc, LineKind::Unknown, 1), kMaxLoiterRingPoints);
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Unknown) {
            QCOMPARE(l.clearance, Clearance::Unknown);
        }
    }
    QCOMPARE(sc.limitWarnings.filter(QStringLiteral("loiter circle")).size(), 1);
    checkLimitWording(sc);

    // The same loiter at the 15 m cap fits (2095 points) and is classed as usual.
    const Scene ok = Builder::build(plan, flatFixture(20));
    QVERIFY(ok.limitWarnings.isEmpty());
    QCOMPARE(countVisual(ok, LineKind::Unknown, 1), 0);
    QVERIFY(countVisual(ok, LineKind::Path, 1) >= 2094);
}

void Plan3DTest::_boundsBeyondExtent()
{
    // Rev 9: an item more than 200 km from home (here about 18 000 km, the
    // review's example) and every leg into or out of it are one violet line
    // each, its marker violet, the ground there never used, and the ground
    // area not widened. Items inside the extent draw as usual. The spline
    // variant takes the same path before any refinement.
    for (const bool spline : {false, true}) {
        SourcePlan plan = fixturePlan();
        const auto a = offset(100, 0), c = offset(300, 0);
        const auto edgeIn = offset(0, 199000), edgeOut = offset(0, 201000);
        const int cmd = spline ? 82 : 16;
        plan.items << item(1, cmd, 3, a.first, a.second, 40, 1)
                   << item(2, cmd, 3, -37.7679, 57.5, 40, 1)
                   << item(3, cmd, 3, c.first, c.second, 40, 1)
                   << item(4, 16, 3, edgeIn.first, edgeIn.second, 40, 1)
                   << item(5, 16, 3, edgeOut.first, edgeOut.second, 40, 1);
        plan.rally << SourcePoint{-37.7679, 57.5, 30};
        SourceItem roi = item(6, 201, 3, -37.7679, 57.5, 30, 1);
        plan.items << roi;

        QElapsedTimer t;
        t.start();
        const Scene sc = Builder::build(plan, flatFixture(20));
        QVERIFY(t.elapsed() < 5000);
        QVERIFY(!sc.overSampleBudget);
        QCOMPARE(countVisual(sc, LineKind::Unknown, 2), 1);     // leg into 2
        QCOMPARE(countVisual(sc, LineKind::Unknown, 3), 1);     // leg out of 2, into 3
        QCOMPARE(countVisual(sc, LineKind::Path, 2), 0);
        QCOMPARE(countVisual(sc, LineKind::Path, 3), 0);
        QVERIFY(neverNoStrike(sc, 2));
        QCOMPARE(sc.limitWarnings.filter(QStringLiteral("Item 2 is more than 200 km from home")).size(), 1);
        QCOMPARE(sc.limitWarnings.filter(QStringLiteral("Item 5 is more than 200 km from home")).size(), 1);
        QCOMPARE(sc.limitWarnings.filter(QStringLiteral("Item 4 is more than")).size(), 0);
        QCOMPARE(sc.limitWarnings.filter(QStringLiteral("Rally 1 is more than 200 km from home")).size(), 1);
        QCOMPARE(sc.limitWarnings.filter(QStringLiteral("ROI 6 is more than 200 km from home")).size(), 1);
        checkLimitWording(sc);
        // Item 4 is inside the extent: the leg into it (from 3, 199 km) is
        // sampled and classed; item 5 just outside is violet.
        QVERIFY(countVisual(sc, LineKind::Path, 4) >= kSamplesPerLeg);
        QCOMPARE(countVisual(sc, LineKind::Unknown, 5), 1);
        QCOMPARE(countVisual(sc, LineKind::Path, 5), 0);
        // The leg into 1 (from home) is sampled, or, for a spline bent wide
        // by the far control point, violet in part or as one line; never no
        // strike beyond the extent.
        QVERIFY(countVisual(sc, LineKind::Path, 1) >= kSamplesPerLeg || countVisual(sc, LineKind::Unknown, 1) == 1);
        // Far rally point and ROI: violet (ground beyond the extent is unknown).
        for (const Marker &m : sc.markers) {
            if (m.kind == MarkerKind::Rally || m.kind == MarkerKind::Roi) {
                QCOMPARE(m.clearance, Clearance::Unknown);
            }
        }
        QVERIFY(allFinite(sc));
    }
    // The ground area: the far items do not widen it.
    SourcePlan nearOnly = fixturePlan();
    const auto a = offset(100, 0);
    nearOnly.items << item(1, 16, 3, a.first, a.second, 40, 1);
    SourcePlan withFar = nearOnly;
    withFar.items << item(2, 16, 3, -37.7679, 57.5, 40, 1);
    withFar.rally << SourcePoint{-37.7679, 57.5, 30};
    const GeoBounds bn = Builder::bounds(nearOnly), bf = Builder::bounds(withFar);
    QCOMPARE(bf.south, bn.south);
    QCOMPARE(bf.east, bn.east);
}

void Plan3DTest::_boundsLegBackstop()
{
    // Rev 9: with no planned home there is no extent; a leg longer than
    // 400 km is one violet line, and its neighbors are drawn as usual.
    SourcePlan plan = fixturePlan();
    plan.homeLat = kNaN;
    plan.homeLon = kNaN;
    const auto a = offset(0, 0), b = offset(0, 500000), c = offset(0, 500300);
    plan.items << item(1, 16, 0, a.first, a.second, 60, 2)
               << item(2, 16, 0, b.first, b.second, 60, 2)
               << item(3, 16, 0, c.first, c.second, 60, 2);
    const Scene sc = Builder::build(plan, flatFixture(20));
    QCOMPARE(countVisual(sc, LineKind::Unknown, 2), 1);
    QCOMPARE(countVisual(sc, LineKind::Path, 2), 0);
    QCOMPARE(sc.limitWarnings.filter(QStringLiteral("Item 2: the leg into it is longer than 400 km")).size(), 1);
    checkLimitWording(sc);
    QVERIFY(countVisual(sc, LineKind::Path, 3) >= kSamplesPerLeg);
    QVERIFY(allFinite(sc));
}

void Plan3DTest::_boundsSampleCapPerLeg()
{
    // The code cap: a 150 km leg (inside the extent and the backstop) on a
    // 2 m ground cell (S = 1 m) needs 150 000 samples, over kMaxSamplesPerLeg:
    // one violet line, never classed, with a warning.
    SourcePlan plan = fixturePlan();
    const auto a = offset(0, 100), b = offset(0, 150100);
    plan.items << item(1, 16, 3, a.first, a.second, 60, 1) << item(2, 16, 3, b.first, b.second, 60, 1);
    QElapsedTimer t;
    t.start();
    const Scene sc = Builder::build(plan, flatFixture(20), 2.0);
    QVERIFY(t.elapsed() < 5000);
    QCOMPARE(countVisual(sc, LineKind::Unknown, 2), 1);
    QCOMPARE(countVisual(sc, LineKind::Path, 2), 0);
    QCOMPARE(sc.limitWarnings.filter(QStringLiteral("Item 2: the leg into it is too long to sample")).size(), 1);
    checkLimitWording(sc);
    QCOMPARE(countVisual(sc, LineKind::Path, 1), 100);   // home to item 1, 100 m at S = 1 m
}

void Plan3DTest::_boundsHugeHeights()
{
    // Rev 9: heights beyond +-100 km (AMSL, above launch, terrain frame,
    // and the planned home itself) are unknown, drawn at the limit, violet,
    // with a warning.
    SourcePlan plan = fixturePlan();
    const auto a = offset(100, 0), b = offset(200, 0), c = offset(300, 0), d = offset(400, 0);
    plan.items << item(1, 16, 0, a.first, a.second, 1e308, 2)
               << item(2, 16, 3, b.first, b.second, -2e5, 1)
               << item(3, 16, 10, c.first, c.second, 5e5, 4)
               << item(4, 16, 3, d.first, d.second, 99000, 1);
    plan.rally << SourcePoint{a.first, a.second, 1e9};
    const Scene sc = Builder::build(plan, flatFixture(20));
    for (const int seq : {1, 2, 3}) {
        QCOMPARE(sc.limitWarnings.filter(QStringLiteral("Item %1: its height is beyond the 100 km drawing limit").arg(seq)).size(), 1);
        const Marker *m = markerForSeq(sc, seq);
        QVERIFY(m);
        QCOMPARE(m->clearance, Clearance::Unknown);
        QVERIFY(std::abs(m->pos.y) <= kMaxDrawnHeightM + 1000);
        QVERIFY(neverNoStrike(sc, seq));
    }
    QCOMPARE(sc.limitWarnings.filter(QStringLiteral("Rally 1: its height is beyond")).size(), 1);
    // 99 km above launch is inside the limit: classed as usual.
    QCOMPARE(sc.limitWarnings.filter(QStringLiteral("Item 4:")).size(), 0);
    QCOMPARE(markerForSeq(sc, 4)->clearance, Clearance::NoStrike);
    checkLimitWording(sc);
    QVERIFY(allFinite(sc));
    for (const LineSeg &l : sc.lines) {
        QVERIFY(std::abs(l.a.y) <= kMaxDrawnHeightM + 1000 && std::abs(l.b.y) <= kMaxDrawnHeightM + 1000);
    }

    // A planned home altitude of 1e308 with no ground loaded: the launch
    // reference is unknown (A-4) and every relative height with it.
    SourcePlan home = fixturePlan();
    home.homeAmsl = 1e308;
    home.items << item(1, 16, 3, a.first, a.second, 40, 1);
    const Scene hs = Builder::build(home, unknownGround());
    QVERIFY(!std::isfinite(hs.launchRef));
    QCOMPARE(hs.limitWarnings.filter(QStringLiteral("planned home altitude is beyond the 100 km drawing limit")).size(), 1);
    checkLimitWording(hs);
    QVERIFY(allFinite(hs));
}

void Plan3DTest::_boundsManyItems()
{
    // 25 000 waypoints 100 m apart need over 600 000 drawn samples: the
    // scene stops at kMaxSceneSamples, the rest is not drawn, and the view
    // says so.
    SourcePlan plan = fixturePlan();
    for (int i = 0; i < 25000; i++) {
        const auto p = offset(100.0 * (i / 150), 100.0 * (i % 150));
        plan.items << item(i + 1, 16, 3, p.first, p.second, 40, 1);
    }
    QElapsedTimer t;
    t.start();
    const Scene sc = Builder::build(plan, flatFixture(20));
    qInfo() << "25 000 items: Builder::build (this build type):" << t.elapsed() << "ms," << sc.lines.size() << "drawn samples";
    QVERIFY(t.elapsed() < 30000);
    QVERIFY(sc.overSampleBudget);
    QVERIFY(sc.lines.size() <= kMaxSceneSamples);
    QVERIFY(sc.lines.size() > kMaxSceneSamples / 2);
    QCOMPARE(sc.limitWarnings.filter(QStringLiteral("too large to draw in full")).size(), 1);
    checkLimitWording(sc);
    // Nothing past the budget: the last item's leg is not drawn.
    QCOMPARE(countVisual(sc, LineKind::Path, 25000), 0);
}

void Plan3DTest::_boundsNanAndInfValues()
{
    // NaN and infinity in coordinates, heights and radii; 1e308 heights and
    // radii; a fence and an outline with bad vertices. Builds, bounded,
    // every drawn vertex finite.
    const double inf = std::numeric_limits<double>::infinity();
    SourcePlan plan = fixturePlan();
    const auto a = offset(100, 0), b = offset(200, 0), c = offset(300, 0);
    plan.items << item(1, 16, 3, kNaN, a.second, 40, 1)
               << item(2, 16, 3, a.first, inf, 40, 1)
               << item(3, 16, 3, -inf, kNaN, 40, 1)
               << item(4, 16, 3, a.first, a.second, kNaN, 1)
               << item(5, 16, 3, b.first, b.second, inf, 1)
               << item(6, 16, 0, c.first, c.second, -inf, 2)
               << item(7, 16, 3, a.first, a.second, 1e308, 1);
    SourceItem l1 = item(8, 17, 3, b.first, b.second, 40, 1);
    l1.params[2] = inf;
    SourceItem l2 = item(9, 18, 3, c.first, c.second, 40, 1);
    l2.params[2] = kNaN;
    SourceItem l3 = item(10, 19, 3, a.first, a.second, 40, 1);
    l3.params[2] = 1e308;
    SourceItem l4 = item(11, 31, 3, b.first, b.second, 40, 1);
    l4.params[1] = -1e308;
    plan.items << l1 << l2 << l3 << l4 << item(12, 16, 3, 1e308, -1e308, 40, 1);

    SourceFence fence;
    fence.polygon << QPair<double, double>(a.first, a.second) << QPair<double, double>(kNaN, b.second)
                  << QPair<double, double>(c.first, inf) << QPair<double, double>(b.first, b.second);
    SourceFence circle;
    circle.circle = true;
    circle.centerLat = a.first;
    circle.centerLon = a.second;
    circle.radiusM = 1e308;
    plan.fences << fence << circle;
    SourceOutline outline;
    outline.path << QPair<double, double>(a.first, a.second) << QPair<double, double>(inf, -inf)
                 << QPair<double, double>(c.first, c.second);
    plan.outlines << outline;
    plan.rally << SourcePoint{kNaN, a.second, 10} << SourcePoint{a.first, a.second, inf};
    plan.breachReturn = SourcePoint{inf, kNaN, 0};

    for (const GroundFn &g : {flatFixture(20), unknownGround()}) {
        const Scene sc = Builder::build(plan, g, kNaN);
        QVERIFY(sc.lines.size() < 20000);
        QVERIFY(!sc.overSampleBudget);
        QVERIFY(allFinite(sc));
        QCOMPARE(sc.limitWarnings.filter(QStringLiteral("loiter radius")).size(), 2);   // 1e308 and -1e308
        checkLimitWording(sc);
        const Marker *m7 = markerForSeq(sc, 7);
        QVERIFY(m7);
        QCOMPARE(m7->clearance, Clearance::Unknown);   // 1e308 above launch: unknown, drawn at the limit
        QVERIFY(std::abs(m7->pos.y) <= kMaxDrawnHeightM + 1000);
    }
    // A planned home that is not a position: the middle of the plan is the
    // reference, there is no extent, and it still builds.
    SourcePlan noHome = plan;
    noHome.homeLat = kNaN;
    noHome.homeLon = inf;
    const Scene sc = Builder::build(noHome, flatFixture(20));
    QVERIFY(allFinite(sc));
}

void Plan3DTest::_boundsLineMeshBudget()
{
    // The GPU side sizes its buffers in qsizetype and draws nothing over the
    // scene budget (the builder never hands it more; this is the guard).
    LineMesh mesh;
    QList<LineMesh::Seg> segs(3);
    for (int i = 0; i < segs.size(); i++) {
        segs[i].a = QVector3D(i, 0, 0);
        segs[i].b = QVector3D(i + 1, 0, 0);
        segs[i].color = Qt::white;
    }
    mesh.setSegments(segs);
    QCOMPARE(mesh.segmentCount(), 3);
    QCOMPARE(mesh.vertexData().size(), qsizetype(3 * 4 * 14 * sizeof(float)));
    QCOMPARE(mesh.indexData().size(), qsizetype(3 * 6 * sizeof(quint32)));

    QList<LineMesh::Seg> over(kMaxSceneSamples + 1);
    mesh.setSegments(over);
    QCOMPARE(mesh.segmentCount(), 0);
    QCOMPARE(mesh.vertexData().size(), qsizetype(0));
    QCOMPARE(mesh.indexData().size(), qsizetype(0));
}

void Plan3DTest::_boundsCorpusUnderLimits()
{
    // No corpus plan reaches a limit, at the 15 m cap or at the sample
    // spacing of its own loaded ground grid (129 x 129): what they draw is unchanged.
    const QStringList files = QDir(QStringLiteral(RL_PLAN_CORPUS_DIR)).entryList({QStringLiteral("*.plan")}, QDir::Files, QDir::Name);
    QCOMPARE(files.size(), 17);
    auto check = [](const QString &f, const SourcePlan &plan) {
        for (const double cell : {kNaN, Builder::gridCellM(Builder::bounds(plan), 129, 129)}) {
            const Scene sc = Builder::build(plan, flatFixture(20), cell);
            QVERIFY2(sc.limitWarnings.isEmpty(), qPrintable(f + sc.limitWarnings.join(QStringLiteral(" | "))));
            QVERIFY2(!sc.overSampleBudget, qPrintable(f));
            QVERIFY2(sc.lines.size() < kMaxSceneSamples / 5, qPrintable(f));
        }
    };
    for (const QString &f : files) {
        if (f.contains(QStringLiteral("structure"))) {
            continue;   // G71: loaded through _loadStructure below
        }
        delete _pmc;
        _pmc = new PlanMasterController(this);
        _pmc->setFlyView(false);
        _pmc->start();
        SourcePlan plan; QJsonObject json;
        QVERIFY2(_load(f, plan, json), qPrintable(f));
        check(f, plan);
        _pmc->setDirty(false);
    }
    delete _pmc;
    _pmc = new PlanMasterController(this);
    _pmc->setFlyView(false);
    _pmc->start();
    SourcePlan plan; QJsonObject json;
    QVERIFY(_loadStructure(QStringLiteral("ardupilot-copter-structure.plan"), plan, json));
    check(QStringLiteral("ardupilot-copter-structure.plan"), plan);
    _pmc->setDirty(false);
}

// ---------------------------------------------------------------------------
// G67 (5.2.0 review must-fixes).

namespace {

/// Exposes the instance table the GPU receives.
class MarkerTableProbe : public MarkerInstances
{
public:
    using MarkerInstances::getInstanceBuffer;
};

} // namespace

void Plan3DTest::_markerColorRoundTrip()
{
    // Visual M1: the marker shader draws the declared sRGB color from the
    // instance data. Qt stores the instance color linear (an approximation),
    // and the unshaded marker wrote that out as is: breach return #f59e0b
    // drew as #e85700 and ROI #ec4899 as #d41051.
    QFile vert(QStringLiteral(":/rotorlab/plan3d/Plan3DMarker.vert"));
    QVERIFY(vert.open(QIODevice::ReadOnly));
    const QString vsrc = QString::fromUtf8(vert.readAll());
    QVERIFY(vsrc.contains(QStringLiteral("vColor = vec4(INSTANCE_DATA.yzw, 1.0);")));
    QVERIFY(!vsrc.contains(QStringLiteral("INSTANCE_COLOR")));
    QFile frag(QStringLiteral(":/rotorlab/plan3d/Plan3DMarker.frag"));
    QVERIFY(frag.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(frag.readAll()).contains(QStringLiteral("FRAGCOLOR = vec4(vColor.rgb * l, 1.0);")));

    const QStringList declared = {
        QStringLiteral("#f59e0b"),  // breach return, fence exclusion
        QStringLiteral("#ec4899"),  // ROI
        QStringLiteral("#b07cff"),  // unknown, dark
        QStringLiteral("#7c3aed"),  // unknown, light
        QStringLiteral("#64748b"),  // ground contact, light
        QStringLiteral("#94a3b8"),  // ground contact, dark
        QStringLiteral("#4d8bff"),  // waypoint
        QStringLiteral("#84cc16"),  // rally
        QStringLiteral("#f0b429"),  // selected
        QStringLiteral("#ff5544"),  // strike
        QStringLiteral("#f8fafc"),  // home, dark
        QStringLiteral("#0f172a"),  // home, light
    };
    for (const QString &hex : declared) {
        MarkerTableProbe probe;
        const QColor c(hex);
        probe.setInstances({MarkerInstances::Instance{QVector3D(1, 2, 3), c, 11.0f}});
        int n = 0;
        const QByteArray buf = probe.getInstanceBuffer(&n);
        QCOMPARE(n, 1);
        QCOMPARE(buf.size(), int(sizeof(QQuick3DInstancing::InstanceTableEntry)));
        const auto *e = reinterpret_cast<const QQuick3DInstancing::InstanceTableEntry *>(buf.constData());
        QCOMPARE(e->instanceData.x(), 11.0f);
        const double out[3] = {e->instanceData.y(), e->instanceData.z(), e->instanceData.w()};
        const double want[3] = {c.redF(), c.greenF(), c.blueF()};
        for (int k = 0; k < 3; k++) {
            QVERIFY2(std::abs(out[k] - want[k]) * 255.0 < 0.01,
                     qPrintable(QStringLiteral("%1 channel %2: %3, declared %4").arg(hex).arg(k).arg(out[k] * 255.0).arg(want[k] * 255.0)));
        }
    }
    // Why: the instance color really is not the declared one (amber's green
    // is stored near 87/255 instead of 158/255, the measured #e85700).
    MarkerTableProbe amber;
    amber.setInstances({MarkerInstances::Instance{QVector3D(), QColor(QStringLiteral("#f59e0b")), 11.0f}});
    int n = 0;
    const QByteArray abuf = amber.getInstanceBuffer(&n);
    const auto *e = reinterpret_cast<const QQuick3DInstancing::InstanceTableEntry *>(abuf.constData());
    QVERIFY(std::abs(e->color.y() * 255.0 - 158.0) > 40.0);
}

void Plan3DTest::_strikePlacesNamed()
{
    // Visual M2, accessibility M2: each item, leg and rally point that meets
    // the terrain is named in words, not only drawn red.
    const auto a = offset(100, 0), b = offset(200, 0), c = offset(300, 0);
    SourcePlan plan = fixturePlan();
    plan.items << item(1, 16, 3, a.first, a.second, 40, 1)
               << item(2, 16, 3, b.first, b.second, 0.5, 1)     // 0.5 m above the ground: a strike
               << item(3, 16, 3, c.first, c.second, -5, 1);     // 5 m below the ground
    plan.rally << SourcePoint{a.first, a.second + 0.001, 0};    // 0 m above launch
    const Scene sc = Builder::build(plan, flatFixture(20));
    const QStringList places = strikePlacesOf(sc);
    QCOMPARE(places, (QStringList{
        QStringLiteral("item 2"), QStringLiteral("leg 1 to 2"),
        QStringLiteral("item 3 (5.0 m below the loaded ground)"), QStringLiteral("leg 2 to 3"),
        QStringLiteral("Rally 1")}));
    // The red rally point says why it is red (visual M5).
    QCOMPARE(sc.warnings.filter(QStringLiteral("Rally 1 is 0 m above launch, within 2 m of the loaded ground, and is drawn red.")).size(), 1);
    for (const QString &w : sc.warnings) {
        QVERIFY2(!w.contains(QStringLiteral("(A-")) && !w.contains(QStringLiteral("(S-")), qPrintable(w));
    }

    // From home: the climb leaves the ground at launch (contact, CL-1a), then
    // meets a step in the ground 20 m out.
    const LocalFrame f{37.7679, -122.4934, 0};
    const GroundFn step = [f](double lat, double lon) { return std::hypot(f.east(lon), f.north(lat)) < 20 ? 20.0 : 45.0; };
    SourcePlan climb = fixturePlan();
    climb.items << item(1, 16, 3, a.first, a.second, 40, 1);
    QCOMPARE(strikePlacesOf(Builder::build(climb, step)), QStringList{QStringLiteral("leg from home to 1")});

    // No strike: nothing named.
    SourcePlan high = fixturePlan();
    high.items << item(1, 16, 3, a.first, a.second, 40, 1);
    QVERIFY(strikePlacesOf(Builder::build(high, flatFixture(20))).isEmpty());

    // Contract rev 9 limits: a part past a drawing limit is violet and never
    // named as meeting the terrain, even at a height that would be a strike.
    const auto far = offset(300000, 0);
    SourcePlan limits = fixturePlan();
    limits.items << item(1, 16, 3, a.first, a.second, 40, 1)
                 << item(2, 16, 3, far.first, far.second, 0.5, 1)        // more than 200 km from home
                 << item(3, 16, 3, b.first, b.second, -200000, 1);      // beyond the 100 km height limit
    const Scene ls = Builder::build(limits, flatFixture(20));
    QVERIFY(!ls.limitWarnings.isEmpty());
    for (const QString &place : strikePlacesOf(ls)) {
        QVERIFY2(!place.contains(QStringLiteral("2")) && !place.contains(QStringLiteral("3")), qPrintable(place));
    }
    for (const QString &w : ls.limitWarnings) {
        QVERIFY2(!w.contains(QStringLiteral("(A-")) && !w.contains(QStringLiteral("(CL-")) && !w.contains(QStringLiteral("rev 9")), qPrintable(w));
    }

    // Every corpus plan, on ground 15 m above the plan's home: a strike is
    // named exactly when there is one, and always by item, leg or pattern.
    const QStringList files = QDir(QStringLiteral(RL_PLAN_CORPUS_DIR)).entryList({QStringLiteral("*.plan")}, QDir::Files, QDir::Name);
    int withStrike = 0;
    for (const QString &f : files) {
        if (f.contains(QStringLiteral("structure"))) {
            continue;   // GC 5.1.1 Debug load (G71); the scan is covered by its own test
        }
        delete _pmc;
        _pmc = new PlanMasterController(this);
        _pmc->setFlyView(false);
        _pmc->start();
        SourcePlan p; QJsonObject json;
        QVERIFY2(_load(f, p, json), qPrintable(f));
        for (double groundM : {p.homeAmsl, p.homeAmsl + 15.0, p.homeAmsl + 60.0}) {
            const Scene s = Builder::build(p, flatFixture(std::isfinite(groundM) ? groundM : 20.0));
            const bool strikeMarker = std::any_of(s.markers.cbegin(), s.markers.cend(), [](const Marker &m) { return m.clearance == Clearance::Strike; });
            const QStringList named = strikePlacesOf(s);
            QVERIFY2((s.stats.samplesStrike > 0 || strikeMarker) == !named.isEmpty(), qPrintable(f + named.join(QStringLiteral(", "))));
            QVERIFY2(!named.contains(QStringLiteral("part of the route")), qPrintable(f));
            withStrike += named.isEmpty() ? 0 : 1;
        }
        _pmc->setDirty(false);
    }
    QVERIFY(withStrike > 0);
}

void Plan3DTest::_labelTextNamesClearance()
{
    // Accessibility M2: hover and selected labels carry the clearance state.
    Marker m;
    m.kind = MarkerKind::Waypoint;
    m.label = QStringLiteral("5");
    m.ground = 20;
    m.amsl = 20.5;
    m.clearance = Clearance::Strike;
    QCOMPARE(markerLabelText(m), QStringLiteral("5  0.5 m above ground: meets the terrain or within 2 m of the ground"));
    m.amsl = 18.2;
    QCOMPARE(markerLabelText(m), QStringLiteral("5  1.8 m below the ground: meets the terrain or within 2 m of the ground"));
    m.amsl = 60;
    m.clearance = Clearance::NoStrike;
    QCOMPARE(markerLabelText(m), QStringLiteral("5  40 m above ground"));
    m.amsl = 20.2;
    m.clearance = Clearance::GroundContact;
    QCOMPARE(markerLabelText(m), QStringLiteral("5  0.2 m above ground: ground contact (takeoff or landing)"));
    m.ground = kNaN;
    m.clearance = Clearance::Unknown;
    QCOMPARE(markerLabelText(m), QStringLiteral("5  unknown ground: not checked"));
    Marker breach;
    breach.kind = MarkerKind::BreachReturn;
    breach.label = QStringLiteral("Breach return (height not used)");
    breach.ground = 20;
    QCOMPARE(markerLabelText(breach), breach.label);
}

void Plan3DTest::_wp300FarHome()
{
    // G67 integration must-fix 1: GC crashed once with the 3D view open on
    // wp300 while the PX4 SITL home was about 12,800 km away (958,793 segments,
    // a 129 x 129 ground grid, 16,641 terrain tiles requested, 2.4 GB). With
    // rev 9 and the ground area cap the scene and the ground are bounded.
    const QString f = QStringLiteral("wp300-px4-copter-lawnmower.plan");
    SourcePlan plan; QJsonObject json;
    QVERIFY(_load(f, plan, json));
    plan.homeLat = -33.0;
    plan.homeLon = 151.0;     // eastern Australia
    const double lat1 = plan.homeLat * M_PI / 180, lat2 = plan.items.first().params[4] * M_PI / 180;
    const double dLon = (plan.items.first().params[5] - plan.homeLon) * M_PI / 180;
    const double km = 6371.0 * std::acos(std::sin(lat1) * std::sin(lat2) + std::cos(lat1) * std::cos(lat2) * std::cos(dLon));
    QVERIFY2(km > 11500 && km < 14000, qPrintable(QString::number(km)));

    for (const GroundFn &ground : {flatFixture(20), unknownGround()}) {
        const Scene sc = Builder::build(plan, ground);
        // Drawn samples within budget, and far from it.
        QVERIFY(!sc.overSampleBudget);
        QVERIFY2(sc.lines.size() < 5000, qPrintable(QString::number(sc.lines.size())));
        // Every leg and point of the mission is violet (unknown), never red or teal.
        for (const LineSeg &l : sc.lines) {
            if (l.kind == LineKind::Path || l.kind == LineKind::Unknown) {
                QCOMPARE(l.clearance, Clearance::Unknown);
            }
        }
        for (const Marker &m : sc.markers) {
            if (m.kind == MarkerKind::Waypoint || m.kind == MarkerKind::PatternPoint) {
                QCOMPARE(m.clearance, Clearance::Unknown);
            }
        }
        QCOMPARE(sc.stats.samplesStrike, 0);
        QCOMPARE(sc.stats.samplesNoStrike, 0);
        QVERIFY(strikePlacesOf(sc).isEmpty());
        // ... with the warning.
        QVERIFY2(!sc.limitWarnings.filter(QStringLiteral("km from home")).isEmpty(), qPrintable(sc.limitWarnings.join(QStringLiteral(" | "))));
    }

    // The ground GC loads: only around home, a few terrain tiles.
    const GeoBounds area = Builder::bounds(plan);
    QVERIFY(area.valid());
    const GroundGrid grid = GroundGrid::layout(area);
    QVERIFY(grid.valid());
    const int tiles = int(groupByTile(grid, 0.01).size());
    QVERIFY2(tiles <= 9, qPrintable(QString::number(tiles)));
    QVERIFY(grid.cols * grid.rows <= 400);
}

// ---------------------------------------------------------------------------
// Rev 11, section 4b: loiter legs as flown (L-1 to L-6).

namespace {

/// A fixed-wing plan: waypoint 1 west of the loiter, loiter 2 at C, then
/// item 3 at `next` (north, east of C in metres).
struct LoiterCase {
    SourcePlan  plan;
    Vec3        c;      ///< loiter center in the scene
};

LoiterCase loiterPlan(bool arduPilot, int command, double radius, double p1, double p2, double p4,
                      QPair<double, double> next = {500, 600})
{
    LoiterCase lc;
    lc.plan = fixturePlan();
    lc.plan.arduPilot = arduPilot;
    lc.plan.multiRotor = false;
    lc.plan.fixedWing = true;
    const auto a = offset(500, -600), c = offset(500, 0), n = offset(next.first, next.second);
    SourceItem loiter = item(2, command, 3, c.first, c.second, 60, 1);
    loiter.params[0] = p1;
    loiter.params[1] = command == 31 ? radius : p2;
    if (command != 31) {
        loiter.params[2] = radius;
    }
    loiter.params[3] = p4;
    lc.plan.items << item(1, 16, 3, a.first, a.second, 60, 1) << loiter << item(3, 16, 3, n.first, n.second, 60, 1);
    lc.c = LocalFrame{37.7679, -122.4934, 20}.toScene(c.first, c.second, 80);
    return lc;
}

double xzDist(const Vec3 &a, const Vec3 &b)
{
    return std::hypot(a.x - b.x, a.z - b.z);
}

/// The first drawn segment of the legs into `visualIndex`, or null.
const LineSeg *firstSeg(const Scene &sc, int visualIndex)
{
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Path && l.visualIndex == visualIndex) {
            return &l;
        }
    }
    return nullptr;
}

enum class Out { Tangent, Center, None };

/// Checks one case: the inbound leg ends on the ring (L-1), the ring radius
/// (L-2), and the outbound leg (L-3 to L-5).
void checkLoiter(const char *what, const LoiterCase &lc, double ringR, Out out, bool clockwise)
{
    const Scene sc = Builder::build(lc.plan, flatFixture(20));
    const QString w = QString::fromLatin1(what);
    // L-1: no drawn line ends at the center, unless the outbound leg starts there.
    for (const LineSeg &l : sc.lines) {
        if (l.kind != LineKind::Path) {
            continue;
        }
        QVERIFY2(xzDist(l.b, lc.c) > 0.5, qPrintable(w + QStringLiteral(": a line ends at the center")));
        if (out != Out::Center) {
            QVERIFY2(xzDist(l.a, lc.c) > 0.5, qPrintable(w + QStringLiteral(": a line starts at the center")));
        }
    }
    // L-1: the inbound leg (into item 2) ends on the ring, on the side it comes from (west).
    bool entry = false;
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Path && l.visualIndex == 2 && std::abs(xzDist(l.b, lc.c) - ringR) < 0.05
                && std::abs(l.b.x - (lc.c.x - ringR)) < 0.05) {
            entry = true;
        }
    }
    QVERIFY2(entry, qPrintable(w + QStringLiteral(": inbound leg does not end on the ring")));
    // L-2: the ring at its radius, sampled (L-6).
    int ring = 0;
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Path && l.visualIndex == 2 && std::abs(xzDist(l.a, lc.c) - ringR) < 0.05
                && std::abs(xzDist(l.b, lc.c) - ringR) < 0.05) {
            ring++;
        }
    }
    QVERIFY2(ring >= 48, qPrintable(w + QStringLiteral(": ring segments %1").arg(ring)));
    // Outbound.
    const LineSeg *o = firstSeg(sc, 3);
    if (out == Out::None) {
        QVERIFY2(!o, qPrintable(w + QStringLiteral(": an outbound leg is drawn")));
        QVERIFY(markerForSeq(sc, 3));
        return;
    }
    QVERIFY2(o, qPrintable(w + QStringLiteral(": no outbound leg")));
    QCOMPARE(countVisual(sc, LineKind::Path, 3) >= kSamplesPerLeg, true);    // L-6: sampled
    const Marker *n = markerForSeq(sc, 3);
    QVERIFY(n);
    if (out == Out::Center) {
        QVERIFY2(xzDist(o->a, lc.c) < 1e-6, qPrintable(w + QStringLiteral(": outbound does not start at the center")));
        return;
    }
    QVERIFY2(std::abs(xzDist(o->a, lc.c) - ringR) < 0.05, qPrintable(w + QStringLiteral(": outbound does not start on the ring")));
    // Tangent: the radius to T is at right angles to T -> N.
    const double dot = (o->a.x - lc.c.x) * (n->pos.x - o->a.x) + (o->a.z - lc.c.z) * (n->pos.z - o->a.z);
    QVERIFY2(std::abs(dot) < 1.0, qPrintable(w + QStringLiteral(": not tangent (%1)").arg(dot)));
    // Heading east to N: clockwise leaves from the north side, counter-clockwise from the south.
    const bool north = -o->a.z > -lc.c.z;
    QVERIFY2(north == clockwise, qPrintable(w + QStringLiteral(": wrong side for the turn direction")));
}

} // namespace

void Plan3DTest::_rev11LoiterLegs()
{
    const double nan = kNaN;
    // ArduPilot (L-3): 18, 19 and 31 always leave on the tangent, whatever param4 is.
    checkLoiter("AP 18 cw", loiterPlan(true, 18, 100, 1, 0, 0), 100, Out::Tangent, true);
    checkLoiter("AP 18 ccw", loiterPlan(true, 18, -100, 1, 0, 0), 100, Out::Tangent, false);
    checkLoiter("AP 31 p4 0", loiterPlan(true, 31, 100, 0, 0, 0), 100, Out::Tangent, true);
    checkLoiter("AP 31 p4 1 ccw", loiterPlan(true, 31, -100, 0, 0, 1), 100, Out::Tangent, false);
    // L-2: ArduPilot 19 flies WP_LOITER_RAD whatever param3 says: the default ring.
    checkLoiter("AP 19", loiterPlan(true, 19, 100, 30, 0, 0), 60, Out::Tangent, true);
    checkLoiter("AP 19 ccw", loiterPlan(true, 19, -100, 30, 0, 0), 60, Out::Tangent, false);
    // L-3: next item inside the ring: from the center.
    checkLoiter("AP 31 N inside", loiterPlan(true, 31, 100, 0, 0, 1, {530, 20}), 100, Out::Center, true);

    // PX4 (L-4): heading required -> tangent; else param4 1 -> tangent, 0 -> center.
    checkLoiter("PX4 31 heading", loiterPlan(false, 31, 100, 1, 0, 0), 100, Out::Tangent, true);
    checkLoiter("PX4 31 p4 1", loiterPlan(false, 31, 100, 0, 0, 1), 100, Out::Tangent, true);
    checkLoiter("PX4 31 p4 0", loiterPlan(false, 31, 100, 0, 0, 0), 100, Out::Center, true);
    checkLoiter("PX4 31 p4 nan", loiterPlan(false, 31, 100, 0, 0, nan), 100, Out::Center, true);
    checkLoiter("PX4 19 heading ccw", loiterPlan(false, 19, -100, 30, 1, 0), 100, Out::Tangent, false);
    checkLoiter("PX4 19 p4 1", loiterPlan(false, 19, 100, 30, 0, 1), 100, Out::Tangent, true);
    checkLoiter("PX4 19 p4 0", loiterPlan(false, 19, 100, 30, 0, 0), 100, Out::Center, true);

    // L-5: 17 has no outbound leg on either firmware.
    checkLoiter("AP 17", loiterPlan(true, 17, 100, 0, 0, 0), 100, Out::None, true);
    checkLoiter("PX4 17", loiterPlan(false, 17, 100, 0, 0, 0), 100, Out::None, true);

    // L-2 (settled): a radius of 0 or 1 is the aircraft's: the firmware's
    // default ring (ArduPlane WP_LOITER_RAD 60 m, PX4 NAV_LOITER_RAD 80 m),
    // labeled; checkLoiter also checks that T lies on that ring.
    for (const bool ap : {true, false}) {
        const double fwR = ap ? 60.0 : 80.0;
        for (const int cmd : {31, 19, 18}) {
            if (cmd == 18 && !ap) {
                continue;   // PX4 refuses 18 (L-5), checked below
            }
            for (const double r : {0.0, 1.0, -1.0}) {
                // 19 on PX4: heading required (param2), so it leaves on the tangent.
                const LoiterCase lc = loiterPlan(ap, cmd, r, cmd == 31 ? 1 : 30, cmd == 19 ? 1 : 0, 1);
                checkLoiter(qPrintable(QStringLiteral("radius %1 cmd %2 %3").arg(r).arg(cmd).arg(ap ? QStringLiteral("AP") : QStringLiteral("PX4"))),
                            lc, fwR, Out::Tangent, r >= 0);
                const Scene sc = Builder::build(lc.plan, flatFixture(20));
                const Marker *m = markerForSeq(sc, 2);
                QVERIFY(m);
                QVERIFY2(m->label.contains(QStringLiteral("radius set by the aircraft")), qPrintable(m->label));
            }
        }
        const Scene plain = Builder::build(loiterPlan(ap, 31, 100, 1, 0, 1).plan, flatFixture(20));
        const Marker *m = markerForSeq(plain, 2);
        QVERIFY(m);
        QVERIFY(!m->label.contains(QStringLiteral("radius set by the aircraft")));
    }

    // L-5: PX4 refuses 18 at upload: nothing past it, and the view says so.
    {
        const Scene sc = Builder::build(loiterPlan(false, 18, 100, 1, 0, 0).plan, flatFixture(20));
        QVERIFY(markerForSeq(sc, 2));
        QVERIFY(!markerForSeq(sc, 3));
        QVERIFY(!firstSeg(sc, 3));
        QCOMPARE(sc.warnings.filter(QStringLiteral("the autopilot does not accept this item")).size(), 1);
        const Scene ap = Builder::build(loiterPlan(true, 18, 100, 1, 0, 0).plan, flatFixture(20));
        QVERIFY(ap.warnings.filter(QStringLiteral("does not accept")).isEmpty());
    }

    // L-6: the ring and both legs are classed sample by sample: a ridge east of
    // the center makes the east half of the ring and the start of the outbound
    // leg red, while the west half stays teal.
    {
        const LoiterCase lc = loiterPlan(true, 31, 100, 0, 0, 1);
        const LocalFrame f{37.7679, -122.4934, 0};
        const GroundFn ridge = [f](double lat, double lon) {
            const double e = f.east(lon), n = f.north(lat);
            return (e > 60 && e < 200 && n > 300 && n < 700) ? 79.0 : 20.0;
        };
        const Scene sc = Builder::build(lc.plan, ridge);
        int red = 0, teal = 0;
        for (const LineSeg &l : sc.lines) {
            if (l.kind == LineKind::Path && l.visualIndex == 2 && std::abs(xzDist(l.a, lc.c) - 100) < 0.05) {
                red += l.clearance == Clearance::Strike ? 1 : 0;
                teal += l.clearance == Clearance::NoStrike ? 1 : 0;
            }
        }
        QVERIFY(red > 0 && teal > 0);
        bool outRed = false;
        for (const LineSeg &l : sc.lines) {
            outRed |= l.kind == LineKind::Path && l.visualIndex == 3 && l.clearance == Clearance::Strike;
        }
        QVERIFY(outRed);
    }

    // A multirotor keeps the earlier drawing (it holds its loiters at the point).
    {
        LoiterCase lc = loiterPlan(true, 31, 100, 0, 0, 1);
        lc.plan.multiRotor = true;
        lc.plan.fixedWing = false;
        const Scene sc = Builder::build(lc.plan, flatFixture(20));
        bool spoke = false;
        for (const LineSeg &l : sc.lines) {
            spoke |= l.kind == LineKind::Path && xzDist(l.b, lc.c) < 1e-6;
        }
        QVERIFY(spoke);
    }
}

void Plan3DTest::_rev11CorpusParity()
{
    // Rev 11 changes only the two landing corpus plans, and there only the
    // approach: the inbound leg ends on the ring and the approach starts at
    // the approach-start coordinate T. Every other corpus plan draws the same
    // samples with and without revision 11.
    const QStringList files = QDir(QStringLiteral(RL_PLAN_CORPUS_DIR)).entryList({QStringLiteral("*.plan")}, QDir::Files, QDir::Name);
    QCOMPARE(files.size(), 17);
    int landings = 0;
    for (const QString &f : files) {
        delete _pmc;
        _pmc = new PlanMasterController(this);
        _pmc->setFlyView(false);
        _pmc->start();
        SourcePlan plan; QJsonObject json;
        if (f.contains(QStringLiteral("structure"))) {
            QVERIFY2(_loadStructure(f, plan, json), qPrintable(f));
        } else {
            QVERIFY2(_load(f, plan, json), qPrintable(f));
        }
        SourcePlan before = plan;
        before.loitersAsFlown = false;
        for (const GroundFn &g : {flatFixture(20), unknownGround()}) {
            const Scene now = Builder::build(plan, g);
            const Scene old = Builder::build(before, g);
            const bool landing = f.contains(QStringLiteral("landing"));
            if (!landing) {
                QCOMPARE(now.lines.size(), old.lines.size());
                for (qsizetype i = 0; i < now.lines.size(); i++) {
                    const LineSeg &a = now.lines[i], &b = old.lines[i];
                    QVERIFY2(a.a.x == b.a.x && a.a.y == b.a.y && a.a.z == b.a.z && a.b.x == b.b.x && a.b.y == b.b.y && a.b.z == b.b.z
                                 && a.clearance == b.clearance && a.kind == b.kind && a.visualIndex == b.visualIndex,
                             qPrintable(f + QStringLiteral(" line %1").arg(i)));
                }
                QCOMPARE(now.markers.size(), old.markers.size());
                for (qsizetype i = 0; i < now.markers.size(); i++) {
                    QVERIFY2(now.markers[i].pos.x == old.markers[i].pos.x && now.markers[i].pos.z == old.markers[i].pos.z
                                 && now.markers[i].clearance == old.markers[i].clearance && now.markers[i].label == old.markers[i].label,
                             qPrintable(f + QStringLiteral(" marker %1").arg(i)));
                }
                QCOMPARE(now.stats.samplesStrike, old.stats.samplesStrike);
                QCOMPARE(now.stats.samplesNoStrike, old.stats.samplesNoStrike);
                QCOMPARE(now.stats.samplesUnknown, old.stats.samplesUnknown);
                QCOMPARE(now.stats.samplesGroundContact, old.stats.samplesGroundContact);
                QCOMPARE(now.warnings, old.warnings);
                continue;
            }
            // The landing patterns: the loiter center C (the 31 marker) is never
            // an end of a drawn line now (it was, for the inbound leg, before).
            const Marker *c = nullptr;
            for (const Marker &m : now.markers) {
                c = m.command == 31 ? &m : c;
            }
            QVERIFY(c);
            bool oldSpoke = false;
            for (const LineSeg &l : old.lines) {
                oldSpoke |= l.kind == LineKind::Path && xzDist(l.b, c->pos) < 1e-6;
            }
            QVERIFY2(oldSpoke, qPrintable(f));
            for (const LineSeg &l : now.lines) {
                if (l.kind == LineKind::Path) {
                    QVERIFY2(xzDist(l.a, c->pos) > 0.5 && xzDist(l.b, c->pos) > 0.5, qPrintable(f));
                }
            }
            // The approach starts at T, GC's approach-start (slope start)
            // coordinate, the tangent point of the pattern GC built. The file's
            // landingApproachCoordinate is the loiter center C.
            QJsonObject pattern;
            for (const QJsonValue &v : json[QStringLiteral("mission")].toObject()[QStringLiteral("items")].toArray()) {
                if (v.toObject()[QStringLiteral("type")].toString() == QStringLiteral("ComplexItem")) {
                    pattern = v.toObject();
                }
            }
            const QJsonArray ap = pattern[QStringLiteral("landingApproachCoordinate")].toArray();
            QCOMPARE(ap.size(), 3);
            QVERIFY2(xzDist(now.frame.toScene(ap[0].toDouble(), ap[1].toDouble(), 0), c->pos) < 0.5, qPrintable(f));
            const LandingComplexItem *landingItem = nullptr;
            QmlObjectListModel *vis = _pmc->missionController()->visualItems();
            for (int k = 0; vis && k < vis->count(); k++) {
                landingItem = landingItem ? landingItem : qobject_cast<const LandingComplexItem *>(vis->get(k));
            }
            QVERIFY2(landingItem, qPrintable(f));
            const QGeoCoordinate ss = landingItem->slopeStartCoordinate();
            const Vec3 t = now.frame.toScene(ss.latitude(), ss.longitude(), 0);
            QVERIFY2(std::abs(xzDist(t, c->pos) - 75.0) < 0.5, qPrintable(f));
            bool fromT = false;
            for (const LineSeg &l : now.lines) {
                if (l.kind == LineKind::Path && xzDist(l.a, t) < 1.0 && xzDist(l.b, c->pos) > xzDist(l.a, c->pos) + 1e-3) {
                    fromT = true;   // a line leaving the ring at T, away from C
                }
            }
            double best = 1e18;
            Vec3 bestA;
            for (const LineSeg &l : now.lines) {
                if (l.kind == LineKind::Path && std::abs(xzDist(l.a, c->pos) - 75.0) < 0.5 && xzDist(l.a, t) < best
                        && xzDist(l.b, c->pos) > xzDist(l.a, c->pos) + 1e-3) {
                    best = xzDist(l.a, t);
                    bestA = l.a;
                }
            }
            QVERIFY2(fromT, qPrintable(f + QStringLiteral(": T %1,%2 C %3,%4 nearest exit %5,%6 (%7 m)")
                                               .arg(t.x).arg(t.z).arg(c->pos.x).arg(c->pos.z).arg(bestA.x).arg(bestA.z).arg(best)));
            // The touchdown and its ground contact are unchanged.
            QCOMPARE(now.stats.samplesGroundContact > 0, old.stats.samplesGroundContact > 0);
            landings++;
        }
        _pmc->setDirty(false);
    }
    QCOMPARE(landings, 4);   // two landing plans, two grounds each
}

#endif  // RL_PLAN3D

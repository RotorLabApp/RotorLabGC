/****************************************************************************
 *
 * RotorLab GC Plan view 3D editing (G65, MG3) - see Plan3DEditTest.h.
 *
 ****************************************************************************/

#include "Plan3DEditTest.h"

#ifdef RL_PLAN3D

#include "Plan3DBuilder.h"
#include "Plan3DEditor.h"
#include "Plan3DMath.h"
#include "Plan3DSource.h"

#include "CameraSection.h"
#include "Fact.h"
#include "MissionController.h"
#include "PlanMasterController.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "SimpleMissionItem.h"
#include "UnitsSettings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QNetworkProxyFactory>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

using namespace Plan3D;

namespace {

QString corpusPath(const QString &fileName)
{
    return QDir(QStringLiteral(RL_PLAN_CORPUS_DIR)).filePath(fileName);
}

/// Every request goes to the discard port on loopback.
class DeadLocalProxy : public QNetworkProxyFactory
{
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &) override
    {
        return { QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 9) };
    }
};

/// Test fixture, not terrain: a gentle slope, 30 m at the corpus home,
/// rising about 1 m per 90 m to the east.
double slopeFixture(double lat, double lon)
{
    Q_UNUSED(lat)
    return 30.0 + 1000.0 * (lon - (-122.4934));
}

PlanMasterController *newController(QObject *parent)
{
    auto *pmc = new PlanMasterController(parent);
    pmc->setFlyView(false);
    pmc->start();
    return pmc;
}

SimpleMissionItem *simpleAt(PlanMasterController *pmc, int vi)
{
    QmlObjectListModel *items = pmc->missionController()->visualItems();
    return (vi > 0 && vi < items->count()) ? qobject_cast<SimpleMissionItem *>(items->get(vi)) : nullptr;
}

/// The first line that differs, for a readable failure.
QString firstDiff(const QByteArray &a, const QByteArray &b)
{
    const QList<QByteArray> la = a.split('\n'), lb = b.split('\n');
    for (int i = 0; i < std::max(la.size(), lb.size()); i++) {
        const QByteArray x = i < la.size() ? la[i] : QByteArray("<end>");
        const QByteArray y = i < lb.size() ? lb[i] : QByteArray("<end>");
        if (x != y) {
            return QStringLiteral("line %1: 3D %2 | 2D %3").arg(i + 1).arg(QString::fromUtf8(x.trimmed()), QString::fromUtf8(y.trimmed()));
        }
    }
    return QString();
}

} // namespace

void Plan3DEditTest::initTestCase()
{
    _proxyFactory = new DeadLocalProxy;
    QNetworkProxyFactory::setApplicationProxyFactory(_proxyFactory);
    // E-3 and E-9 (revision 10): 0.1 in the user's own unit; the suite runs
    // once in metres and once in feet (RL_G65_TEST_UNITS=feet). The unit is
    // set before the first mission item exists (GC fixes it per process).
    _feet = qgetenv("RL_G65_TEST_UNITS") == "feet";
    SettingsManager::instance()->unitsSettings()->verticalDistanceUnits()->setRawValue(
        _feet ? UnitsSettings::VerticalDistanceUnitsFeet : UnitsSettings::VerticalDistanceUnitsMeters);
    SettingsManager::instance()->unitsSettings()->horizontalDistanceUnits()->setRawValue(
        _feet ? UnitsSettings::HorizontalDistanceUnitsFeet : UnitsSettings::HorizontalDistanceUnitsMeters);
    qInfo() << "Plan3DEditTest units:" << (_feet ? "feet" : "metres");
}

void Plan3DEditTest::cleanupTestCase()
{
    QNetworkProxyFactory::setApplicationProxyFactory(nullptr);   // deletes ours
    _proxyFactory = nullptr;
}

void Plan3DEditTest::_snapRules()
{
    // E-3: 0.1 m and 1e-7 degrees.
    QCOMPARE(Editor::snapAltitude(45.37), 45.4);
    // M4: through the same decimal text 2D's field holds. The double nearest
    // -3.05 is -3.04999999999999982..., which 2D shows and types as -3.0.
    QCOMPARE(Editor::snapAltitude(-3.05), QString::number(-3.05, 'f', 1).toDouble());
    QCOMPARE(Editor::snapAltitude(-3.05), -3.0);
    QCOMPARE(Editor::snapAltitude(0.15), QString::number(0.15, 'f', 1).toDouble());
    QCOMPARE(Editor::snapAltitude(2.675), QString::number(2.675, 'f', 1).toDouble());
    QCOMPARE(Editor::snapAltitude(12.0), 12.0);
    QCOMPARE(Editor::snapDegrees(37.76801234567), QStringLiteral("37.7680123").toDouble());
    QCOMPARE(Editor::snapDegrees(-122.49340005), QStringLiteral("-122.4934001").toDouble());
    QVERIFY(std::isnan(Editor::snapAltitude(kNaN)));
}

void Plan3DEditTest::_altitudeForAmslPerMode()
{
    // E-2, per GC altitude mode; R = 41, G(p) = 52.
    QCOMPARE(Editor::altitudeForAmsl(1, 100, 41, 52), 59.0);   // relative: AMSL - R
    QCOMPARE(Editor::altitudeForAmsl(2, 100, 41, 52), 100.0);  // AMSL
    QCOMPARE(Editor::altitudeForAmsl(3, 100, 41, 52), 48.0);   // calculated above terrain: AMSL - G(p)
    QCOMPARE(Editor::altitudeForAmsl(4, 100, 41, 52), 48.0);   // terrain frame: AMSL - G(p)
    // Unknown references: no value, so no edit.
    QVERIFY(std::isnan(Editor::altitudeForAmsl(1, 100, kNaN, 52)));
    QVERIFY(std::isnan(Editor::altitudeForAmsl(3, 100, 41, kNaN)));
    QVERIFY(std::isnan(Editor::altitudeForAmsl(4, 100, 41, kNaN)));
    QCOMPARE(Editor::altitudeForAmsl(2, 100, kNaN, kNaN), 100.0);
    QVERIFY(std::isnan(Editor::altitudeForAmsl(0, 100, 41, 52)));   // mixed is not an item mode
}

void Plan3DEditTest::_editableItems()
{
    PlanMasterController *pmc = newController(this);
    pmc->loadFromFile(corpusPath(QStringLiteral("ardupilot-copter-alt-relative.plan")));
    Editor ed;
    ed.setPlanMasterController(pmc);
    ed.setReferences(slopeFixture, []() { return 30.0; });

    // Items: 0 home, 1 takeoff, 2..5 waypoints, 6 RTL.
    QVERIFY(!ed.canMove(0));
    QVERIFY(!ed.canChangeHeight(0));                  // the planned home is read only (E-4)
    QVERIFY(!ed.canMove(1));                          // takeoff: height only (E-4, A-11)
    QVERIFY(ed.canChangeHeight(1));
    QVERIFY(ed.canMove(2));
    QVERIFY(ed.canChangeHeight(2));
    QVERIFY(!ed.canMove(6));                          // RTL: no position, no height
    QVERIFY(!ed.canChangeHeight(6));

    // A refused edit changes nothing.
    const QByteArray before = pmc->saveToJson().toJson();
    QVERIFY(!ed.moveTo(1, 37.0, -122.0));
    QVERIFY(!ed.message().isEmpty());
    QCOMPARE(pmc->saveToJson().toJson(), before);

    // E-5: only the mission layer is editable.
    ed.setMissionLayer(false);
    QVERIFY(!ed.canMove(2));
    QVERIFY(!ed.canChangeHeight(2));
    QVERIFY(!ed.beginDrag(2));
    ed.setMissionLayer(true);
    QVERIFY(ed.beginDrag(2));
    QVERIFY(ed.dragging());
    ed.endDrag();
    QVERIFY(!ed.dragging());

    pmc->setDirty(false);
    delete pmc;
}

void Plan3DEditTest::_heightNeedsReference()
{
    // E-2: a height drag is off when the reference its mode needs is unknown;
    // a horizontal move is still allowed.
    PlanMasterController *pmc = newController(this);
    Editor ed;
    ed.setPlanMasterController(pmc);
    ed.setReferences([](double, double) { return kNaN; }, []() { return kNaN; });

    pmc->loadFromFile(corpusPath(QStringLiteral("ardupilot-copter-alt-mixed.plan")));
    // 2 relative, 3 AMSL, 4 terrain frame.
    QVERIFY(!ed.canChangeHeight(2));
    QVERIFY(ed.canMove(2));
    QVERIFY(ed.canChangeHeight(3));
    QVERIFY(!ed.canChangeHeight(4));
    QVERIFY(ed.canMove(4));
    QVERIFY(!ed.setHeightAmsl(2, 80));
    QVERIFY(!ed.message().isEmpty());

    // Known R, unknown ground: relative works, terrain frame does not.
    ed.setReferences([](double, double) { return kNaN; }, []() { return 30.0; });
    QVERIFY(ed.canChangeHeight(2));
    QVERIFY(!ed.canChangeHeight(4));

    pmc->setDirty(false);
    delete pmc;
}

void Plan3DEditTest::_parity(const QString &fileName, int maxItems, const QList<double> &expected)
{
    PlanMasterController *a = newController(this);    // edited through the 3D editor
    PlanMasterController *b = newController(this);    // edited the 2D way
    a->loadFromFile(corpusPath(fileName));
    b->loadFromFile(corpusPath(fileName));
    QTest::qWait(100);
    QCOMPARE(a->saveToJson().toJson(), b->saveToJson().toJson());

    const QGeoCoordinate home = a->missionController()->plannedHomePosition();
    // A-0: R as the scene computes it on the fixture ground (G(home), not H).
    const double R = Builder::build(sourceFromPlan(a), slopeFixture).launchRef;
    // The altitude field shows the user's unit; 2D types that (E-3, revision 10).
    QCOMPARE(simpleAt(a, 1)->altitude()->cookedUnits(), _feet ? QStringLiteral("ft") : QStringLiteral("m"));
    if (!expected.isEmpty()) {
        QCOMPARE(R, 30.0);     // G(home) on the fixture; the files' H is 20
    }
    Editor ed;
    ed.setPlanMasterController(a);
    ed.setReferences(slopeFixture, [R]() { return R; });

    int edits = 0;
    const int count = a->missionController()->visualItems()->count();
    for (int vi = 1; vi < count; vi++) {
        SimpleMissionItem *ia = simpleAt(a, vi);
        SimpleMissionItem *ib = simpleAt(b, vi);
        if (!ia || !ib) {
            continue;
        }
        bool edited = false;
        if (ed.canMove(vi)) {
            const double lat = ia->coordinate().latitude() + 0.00031234567;
            const double lon = ia->coordinate().longitude() + 0.00042345678;
            QVERIFY(ed.beginDrag(vi));
            QVERIFY(ed.moveTo(vi, lat, lon));
            ed.endDrag();
            // 2D: the drag handle sets the coordinate, keeping the altitude
            // (MissionItemIndicatorDrag.qml liveDrag); E-3's rounded value.
            QGeoCoordinate c(QString::number(lat, 'f', 7).toDouble(), QString::number(lon, 'f', 7).toDouble());
            c.setAltitude(ib->coordinate().altitude());
            ib->setCoordinate(c);
            edited = true;
        }
        if (ed.canChangeHeight(vi)) {
            const auto mode = ia->altitudeMode();
            const int frame = int(ia->missionItem().frame());
            const double amsl = 75.37 + 0.013 * vi;
            double lat = ib->coordinate().latitude(), lon = ib->coordinate().longitude();
            if (lat == 0 && lon == 0) {
                lat = home.latitude();      // A-11
                lon = home.longitude();
            }
            // The contract's formula, computed here on its own (E-2), typed with one decimal (E-3).
            double typed = kNaN;
            switch (mode) {
            case QGroundControlQmlGlobal::AltitudeModeRelative:         typed = amsl - R; break;
            case QGroundControlQmlGlobal::AltitudeModeAbsolute:         typed = amsl; break;
            case QGroundControlQmlGlobal::AltitudeModeCalcAboveTerrain:
            case QGroundControlQmlGlobal::AltitudeModeTerrainFrame:     typed = amsl - slopeFixture(lat, lon); break;
            default: QFAIL("unexpected item altitude mode");
            }
            QVERIFY(ed.setHeightAmsl(vi, amsl));
            // 2D: the altitude text field (FactTextField: fact.value = text),
            // in the unit it shows: metres, or feet (1 ft = 0.3048 m exactly).
            ib->altitude()->setCookedValue(QString::number(_feet ? typed / 0.3048 : typed, 'f', 1));
            // The mode and frame are never changed by an edit (E-2).
            QCOMPARE(ia->altitudeMode(), mode);
            QCOMPARE(int(ia->missionItem().frame()), frame);
            edited = true;
        }
        if (edited && ++edits == maxItems) {
            break;
        }
    }
    QVERIFY(edits > 0);

    // GC's queued recalculations and its refused terrain replies, both sides.
    QTest::qWait(300);
    const QByteArray pa = a->saveToJson().toJson();
    const QByteArray pb = b->saveToJson().toJson();
    QVERIFY2(pa == pb, qPrintable(fileName + QStringLiteral(": ") + firstDiff(pa, pb)));
    // And the edit did change the file.
    QFile f(corpusPath(fileName));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QVERIFY(QJsonDocument::fromJson(f.readAll()).toJson() != pa);
    // Hand-checked values (review section 4), in metres: saved Altitude and params[6].
    const QList<double> metresOnly = _feet ? QList<double>() : expected;
    const QJsonArray saved = QJsonDocument::fromJson(pa).object()[QStringLiteral("mission")].toObject()[QStringLiteral("items")].toArray();
    for (int vi = 1; vi < metresOnly.size(); vi++) {
        if (std::isnan(metresOnly[vi])) {
            continue;
        }
        const QJsonObject item = saved.at(vi - 1).toObject();     // these files have no section items
        QCOMPARE(item[QStringLiteral("Altitude")].toDouble(), metresOnly[vi]);
        QCOMPARE(item[QStringLiteral("params")].toArray().at(6).toDouble(), metresOnly[vi]);
    }
    if (_feet && !expected.isEmpty()) {
        // In feet, the stored height is the 0.1 ft value converted back, as 2D stores it.
        for (int vi = 1; vi < expected.size(); vi++) {
            if (std::isnan(expected[vi])) {
                continue;
            }
            const double stored = saved.at(vi - 1).toObject()[QStringLiteral("Altitude")].toDouble();
            QVERIFY2(std::abs(stored - expected[vi]) <= 0.05 + 0.0153, qPrintable(QStringLiteral("vi %1 stored %2").arg(vi).arg(stored, 0, 'g', 17)));
            const double feet = stored / 0.3048;
            QVERIFY(std::abs(feet * 10 - std::round(feet * 10)) < 1e-6);   // a 0.1 ft value
        }
    }
    qInfo() << fileName << "items edited both ways:" << edits << "saved files identical," << pa.size() << "bytes";

    a->setDirty(false);
    b->setDirty(false);
    delete a;
    delete b;
}

void Plan3DEditTest::_parity_ardupilot_copter_alt_relative()        { _parity(QStringLiteral("ardupilot-copter-alt-relative.plan"), 0, {kNaN, 45.4, 45.4, 45.4, 45.4, 45.4}); }
void Plan3DEditTest::_parity_ardupilot_copter_alt_amsl()            { _parity(QStringLiteral("ardupilot-copter-alt-amsl.plan")); }
void Plan3DEditTest::_parity_SYNTHETIC_TERRAIN_ardupilot_copter_alt_calc_above_terrain() { _parity(QStringLiteral("ardupilot-copter-alt-calc-above-terrain.plan")); }
void Plan3DEditTest::_parity_ardupilot_copter_alt_terrain_frame()   { _parity(QStringLiteral("ardupilot-copter-alt-terrain-frame.plan"), 0, {kNaN, 45.4, 43.3, 39.9, 39.9, 43.3}); }
void Plan3DEditTest::_parity_ardupilot_copter_alt_mixed()           { _parity(QStringLiteral("ardupilot-copter-alt-mixed.plan")); }
void Plan3DEditTest::_parity_px4_copter_alt_amsl()                  { _parity(QStringLiteral("px4-copter-alt-amsl.plan")); }
void Plan3DEditTest::_parity_ardupilot_copter_spline()              { _parity(QStringLiteral("ardupilot-copter-spline.plan"), 0, {kNaN, 45.4, 45.4, 45.4, 45.4, 45.4, 45.4, 45.5}); }
void Plan3DEditTest::_parity_wp300_px4_copter_lawnmower()           { _parity(QStringLiteral("wp300-px4-copter-lawnmower.plan"), 40); }

// ---------------------------------------------------------------- E-6 spline

namespace {

/// Loads the same file into two controllers; the first is edited in 3D.
struct Pair {
    PlanMasterController *a = nullptr;
    PlanMasterController *b = nullptr;
    Pair(QObject *parent, const QString &fileName)
    {
        a = newController(parent);
        b = newController(parent);
        a->loadFromFile(corpusPath(fileName));
        b->loadFromFile(corpusPath(fileName));
    }
    ~Pair()
    {
        a->setDirty(false);
        b->setDirty(false);
        delete a;
        delete b;
    }
    QString diff()
    {
        QTest::qWait(300);
        const QByteArray pa = a->saveToJson().toJson(), pb = b->saveToJson().toJson();
        return pa == pb ? QString() : firstDiff(pa, pb);
    }
};

/// 2D's reference for E-6: the command picker, then the altitude mode, the
/// altitude and the hold typed back as they were.
void splineThe2DWay(SimpleMissionItem *item, int command)
{
    const auto mode = item->altitudeMode();
    const QString altitude = QString::number(item->altitude()->rawValue().toDouble());
    const double hold = item->missionItem().param1();
    item->setCommand(command);
    item->setAltitudeMode(mode);
    item->altitude()->setCookedValue(altitude);
    item->missionItem().setParam1(QString::number(hold).toDouble());
}

} // namespace

void Plan3DEditTest::_splineOnlyArduPilotMultirotor()
{
    // S-2, S-3, E-6: offered for ArduPilot multirotor waypoints only.
    PlanMasterController *pmc = newController(this);
    Editor ed;
    ed.setPlanMasterController(pmc);

    pmc->loadFromFile(corpusPath(QStringLiteral("ardupilot-copter-alt-relative.plan")));
    QVERIFY(ed.splineBlockedReason(2).isEmpty());
    QVERIFY(!ed.splineBlockedReason(1).isEmpty());     // takeoff
    QVERIFY(!ed.splineBlockedReason(6).isEmpty());     // RTL

    pmc->loadFromFile(corpusPath(QStringLiteral("px4-copter-alt-amsl.plan")));
    QVERIFY(!ed.splineBlockedReason(2).isEmpty());
    QVERIFY(!ed.setSpline(2, true));
    QVERIFY(!ed.message().isEmpty());

    pmc->loadFromFile(corpusPath(QStringLiteral("ardupilot-plane-fw-landing.plan")));
    bool anyOffered = false;
    for (int vi = 1; vi < pmc->missionController()->visualItems()->count(); vi++) {
        anyOffered |= ed.splineBlockedReason(vi).isEmpty();
    }
    QVERIFY(!anyOffered);

    pmc->setDirty(false);
    delete pmc;
}

void Plan3DEditTest::_splineParity()
{
    if (_feet) {
        // E-6 writes the altitude it had back exactly; a 2D user in feet can
        // only retype the shown value, so the byte reference is metres only.
        QSKIP("E-6 parity reference is defined in metres");
    }
    Pair p(this, QStringLiteral("ardupilot-copter-spline.plan"));
    QTest::qWait(100);
    Editor ed;
    ed.setPlanMasterController(p.a);

    // Items: 1 takeoff, 2 waypoint, 3..6 spline waypoints, 7 waypoint, 8 RTL.
    // Give waypoint 2 a gimbal setting and a hold first, both the 2D way, so
    // the removal E-6 describes is in the file.
    for (PlanMasterController *pmc : {p.a, p.b}) {
        SimpleMissionItem *wp = simpleAt(pmc, 2);
        QVERIFY(wp && wp->command() == 16);
        wp->cameraSection()->setSpecifyGimbal(true);
        wp->cameraSection()->gimbalPitch()->setCookedValue(QStringLiteral("30"));   // 2D shows degrees down, positive
        wp->missionItem().setParam1(4);
    }
    QCOMPARE(p.diff(), QString());
    const double alt2 = simpleAt(p.a, 2)->altitude()->rawValue().toDouble();
    const auto mode2 = simpleAt(p.a, 2)->altitudeMode();

    QVERIFY(ed.setSpline(2, true));
    QVERIFY(ed.setSpline(3, false));
    QVERIFY(ed.setSpline(7, true));
    splineThe2DWay(simpleAt(p.b, 2), 82);
    splineThe2DWay(simpleAt(p.b, 3), 16);
    splineThe2DWay(simpleAt(p.b, 7), 82);

    QCOMPARE(p.diff(), QString());
    SimpleMissionItem *a2 = simpleAt(p.a, 2);
    QCOMPARE(a2->command(), 82);
    QCOMPARE(a2->altitude()->rawValue().toDouble(), alt2);     // written back (E-6)
    QCOMPARE(a2->altitudeMode(), mode2);
    QCOMPARE(a2->missionItem().param1(), 4.0);                 // hold written back
    QCOMPARE(a2->missionItem().param4(), 0.0);                 // S-4
    QVERIFY(!a2->cameraSection()->available());                // camera setting removed (E-6)
    // No DO_MOUNT_CONTROL left in the saved file.
    const QJsonArray items = p.a->saveToJson().object()[QStringLiteral("mission")].toObject()[QStringLiteral("items")].toArray();
    for (const QJsonValue &v : items) {
        QVERIFY(v.toObject()[QStringLiteral("command")].toInt() != 205);
    }
    QCOMPARE(simpleAt(p.a, 3)->command(), 16);
    // Back again: a straight leg into 2.
    QVERIFY(ed.setSpline(2, false));
    splineThe2DWay(simpleAt(p.b, 2), 16);
    QCOMPARE(p.diff(), QString());
}

// ---------------------------------------------------------------- E-7 heading

void Plan3DEditTest::_headingSnap()
{
    // E-3 (rev 6): 0.1 degree, 0 up to but not including 360.
    QCOMPARE(Editor::snapHeading(123.456), 123.5);
    QCOMPARE(Editor::snapHeading(-10), 350.0);
    QCOMPARE(Editor::snapHeading(400.04), 40.0);
    QCOMPARE(Editor::snapHeading(359.96), 0.0);
    QCOMPARE(Editor::snapHeading(720), 0.0);
    // F5: never -0 (saved as "-0", where 2D types 0).
    QCOMPARE(Editor::snapHeading(-0.04), 0.0);
    QVERIFY(!std::signbit(Editor::snapHeading(-0.04)));
    QVERIFY(!std::signbit(Editor::snapHeading(-360)));
    QCOMPARE(Editor::snapGimbal(-45.04, -90, 0), -45.0);
    QCOMPARE(Editor::snapGimbal(20, -90, 0), 0.0);
    QCOMPARE(Editor::snapGimbal(-120, -90, 0), -90.0);
}

void Plan3DEditTest::_headingParityPx4()
{
    // PX4: the waypoint's yaw, parameter 4, as 2D's Yaw field writes it.
    Pair p(this, QStringLiteral("px4-copter-alt-amsl.plan"));
    QTest::qWait(100);
    Editor ed;
    ed.setPlanMasterController(p.a);

    QVERIFY(!ed.headingBlockedReason(1).isEmpty());    // takeoff
    QVERIFY(ed.setHeading(2, 123.456));
    QVERIFY(ed.setHeading(3, -10));
    QVERIFY(ed.clearHeading(3));
    QCOMPARE(ed.heading(2), 123.5);
    QVERIFY(std::isnan(ed.heading(3)));
    QCOMPARE(p.a->missionController()->visualItems()->count(), p.b->missionController()->visualItems()->count());

    simpleAt(p.b, 2)->missionItem().setParam4(QStringLiteral("123.5").toDouble());
    simpleAt(p.b, 3)->missionItem().setParam4(QStringLiteral("350.0").toDouble());
    simpleAt(p.b, 3)->missionItem().setParam4(kNaN);
    QCOMPARE(p.diff(), QString());
}

void Plan3DEditTest::_headingParityArduPilot()
{
    // ArduPilot: a CONDITION_YAW (115) right after the waypoint, through
    // MissionController's insert, as 2D adds one; parameter 4 never written.
    Pair p(this, QStringLiteral("ardupilot-copter-alt-relative.plan"));
    QTest::qWait(100);
    Editor ed;
    ed.setPlanMasterController(p.a);
    const double p4Before = simpleAt(p.a, 2)->missionItem().param4();

    // Items: 1 takeoff, 2..5 waypoints, 6 RTL.
    QVERIFY(ed.setHeading(2, 400.04));      // adds 115 at 3; the waypoints move to 4..6
    QVERIFY(ed.setHeading(2, 90.06));       // edits that same item
    QVERIFY(ed.setHeading(4, 270));         // adds 115 at 5
    QVERIFY(ed.clearHeading(4));            // removes it
    QCOMPARE(ed.heading(2), 90.1);
    QVERIFY(std::isnan(ed.heading(4)));
    QCOMPARE(simpleAt(p.a, 2)->missionItem().param4(), p4Before);
    SimpleMissionItem *yaw = simpleAt(p.a, 3);
    QVERIFY(yaw && yaw->command() == 115);
    QCOMPARE(yaw->missionItem().param2(), 0.0);
    QCOMPARE(yaw->missionItem().param3(), 0.0);
    QCOMPARE(yaw->missionItem().param4(), 0.0);

    // 2D: insert after the waypoint, pick "Condition yaw", type the values.
    auto add2D = [&p](int vi, const QString &heading) {
        SimpleMissionItem *wp = simpleAt(p.b, vi);
        auto *item = qobject_cast<SimpleMissionItem *>(p.b->missionController()->insertSimpleMissionItem(wp->coordinate(), vi + 1, false));
        item->setCommand(115);
        item->missionItem().setParam1(heading.toDouble());
        item->missionItem().setParam2(0);
        item->missionItem().setParam3(0);
        item->missionItem().setParam4(0);
    };
    add2D(2, QStringLiteral("40.0"));
    simpleAt(p.b, 3)->missionItem().setParam1(QStringLiteral("90.1").toDouble());
    add2D(4, QStringLiteral("270.0"));
    p.b->missionController()->removeVisualItem(5);
    QCOMPARE(p.diff(), QString());
}

// ---------------------------------------------------------------- E-8 gimbal

void Plan3DEditTest::_gimbalParity()
{
    for (const QString &file : {QStringLiteral("px4-copter-alt-amsl.plan"), QStringLiteral("ardupilot-copter-alt-relative.plan")}) {
        Pair p(this, file);
        QTest::qWait(100);
        Editor ed;
        ed.setPlanMasterController(p.a);

        QVERIFY(!ed.gimbalBlockedReason(1).isEmpty());     // takeoff
        // Pitch: the camera section's gimbal pitch, clamped to its Fact (-90 to 0).
        QVERIFY(ed.setGimbalPitch(2, -45.04));
        QVERIFY(ed.setGimbalPitch(3, 20));
        QCOMPARE(ed.gimbalPitch(2), -45.0);
        QCOMPARE(ed.gimbalPitch(3), 0.0);
        // Yaw is not edited in 3D until the E-8 SITL check (F10).
        QVERIFY(!ed.setGimbalYaw(2, 10));
        QVERIFY(!ed.message().isEmpty());
        QVERIFY(ed.clearGimbal(3));
        QVERIFY(!ed.hasGimbal(3));
        QVERIFY(ed.setHeading(2, 90));
        QVERIFY(!ed.setGimbalYaw(2, 200));

        // 2D: the camera section's Gimbal box and fields; the heading as in _heading* tests.
        const bool px4 = file.startsWith(QStringLiteral("px4"));
        auto cs = [&p](int vi) { return simpleAt(p.b, vi)->cameraSection(); };
        cs(2)->setSpecifyGimbal(true);
        cs(2)->gimbalPitch()->setCookedValue(QStringLiteral("45.0"));     // 2D: 45 down = MAVLink -45
        cs(3)->setSpecifyGimbal(true);
        cs(3)->gimbalPitch()->setCookedValue(QStringLiteral("0.0"));
        cs(3)->setSpecifyGimbal(false);
        if (px4) {
            simpleAt(p.b, 2)->missionItem().setParam4(90);
        } else {
            auto *yaw = qobject_cast<SimpleMissionItem *>(p.b->missionController()->insertSimpleMissionItem(simpleAt(p.b, 2)->coordinate(), 3, false));
            yaw->setCommand(115);
            yaw->missionItem().setParam1(90);
            yaw->missionItem().setParam2(0);
            yaw->missionItem().setParam3(0);
            yaw->missionItem().setParam4(0);
        }
        QCOMPARE(p.diff(), QString());
    }
}

// ---------------------------------------------------------------- review fixes

void Plan3DEditTest::_syncGate()
{
    // E-5 and M2: no edit while the plan is sent to the vehicle, and a drag
    // running when the sync starts is ended. A MockLink ArduPilot vehicle
    // makes the sync real.
    _connectMockLink(MAV_AUTOPILOT_ARDUPILOTMEGA);
    PlanMasterController *pmc = newController(this);
    QSignalSpy sync(pmc, &PlanMasterController::syncInProgressChanged);
    for (int i = 0; i < 50 && pmc->syncInProgress(); i++) {
        sync.wait(200);     // the initial read from the vehicle, if any
    }
    pmc->loadFromFile(corpusPath(QStringLiteral("ardupilot-copter-alt-relative.plan")));
    Editor ed;
    ed.setPlanMasterController(pmc);
    ed.setReferences(slopeFixture, []() { return 30.0; });
    QVERIFY(ed.beginDrag(2));
    QVERIFY(ed.dragging());

    pmc->sendToVehicle();
    if (!pmc->syncInProgress()) {
        QVERIFY(sync.wait(2000));
    }
    QVERIFY(pmc->syncInProgress());
    QVERIFY(!ed.dragging());                    // M2: ended by the sync
    QVERIFY(!ed.canMove(2));
    QVERIFY(!ed.canChangeHeight(2));
    QVERIFY(!ed.moveTo(2, 37.77, -122.49));
    QVERIFY(!ed.message().isEmpty());
    for (int i = 0; i < 100 && pmc->syncInProgress(); i++) {
        sync.wait(200);
    }
    QVERIFY(!pmc->syncInProgress());
    QVERIFY(ed.canMove(2));

    pmc->setDirty(false);
    delete pmc;
    _disconnectMockLink();
}

void Plan3DEditTest::_modeFrameDisagreeRefused()
{
    // M1 (A-1): an item with mode 1 but frame 0 flies by its frame; 3D does
    // not change its height or its command, and says why.
    PlanMasterController *pmc = newController(this);
    pmc->loadFromFile(corpusPath(QStringLiteral("ardupilot-copter-alt-relative.plan")));
    Editor ed;
    ed.setPlanMasterController(pmc);
    ed.setReferences(slopeFixture, []() { return 30.0; });
    SimpleMissionItem *wp = simpleAt(pmc, 2);
    QCOMPARE(int(wp->altitudeMode()), 1);
    wp->missionItem().setFrame(MAV_FRAME_GLOBAL);     // fixture: mode 1, frame 0
    const QByteArray before = pmc->saveToJson().toJson();

    QVERIFY(ed.heightBlockedReason(2).contains(QStringLiteral("disagree")));
    QVERIFY(!ed.setHeightAmsl(2, 80));
    QVERIFY(ed.message().contains(QStringLiteral("disagree")));
    QVERIFY(ed.splineBlockedReason(2).contains(QStringLiteral("disagree")));
    QVERIFY(!ed.setSpline(2, true));
    QVERIFY(ed.canMove(2));                           // a horizontal move keeps the datum
    QCOMPARE(pmc->saveToJson().toJson(), before);
    QCOMPARE(int(wp->missionItem().frame()), int(MAV_FRAME_GLOBAL));
    QCOMPARE(int(wp->altitudeMode()), 1);

    pmc->setDirty(false);
    delete pmc;
}

void Plan3DEditTest::_dragBoundToItem()
{
    // M2: a drag is bound to the item object. A load ends it; an edit after
    // the list changed under it is refused and ends it.
    PlanMasterController *pmc = newController(this);
    pmc->loadFromFile(corpusPath(QStringLiteral("ardupilot-copter-alt-relative.plan")));
    Editor ed;
    ed.setPlanMasterController(pmc);
    ed.setReferences(slopeFixture, []() { return 30.0; });

    QVERIFY(ed.beginDrag(3));
    pmc->loadFromFile(corpusPath(QStringLiteral("ardupilot-copter-alt-relative.plan")));
    QVERIFY(!ed.dragging());
    QVERIFY(!ed.dragItem());

    QVERIFY(ed.beginDrag(3));
    SimpleMissionItem *dragged = ed.dragItem();
    QVERIFY(dragged && dragged == simpleAt(pmc, 3));
    pmc->missionController()->removeVisualItem(2);    // item 3 is now the old 4
    const QByteArray before = pmc->saveToJson().toJson();
    if (ed.dragging()) {
        QVERIFY(!ed.moveTo(3, 37.77, -122.49));       // refused, and the drag ends
    }
    QVERIFY(!ed.dragging());
    QCOMPARE(pmc->saveToJson().toJson(), before);     // nothing written to the wrong item

    pmc->setDirty(false);
    delete pmc;
}

void Plan3DEditTest::_relativeTurnNotEdited()
{
    // M3: a CONDITION_YAW that is relative (param4 = 1), or has its own rate
    // or direction, is not 3D's heading: not read as one, not edited.
    PlanMasterController *pmc = newController(this);
    pmc->loadFromFile(corpusPath(QStringLiteral("ardupilot-copter-alt-relative.plan")));
    Editor ed;
    ed.setPlanMasterController(pmc);

    auto *turn = qobject_cast<SimpleMissionItem *>(pmc->missionController()->insertSimpleMissionItem(simpleAt(pmc, 2)->coordinate(), 3, false));
    turn->setCommand(115);
    turn->missionItem().setParam1(30);
    turn->missionItem().setParam2(0);
    turn->missionItem().setParam3(0);
    turn->missionItem().setParam4(1);        // relative
    const QByteArray before = pmc->saveToJson().toJson();

    QVERIFY(std::isnan(ed.heading(2)));
    QVERIFY(!ed.yawItemAfter(2));
    QCOMPARE(ed.otherTurnAfter(2), turn);
    QVERIFY(ed.headingNote(2).contains(QStringLiteral("relative turn command")));
    QVERIFY(!ed.headingBlockedReason(2).isEmpty());
    QVERIFY(!ed.setHeading(2, 90));
    QVERIFY(ed.message().contains(QStringLiteral("relative turn command")));
    QVERIFY(!ed.clearHeading(2));
    QCOMPARE(pmc->saveToJson().toJson(), before);

    // Absolute but with a rate: also not 3D's.
    turn->missionItem().setParam4(0);
    turn->missionItem().setParam2(10);
    QVERIFY(std::isnan(ed.heading(2)));
    QVERIFY(!ed.setHeading(2, 90));
    // E-7's own form is 3D's.
    turn->missionItem().setParam2(0);
    QCOMPARE(ed.heading(2), 30.0);

    pmc->setDirty(false);
    delete pmc;
}

void Plan3DEditTest::_heldHeightDrawnUnknown()
{
    // F4 (CL-2): a mode 3 point held at its last height while GC's terrain
    // query is pending is drawn there, but unknown: never "no strike found",
    // no height above ground, and its legs unknown.
    SourcePlan plan;
    plan.homeLat = 37.7679;
    plan.homeLon = -122.4934;
    plan.homeAmsl = 100;
    plan.arduPilot = true;
    plan.multiRotor = true;
    auto item = [](int seq, double lat, double lon, double p7, bool held) {
        SourceItem s;
        s.seq = seq;
        s.visualIndex = seq;
        s.command = 16;
        s.frame = 0;
        s.altitudeMode = 3;
        s.params[0] = s.params[1] = s.params[2] = s.params[3] = 0;
        s.params[4] = lat;
        s.params[5] = lon;
        s.params[6] = p7;
        s.heightHeld = held;
        return s;
    };
    plan.items << item(1, 37.7689, -122.4934, 140, false) << item(2, 37.7699, -122.4934, 140, true) << item(3, 37.7709, -122.4934, 140, false);
    const Scene sc = Builder::build(plan, [](double, double) { return 100.0; });

    const Marker *held = nullptr;
    for (const Marker &m : sc.markers) {
        if (m.visualIndex == 2) {
            held = &m;
        }
    }
    QVERIFY(held);
    QVERIFY(held->heightPending);
    QVERIFY(held->clearance == Clearance::Unknown);
    QVERIFY(std::isnan(held->amsl));
    QCOMPARE(held->pos.y, 140.0 - sc.frame.alt0);       // drawn at the held height
    int legsIntoOrOut = 0, unknown = 0;
    for (const LineSeg &l : sc.lines) {
        if (l.kind == LineKind::Path && (l.visualIndex == 2 || l.visualIndex == 3)) {
            legsIntoOrOut++;
            unknown += l.clearance == Clearance::Unknown ? 1 : 0;
        }
    }
    QVERIFY(legsIntoOrOut > 0);
    QCOMPARE(unknown, legsIntoOrOut);
    QVERIFY(!held->terrainUnavailable);
    // F4: when GC never answers, the point stays unknown, labelled
    // "terrain height not available"; it never turns colored on its own.
    plan.items[1].heightUnavailable = true;
    const Scene sc3 = Builder::build(plan, [](double, double) { return 100.0; });
    for (const Marker &m : sc3.markers) {
        if (m.visualIndex == 2) {
            QVERIFY(m.heightPending && m.terrainUnavailable);
            QVERIFY(m.clearance == Clearance::Unknown);
            QVERIFY(std::isnan(m.amsl));
        }
    }
    // The same item not held is known and checked.
    plan.items[1].heightHeld = false;
    plan.items[1].heightUnavailable = false;
    const Scene sc2 = Builder::build(plan, [](double, double) { return 100.0; });
    for (const Marker &m : sc2.markers) {
        if (m.visualIndex == 2) {
            QVERIFY(!m.heightPending);
            QVERIFY(m.clearance == Clearance::NoStrike);
        }
    }
}

void Plan3DEditTest::_headingArduPilotMultirotorOnly()
{
    // F8: on ArduPilot, a CONDITION_YAW heading is offered for multirotors only.
    PlanMasterController *pmc = newController(this);
    Editor ed;
    ed.setPlanMasterController(pmc);
    pmc->loadFromFile(corpusPath(QStringLiteral("ardupilot-plane-fw-landing.plan")));
    for (int vi = 1; vi < pmc->missionController()->visualItems()->count(); vi++) {
        QVERIFY(!ed.headingBlockedReason(vi).isEmpty());
    }
    pmc->loadFromFile(corpusPath(QStringLiteral("ardupilot-vtol-vtol-landing.plan")));
    for (int vi = 1; vi < pmc->missionController()->visualItems()->count(); vi++) {
        QVERIFY(!ed.headingBlockedReason(vi).isEmpty());
    }
    pmc->loadFromFile(corpusPath(QStringLiteral("ardupilot-copter-alt-relative.plan")));
    QVERIFY(ed.headingBlockedReason(2).isEmpty());
    pmc->setDirty(false);
    delete pmc;
}

void Plan3DEditTest::_gimbalYawGated()
{
    // F10 (E-8): gimbal yaw is not edited until the SITL yaw-frame check.
    QVERIFY(!Editor::kGimbalYawEditing);
    Pair p(this, QStringLiteral("px4-copter-alt-amsl.plan"));
    Editor ed;
    ed.setPlanMasterController(p.a);
    QVERIFY(ed.setHeading(2, 90));
    QVERIFY(ed.setGimbalPitch(2, -30));
    const double yawBefore = ed.gimbalYaw(2);
    QVERIFY(!ed.setGimbalYaw(2, 45));
    QVERIFY(ed.message().contains(QStringLiteral("Camera section")));   // 5.3.0 review, visual-ux M2
    QVERIFY(!ed.message().contains(QStringLiteral("SITL")));
    QCOMPARE(ed.gimbalYaw(2), yawBefore);
}

void Plan3DEditTest::_checkEditPatternReadOnly()
{
    // 5.3.0 review, visual-ux M1: a drag on a point that cannot be edited
    // (here a pattern) asks checkEdit, which gives the read-only reason.
    PlanMasterController *pmc = newController(this);
    pmc->loadFromFile(corpusPath(QStringLiteral("ardupilot-copter-structure.plan")));
    Editor ed;
    ed.setPlanMasterController(pmc);
    ed.setReferences(slopeFixture, []() { return 30.0; });

    QmlObjectListModel *items = pmc->missionController()->visualItems();
    int pattern = -1;
    for (int vi = 1; vi < items->count(); vi++) {
        if (!qobject_cast<SimpleMissionItem *>(items->get(vi))) {
            pattern = vi;
            break;
        }
    }
    QVERIFY(pattern > 0);
    QVERIFY(!ed.canMove(pattern));
    QVERIFY(!ed.canChangeHeight(pattern));
    const QByteArray before = pmc->saveToJson().toJson();
    QVERIFY(!ed.checkEdit(pattern, false));
    QCOMPARE(ed.message(), QStringLiteral("This item is read only in 3D."));
    ed.clearMessage();
    QVERIFY(!ed.checkEdit(pattern, true));         // Ctrl + drag: the same reason
    QCOMPARE(ed.message(), QStringLiteral("This item is read only in 3D."));
    QCOMPARE(pmc->saveToJson().toJson(), before);  // nothing changed

    pmc->setDirty(false);
    delete pmc;
}

void Plan3DEditTest::_boundsWarning()
{
    // IB-6 (revision 9): an edit past 200 km from home, or a height past
    // +-100 km, is written as 2D would but always comes with the warning.
    PlanMasterController *pmc = newController(this);
    pmc->loadFromFile(corpusPath(QStringLiteral("ardupilot-copter-alt-amsl.plan")));
    Editor ed;
    ed.setPlanMasterController(pmc);
    ed.setReferences(slopeFixture, []() { return 30.0; });
    const QGeoCoordinate home = pmc->missionController()->plannedHomePosition();

    QVERIFY(ed.moveTo(2, home.latitude() + 1.0, home.longitude()));      // about 111 km
    QVERIFY(!ed.message().contains(QStringLiteral("km")));
    QVERIFY(ed.moveTo(2, home.latitude() + 2.0, home.longitude()));      // about 222 km
    QVERIFY(ed.message().contains(QStringLiteral("200 km")));
    QVERIFY(ed.message().contains(QStringLiteral("not checked against the terrain")));
    QVERIFY(ed.moveTo(2, home.latitude() + 0.01, home.longitude()));     // back inside: warning cleared
    QVERIFY(ed.message().isEmpty());

    QVERIFY(ed.setHeightAmsl(3, 150000));                                 // AMSL item
    QVERIFY(ed.message().contains(QStringLiteral("100 km drawing limit")));
    QVERIFY(!ed.message().contains(QStringLiteral("IB-")));      // no contract codes on screen (G67 M5)
    if (!_feet) {
        QCOMPARE(simpleAt(pmc, 3)->altitude()->rawValue().toDouble(), 150000.0);   // stored as written
    }
    QVERIFY(ed.setHeightAmsl(3, 80));
    QVERIFY(ed.message().isEmpty());

    pmc->setDirty(false);
    delete pmc;
}

void Plan3DEditTest::_dragLoiterToAltKeepsRing()
{
    // Rev 11 (L-1, L-3): after a 3D move of an ArduPlane NAV_LOITER_TO_ALT,
    // the ring is drawn around the item's new position at the item's own
    // radius, and the outbound leg leaves from the tangent point toward the
    // next point. The scene is built as the 3D view builds it: from GC's
    // items (sourceFromPlan) after the editor's write.
    PlanMasterController *pmc = newController(this);
    pmc->loadFromFile(corpusPath(QStringLiteral("ardupilot-plane-fw-landing.plan")));
    Editor ed;
    ed.setPlanMasterController(pmc);
    ed.setReferences(slopeFixture, []() { return 30.0; });

    // Items: 1 takeoff, 2..5 waypoints, 6 landing pattern. Make 3 a loiter
    // to altitude the 2D way (command picker, then the radius field).
    const int vi = 3;
    SimpleMissionItem *wp = simpleAt(pmc, vi);
    QVERIFY(wp && wp->command() == 16);
    wp->setCommand(31);
    wp->missionItem().setParam2(150);       // radius 150 m, clockwise
    QVERIFY(ed.canMove(vi));
    const QGeoCoordinate before = wp->coordinate();

    auto check = [&](const char *when, Vec3 &centreOut) {
        const Scene sc = Builder::build(sourceFromPlan(pmc), [](double, double) { return 20.0; });
        const Marker *centre = nullptr, *next = nullptr;
        for (const Marker &m : sc.markers) {
            centre = (m.visualIndex == vi && m.kind == MarkerKind::Waypoint) ? &m : centre;
            next = (!next && m.visualIndex == vi + 1 && m.kind == MarkerKind::Waypoint) ? &m : next;
        }
        QVERIFY2(centre && next, when);
        const double cx = centre->pos.x, cz = centre->pos.z;
        auto rad = [&](const Vec3 &v) { return std::hypot(v.x - cx, v.z - cz); };
        // The ring: every point at the item's radius.
        int ring = 0;
        for (const LineSeg &l : sc.lines) {
            if (l.kind == LineKind::Path && l.visualIndex == vi && std::abs(rad(l.a) - 150) < 0.5 && std::abs(rad(l.b) - 150) < 0.5) {
                ring++;
            }
        }
        QVERIFY2(ring >= 48, qPrintable(QStringLiteral("%1: ring segments %2").arg(when).arg(ring)));
        bool ringLeg = false;
        for (const SampleLeg &leg : sc.sampleLegs) {
            ringLeg |= leg.shape == LegShape::Ring && leg.visualIndex == vi;
        }
        QVERIFY2(ringLeg, when);      // G66's record names the ring too
        // The outbound leg: from a point on the ring, along the tangent.
        const LineSeg *out = nullptr;
        for (const LineSeg &l : sc.lines) {
            if (l.kind == LineKind::Path && l.visualIndex == vi + 1) {
                out = &l;
                break;
            }
        }
        QVERIFY2(out, when);
        QVERIFY2(std::abs(rad(out->a) - 150) < 0.5, qPrintable(QStringLiteral("%1: exit %2 m from the centre").arg(when).arg(rad(out->a))));
        const double rx = out->a.x - cx, rz = out->a.z - cz;
        const double dx = next->pos.x - out->a.x, dz = next->pos.z - out->a.z;
        const double cosAngle = (rx * dx + rz * dz) / (std::hypot(rx, rz) * std::hypot(dx, dz));
        QVERIFY2(std::abs(cosAngle) < 0.01, qPrintable(QStringLiteral("%1: exit not on the tangent, cos %2").arg(when).arg(cosAngle)));
        centreOut = Vec3(cx, 0, cz);
    };

    Vec3 c0, c1;
    check("before the move", c0);
    if (QTest::currentTestFailed()) {
        return;
    }
    QVERIFY(ed.beginDrag(vi));
    QVERIFY(ed.moveTo(vi, before.latitude() + 0.002, before.longitude() + 0.002));
    ed.endDrag();
    QCOMPARE(simpleAt(pmc, vi)->missionItem().param2(), 150.0);     // the radius is kept
    check("after the move", c1);
    if (QTest::currentTestFailed()) {
        return;
    }
    // The ring moved with the item (about 222 m north and 176 m east).
    QVERIFY(std::hypot(c1.x - c0.x, c1.z - c0.z) > 200);

    pmc->setDirty(false);
    delete pmc;
}

#endif // RL_PLAN3D

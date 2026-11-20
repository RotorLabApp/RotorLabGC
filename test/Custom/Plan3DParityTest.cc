/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G66, MG4) - corpus parity with RotorLab's 3D.
 * See Plan3DParityTest.h.
 *
 ****************************************************************************/

#include "Plan3DParityTest.h"

#ifdef RL_PLAN3D

#include "Plan3DBuilder.h"
#include "Plan3DGround.h"
#include "Plan3DMath.h"
#include "Plan3DSource.h"

#include "PlanMasterController.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QRegularExpression>
#include <QtCore/QVector>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkProxyFactory>
#include <QtTest/QTest>

#include <algorithm>
#include <cmath>
#include <functional>

using namespace Plan3D;

namespace {

#ifndef RL_PLAN_CORPUS_DIR
#error "RL_PLAN_CORPUS_DIR must name test/Custom/plan-corpus"
#endif
#ifndef RL_G66_DIR
#error "RL_G66_DIR must name test/Custom/plan-corpus/g66"
#endif
#ifndef RL_PLAN3D_RES_DIR
#error "RL_PLAN3D_RES_DIR must name custom/res/plan3d"
#endif

// ---- The method's numbers (G66-PARITY-PLAN.md, "Tolerances") -------------

constexpr double kStrikeM        = 2.0;    ///< section 4, M (decision 756 OQ7)
constexpr double kContactFloorM  = -0.5;   ///< CL-1a
constexpr double kEndTolM        = 0.5;    ///< leg ends and markers, horizontal and vertical
constexpr double kPosTolM        = 0.5;    ///< sample to the other product's line, straight and terrain legs
constexpr double kPosTolCurvedM  = 0.75;   ///< spline and ring legs (chord of the other product's sampling)
constexpr double kHagTolM        = 0.05;   ///< per sample height above the fixture, straight and terrain legs
constexpr double kHagTolCurvedM  = 0.10;   ///< spline and ring legs
constexpr double kBandM          = 0.10;   ///< a state may differ within this of 2 m or of -0.5 m
constexpr double kMarkerAmslTolM = 0.01;   ///< RT-5
constexpr double kRingRadiusRel  = 0.01;   ///< loiter rings: radius within 1 % (local plane conventions, plan "Geometry")

/// G66-F1.json as make_g66_fixture.py writes it. A change of fixture is a change of method.
const char kFixtureSha256[] = "f20dd5bb2edd9385d2502c991143e7688cdb9bcc0e251be2583d4a64e3eb1fda";

const QStringList &corpusFiles()
{
    static const QStringList files = {
        QStringLiteral("ardupilot-copter-alt-amsl.plan"),
        QStringLiteral("ardupilot-copter-alt-calc-above-terrain.plan"),
        QStringLiteral("ardupilot-copter-alt-mixed.plan"),
        QStringLiteral("ardupilot-copter-alt-relative.plan"),
        QStringLiteral("ardupilot-copter-alt-terrain-frame.plan"),
        QStringLiteral("ardupilot-copter-fence-rally.plan"),
        QStringLiteral("ardupilot-copter-spline.plan"),
        QStringLiteral("ardupilot-copter-structure.plan"),
        QStringLiteral("ardupilot-copter-survey-custom-camera.plan"),
        QStringLiteral("ardupilot-copter-survey-follow-terrain.plan"),
        QStringLiteral("ardupilot-plane-fw-landing.plan"),
        QStringLiteral("ardupilot-vtol-vtol-landing.plan"),
        QStringLiteral("px4-copter-alt-amsl.plan"),
        QStringLiteral("px4-copter-corridor.plan"),
        QStringLiteral("px4-copter-fence-rally.plan"),
        QStringLiteral("px4-copter-survey-sony-a6000.plan"),
        QStringLiteral("wp300-px4-copter-lawnmower.plan"),
    };
    return files;
}

QString g66Dir()            { return QStringLiteral(RL_G66_DIR); }
QString exportPath(const QString &f) { return QDir(QStringLiteral(RL_PLAN_CORPUS_DIR)).filePath(QStringLiteral("a204-rotorlab/") + f); }
QString corpusPath(const QString &f) { return QDir(QStringLiteral(RL_PLAN_CORPUS_DIR)).filePath(f); }

QString rotorLabDir()
{
    const QString env = qEnvironmentVariable("RL_G66_ROTORLAB_DIR");
    return env.isEmpty() ? QDir(g66Dir()).filePath(QStringLiteral("rotorlab")) : env;
}

QString gcOutDir()
{
    const QString env = qEnvironmentVariable("RL_G66_OUT");
    return env.isEmpty() ? QDir::current().filePath(QStringLiteral("g66-gc")) : env;
}

QString resultName(const QString &planFile)
{
    QString base = planFile;
    base.chop(5);   // ".plan"
    return base + QStringLiteral(".samples.json");
}

/// Every request goes to the discard port on loopback.
class DeadLocalProxyFactory : public QNetworkProxyFactory
{
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &) override
    {
        return { QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 9) };
    }
};

// ---- The shared terrain fixture ---------------------------------------------

struct Fixture {
    GroundGrid  grid;
    QString     id;
    QString     sha256;
    double      cellM = kNaN;
    double      homeLat = kNaN, homeLon = kNaN;
    bool        ok = false;
    double G(double lat, double lon) const { return grid.elevation(lat, lon); }
};

const Fixture &fixture()
{
    static Fixture fx;
    static bool loaded = false;
    if (loaded) {
        return fx;
    }
    loaded = true;
    QFile f(QDir(g66Dir()).filePath(QStringLiteral("G66-F1.json")));
    if (!f.open(QIODevice::ReadOnly)) {
        return fx;
    }
    const QByteArray bytes = f.readAll();
    fx.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const QJsonObject o = QJsonDocument::fromJson(bytes).object();
    fx.id = o[QStringLiteral("id")].toString();
    fx.cellM = o[QStringLiteral("cellM")].toDouble(kNaN);
    const QJsonArray home = o[QStringLiteral("home")].toArray();
    fx.homeLat = home.at(0).toDouble(kNaN);
    fx.homeLon = home.at(1).toDouble(kNaN);
    fx.grid.area.south = o[QStringLiteral("south")].toDouble(kNaN);
    fx.grid.area.north = o[QStringLiteral("north")].toDouble(kNaN);
    fx.grid.area.west = o[QStringLiteral("west")].toDouble(kNaN);
    fx.grid.area.east = o[QStringLiteral("east")].toDouble(kNaN);
    fx.grid.cols = o[QStringLiteral("cols")].toInt();
    fx.grid.rows = o[QStringLiteral("rows")].toInt();
    const QJsonArray h = o[QStringLiteral("heights")].toArray();
    fx.grid.heights.reserve(h.size());
    for (const QJsonValue &v : h) {
        fx.grid.heights.append(v.isNull() ? kNaN : v.toDouble());
    }
    fx.ok = fx.grid.valid() && o[QStringLiteral("schema")].toString() == QStringLiteral("g66-fixture/1");
    return fx;
}

// ---- One product's result for one file (schema g66-samples/1) --------------

enum class Cls { Path, Strike, Contact, Unknown, None };

QString clsName(Cls c)
{
    switch (c) {
    case Cls::Path:    return QStringLiteral("path");
    case Cls::Strike:  return QStringLiteral("strike");
    case Cls::Contact: return QStringLiteral("contact");
    case Cls::Unknown: return QStringLiteral("unknown");
    case Cls::None:    break;
    }
    return QStringLiteral("none");
}

Cls clsFromName(const QString &s)
{
    if (s == QLatin1String("path"))    return Cls::Path;
    if (s == QLatin1String("strike"))  return Cls::Strike;
    if (s == QLatin1String("contact")) return Cls::Contact;
    if (s == QLatin1String("unknown")) return Cls::Unknown;
    return Cls::None;
}

/// GC's state of a sample (Plan3DMath.h Clearance, CL-1a GroundContact since G64 M4).
Cls clsOf(Clearance c)
{
    switch (c) {
    case Clearance::NoStrike:      return Cls::Path;
    case Clearance::Strike:        return Cls::Strike;
    case Clearance::Unknown:       return Cls::Unknown;
    case Clearance::GroundContact: return Cls::Contact;
    }
    return Cls::Unknown;
}

struct RSample {
    double t = 0, lat = kNaN, lon = kNaN, amsl = kNaN, ground = kNaN, hag = kNaN;
    Cls cls = Cls::Unknown;
};

struct RLeg {
    double fLat = kNaN, fLon = kNaN, fAmsl = kNaN, tLat = kNaN, tLon = kNaN, tAmsl = kNaN;
    QString shape;      ///< straight, spline, terrain, ring, unknown
    QString contact;    ///< none, takeoff, land
    int item = -1, command = 0;
    QVector<RSample> s;
    bool curved() const { return shape == QLatin1String("spline") || shape == QLatin1String("ring"); }
};

struct RMarker {
    QString kind;       ///< home, waypoint, pattern, roi, rally, breach
    int item = -1, command = 0;
    double lat = kNaN, lon = kNaN, amsl = kNaN, ground = kNaN;
    Cls cls = Cls::None;
};

struct Result {
    QString product, file, fileSha256, fixtureId, fixtureSha256;
    double cellM = kNaN, sampleSpacingM = kNaN;
    double launchRef = kNaN;
    bool launchRefFromGround = false;
    double fileHomeAmsl = kNaN;
    bool homeDatumNote = false;
    QVector<RLeg> legs;
    QVector<RMarker> markers;
    int fences = 0, outlines = 0;
    QMap<QString, QString> legend;   ///< class name -> line
    QStringList warnings;
    QStringList schemaNotes;    ///< where a result's leg ends disagree with its own samples
    bool ok = false;
};

QJsonValue num(double v) { return std::isfinite(v) ? QJsonValue(v) : QJsonValue(QJsonValue::Null); }
double numOf(const QJsonValue &v) { return v.isDouble() ? v.toDouble() : kNaN; }
double hagOf(double amsl, double ground) { return (std::isfinite(amsl) && std::isfinite(ground)) ? amsl - ground : kNaN; }

QString shapeName(LegShape s)
{
    switch (s) {
    case LegShape::Straight:      return QStringLiteral("straight");
    case LegShape::Spline:        return QStringLiteral("spline");
    case LegShape::Terrain:       return QStringLiteral("terrain");
    case LegShape::Ring:          return QStringLiteral("ring");
    case LegShape::UnknownHeight: return QStringLiteral("unknown");
    }
    return QStringLiteral("straight");
}

QString markerKindName(MarkerKind k)
{
    switch (k) {
    case MarkerKind::Home:         return QStringLiteral("home");
    case MarkerKind::Waypoint:     return QStringLiteral("waypoint");
    case MarkerKind::PatternPoint: return QStringLiteral("pattern");
    case MarkerKind::Roi:          return QStringLiteral("roi");
    case MarkerKind::Rally:        return QStringLiteral("rally");
    case MarkerKind::BreachReturn: return QStringLiteral("breach");
    }
    return QStringLiteral("other");
}

/// The legend lines of GC's 3D view (Plan3DLegend.qml), by state.
QMap<QString, QString> gcLegend()
{
    QMap<QString, QString> out;
    QFile f(QDir(QStringLiteral(RL_PLAN3D_RES_DIR)).filePath(QStringLiteral("Plan3DLegend.qml")));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return out;
    }
    static const QRegularExpression re(QStringLiteral("state:\\s*\"(\\w+)\"\\s*,\\s*text:\\s*qsTr\\(\"([^\"]*)\"\\)"));
    auto it = re.globalMatch(QString::fromUtf8(f.readAll()));
    while (it.hasNext()) {
        const auto m = it.next();
        const QString state = m.captured(1);
        const QString key = state == QLatin1String("noStrike") ? QStringLiteral("path")
                          : state.contains(QLatin1String("ontact")) ? QStringLiteral("contact")
                          : state;
        out.insert(key, m.captured(2));
    }
    return out;
}

Result fromScene(const Scene &sc, const SourcePlan &plan, const QString &file, const QByteArray &planBytes)
{
    Result r;
    r.product = QStringLiteral("GC");
    r.file = file;
    r.fileSha256 = QString::fromLatin1(QCryptographicHash::hash(planBytes, QCryptographicHash::Sha256).toHex());
    r.fixtureId = fixture().id;
    r.fixtureSha256 = fixture().sha256;
    r.cellM = fixture().cellM;
    r.sampleSpacingM = sc.sampleSpacingM;
    r.launchRef = sc.launchRef;
    r.launchRefFromGround = sc.launchRefFromGround;
    r.fileHomeAmsl = plan.homeAmsl;
    r.homeDatumNote = sc.homeDatumNote;
    for (const SampleLeg &l : sc.sampleLegs) {
        RLeg o;
        o.fLat = l.fromLat; o.fLon = l.fromLon; o.fAmsl = l.fromAmsl;
        o.tLat = l.toLat;   o.tLon = l.toLon;   o.tAmsl = l.toAmsl;
        o.shape = shapeName(l.shape);
        o.contact = !l.contactLeg ? QStringLiteral("none") : (Builder::isLand(l.command) ? QStringLiteral("land") : QStringLiteral("takeoff"));
        o.item = l.visualIndex - 1;     // visual item 0 is the planned home; 1.. are mission.items
        o.command = l.command;
        r.legs.append(o);
    }
    for (const PathSample &s : sc.samples) {
        if (s.leg < 0 || s.leg >= r.legs.size()) {
            continue;
        }
        RSample o;
        o.t = s.t; o.lat = s.lat; o.lon = s.lon; o.amsl = s.amsl; o.ground = s.ground;
        o.hag = hagOf(s.amsl, s.ground);
        o.cls = clsOf(s.clearance);
        r.legs[s.leg].s.append(o);
    }
    for (const Marker &m : sc.markers) {
        RMarker o;
        o.kind = markerKindName(m.kind);
        o.item = m.visualIndex - 1;
        o.command = m.command;
        o.lat = sc.frame.latFromNorth(-m.pos.z);
        o.lon = sc.frame.lonFromEast(m.pos.x);
        o.amsl = m.amsl;
        o.ground = m.ground;
        o.cls = (m.kind == MarkerKind::Home) ? Cls::None : clsOf(m.clearance);
        r.markers.append(o);
    }
    r.fences = int(plan.fences.size());
    r.outlines = int(plan.outlines.size());
    r.legend = gcLegend();
    r.warnings = sc.warnings;
    r.ok = true;
    return r;
}

QJsonObject toJson(const Result &r)
{
    QJsonObject o;
    o[QStringLiteral("schema")] = QStringLiteral("g66-samples/1");
    o[QStringLiteral("product")] = r.product;
    o[QStringLiteral("file")] = r.file;
    o[QStringLiteral("fileSha256")] = r.fileSha256;
    o[QStringLiteral("fixture")] = QJsonObject{{QStringLiteral("id"), r.fixtureId}, {QStringLiteral("sha256"), r.fixtureSha256}};
    o[QStringLiteral("cellM")] = num(r.cellM);
    o[QStringLiteral("sampleSpacingM")] = num(r.sampleSpacingM);
    o[QStringLiteral("launchRef")] = QJsonObject{{QStringLiteral("amsl"), num(r.launchRef)}, {QStringLiteral("fromGround"), r.launchRefFromGround}};
    o[QStringLiteral("fileHomeAmsl")] = num(r.fileHomeAmsl);
    o[QStringLiteral("homeDatumNote")] = r.homeDatumNote;
    QJsonArray legs;
    QMap<QString, int> tally;
    for (const RLeg &l : r.legs) {
        QJsonArray samples;
        for (const RSample &s : l.s) {
            samples.append(QJsonArray{s.t, s.lat, s.lon, num(s.amsl), num(s.ground), num(s.hag), clsName(s.cls)});
            tally[clsName(s.cls)]++;
        }
        legs.append(QJsonObject{
            {QStringLiteral("from"), QJsonArray{l.fLat, l.fLon, num(l.fAmsl)}},
            {QStringLiteral("to"), QJsonArray{l.tLat, l.tLon, num(l.tAmsl)}},
            {QStringLiteral("shape"), l.shape},
            {QStringLiteral("contact"), l.contact},
            {QStringLiteral("item"), l.item},
            {QStringLiteral("command"), l.command},
            {QStringLiteral("samples"), samples},
        });
    }
    o[QStringLiteral("legs")] = legs;
    QJsonArray markers;
    for (const RMarker &m : r.markers) {
        markers.append(QJsonObject{
            {QStringLiteral("kind"), m.kind}, {QStringLiteral("item"), m.item}, {QStringLiteral("command"), m.command},
            {QStringLiteral("lat"), m.lat}, {QStringLiteral("lon"), m.lon}, {QStringLiteral("amsl"), num(m.amsl)},
            {QStringLiteral("ground"), num(m.ground)}, {QStringLiteral("class"), clsName(m.cls)},
        });
    }
    o[QStringLiteral("markers")] = markers;
    o[QStringLiteral("fences")] = r.fences;
    o[QStringLiteral("outlines")] = r.outlines;
    QJsonArray legend;
    for (auto it = r.legend.cbegin(); it != r.legend.cend(); ++it) {
        legend.append(QJsonObject{{QStringLiteral("class"), it.key()}, {QStringLiteral("text"), it.value()}});
    }
    o[QStringLiteral("legend")] = legend;
    QJsonObject t;
    for (const char *k : {"path", "strike", "contact", "unknown"}) {
        t[QLatin1String(k)] = tally.value(QLatin1String(k));
    }
    o[QStringLiteral("tally")] = t;
    o[QStringLiteral("warnings")] = QJsonArray::fromStringList(r.warnings);
    return o;
}

Result fromJson(const QJsonObject &o)
{
    Result r;
    if (o[QStringLiteral("schema")].toString() != QStringLiteral("g66-samples/1")) {
        return r;
    }
    r.product = o[QStringLiteral("product")].toString();
    r.file = o[QStringLiteral("file")].toString();
    r.fileSha256 = o[QStringLiteral("fileSha256")].toString();
    r.fixtureId = o[QStringLiteral("fixture")].toObject()[QStringLiteral("id")].toString();
    r.fixtureSha256 = o[QStringLiteral("fixture")].toObject()[QStringLiteral("sha256")].toString();
    r.cellM = numOf(o[QStringLiteral("cellM")]);
    r.sampleSpacingM = numOf(o[QStringLiteral("sampleSpacingM")]);
    r.launchRef = numOf(o[QStringLiteral("launchRef")].toObject()[QStringLiteral("amsl")]);
    r.launchRefFromGround = o[QStringLiteral("launchRef")].toObject()[QStringLiteral("fromGround")].toBool();
    r.fileHomeAmsl = numOf(o[QStringLiteral("fileHomeAmsl")]);
    r.homeDatumNote = o[QStringLiteral("homeDatumNote")].toBool();
    for (const QJsonValue &lv : o[QStringLiteral("legs")].toArray()) {
        const QJsonObject lo = lv.toObject();
        RLeg l;
        const QJsonArray f = lo[QStringLiteral("from")].toArray(), t = lo[QStringLiteral("to")].toArray();
        l.fLat = numOf(f.at(0)); l.fLon = numOf(f.at(1)); l.fAmsl = numOf(f.at(2));
        l.tLat = numOf(t.at(0)); l.tLon = numOf(t.at(1)); l.tAmsl = numOf(t.at(2));
        l.shape = lo[QStringLiteral("shape")].toString();
        l.contact = lo[QStringLiteral("contact")].toString();
        l.item = lo[QStringLiteral("item")].toInt(-1);
        l.command = lo[QStringLiteral("command")].toInt();
        for (const QJsonValue &sv : lo[QStringLiteral("samples")].toArray()) {
            const QJsonArray a = sv.toArray();
            RSample s;
            s.t = numOf(a.at(0)); s.lat = numOf(a.at(1)); s.lon = numOf(a.at(2));
            s.amsl = numOf(a.at(3)); s.ground = numOf(a.at(4)); s.hag = numOf(a.at(5));
            s.cls = clsFromName(a.at(6).toString());
            l.s.append(s);
        }
        // The drawn line is the samples: a leg's ends are its first and last
        // sample (a ring's from and to stay its centre). Where the written
        // ends differ, the samples win and the difference is reported.
        if (l.shape != QLatin1String("ring") && !l.s.isEmpty()) {
            const RSample &first = l.s.first(), &last = l.s.last();
            const LocalFrame f{first.lat, first.lon, 0};
            auto off = [&f](double lat1, double lon1, double lat2, double lon2) {
                return std::hypot(f.east(lon1) - f.east(lon2), f.north(lat1) - f.north(lat2));
            };
            if (off(l.fLat, l.fLon, first.lat, first.lon) > kEndTolM || off(l.tLat, l.tLon, last.lat, last.lon) > kEndTolM) {
                r.schemaNotes << QStringLiteral("leg %1 (item %2, %3): written ends are not its first and last samples; the samples are used")
                                     .arg(r.legs.size()).arg(l.item).arg(l.shape);
            }
            l.fLat = first.lat; l.fLon = first.lon; l.fAmsl = first.amsl;
            l.tLat = last.lat;  l.tLon = last.lon;  l.tAmsl = last.amsl;
        }
        r.legs.append(l);
    }
    for (const QJsonValue &mv : o[QStringLiteral("markers")].toArray()) {
        const QJsonObject mo = mv.toObject();
        RMarker m;
        m.kind = mo[QStringLiteral("kind")].toString();
        m.item = mo[QStringLiteral("item")].toInt(-1);
        m.command = mo[QStringLiteral("command")].toInt();
        m.lat = numOf(mo[QStringLiteral("lat")]);
        m.lon = numOf(mo[QStringLiteral("lon")]);
        m.amsl = numOf(mo[QStringLiteral("amsl")]);
        m.ground = numOf(mo[QStringLiteral("ground")]);
        m.cls = clsFromName(mo[QStringLiteral("class")].toString());
        r.markers.append(m);
    }
    r.fences = o[QStringLiteral("fences")].toInt();
    r.outlines = o[QStringLiteral("outlines")].toInt();
    for (const QJsonValue &v : o[QStringLiteral("legend")].toArray()) {
        r.legend.insert(v.toObject()[QStringLiteral("class")].toString(), v.toObject()[QStringLiteral("text")].toString());
    }
    for (const QJsonValue &v : o[QStringLiteral("warnings")].toArray()) {
        r.warnings.append(v.toString());
    }
    r.ok = true;
    return r;
}

// ---- The comparison --------------------------------------------------------

struct Report {
    QStringList failures;   ///< each one fails the row
    QStringList notes;      ///< printed, never hidden
    QStringList known;      ///< the coordinator's known RotorLab-side findings: printed, not GC failures
    int compared = 0;       ///< samples matched to the other product's line
    int boundary = 0;       ///< of those, states that differ within a band (allowed, counted)
};

struct Enu { double e = 0, n = 0, u = 0; };

class Comparator
{
public:
    Comparator(const Fixture &fx) : _fx(fx), _f{fx.homeLat, fx.homeLon, 0} {}

    /// Compares A against B. `dir` names the direction in messages ("GC->RotorLab").
    void compare(const Result &A, const Result &B, const QString &dir, Report &rep) const
    {
        if (!(std::abs(A.launchRef - B.launchRef) <= 1e-6) || A.launchRefFromGround != B.launchRefFromGround) {
            rep.failures << QStringLiteral("%1: launch reference R %2 (from ground %3) against %4 (from ground %5) (A-0)")
                                .arg(dir).arg(A.launchRef).arg(int(A.launchRefFromGround)).arg(B.launchRef).arg(int(B.launchRefFromGround));
        }
        if (A.homeDatumNote != B.homeDatumNote) {
            rep.failures << QStringLiteral("%1: A-0b note %2 against %3").arg(dir).arg(int(A.homeDatumNote)).arg(int(B.homeDatumNote));
        }
        if (A.fences != B.fences || A.outlines != B.outlines) {
            rep.failures << QStringLiteral("%1: drawn fences %2 and outlines %3 against %4 and %5 (CL-7)")
                                .arg(dir).arg(A.fences).arg(A.outlines).arg(B.fences).arg(B.outlines);
        }
        for (int i = 0; i < A.legs.size(); i++) {
            _compareLeg(A.legs[i], i, A, B, dir, rep);
        }
        _compareMarkers(A, B, dir, rep);
    }

private:
    Enu enu(double lat, double lon, double amsl) const { return {_f.east(lon), _f.north(lat), std::isfinite(amsl) ? amsl : 0.0}; }
    double hdist(double lat1, double lon1, double lat2, double lon2) const
    {
        return std::hypot(_f.east(lon1) - _f.east(lon2), _f.north(lat1) - _f.north(lat2));
    }
    bool samePoint(double lat1, double lon1, double amsl1, double lat2, double lon2, double amsl2) const
    {
        if (!(hdist(lat1, lon1, lat2, lon2) <= kEndTolM)) {
            return false;
        }
        // An unknown height matches on position alone.
        return !(std::isfinite(amsl1) && std::isfinite(amsl2)) || std::abs(amsl1 - amsl2) <= kEndTolM;
    }

    /// Legs of B, in order, that run from a's start to a's end: one leg, or a
    /// chain of up to six whose joints meet (a product may split a leg where
    /// the other does not). Rings are matched by centre and radius.
    QVector<int> _match(const RLeg &a, const Result &B) const
    {
        const bool ring = a.shape == QLatin1String("ring");
        if (ring) {
            const double ra = ringRadius(a);
            for (int k = 0; k < B.legs.size(); k++) {
                const RLeg &b = B.legs[k];
                if (b.shape == QLatin1String("ring") && hdist(a.fLat, a.fLon, b.fLat, b.fLon) <= kEndTolM
                        && std::abs(ringRadius(b) - ra) <= kRingRadiusRel * ra + 1e-6) {
                    return {k};
                }
            }
            return {};
        }
        // A leg with no height in the file (A-10) only answers another such
        // leg, and a flown leg only a flown leg: an RTL descent and a takeoff
        // climb can share both ends' positions.
        const bool aUnknown = a.shape == QLatin1String("unknown");
        auto sameGroup = [aUnknown](const RLeg &b) {
            return b.shape != QLatin1String("ring") && (b.shape == QLatin1String("unknown")) == aUnknown;
        };
        QVector<int> chain;
        std::function<bool(int, int)> walk = [&](int k, int depth) -> bool {
            const RLeg &b = B.legs[k];
            chain.append(k);
            if (samePoint(b.tLat, b.tLon, b.tAmsl, a.tLat, a.tLon, a.tAmsl)) {
                return true;
            }
            if (depth < 6) {
                for (int j = 0; j < B.legs.size(); j++) {
                    const RLeg &n = B.legs[j];
                    if (!sameGroup(n) || chain.contains(j)) {
                        continue;
                    }
                    if (samePoint(n.fLat, n.fLon, n.fAmsl, b.tLat, b.tLon, b.tAmsl) && walk(j, depth + 1)) {
                        return true;
                    }
                }
            }
            chain.removeLast();
            return false;
        };
        for (int k = 0; k < B.legs.size(); k++) {
            const RLeg &b = B.legs[k];
            if (!sameGroup(b)) {
                continue;
            }
            if (samePoint(b.fLat, b.fLon, b.fAmsl, a.fLat, a.fLon, a.fAmsl) && walk(k, 1)) {
                return chain;
            }
        }
        return {};
    }

    double ringRadius(const RLeg &l) const
    {
        double sum = 0;
        int n = 0;
        for (const RSample &s : l.s) {
            sum += hdist(s.lat, s.lon, l.fLat, l.fLon);
            n++;
        }
        return n ? sum / n : 0.0;
    }

    struct Proj {
        double d = 1e18;        ///< horizontal distance from the sample to B's line
        double score = 1e18;    ///< d with the height difference, to pick the segment
        double alongNear = 0;   ///< distance along B's segment to its nearer sample
        double amsl = kNaN, hag = kNaN, lat = kNaN, lon = kNaN;
        Cls near = Cls::Unknown, other = Cls::Unknown;
    };

    /// The point of B's sampled line that answers sample `s`: on each of B's
    /// segments, the point nearest along the ground (on a vertical segment,
    /// the point at the sample's height); of those, the one nearest in
    /// position and height. B's height there is interpolated, and its height
    /// above the fixture is taken from the fixture at that point (on a
    /// terrain leg, the interpolated height above the ground, A-7).
    Proj _project(const RSample &s, const Result &B, const QVector<int> &legs) const
    {
        Proj best;
        const Enu x = enu(s.lat, s.lon, s.amsl);
        for (int k : legs) {
            const RLeg &b = B.legs[k];
            const bool terrain = b.shape == QLatin1String("terrain");
            for (int i = 0; i + 1 < b.s.size(); i++) {
                const RSample &p = b.s[i], &q = b.s[i + 1];
                const Enu P = enu(p.lat, p.lon, p.amsl), Q = enu(q.lat, q.lon, q.amsl);
                const double de = Q.e - P.e, dn = Q.n - P.n;
                const double lenH2 = de * de + dn * dn;
                const bool heights = std::isfinite(s.amsl) && std::isfinite(p.amsl) && std::isfinite(q.amsl);
                double u = 0;
                if (lenH2 > 1e-6) {
                    u = ((x.e - P.e) * de + (x.n - P.n) * dn) / lenH2;
                } else if (heights && std::abs(q.amsl - p.amsl) > 1e-9) {
                    u = (s.amsl - p.amsl) / (q.amsl - p.amsl);     // a vertical segment: by height
                }
                u = std::clamp(u, 0.0, 1.0);
                const double ce = P.e + u * de, cn = P.n + u * dn;
                const double d = std::hypot(x.e - ce, x.n - cn);
                const double amsl = (std::isfinite(p.amsl) && std::isfinite(q.amsl)) ? p.amsl + u * (q.amsl - p.amsl) : kNaN;
                const double dv = (std::isfinite(s.amsl) && std::isfinite(amsl)) ? s.amsl - amsl : 0.0;
                const double score = std::hypot(d, dv);
                if (score < best.score) {
                    best.score = score;
                    best.d = d;
                    best.lat = p.lat + u * (q.lat - p.lat);
                    best.lon = p.lon + u * (q.lon - p.lon);
                    best.amsl = amsl;
                    if (terrain || !std::isfinite(amsl)) {
                        best.hag = (std::isfinite(p.hag) && std::isfinite(q.hag)) ? p.hag + u * (q.hag - p.hag) : kNaN;
                    } else {
                        best.hag = hagOf(amsl, _fx.G(best.lat, best.lon));
                    }
                    const double len = std::sqrt(lenH2 + (heights ? (q.amsl - p.amsl) * (q.amsl - p.amsl) : 0.0));
                    best.alongNear = std::min(u, 1.0 - u) * len;
                    best.near = u < 0.5 ? p.cls : q.cls;
                    best.other = u < 0.5 ? q.cls : p.cls;
                }
            }
        }
        return best;
    }

    void _compareLeg(const RLeg &a, int index, const Result &A, const Result &B, const QString &dir, Report &rep) const
    {
        const QString where = QStringLiteral("%1 leg %2 (item %3, command %4, %5)").arg(dir).arg(index).arg(a.item).arg(a.command).arg(a.shape);
        bool allUnknown = true;
        for (const RSample &s : a.s) {
            allUnknown &= s.cls == Cls::Unknown;
        }
        const QVector<int> legs = _match(a, B);
        if (legs.isEmpty()) {
            if (allUnknown) {
                // A-10: a leg with no height in the file (RTL) may be drawn differently; it must stay unknown.
                rep.notes << QStringLiteral("%1: no matching leg in the other product; every sample unknown (A-10), not compared").arg(where);
            } else if (const QString inbound = _loiterInbound(a, A, B); !inbound.isEmpty()) {
                // Rev 11 L-1: the leg into a loiter ring. Recorded with how far apart the drawings are.
                rep.known << QStringLiteral("L-1 %1: %2").arg(where, inbound);
            } else {
                rep.failures << QStringLiteral("%1: no matching leg in the other product from %2,%3 to %4,%5 (CL-7)")
                                    .arg(where).arg(a.fLat, 0, 'f', 7).arg(a.fLon, 0, 'f', 7).arg(a.tLat, 0, 'f', 7).arg(a.tLon, 0, 'f', 7);
            }
            return;
        }
        bool curved = a.curved();
        for (int k : legs) {
            curved |= B.legs[k].curved();
        }
        const double posTol = curved ? kPosTolCurvedM : kPosTolM;
        const double hagTol = curved ? kHagTolCurvedM : kHagTolM;
        int reported = 0;
        auto fail = [&](const QString &msg) {
            if (reported++ < 5) {
                rep.failures << QStringLiteral("%1: %2").arg(where, msg);
            } else if (reported == 6) {
                rep.failures << QStringLiteral("%1: more differences on this leg not listed").arg(where);
            }
        };
        for (int i = 0; i < a.s.size(); i++) {
            const RSample &s = a.s[i];
            const Proj p = _project(s, B, legs);
            rep.compared++;
            if (!(p.d <= posTol)) {
                fail(QStringLiteral("sample %1 (t %2) is %3 m from the other product's line").arg(i).arg(s.t).arg(p.d, 0, 'f', 3));
                continue;
            }
            const bool aKnown = std::isfinite(s.hag), bKnown = std::isfinite(p.hag);
            if (aKnown && bKnown && std::abs(s.hag - p.hag) > hagTol) {
                fail(QStringLiteral("sample %1 (t %2): height above the fixture %3 m against %4 m").arg(i).arg(s.t).arg(s.hag, 0, 'f', 3).arg(p.hag, 0, 'f', 3));
            }
            if (s.cls == p.near) {
                continue;
            }
            const bool inBand = aKnown && (std::abs(s.hag - kStrikeM) <= kBandM || std::abs(s.hag - kContactFloorM) <= kBandM);
            // Between two of B's samples that differ, either state is B's
            // answer; on one of B's samples (within 5 cm), only that one is.
            const bool between = p.alongNear > 0.05 && s.cls == p.other;
            if (between || inBand) {
                rep.boundary++;    // the other product changes state right here
                continue;
            }
            fail(QStringLiteral("sample %1 (t %2, height above the fixture %3 m): %4 against %5")
                     .arg(i).arg(s.t).arg(aKnown ? QString::number(s.hag, 'f', 3) : QStringLiteral("unknown"))
                     .arg(clsName(s.cls), clsName(p.near)));
        }
    }

    /// Whether a straight leg ends on a loiter ring of its own product
    /// (within 1 m of the radius); returns the ring's index or -1.
    int _ringAtEnd(const RLeg &l, const Result &R) const
    {
        for (int k = 0; k < R.legs.size(); k++) {
            const RLeg &r = R.legs[k];
            if (r.shape == QLatin1String("ring") && std::abs(hdist(l.tLat, l.tLon, r.fLat, r.fLon) - ringRadius(r)) < 1.0) {
                return k;
            }
        }
        return -1;
    }

    /// An inbound leg to a loiter ring (contract rev 11, L-1) that the other
    /// product draws to another point of the same ring: described with the
    /// distances between the two drawings, sample by sample. Empty otherwise.
    QString _loiterInbound(const RLeg &a, const Result &A, const Result &B) const
    {
        const int ra = _ringAtEnd(a, A);
        if (a.shape != QLatin1String("straight") || ra < 0) {
            return {};
        }
        const RLeg &ring = A.legs[ra];
        for (int k = 0; k < B.legs.size(); k++) {
            const RLeg &b = B.legs[k];
            const int rb = _ringAtEnd(b, B);
            if (b.shape != QLatin1String("straight") || rb < 0
                    || !samePoint(b.fLat, b.fLon, b.fAmsl, a.fLat, a.fLon, a.fAmsl)
                    || hdist(B.legs[rb].fLat, B.legs[rb].fLon, ring.fLat, ring.fLon) > kEndTolM) {
                continue;
            }
            double worstD = 0, sum = 0;
            int apart = 0;
            for (const RSample &s : a.s) {
                const Proj p = _project(s, B, {k});
                worstD = std::max(worstD, p.d);
                sum += p.d;
                apart += p.d > kPosTolM ? 1 : 0;
            }
            return QStringLiteral("the other product ends this inbound leg %1 m away on the same ring; %2 of %3 samples more than %4 m from its line (max %5 m, mean %6 m)")
                .arg(hdist(a.tLat, a.tLon, b.tLat, b.tLon), 0, 'f', 1).arg(apart).arg(a.s.size()).arg(kPosTolM)
                .arg(worstD, 0, 'f', 1).arg(a.s.isEmpty() ? 0.0 : sum / a.s.size(), 0, 'f', 1);
        }
        return {};
    }

    void _compareMarkers(const Result &A, const Result &B, const QString &dir, Report &rep) const
    {
        bool bHasPattern = false;
        for (const RMarker &m : B.markers) {
            bHasPattern |= m.kind == QLatin1String("pattern");
        }
        for (const RMarker &m : A.markers) {
            if (m.kind == QLatin1String("pattern") && !bHasPattern) {
                continue;   // a product may mark pattern points or not; their legs are compared either way
            }
            const RMarker *best = nullptr;
            double bestD = 1e18;
            for (const RMarker &o : B.markers) {
                if (o.kind != m.kind) {
                    continue;
                }
                const double d = hdist(m.lat, m.lon, o.lat, o.lon)
                               + ((std::isfinite(m.amsl) && std::isfinite(o.amsl)) ? std::abs(m.amsl - o.amsl) : 0.0);
                if (d < bestD) {
                    bestD = d;
                    best = &o;
                }
            }
            const QString where = QStringLiteral("%1 %2 marker (item %3, command %4)").arg(dir, m.kind).arg(m.item).arg(m.command);
            if (!best || hdist(m.lat, m.lon, best->lat, best->lon) > kEndTolM) {
                if (m.kind == QLatin1String("pattern") && m.command == 31) {
                    // A landing pattern's loiter center: GC marks it, RotorLab does not. No clause requires the marker.
                    rep.known << QStringLiteral("M31 %1: the loiter center is not marked by the other product (no contract clause)").arg(where);
                } else if (m.kind == QLatin1String("roi") && B.product == QLatin1String("RotorLab")) {
                    // Known RotorLab finding K1 (A222): RotorLab 3.6.0 draws no 195 for an ArduPilot structure scan (FW-4 rev 7).
                    rep.known << QStringLiteral("K1 %1: not drawn by RotorLab (A222, FW-4 rev 7)").arg(where);
                } else {
                    rep.failures << QStringLiteral("%1: not drawn by the other product (CL-7)").arg(where);
                }
                continue;
            }
            const bool breach = m.kind == QLatin1String("breach");
            // Known RotorLab finding K4: a rally point at 0 m above launch is drawn 30 m up
            // (missionedit.js:146) and its ball is not state-colored.
            const RMarker &rlRally = A.product == QLatin1String("RotorLab") ? m : *best;
            const RMarker &gcRally = A.product == QLatin1String("RotorLab") ? *best : m;
            const double rlBase = std::isfinite(rlRally.ground) ? std::max(rlRally.ground, A.launchRef) : A.launchRef;
            const bool k4 = m.kind == QLatin1String("rally") && std::isfinite(rlRally.amsl)
                         && std::abs(gcRally.amsl - A.launchRef) < kMarkerAmslTolM
                         && (std::abs(rlRally.amsl - (A.launchRef + 30.0)) < kMarkerAmslTolM
                             || std::abs(rlRally.amsl - (rlBase + 30.0)) < kMarkerAmslTolM);
            if (std::isfinite(m.amsl) != std::isfinite(best->amsl)
                    || (std::isfinite(m.amsl) && std::abs(m.amsl - best->amsl) > kMarkerAmslTolM)) {
                const QString msg = QStringLiteral("%1: AMSL %2 against %3").arg(where).arg(m.amsl).arg(best->amsl);
                if (k4) {
                    rep.known << QStringLiteral("K4 ") + msg + QStringLiteral(" (rally at 0 m drawn 30 m up, missionedit.js:146)");
                } else if (breach) {
                    // G66-PARITY-PLAN.md, known differences: the breach return height rule (A-9, A202, G86) is open.
                    rep.notes << msg + QStringLiteral(" (breach return height rule, open)");
                } else {
                    rep.failures << msg + QStringLiteral(" (section 2)");
                }
            }
            if (!breach && m.cls != Cls::None && best->cls != Cls::None && m.cls != best->cls) {
                rep.failures << QStringLiteral("%1: state %2 against %3").arg(where, clsName(m.cls), clsName(best->cls));
            }
        }
    }

    const Fixture &_fx;
    LocalFrame _f;
};

// ---- GC against the contract, on its own -----------------------------------

double expectedAmsl(int frame, double p7, double R, double G)
{
    switch (frame) {
    case 0: case 5:     return p7;
    case 3: case 6:     return R + p7;
    case 10: case 11:   return G + p7;
    default:            return kNaN;
    }
}

/// CL-1a, independently of Plan3DMath: the contact span of a takeoff or
/// landing leg, from the leg's ground end to the first sample whose HAG
/// reaches M. An unknown sample ends the span as GC reads it (its own state
/// is unknown either way).
QPair<int, int> oracleContactSpan(const QVector<RSample> &s, bool landing)
{
    auto ends = [&s](int k) { return !std::isfinite(s[k].hag) || s[k].hag >= kStrikeM; };
    const int n = int(s.size());
    if (!landing) {
        int k = 0;
        while (k < n && !ends(k)) {
            k++;
        }
        return {0, k};
    }
    int k = n - 1;
    while (k >= 0 && !ends(k)) {
        k--;
    }
    return {k + 1, n};
}

Cls expectedState(const RSample &s, bool inSpan)
{
    if (!std::isfinite(s.hag)) {
        return Cls::Unknown;
    }
    if (s.hag < kStrikeM) {
        return (inSpan && s.hag >= kContactFloorM) ? Cls::Contact : Cls::Strike;
    }
    return Cls::Path;
}

QMap<QString, int> &stateTotals()
{
    static QMap<QString, int> totals;
    return totals;
}

bool hasBannedWord(const QString &line)
{
    // CL-5: never clear, cleared, safe, OK, approved, or any word that reads as permission.
    static const QRegularExpression re(QStringLiteral("\\b(clear|cleared|safe|safely|approved|permitted)\\b"),
                                       QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression ok(QStringLiteral("\\bOK\\b"));
    return re.match(line).hasMatch() || ok.match(line).hasMatch();
}

QByteArray stripCr(QByteArray b)
{
    b.replace("\r\n", "\n");
    return b;
}

} // namespace

// ---------------------------------------------------------------------------

void Plan3DParityTest::initTestCase()
{
    _proxyFactory = new DeadLocalProxyFactory;
    QNetworkProxyFactory::setApplicationProxyFactory(_proxyFactory);
}

void Plan3DParityTest::cleanupTestCase()
{
    QNetworkProxyFactory::setApplicationProxyFactory(nullptr);   // deletes ours
    _proxyFactory = nullptr;
}

void Plan3DParityTest::init()
{
    UnitTest::init();
    _pmc = new PlanMasterController(this);
    _pmc->setFlyView(false);
    _pmc->start();
}

void Plan3DParityTest::cleanup()
{
    if (_pmc) {
        _pmc->setDirty(false);
    }
    delete _pmc;
    _pmc = nullptr;
    UnitTest::cleanup();
}

void Plan3DParityTest::_fixtureIsG66F1()
{
    const Fixture &fx = fixture();
    QVERIFY2(fx.ok, "test/Custom/plan-corpus/g66/G66-F1.json missing or not g66-fixture/1");
    QCOMPARE(fx.id, QStringLiteral("G66-F1"));
    QCOMPARE(fx.sha256, QString::fromLatin1(kFixtureSha256));
    QCOMPARE(fx.cellM, 10.0);
    // The cell GC's own rule computes from the grid is the cell both products sample by (CL-1 rev 4).
    QVERIFY(std::abs(Builder::gridCellM(fx.grid.area, fx.grid.cols, fx.grid.rows) - fx.cellM) < 1e-6);
    QCOMPARE(sampleSpacingM(fx.cellM), 5.0);
    // What the plan says the fixture holds (make_g66_fixture.py, "Features").
    LocalFrame f{fx.homeLat, fx.homeLon, 0};
    auto at = [&](double n, double e) { return fx.G(f.latFromNorth(n), f.lonFromEast(e)); };
    QCOMPARE(at(0, 0), 20.0);                                   // R = G(home) = H (A-0)
    QVERIFY(std::abs(at(0, 150) - 20.3) < 0.01);                // fixed-wing touchdown bump (CL-1a)
    QVERIFY(std::isnan(at(-150, 400)));                         // the hole (A-3)
    QVERIFY(at(150, 300) > 74.0);                               // the hill
    QVERIFY(at(250, 1205) > 100.0);                             // the ridge under the VTOL legs
    QVERIFY(std::isnan(at(0, 5000)));                           // outside the grid
}

void Plan3DParityTest::_exportsAreTheCorpus()
{
    // The files opened here are RotorLab's A204 exports. They must be the
    // corpus (T1, RT-1 and RT-8 after RT-7), so the parity is on the corpus.
    for (const QString &file : corpusFiles()) {
        QFile e(exportPath(file)), c(corpusPath(file));
        QVERIFY2(e.open(QIODevice::ReadOnly), qPrintable(e.fileName()));
        QVERIFY2(c.open(QIODevice::ReadOnly), qPrintable(c.fileName()));
        const QByteArray eb = e.readAll(), cb = c.readAll();
        QCOMPARE(QJsonDocument::fromJson(eb).object(), QJsonDocument::fromJson(cb).object());
        QVERIFY2(stripCr(eb) == stripCr(cb), qPrintable(file));
    }
}

void Plan3DParityTest::_comparatorSelfCheck()
{
    // The comparison finds nothing between a result and itself, and finds a
    // changed state and a changed height (so a pass means something).
    const Fixture &fx = fixture();
    QVERIFY(fx.ok);
    QFile f(exportPath(QStringLiteral("ardupilot-copter-alt-relative.plan")));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    _pmc->loadFromFile(f.fileName());
    const SourcePlan plan = sourceFromPlan(_pmc);
    QVERIFY(!plan.items.isEmpty());
    const Scene sc = Builder::build(plan, [&fx](double lat, double lon) { return fx.G(lat, lon); }, fx.cellM);
    const Result a = fromScene(sc, plan, f.fileName(), bytes);
    const Result round = fromJson(toJson(a));
    QVERIFY(round.ok);
    QCOMPARE(round.legs.size(), a.legs.size());

    const Comparator cmp(fx);
    Report same;
    cmp.compare(a, round, QStringLiteral("self"), same);
    QVERIFY2(same.failures.isEmpty(), qPrintable(same.failures.join(QLatin1Char('\n'))));
    QVERIFY(same.compared > 0);
    QCOMPARE(same.boundary, 0);

    // One path sample well clear of every band turned red: exactly that sample differs.
    Result changed = round;
    bool done = false;
    for (RLeg &l : changed.legs) {
        for (int i = 1; i + 1 < l.s.size() && !done; i++) {
            RSample &s = l.s[i];
            if (s.cls == Cls::Path && s.hag > 10 && l.s[i - 1].cls == Cls::Path && l.s[i + 1].cls == Cls::Path) {
                s.cls = Cls::Strike;
                done = true;
            }
        }
    }
    QVERIFY(done);
    Report one;
    cmp.compare(a, changed, QStringLiteral("self"), one);
    QVERIFY2(one.failures.size() >= 1, "a changed state was not found (A against B)");
    Report oneBack;
    cmp.compare(changed, a, QStringLiteral("self"), oneBack);
    QVERIFY2(oneBack.failures.size() >= 1, "a changed state was not found (B against A)");

    // Every height 0.2 m off: found on every leg with known heights.
    Result lifted = round;
    for (RLeg &l : lifted.legs) {
        for (RSample &s : l.s) {
            s.amsl += 0.2;
            s.hag += 0.2;
        }
        l.fAmsl += 0.2;
        l.tAmsl += 0.2;
    }
    Report off;
    cmp.compare(a, lifted, QStringLiteral("self"), off);
    QVERIFY2(!off.failures.filter(QStringLiteral("height above the fixture")).isEmpty(), "a 0.2 m height change was not found");
}

void Plan3DParityTest::_parity_data()
{
    QTest::addColumn<QString>("file");
    for (const QString &file : corpusFiles()) {
        QTest::newRow(qPrintable(file)) << file;
    }
}

void Plan3DParityTest::_parity()
{
    QFETCH(QString, file);
    const Fixture &fx = fixture();
    QVERIFY(fx.ok);
    const GroundFn G = [&fx](double lat, double lon) { return fx.G(lat, lon); };

    // ---- Open the RotorLab export through GC's own load path, read it as the 3D view does.
    QFile f(exportPath(file));
    QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(f.fileName()));
    const QByteArray bytes = f.readAll();
    const QJsonObject json = QJsonDocument::fromJson(bytes).object();
    _pmc->loadFromFile(f.fileName());
    // Read at once: GC's terrain replies (refused offline) need the event loop.
    const SourcePlan plan = sourceFromPlan(_pmc);
    QVERIFY2(!plan.items.isEmpty(), qPrintable(file));
    const Scene sc = Builder::build(plan, G, fx.cellM);
    const Result gc = fromScene(sc, plan, file, bytes);

    // ---- Write GC's result (schema g66-samples/1) for the comparison and the reviewers.
    QVERIFY(QDir().mkpath(gcOutDir()));
    {
        QFile out(QDir(gcOutDir()).filePath(resultName(file)));
        QVERIFY2(out.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(out.fileName()));
        out.write(QJsonDocument(toJson(gc)).toJson(QJsonDocument::Indented));
    }

    // ---- GC against the contract on the fixture, on its own.
    const double H = json[QStringLiteral("mission")].toObject()[QStringLiteral("plannedHomePosition")].toArray().at(2).toDouble();
    QCOMPARE(sc.launchRef, 20.0);                                   // A-0: R = G(home) on G66-F1
    QVERIFY(sc.launchRefFromGround);
    QCOMPARE(sc.homeDatumNote, std::abs(H - 20.0) > kHomeDatumNoteM);   // A-0b: the two synthetic files (H = 100)
    QCOMPARE(sc.sampleSpacingM, 5.0);                               // CL-1 rev 4: half the 10 m cell
    QVERIFY(!sc.samples.isEmpty());

    LocalFrame lf{fx.homeLat, fx.homeLon, 0};
    for (int li = 0; li < gc.legs.size(); li++) {
        const RLeg &l = gc.legs[li];
        const QString where = QStringLiteral("%1 leg %2 (item %3, command %4, %5)").arg(file).arg(li).arg(l.item).arg(l.command).arg(l.shape);
        QVERIFY2(l.s.size() >= 2, qPrintable(where));
        if (l.shape == QLatin1String("unknown")) {
            for (const RSample &s : l.s) {
                QVERIFY2(s.cls == Cls::Unknown, qPrintable(where + QStringLiteral(": an RTL sample not unknown (A-10)")));
            }
            continue;
        }
        if (l.shape != QLatin1String("ring")) {
            QVERIFY2(l.s.size() >= kSamplesPerLeg + 1, qPrintable(where + QStringLiteral(": fewer than 24 steps (CL-1)")));
        }
        const bool landing = l.contact == QLatin1String("land");
        const QPair<int, int> span = l.contact == QLatin1String("none") ? qMakePair(0, 0) : oracleContactSpan(l.s, landing);
        for (int i = 0; i < l.s.size(); i++) {
            const RSample &s = l.s[i];
            // Every sample is checked against the fixture itself.
            QVERIFY2((std::isnan(s.ground) && std::isnan(fx.G(s.lat, s.lon))) || std::abs(s.ground - fx.G(s.lat, s.lon)) < 1e-9,
                     qPrintable(where + QStringLiteral(": ground not the fixture's")));
            if (i > 0) {
                const double gap = std::hypot(lf.east(s.lon) - lf.east(l.s[i - 1].lon), lf.north(s.lat) - lf.north(l.s[i - 1].lat));
                QVERIFY2(gap <= sc.sampleSpacingM + 1e-6,
                         qPrintable(where + QStringLiteral(": samples %1 m apart (CL-1 rev 4)").arg(gap)));
            }
            const Cls want = expectedState(s, i >= span.first && i < span.second);
            QVERIFY2(s.cls == want,
                     qPrintable(where + QStringLiteral(": sample %1 (t %2, height above the fixture %3 m) is %4, the contract says %5 (section 4, CL-1a)")
                                    .arg(i).arg(s.t).arg(s.hag).arg(clsName(s.cls), clsName(want))));
            stateTotals()[clsName(s.cls)]++;
        }
    }

    // Section 2 at every route point, by frame and params[6] from R and the fixture.
    for (const Marker &m : sc.markers) {
        if (m.kind != MarkerKind::Waypoint && m.kind != MarkerKind::PatternPoint) {
            continue;
        }
        const SourceItem *it = nullptr;
        for (const SourceItem &s : plan.items) {
            if (s.seq == m.seq) {
                it = &s;
            }
        }
        QVERIFY2(it, qPrintable(QStringLiteral("%1: no item for seq %2").arg(file).arg(m.seq)));
        const double lat = sc.frame.latFromNorth(-m.pos.z), lon = sc.frame.lonFromEast(m.pos.x);
        const double want = expectedAmsl(it->frame, it->params[6], 20.0, fx.G(lat, lon));
        QVERIFY2((std::isnan(m.amsl) && std::isnan(want)) || std::abs(m.amsl - want) < 1e-6,
                 qPrintable(QStringLiteral("%1 seq %2: AMSL %3, the datum table says %4").arg(file).arg(m.seq).arg(m.amsl).arg(want)));
    }

    // FW-4 (rev 7): an ArduPilot structure scan aims a 195 at the structure center, never a 196.
    if (file == QLatin1String("ardupilot-copter-structure.plan")) {
        int roi195 = 0;
        for (const SourceItem &s : plan.items) {
            QVERIFY2(s.command != 196, "ArduPilot structure scan still expands to 196 (FW-4 rev 7, G85)");
            roi195 += s.command == 195 ? 1 : 0;
        }
        QCOMPARE(roi195, 1);
        double cLat = 0, cLon = 0;
        int n = 0;
        for (const QJsonValue &item : json[QStringLiteral("mission")].toObject()[QStringLiteral("items")].toArray()) {
            for (const QJsonValue &v : item.toObject()[QStringLiteral("polygon")].toArray()) {
                cLat += v.toArray().at(0).toDouble();
                cLon += v.toArray().at(1).toDouble();
                n++;
            }
        }
        QVERIFY(n > 0);
        int rois = 0;
        for (const RMarker &m : gc.markers) {
            if (m.kind == QLatin1String("roi")) {
                rois++;
                QCOMPARE(m.command, 195);
                QVERIFY(std::hypot(lf.east(m.lon) - lf.east(cLon / n), lf.north(m.lat) - lf.north(cLat / n)) < 0.05);
                QVERIFY(m.amsl >= 20.0 + 5.0 && m.amsl <= 20.0 + 25.0);     // within the scanned band, above launch
            }
        }
        QCOMPARE(rois, 1);
    }

    // ---- RotorLab's result for the same file, when delivered.
    QFile rf(QDir(rotorLabDir()).filePath(resultName(file)));
    if (!rf.exists()) {
        QSKIP(qPrintable(QStringLiteral("RotorLab result not delivered: %1 (GC result written to %2)")
                             .arg(rf.fileName(), gcOutDir())));
    }
    QVERIFY2(rf.open(QIODevice::ReadOnly), qPrintable(rf.fileName()));
    const Result rl = fromJson(QJsonDocument::fromJson(rf.readAll()).object());
    QVERIFY2(rl.ok, qPrintable(rf.fileName() + QStringLiteral(": not g66-samples/1")));
    QCOMPARE(rl.product, QStringLiteral("RotorLab"));
    QCOMPARE(rl.file, file);
    QCOMPARE(rl.fileSha256, gc.fileSha256);             // the same bytes were opened
    QCOMPARE(rl.fixtureId, fx.id);                      // colors only on the shared fixture (CL-6, OQ9)
    QCOMPARE(rl.fixtureSha256, fx.sha256);
    QCOMPARE(rl.cellM, fx.cellM);
    for (const char *k : {"path", "strike", "contact", "unknown"}) {
        QVERIFY2(rl.legend.contains(QLatin1String(k)), qPrintable(QStringLiteral("RotorLab legend has no %1 line").arg(QLatin1String(k))));
    }
    for (const QString &line : rl.legend) {
        QVERIFY2(!hasBannedWord(line), qPrintable(QStringLiteral("RotorLab legend line reads as permission (CL-5): ") + line));
    }

    const Comparator cmp(fx);
    Report rep;
    cmp.compare(gc, rl, QStringLiteral("GC->RotorLab"), rep);
    cmp.compare(rl, gc, QStringLiteral("RotorLab->GC"), rep);
    for (const QString &n : rl.schemaNotes) {
        qInfo().noquote() << file << "RotorLab result note:" << n;
    }
    for (const QString &n : rep.known) {
        qInfo().noquote() << file << "known RotorLab finding:" << n;
    }
    for (const QString &n : rep.notes) {
        qInfo().noquote() << file << "note:" << n;
    }
    qInfo().noquote() << file << "compared" << rep.compared << "samples," << rep.boundary << "within a band," << rep.known.size() << "known RotorLab findings," << rep.failures.size() << "differences";
    QVERIFY2(rep.failures.isEmpty(), qPrintable(file + QStringLiteral(":\n") + rep.failures.join(QLatin1Char('\n'))));
}

void Plan3DParityTest::_fixtureExercisesEveryState()
{
    // G66-F1 puts every section 4 state on the corpus, so a pass covers them all.
    const QMap<QString, int> &t = stateTotals();
    for (const char *k : {"path", "strike", "contact", "unknown"}) {
        QVERIFY2(t.value(QLatin1String(k)) > 0, qPrintable(QStringLiteral("no %1 sample on the 17 files").arg(QLatin1String(k))));
    }
}

void Plan3DParityTest::_legendLines()
{
    // CL-5 and section 4: one legend line per state, contact included (rev 5), none reading as permission.
    const QMap<QString, QString> legend = gcLegend();
    for (const char *k : {"path", "strike", "contact", "unknown"}) {
        QVERIFY2(legend.contains(QLatin1String(k)), qPrintable(QStringLiteral("GC legend has no %1 line").arg(QLatin1String(k))));
    }
    QVERIFY(legend.value(QStringLiteral("path")).contains(QStringLiteral("2 m")));
    QVERIFY(legend.value(QStringLiteral("strike")).contains(QStringLiteral("2 m")));
    QVERIFY(legend.value(QStringLiteral("contact")).contains(QStringLiteral("ground contact"), Qt::CaseInsensitive));
    QVERIFY(legend.value(QStringLiteral("contact")).contains(QStringLiteral("takeoff or landing"), Qt::CaseInsensitive));
    for (const QString &line : legend) {
        QVERIFY2(!hasBannedWord(line), qPrintable(QStringLiteral("GC legend line reads as permission (CL-5): ") + line));
    }
}

#endif  // RL_PLAN3D

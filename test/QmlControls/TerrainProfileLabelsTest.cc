/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

// RotorLab GC 5.1.2 (G78). Runs thinLabels() from TerrainProfileLabels.js (the same file
// TerrainStatus.qml imports) in a QJSEngine.

#include "TerrainProfileLabelsTest.h"

#include <QtCore/QFile>
#include <QtQml/QJSEngine>
#include <QtTest/QTest>

#include <cmath>

namespace {

// Loads TerrainProfileLabels.js (the file TerrainStatus.qml imports) from the source tree and returns
// its thinLabels function. The QML module resources are not loaded in a unit-test run.
QJSValue thinLabelsFunction(QJSEngine& engine)
{
    const QString path = QStringLiteral(QT_TESTCASE_SOURCEDIR "/src/QmlControls/TerrainProfileLabels.js");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "cannot read" << path;
        return QJSValue();
    }
    // ".pragma library" is QML import syntax, not JavaScript; drop it for a plain engine.
    QString source = QString::fromUtf8(file.readAll());
    source.replace(QStringLiteral(".pragma library"), QString());
    const QJSValue result = engine.evaluate(source, path);
    if (result.isError()) {
        qWarning() << "TerrainProfileLabels.js:" << result.toString();
        return QJSValue();
    }
    return engine.globalObject().property(QStringLiteral("thinLabels"));
}

QList<bool> thin(const QList<double>& positions, const QList<bool>& forced, double minSpacing, const QList<double>& widths = {})
{
    QJSEngine engine;
    QJSValue fn = thinLabelsFunction(engine);
    if (!fn.isCallable()) {
        return {};
    }
    QJSValue jsPositions = engine.newArray(positions.count());
    QJSValue jsForced = engine.newArray(forced.count());
    for (int i = 0; i < positions.count(); i++) {
        jsPositions.setProperty(i, positions[i]);
        jsForced.setProperty(i, forced.value(i, false));
    }
    QJSValueList args = { jsPositions, jsForced, minSpacing };
    if (!widths.isEmpty()) {
        QJSValue jsWidths = engine.newArray(widths.count());
        for (int i = 0; i < widths.count(); i++) {
            jsWidths.setProperty(i, widths[i]);
        }
        args.append(jsWidths);
    }
    const QJSValue out = fn.call(args);
    QList<bool> shown;
    const int length = out.property(QStringLiteral("length")).toInt();
    for (int i = 0; i < length; i++) {
        shown.append(out.property(i).toBool());
    }
    return shown;
}

// The smallest distance between two shown labels.
double minGap(const QList<double>& positions, const QList<bool>& shown)
{
    double gap = INFINITY;
    double last = NAN;
    for (int i = 0; i < shown.count(); i++) {
        if (!shown[i]) {
            continue;
        }
        if (!std::isnan(last)) {
            gap = qMin(gap, positions[i] - last);
        }
        last = positions[i];
    }
    return gap;
}

} // namespace

// 300 waypoints over 900 px (3 px apart), labels need 20 px: before G78 all 300 were drawn.
void TerrainProfileLabelsTest::_longPlanIsThinned()
{
    QList<double> positions;
    for (int i = 0; i < 300; i++) {
        positions.append(i * 3.0);
    }
    const QList<bool> shown = thin(positions, QList<bool>(300, false), 20.0);
    QCOMPARE(shown.count(), 300);

    const int count = shown.count(true);
    QVERIFY2(count >= 40 && count <= 46, qPrintable(QString::number(count)));   // about 900 / 20
    QVERIFY(minGap(positions, shown) >= 20.0);
    QVERIFY(shown.first());
    QVERIFY(shown.last());
}

void TerrainProfileLabelsTest::_firstLastAndSelectedKept()
{
    QList<double> positions;
    for (int i = 0; i < 100; i++) {
        positions.append(i * 2.0);
    }
    QList<bool> forced(100, false);
    forced[51] = true;  // selected item, not on the thinning grid

    const QList<bool> shown = thin(positions, forced, 15.0);
    QCOMPARE(shown.count(), 100);
    QVERIFY(shown[0]);
    QVERIFY(shown[51]);
    QVERIFY(shown[99]);
    // No thinned label crowds a kept one, and none crowds another.
    QVERIFY(minGap(positions, shown) >= 15.0);
}

// A short plan with room for every label loses none.
void TerrainProfileLabelsTest::_sparsePlanKeepsEveryLabel()
{
    const QList<double> positions = { 0, 50, 100, 150, 200 };
    const QList<bool> shown = thin(positions, QList<bool>(5, false), 20.0);
    QCOMPARE(shown, QList<bool>({ true, true, true, true, true }));
}

// Slots with no label (NaN: a complex item's unused exit slot, an item with no coordinate) stay hidden
// and do not count as the first or last label.
void TerrainProfileLabelsTest::_slotsWithoutLabel()
{
    const QList<double> positions = { NAN, 0, NAN, 5, NAN, 100, NAN };
    const QList<bool> shown = thin(positions, QList<bool>(7, false), 20.0);
    QCOMPARE(shown, QList<bool>({ false, true, false, false, false, true, false }));

    QCOMPARE(thin({}, {}, 20.0), QList<bool>());
}

// Labels have their real widths (1, 2 or 3 digits): shown labels keep the gap edge to edge, so
// 3-digit labels end up further apart than 1-digit ones (the 5.1.2 review saw 114/123/132 touch).
void TerrainProfileLabelsTest::_widerLabelsKeepClearance()
{
    const double charWidth = 8.0;
    const double gap = charWidth * 2.5;
    QList<double> positions;
    QList<double> widths;
    for (int i = 0; i < 302; i++) {
        positions.append(i * 6.0);
        widths.append(qMax(10.0, QString::number(i).length() * charWidth));
    }
    const QList<bool> shown = thin(positions, QList<bool>(302, false), gap, widths);
    QCOMPARE(shown.count(), 302);

    // Edge-to-edge gap between every pair of shown neighbours.
    int last = -1;
    double smallestGap = INFINITY;
    for (int i = 0; i < shown.count(); i++) {
        if (!shown[i]) {
            continue;
        }
        if (last >= 0) {
            smallestGap = qMin(smallestGap, positions[i] - positions[last] - (widths[i] + widths[last]) / 2);
        }
        last = i;
    }
    QVERIFY2(smallestGap >= gap, qPrintable(QString::number(smallestGap)));
    QVERIFY(shown.first());
    QVERIFY(shown.last());
}

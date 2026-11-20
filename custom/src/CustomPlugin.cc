/****************************************************************************
 *
 * (c) 2009-2019 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 *   @brief Custom QGCCorePlugin Implementation
 *   @author Gus Grubba <gus@auterion.com>
 */

/****************************************************************************
 *
 * RotorLab GC custom build plugin. Modified by RotorLab, Inc., 2026, from
 * custom-example/src/CustomPlugin.cc.
 *
 ****************************************************************************/

#include "CustomPlugin.h"
#include "QGCLoggingCategory.h"
#include "QGCPalette.h"
#include "tak/TakManager.h"
#include "license/LicenseManager.h"
#include "LinkManager.h"
#include "fleet/FleetConnector.h"
#include "rotorlab/RotorLabConnector.h"
#include "video/common/VideoTapRegistry.h"
#include "video/record/VideoRecorder.h"
// G109: the live relay, where it is on the branch and the FFmpeg receiver is built.
#if defined(QGC_FFMPEG_STREAMING) && __has_include("video/relay/VideoRelay.h")
#define RLGC_VIDEO_RELAY
#include "video/relay/VideoRelay.h"
#endif
#ifdef RL_PLAN3D
#include "plan3d/Plan3DScene.h"
#include "plan3d/Plan3DGroundMesh.h"
#ifdef QGC_DAILY_BUILD
#include "plan3d/Plan3DProbe.h"
#endif
#endif

#include <QtCore/QApplicationStatic>
#include <QtCore/QFile>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>

QGC_LOGGING_CATEGORY(CustomLog, "gcs.custom.customplugin")

Q_APPLICATION_STATIC(CustomPlugin, _customPluginInstance);

CustomPlugin::CustomPlugin(QObject *parent)
    : QGCCorePlugin(parent)
{
}

CustomPlugin::~CustomPlugin()
{
}

// No link behind the gate. LinkManager opens vehicle
// links only while LicenseManager::linksAllowed() is true: before the first
// link starts at boot (QGCApplication runs this before LinkManager::init and
// startAutoConnectedLinks), and on every change after. Not run in unit tests,
// which drive LinkManager::setLinksAllowed themselves.
void CustomPlugin::init()
{
    QGCCorePlugin::init();
    wireLinkGate(LicenseManager::instance(), LinkManager::instance());
}

void CustomPlugin::wireLinkGate(LicenseManager *license, LinkManager *links)
{
    links->setLinksAllowed(license->linksAllowed(), LicenseManager::linkRefusedText());
    (void) connect(license, &LicenseManager::linksAllowedChanged, links, [links](bool allowed) {
        links->setLinksAllowed(allowed);
    });
}

void CustomPlugin::cleanup()
{
    if (_qmlEngine) {
        _qmlEngine->removeUrlInterceptor(_selector);
    }
    delete _selector;

    QGCCorePlugin::cleanup();
}

QQmlApplicationEngine *CustomPlugin::createQmlApplicationEngine(QObject *parent)
{
    _qmlEngine = QGCCorePlugin::createQmlApplicationEngine(parent);

    _selector = new CustomOverrideInterceptor();
    _qmlEngine->addUrlInterceptor(_selector);

    // Expose the TAK/ATAK manager to QML as `TAKManager` for the settings page.
    _qmlEngine->rootContext()->setContextProperty(QStringLiteral("TAKManager"), TakManager::instance());

    // Expose the license manager to QML as `LicenseManager` for the startup
    // gate and the License settings page.
    _qmlEngine->rootContext()->setContextProperty(QStringLiteral("LicenseManager"), LicenseManager::instance());

    // Expose the Fleet Server connector to QML as `FleetConnector` for the
    // Fleet Connector settings page.
    _qmlEngine->rootContext()->setContextProperty(QStringLiteral("FleetConnector"), FleetConnector::instance());

    // Expose the direct RotorLab.app connector to QML as `RotorLabConnector`.
    // It is the alternative to the Fleet Connector, not an addition to it -
    // exactly one of the two is active at a time.
    _qmlEngine->rootContext()->setContextProperty(QStringLiteral("RotorLabConnector"), RotorLabConnector::instance());

    // G95: local video recording, as `VideoRecorder` for its settings section
    // and the Fly view status chip. Present in every build; `available` is
    // false where GC's own video receiver is not built.
    _qmlEngine->rootContext()->setContextProperty(QStringLiteral("VideoRecorder"), VideoRecorder::instance());
#ifdef RLGC_VIDEO_RELAY
    // G109: live relay to the Fleet Server, as `VideoRelay` for its settings section and the Fly view chip
    _qmlEngine->rootContext()->setContextProperty(QStringLiteral("VideoRelay"), VideoRelay::instance());
#endif

    // G64: Plan view 3D, Windows and Linux builds only. PlanView.qml shows
    // the "3D" toggle only when RLPlan3DAvailable is true; on Android it is
    // false and the 2D Plan view is all there is.
#ifdef RL_PLAN3D
    Plan3D::registerQmlTypes();
    Plan3D::registerGroundQmlTypes();
    _qmlEngine->rootContext()->setContextProperty(QStringLiteral("RLPlan3DAvailable"), true);
    QObject *probe = nullptr;
#ifdef QGC_DAILY_BUILD
    // Test-only timing and screenshot autorun (G64), Daily builds only.
    if (qEnvironmentVariableIntValue("RL_G64_AUTORUN") == 1) {
        probe = new Plan3D::FrameProbe(_qmlEngine);
    }
#endif
    _qmlEngine->rootContext()->setContextProperty(QStringLiteral("RLPlan3DProbe"), probe);
    _qmlEngine->rootContext()->setContextProperty(QStringLiteral("RLPlan3DAutorun"), probe != nullptr);
#else
    _qmlEngine->rootContext()->setContextProperty(QStringLiteral("RLPlan3DAvailable"), false);
    _qmlEngine->rootContext()->setContextProperty(QStringLiteral("RLPlan3DAutorun"), false);
#endif

    return _qmlEngine;
}

VideoReceiver *CustomPlugin::createVideoReceiver(QObject *parent)
{
    VideoReceiver *receiver = QGCCorePlugin::createVideoReceiver(parent);
    VideoTapRegistry::noteReceiver(receiver);
    return receiver;
}

QGCCorePlugin *CustomPlugin::instance()
{
    return _customPluginInstance();
}

QString CustomPlugin::brandImageIndoor() const
{
    return QStringLiteral("/custom/img/rotorlab-original.svg");
}

QString CustomPlugin::brandImageOutdoor() const
{
    return QStringLiteral("/custom/img/rotorlab-original.svg");
}

// RotorLab theme (matches the RotorLab web app).
//
// Dark:  bg #0d1117 / surface #151b23 / raised #1b222c / lines #2c3744
//        ink #e6edf3, accent #4d8bff (logo blue), accent-dim #2f5db3,
//        accent-soft #16233b
// Light: bg #eef1f5 / surface #ffffff / raised #f3f6fa / lines #d3dae3
//        ink #0f1722, accent #1f6feb, accent-soft #e8f0fe
void CustomPlugin::paletteOverride(const QString &colorName, QGCPalette::PaletteColorInfo_t &colorInfo)
{
    if (colorName == QStringLiteral("window")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#0d1117");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#0d1117");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#eef1f5");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#eef1f5");
    }
    else if (colorName == QStringLiteral("toolbarBackground")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#0a0d12");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#0a0d12");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#ffffff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#ffffff");
    }
    else if (colorName == QStringLiteral("windowShadeLight")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#2c3744");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#222b36");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#8b97a6");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#9aa4b2");
    }
    else if (colorName == QStringLiteral("windowShade")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#151b23");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#151b23");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#f3f6fa");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#f3f6fa");
    }
    else if (colorName == QStringLiteral("windowShadeDark")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#0a0d12");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#0a0d12");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#d3dae3");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#d3dae3");
    }
    else if (colorName == QStringLiteral("text")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#e6edf3");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#5b6573");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#0f1722");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#9aa4b2");
    }
    else if (colorName == QStringLiteral("button")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#1b222c");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#151b23");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#ffffff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#ffffff");
    }
    else if (colorName == QStringLiteral("buttonBorder")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#2c3744");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#222b36");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#d3dae3");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#e2e7ee");
    }
    else if (colorName == QStringLiteral("buttonText")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#e6edf3");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#5b6573");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#0f1722");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#9aa4b2");
    }
    else if (colorName == QStringLiteral("buttonHighlight")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#4d8bff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#2c3744");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#1f6feb");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#e2e7ee");
    }
    else if (colorName == QStringLiteral("buttonHighlightText")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#0d1117");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#5b6573");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#ffffff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#5b6573");
    }
    else if (colorName == QStringLiteral("primaryButton")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#4d8bff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#2c3744");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#1f6feb");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#e2e7ee");
    }
    else if (colorName == QStringLiteral("primaryButtonText")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#0d1117");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#5b6573");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#ffffff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#9aa4b2");
    }
    else if (colorName == QStringLiteral("textField")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#1b222c");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#151b23");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#ffffff");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#ffffff");
    }
    else if (colorName == QStringLiteral("textFieldText")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#e6edf3");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#5b6573");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#0f1722");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#8b97a6");
    }
    else if (colorName == QStringLiteral("mapButton")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#0d1117");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#2c3744");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#0d1117");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#2c3744");
    }
    else if (colorName == QStringLiteral("mapButtonHighlight")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#4d8bff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#2c3744");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#1f6feb");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#2c3744");
    }
    else if (colorName == QStringLiteral("mapIndicator")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#4d8bff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#2c3744");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#1f6feb");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#2c3744");
    }
    else if (colorName == QStringLiteral("mapIndicatorChild")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#2f5db3");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#2c3744");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#3a72e6");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#2c3744");
    }
    else if (colorName == QStringLiteral("missionItemEditor")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#16233b");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#151b23");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#e8f0fe");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#f3f6fa");
    }
    else if (colorName == QStringLiteral("toolStripHoverColor")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#4d8bff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#2c3744");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#1f6feb");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#9aa4b2");
    }
    else if (colorName == QStringLiteral("groupBorder")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#2c3744");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#222b36");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#d3dae3");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#d3dae3");
    }
    // Toolbar gradient behind the logo/status area (purple in stock QGC)
    else if (colorName == QStringLiteral("brandingPurple")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#151b23");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#151b23");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#151b23");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#151b23");
    }
    // Slider border accent (light blue / purple in stock QGC)
    else if (colorName == QStringLiteral("brandingBlue")) {
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupEnabled]   = QColor("#4d8bff");
        colorInfo[QGCPalette::Dark][QGCPalette::ColorGroupDisabled]  = QColor("#2f5db3");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupEnabled]  = QColor("#1f6feb");
        colorInfo[QGCPalette::Light][QGCPalette::ColorGroupDisabled] = QColor("#2f5db3");
    }
}

/*===========================================================================*/

CustomOverrideInterceptor::CustomOverrideInterceptor()
    : QQmlAbstractUrlInterceptor()
{
}

QUrl CustomOverrideInterceptor::intercept(const QUrl &url, QQmlAbstractUrlInterceptor::DataType type)
{
    switch (type) {
    using DataType = QQmlAbstractUrlInterceptor::DataType;
    case DataType::QmlFile:
    case DataType::UrlString:
        if (url.scheme() == QStringLiteral("qrc")) {
            const QString origPath = url.path();
            const QString overrideRes = QStringLiteral(":/Custom%1").arg(origPath);
            if (QFile::exists(overrideRes)) {
                const QString relPath = overrideRes.mid(2);
                QUrl result;
                result.setScheme(QStringLiteral("qrc"));
                result.setPath('/' + relPath);
                return result;
            }
        }
        break;
    default:
        break;
    }

    return url;
}

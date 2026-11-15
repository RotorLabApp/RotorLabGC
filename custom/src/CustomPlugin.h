/****************************************************************************
 *
 * (c) 2009-2019 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 *   @brief Custom QGCCorePlugin Declaration
 *   @author Gus Grubba <gus@auterion.com>
 */

/****************************************************************************
 *
 * RotorLab GC custom build plugin. Modified by RotorLab Inc, 2026, from
 * custom-example/src/CustomPlugin.h.
 *
 * Branding-only QGCCorePlugin override: app name, brand images and the
 * RotorLab blue color palette. All stock QGroundControl functionality is
 * left untouched.
 *
 ****************************************************************************/

#pragma once

#include <QtQml/QQmlAbstractUrlInterceptor>

#include "QGCCorePlugin.h"

class QQmlApplicationEngine;

Q_DECLARE_LOGGING_CATEGORY(CustomLog)

class CustomPlugin : public QGCCorePlugin
{
    Q_OBJECT

public:
    explicit CustomPlugin(QObject *parent = nullptr);
    ~CustomPlugin();

    static QGCCorePlugin *instance();

    // Overrides from QGCCorePlugin
    void cleanup() final;
    QString brandImageIndoor() const final;
    QString brandImageOutdoor() const final;
    void paletteOverride(const QString &colorName, QGCPalette::PaletteColorInfo_t &colorInfo) final;
    QQmlApplicationEngine *createQmlApplicationEngine(QObject *parent) final;

private:
    QQmlApplicationEngine *_qmlEngine = nullptr;
    class CustomOverrideInterceptor *_selector = nullptr;
};

/*===========================================================================*/

// Redirects qrc:/<path> to qrc:/Custom/<path> when the custom resource exists,
// letting custom.qrc replace stock resources (e.g. the QGC logo) without
// touching core files.
class CustomOverrideInterceptor : public QQmlAbstractUrlInterceptor
{
public:
    CustomOverrideInterceptor();

    QUrl intercept(const QUrl &url, QQmlAbstractUrlInterceptor::DataType type) final;
};

/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - frame-time probe, Daily builds only.
 *
 * The G63 spike's method (SPIKE.md, "Method"): the interval between
 * successive QQuickWindow::frameSwapped signals (render thread, direct
 * connection), with a new frame requested after every swap while a run is
 * active, so a still scene is measured at its sustained cost; plus the
 * render-thread time from beforeSynchronizing to afterRendering. Used by
 * Plan3DAutorun.qml when RL_G64_AUTORUN=1. Not compiled into stable builds.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QMutex>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QVector>
#include <QtQuick/QQuickWindow>

namespace Plan3D {

class FrameProbe : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QQuickWindow *window READ window WRITE setWindow NOTIFY windowChanged)

public:
    explicit FrameProbe(QObject *parent = nullptr);

    QQuickWindow *window() const { return _window; }
    void setWindow(QQuickWindow *w);

    Q_INVOKABLE void start(const QString &label, int seconds);
    Q_INVOKABLE QString env(const QString &name) const;
    Q_INVOKABLE QString outDir() const;
    Q_INVOKABLE bool saveScreenshot(const QString &name);
    Q_INVOKABLE void log(const QString &line);
    Q_INVOKABLE double nowMs() const { return _clock.nsecsElapsed() / 1.0e6; }

signals:
    void windowChanged();
    void finished(const QString &label);
    void frame();

private:
    void _onFrameSwapped();
    void _finish();

    QPointer<QQuickWindow> _window;
    QElapsedTimer   _clock;
    QMutex          _mutex;
    bool            _running = false;
    QString         _label;
    QString         _device;
    qint64          _lastSwapNs = -1;
    qint64          _syncStartNs = -1;
    QVector<double> _intervalsMs;
    QVector<double> _renderMs;
};

} // namespace Plan3D

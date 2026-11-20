/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - frame-time probe, Daily builds only.
 *
 ****************************************************************************/

#include "Plan3DProbe.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QSysInfo>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <rhi/qrhi.h>

#include <algorithm>

namespace Plan3D {

FrameProbe::FrameProbe(QObject *parent)
    : QObject(parent)
{
    _clock.start();
}

void FrameProbe::setWindow(QQuickWindow *w)
{
    if (_window == w) {
        return;
    }
    if (_window) {
        disconnect(_window, nullptr, this, nullptr);
    }
    _window = w;
    if (_window) {
        connect(_window, &QQuickWindow::frameSwapped, this, &FrameProbe::_onFrameSwapped, Qt::DirectConnection);
        connect(_window, &QQuickWindow::beforeSynchronizing, this, [this]() {
            QMutexLocker lock(&_mutex);
            _syncStartNs = _clock.nsecsElapsed();
        }, Qt::DirectConnection);
        connect(_window, &QQuickWindow::afterRendering, this, [this]() {
            QMutexLocker lock(&_mutex);
            if (_running && _syncStartNs >= 0) {
                _renderMs.append((_clock.nsecsElapsed() - _syncStartNs) / 1.0e6);
            }
        }, Qt::DirectConnection);
    }
    emit windowChanged();
}

QString FrameProbe::env(const QString &name) const
{
    return qEnvironmentVariable(name.toUtf8().constData());
}

QString FrameProbe::outDir() const
{
    QString d = qEnvironmentVariable("RL_G64_OUT");
    if (d.isEmpty()) {
        d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    QDir().mkpath(d);
    return d;
}

bool FrameProbe::saveScreenshot(const QString &name)
{
    if (!_window) {
        return false;
    }
    const QString path = outDir() + QLatin1Char('/') + name;
    const bool ok = _window->grabWindow().save(path);
    log(QStringLiteral("screenshot %1 %2").arg(name, ok ? QStringLiteral("saved") : QStringLiteral("FAILED")));
    return ok;
}

void FrameProbe::log(const QString &line)
{
    const QString stamped = QDateTime::currentDateTime().toString(Qt::ISODate) + QLatin1Char(' ') + line;
    qInfo().noquote() << "G64" << stamped;
    QFile f(outDir() + QStringLiteral("/g64_results.txt"));
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream(&f) << stamped << '\n';
    }
}

void FrameProbe::start(const QString &label, int seconds)
{
    {
        QMutexLocker lock(&_mutex);
        _label = label;
        _intervalsMs.clear();
        _renderMs.clear();
        _lastSwapNs = -1;
        _running = true;
    }
    log(QStringLiteral("run start label=%1 seconds=%2").arg(label).arg(seconds));
    QTimer::singleShot(seconds * 1000, this, &FrameProbe::_finish);
    if (_window) {
        _window->update();
    }
}

void FrameProbe::_onFrameSwapped()
{
    bool running;
    {
        QMutexLocker lock(&_mutex);
        const qint64 now = _clock.nsecsElapsed();
        if (_device.isEmpty() && _window && _window->rhi()) {
            const QRhiDriverInfo di = _window->rhi()->driverInfo();
            _device = QStringLiteral("backend=%1 device=\"%2\" os=\"%3\"")
                          .arg(QString::fromLatin1(_window->rhi()->backendName()), QString::fromUtf8(di.deviceName), QSysInfo::prettyProductName());
        }
        running = _running;
        if (running) {
            if (_lastSwapNs >= 0) {
                _intervalsMs.append((now - _lastSwapNs) / 1.0e6);
            }
            _lastSwapNs = now;
        }
    }
    if (running) {
        QMetaObject::invokeMethod(this, [this]() {
            emit frame();
            if (_window) {
                _window->update();
            }
        }, Qt::QueuedConnection);
    }
}

static double pct(const QVector<double> &sorted, double p)
{
    if (sorted.isEmpty()) {
        return 0;
    }
    const int i = std::clamp(int(p * (sorted.size() - 1) + 0.5), 0, int(sorted.size() - 1));
    return sorted[i];
}

void FrameProbe::_finish()
{
    QVector<double> iv, rv;
    QString label;
    {
        QMutexLocker lock(&_mutex);
        _running = false;
        iv = _intervalsMs;
        rv = _renderMs;
        label = _label;
    }
    std::sort(iv.begin(), iv.end());
    std::sort(rv.begin(), rv.end());
    double sum = 0;
    for (double v : iv) {
        sum += v;
    }
    log(QStringLiteral("RESULT label=%1 frames=%2 fps_mean=%3 frame_ms_min=%4 median=%5 p95=%6 max=%7 renderthread_ms_median=%8 p95=%9 %10 window=%11x%12")
            .arg(label)
            .arg(iv.size())
            .arg(sum > 0 ? 1000.0 * iv.size() / sum : 0.0, 0, 'f', 1)
            .arg(pct(iv, 0), 0, 'f', 2)
            .arg(pct(iv, 0.5), 0, 'f', 2)
            .arg(pct(iv, 0.95), 0, 'f', 2)
            .arg(pct(iv, 1), 0, 'f', 2)
            .arg(pct(rv, 0.5), 0, 'f', 2)
            .arg(pct(rv, 0.95), 0, 'f', 2)
            .arg(_device)
            .arg(_window ? _window->width() : 0)
            .arg(_window ? _window->height() : 0));
    emit finished(label);
}

} // namespace Plan3D

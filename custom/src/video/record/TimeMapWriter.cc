/****************************************************************************
 *
 * RotorLab GC - the recording's time map sidecar (G95).
 *
 ****************************************************************************/

#include "TimeMapWriter.h"

#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

TimeMapWriter::TimeMapWriter()
    : _hash(QCryptographicHash::Sha256)
{
}

TimeMapWriter::~TimeMapWriter()
{
    if (_file.isOpen()) {
        (void) close();
    }
}

bool TimeMapWriter::syncFile(QFile &file)
{
    if (!file.isOpen()) {
        return false;
    }
    if (!file.flush()) {
        return false;
    }
#ifdef Q_OS_WIN
    const intptr_t h = _get_osfhandle(file.handle());
    if (h == -1) {
        return false;
    }
    return FlushFileBuffers(reinterpret_cast<HANDLE>(h)) != 0;
#else
    return ::fsync(file.handle()) == 0;
#endif
}

bool TimeMapWriter::open(const QString &path)
{
    _error.clear();
    _hash.reset();
    _size = 0;
    if (QFileInfo(path).exists() && QFileInfo(path).size() > 0) {
        _error = QStringLiteral("the time map already exists");
        return false;
    }
    _file.setFileName(path);
    // Append and nothing else: never seeks back, never truncates.
    if (!_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        _error = _file.errorString();
        return false;
    }
    return true;
}

bool TimeMapWriter::writeLine(const QJsonObject &line)
{
    if (!_file.isOpen()) {
        return false;
    }
    QByteArray bytes = QJsonDocument(line).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    if (_file.write(bytes) != bytes.size()) {
        _error = _file.errorString();
        return false;
    }
    _hash.addData(bytes);
    _size += bytes.size();
    return true;
}

bool TimeMapWriter::flush()
{
    return _file.isOpen() && _file.flush();
}

bool TimeMapWriter::sync()
{
    return syncFile(_file);
}

QByteArray TimeMapWriter::close()
{
    if (!_file.isOpen()) {
        return {};
    }
    (void) syncFile(_file);
    _file.close();
    return _hash.result().toHex();
}

QByteArray TimeMapWriter::chainStart(const QByteArray &initSegment)
{
    return QCryptographicHash::hash(initSegment, QCryptographicHash::Sha256);
}

QByteArray TimeMapWriter::chainNext(const QByteArray &previousChain, const QByteArray &fragment)
{
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(previousChain);
    h.addData(fragment);
    return h.result();
}

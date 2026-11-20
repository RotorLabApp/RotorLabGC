/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "FFmpegSource.h"
#include "VideoReceiver.h"

#include <QtNetwork/QHostAddress>

namespace {

constexpr int kMaxUriLength = 2048;

// "host:port", ":port", "port" or "[v6]:port" after the scheme, nothing after the port.
// Returns false on junk.
bool splitHostPort(const QString &hostPort, QString &host, int &port)
{
    if (hostPort.contains(QLatin1Char('@'))) {
        return false;   // user info has no meaning here
    }

    QString portText;
    if (hostPort.startsWith(QLatin1Char('['))) {
        const qsizetype close = hostPort.indexOf(QLatin1Char(']'));
        if ((close < 0) || (close + 1 >= hostPort.size()) || (hostPort.at(close + 1) != QLatin1Char(':'))) {
            return false;
        }
        host = hostPort.mid(1, close - 1);
        portText = hostPort.mid(close + 2);
    } else {
        const qsizetype colon = hostPort.lastIndexOf(QLatin1Char(':'));
        if (colon < 0) {
            host.clear();
            portText = hostPort;
        } else {
            if (hostPort.indexOf(QLatin1Char(':')) != colon) {
                return false;   // a bare IPv6 address needs brackets
            }
            host = hostPort.left(colon);
            portText = hostPort.mid(colon + 1);
        }
    }

    for (const QChar c : std::as_const(portText)) {
        if ((c < QLatin1Char('0')) || (c > QLatin1Char('9'))) {
            return false;
        }
    }
    bool ok = false;
    port = portText.toInt(&ok);
    if (!ok || (port < 1) || (port > 65535)) {
        port = 0;
        return false;
    }
    return true;
}

// A DNS name or an IP address literal: letters, digits, '-', '.', and ':' inside brackets only
bool plainHostName(const QString &host)
{
    if (host.isEmpty() || (host.size() > 253)) {
        return false;
    }
    QHostAddress address;
    if (address.setAddress(host)) {
        return true;
    }
    for (const QChar c : host) {
        const bool ok = ((c >= QLatin1Char('a')) && (c <= QLatin1Char('z'))) || ((c >= QLatin1Char('A')) && (c <= QLatin1Char('Z'))) ||
                        ((c >= QLatin1Char('0')) && (c <= QLatin1Char('9'))) || (c == QLatin1Char('-')) || (c == QLatin1Char('.'));
        if (!ok) {
            return false;
        }
    }
    return true;
}

QByteArray urlHost(const QString &host)
{
    QHostAddress address;
    if (address.setAddress(host) && (address.protocol() == QAbstractSocket::IPv6Protocol)) {
        return QByteArrayLiteral("[") + host.toLatin1() + QByteArrayLiteral("]");
    }
    return host.toLatin1();
}

} // namespace

QString FFmpegSource::logText(const QString &uri)
{
    return VideoReceiver::redactedUri(uri);
}

FFmpegSource FFmpegSource::fromUri(const QString &uri, bool lowLatency)
{
    FFmpegSource source;
    const QString trimmed = uri.trimmed();

    if (trimmed.isEmpty()) {
        source.error = QStringLiteral("no video URI");
        return source;
    }
    if (trimmed.size() > kMaxUriLength) {
        source.error = QStringLiteral("video URI is longer than %1 characters").arg(kMaxUriLength);
        return source;
    }
    for (const QChar c : trimmed) {
        if ((c.unicode() < 0x21) || (c.unicode() > 0x7e)) {
            source.error = QStringLiteral("video URI has spaces, control or non-ASCII characters: %1").arg(logText(trimmed));
            return source;
        }
    }

    const qsizetype sep = trimmed.indexOf(QStringLiteral("://"));
    if (sep <= 0) {
        source.error = QStringLiteral("not a video URI: %1").arg(logText(trimmed));
        return source;
    }
    source.scheme = trimmed.left(sep).toLower();
    const QString rest = trimmed.mid(sep + 3);

    // Design: every option is set on this one input, never a process-wide environment variable.
    // The whitelist names the only protocols FFmpeg may open for it (never file, http or data).
    const QByteArray timeoutUs = QByteArrayLiteral("5000000");

    if ((source.scheme == QLatin1String("udp")) || (source.scheme == QLatin1String("udp265"))) {
        source.codecName = (source.scheme == QLatin1String("udp")) ? QStringLiteral("h264") : QStringLiteral("hevc");
        QString host;
        int port = 0;
        if (rest.contains(QLatin1Char('/')) || rest.contains(QLatin1Char('?')) || !splitHostPort(rest, host, port)) {
            source.error = QStringLiteral("cannot read host and port from %1").arg(logText(trimmed));
            return source;
        }
        if (host.isEmpty()) {
            host = QStringLiteral("0.0.0.0");
        }
        QHostAddress address;
        if (!address.setAddress(host)) {
            // A receiver listens on an address; a host name would need a DNS lookup
            source.error = QStringLiteral("\"%1\" is not an IP address").arg(host);
            return source;
        }
        source.kind = Kind::Rtp;
        source.host = host;
        source.port = port;
        return source;
    }

    if ((source.scheme == QLatin1String("rtsp")) || (source.scheme == QLatin1String("rtsps"))) {
        // Authority (with optional user info) and path go to FFmpeg as given
        const qsizetype slash = rest.indexOf(QLatin1Char('/'));
        QString authority = (slash < 0) ? rest : rest.left(slash);
        const qsizetype at = authority.lastIndexOf(QLatin1Char('@'));
        if (at >= 0) {
            authority = authority.mid(at + 1);
        }
        QString host;
        int port = 0;
        const bool hasPort = authority.startsWith(QLatin1Char('[')) ? authority.contains(QStringLiteral("]:")) : authority.contains(QLatin1Char(':'));
        if (hasPort) {
            if (!splitHostPort(authority, host, port)) {
                source.error = QStringLiteral("cannot read host and port from %1").arg(logText(trimmed));
                return source;
            }
        } else {
            host = (authority.startsWith(QLatin1Char('[')) && authority.endsWith(QLatin1Char(']'))) ? authority.mid(1, authority.size() - 2) : authority;
            port = 554;
        }
        if (!plainHostName(host)) {
            source.error = QStringLiteral("cannot read the host from %1").arg(logText(trimmed));
            return source;
        }
        source.kind = Kind::Avformat;
        source.host = host;
        source.port = port;
        source.url = trimmed.toLatin1();
        source.inputFormat = QByteArrayLiteral("rtsp");   // never probed from the data
        source.formatOptions = {
            { QByteArrayLiteral("protocol_whitelist"), (source.scheme == QLatin1String("rtsps")) ? QByteArrayLiteral("tcp,tls,udp,rtp") : QByteArrayLiteral("tcp,udp,rtp") },
            { QByteArrayLiteral("timeout"), timeoutUs },
            { QByteArrayLiteral("fflags"), QByteArrayLiteral("nobuffer") },
            { QByteArrayLiteral("probesize"), QByteArrayLiteral("32768") },
            { QByteArrayLiteral("analyzeduration"), QByteArrayLiteral("0") },
            { QByteArrayLiteral("max_delay"), QByteArrayLiteral("0") },
            { QByteArrayLiteral("reorder_queue_size"), lowLatency ? QByteArrayLiteral("0") : QByteArrayLiteral("4") },
            { QByteArrayLiteral("buffer_size"), QByteArrayLiteral("4194304") },
            { QByteArrayLiteral("max_streams"), QByteArrayLiteral("8") },
        };
        return source;
    }

    if ((source.scheme == QLatin1String("mpegts")) || (source.scheme == QLatin1String("tcp"))) {
        const bool udp = (source.scheme == QLatin1String("mpegts"));
        QString host;
        int port = 0;
        if (rest.contains(QLatin1Char('/')) || rest.contains(QLatin1Char('?')) || !splitHostPort(rest, host, port)) {
            source.error = QStringLiteral("cannot read host and port from %1").arg(logText(trimmed));
            return source;
        }
        if (host.isEmpty()) {
            if (!udp) {
                source.error = QStringLiteral("a TCP video source needs the camera's address: %1").arg(logText(trimmed));
                return source;
            }
            host = QStringLiteral("0.0.0.0");
        }
        if (udp) {
            QHostAddress address;
            if (!address.setAddress(host)) {
                source.error = QStringLiteral("\"%1\" is not an IP address").arg(host);
                return source;
            }
        } else if (!plainHostName(host)) {
            source.error = QStringLiteral("cannot read the host from %1").arg(logText(trimmed));
            return source;
        }
        source.kind = Kind::Avformat;
        source.host = host;
        source.port = port;
        source.url = (udp ? QByteArrayLiteral("udp://") : QByteArrayLiteral("tcp://")) + urlHost(host) + ':' + QByteArray::number(port);
        source.inputFormat = QByteArrayLiteral("mpegts");
        source.formatOptions = {
            { QByteArrayLiteral("protocol_whitelist"), udp ? QByteArrayLiteral("udp") : QByteArrayLiteral("tcp") },
            { QByteArrayLiteral("timeout"), timeoutUs },
            { QByteArrayLiteral("fflags"), QByteArrayLiteral("nobuffer") },
            // MPEG-TS carries no codec description up front: enough to see the PAT and PMT
            { QByteArrayLiteral("probesize"), QByteArrayLiteral("500000") },
            { QByteArrayLiteral("analyzeduration"), QByteArrayLiteral("500000") },
            { QByteArrayLiteral("max_delay"), QByteArrayLiteral("0") },
            { QByteArrayLiteral("max_streams"), QByteArrayLiteral("8") },
        };
        if (udp) {
            source.formatOptions.append({ QByteArrayLiteral("buffer_size"), QByteArrayLiteral("4194304") });
            source.formatOptions.append({ QByteArrayLiteral("overrun_nonfatal"), QByteArrayLiteral("1") });
        }
        return source;
    }

    source.error = QStringLiteral("unknown video scheme \"%1\"").arg(source.scheme);
    source.scheme.clear();
    return source;
}

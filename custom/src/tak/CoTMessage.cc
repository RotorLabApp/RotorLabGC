/****************************************************************************
 *
 * RotorLab GC TAK integration - Cursor on Target message build/parse.
 *
 ****************************************************************************/

#include "CoTMessage.h"

#include <QtCore/QDateTime>
#include <QtCore/QUrl>
#include <QtCore/QXmlStreamReader>
#include <QtCore/QXmlStreamWriter>

namespace CoT {

QString typeForVehicle(int mavType, const QString &fallback)
{
    if (mavType == 1) {                                     // MAV_TYPE_FIXED_WING
        return QStringLiteral("a-f-A-M-F-Q");
    }
    switch (mavType) {                                      // quad/coax/heli/hexa/octo/tri
    case 2: case 3: case 4: case 13: case 14: case 15:
        return QStringLiteral("a-f-A-M-H-Q");               // rotary wing UAS icon
    default:
        return fallback;
    }
}

static QString iso(const QDateTime &t)
{
    return t.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzzZ"));
}

QByteArray build(const Entity &e)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString tNow   = iso(now);
    const QString tStale = iso(now.addSecs(e.staleSeconds > 0 ? e.staleSeconds : 1));

    QString xml;
    QXmlStreamWriter w(&xml);
    // No <?xml?> prolog: TAK servers stream bare <event> elements and split the
    // byte stream on </event>. A prolog is only legal at byte 0, so any event
    // after the first in a flush would fail to parse ("declaration not at start
    // of entity"). Emitting just the element keeps multi-entity flushes valid.
    w.writeStartElement(QStringLiteral("event"));
    w.writeAttribute(QStringLiteral("version"), QStringLiteral("2.0"));
    w.writeAttribute(QStringLiteral("uid"), e.uid);
    w.writeAttribute(QStringLiteral("type"), e.cotType);
    // m-g: machine, GPS derived. With no position shared, h-g-i-g-o marks the
    // point as not a real fix.
    w.writeAttribute(QStringLiteral("how"), e.positionKnown ? QStringLiteral("m-g") : QStringLiteral("h-g-i-g-o"));
    w.writeAttribute(QStringLiteral("time"), tNow);
    w.writeAttribute(QStringLiteral("start"), tNow);
    w.writeAttribute(QStringLiteral("stale"), tStale);

    // A CoT point is required. With no position shared it carries the
    // standard unknown values instead of any real location.
    const QString unknown = QString::number(kUnknownValue, 'f', 1);
    w.writeStartElement(QStringLiteral("point"));
    w.writeAttribute(QStringLiteral("lat"), QString::number(e.positionKnown ? e.lat : 0.0, 'f', 7));
    w.writeAttribute(QStringLiteral("lon"), QString::number(e.positionKnown ? e.lon : 0.0, 'f', 7));
    w.writeAttribute(QStringLiteral("hae"), e.positionKnown ? QString::number(e.hae, 'f', 1) : unknown);
    w.writeAttribute(QStringLiteral("ce"), e.positionKnown ? QStringLiteral("9.0") : unknown);
    w.writeAttribute(QStringLiteral("le"), e.positionKnown ? QStringLiteral("9.0") : unknown);
    w.writeEndElement(); // point

    w.writeStartElement(QStringLiteral("detail"));

    // Contact: callsign always (label); endpoint only when addressable/send-to-able.
    w.writeStartElement(QStringLiteral("contact"));
    w.writeAttribute(QStringLiteral("callsign"), e.callsign);
    if (e.addressable) {
        w.writeAttribute(QStringLiteral("endpoint"), QString::fromLatin1(kServerEndpoint));
    }
    w.writeEndElement();

    w.writeStartElement(QStringLiteral("__group"));
    w.writeAttribute(QStringLiteral("name"), QStringLiteral("Cyan"));
    w.writeAttribute(QStringLiteral("role"), QStringLiteral("Team Member"));
    w.writeEndElement();

    // takv identifies the reporting EUD's software. Only the GCS self-beacon is
    // a TAK EUD; drones are tracks reported by it and must NOT claim takv.
    if (e.selfEud) {
        w.writeStartElement(QStringLiteral("takv"));
        w.writeAttribute(QStringLiteral("device"), QStringLiteral("RotorLab"));
        w.writeAttribute(QStringLiteral("platform"), QStringLiteral("RotorLab GC"));
#if defined(Q_OS_ANDROID)
        w.writeAttribute(QStringLiteral("os"), QStringLiteral("Android"));
#elif defined(Q_OS_WIN)
        w.writeAttribute(QStringLiteral("os"), QStringLiteral("Windows"));
#else
        w.writeAttribute(QStringLiteral("os"), QStringLiteral("Desktop"));
#endif
        w.writeAttribute(QStringLiteral("version"), QStringLiteral("1.0"));
        w.writeEndElement();
    }

    if (e.hasTrack && e.positionKnown) {
        w.writeStartElement(QStringLiteral("track"));
        w.writeAttribute(QStringLiteral("course"), QString::number(e.course, 'f', 1));
        w.writeAttribute(QStringLiteral("speed"), QString::number(e.speed, 'f', 1));
        w.writeEndElement();
    }

    // GPS-quality position hint so ATAK treats the point as a real fix.
    if (e.positionKnown) {
        w.writeStartElement(QStringLiteral("precisionlocation"));
        w.writeAttribute(QStringLiteral("geopointsrc"), QStringLiteral("GPS"));
        w.writeAttribute(QStringLiteral("altsrc"), QStringLiteral("GPS"));
        w.writeEndElement();
    }

    w.writeStartElement(QStringLiteral("uid"));
    w.writeAttribute(QStringLiteral("Droid"), e.callsign);
    w.writeEndElement();

    // Association: link a drone track to the GCS EUD that reports it.
    if (!e.parentUid.isEmpty()) {
        w.writeStartElement(QStringLiteral("link"));
        w.writeAttribute(QStringLiteral("uid"), e.parentUid);
        w.writeAttribute(QStringLiteral("type"), e.parentType);
        if (!e.parentCallsign.isEmpty()) {
            w.writeAttribute(QStringLiteral("parent_callsign"), e.parentCallsign);
        }
        w.writeAttribute(QStringLiteral("production_time"), tNow);
        w.writeAttribute(QStringLiteral("relation"), QStringLiteral("p-p"));
        w.writeEndElement();
    }

    // Delivery: <marti><dest> for direct addressing; omit for broadcast.
    if (!e.dest.isEmpty()) {
        w.writeStartElement(QStringLiteral("marti"));
        for (const QString &d : e.dest) {
            const QString cs = d.trimmed();
            if (cs.isEmpty()) {
                continue;
            }
            w.writeStartElement(QStringLiteral("dest"));
            w.writeAttribute(QStringLiteral("callsign"), cs);
            w.writeEndElement();
        }
        w.writeEndElement();
    }

    // Video feed: publish a <__video><ConnectionEntry> so ATAK/WinTAK offer
    // playback on this marker and OTS registers the stream. RotorLab GC never
    // plays it: the feed URL is handed to ATAK, which handles the video.
    // All ConnectionEntry attributes OTS' parse_video reads are emitted.
    if (!e.videoUrl.isEmpty()) {
        const QUrl u(e.videoUrl);
        QString scheme = u.scheme().isEmpty() ? QStringLiteral("raw") : u.scheme().toLower();
        int port = u.port(-1);
        if (port < 0) {
            port = scheme == QLatin1String("rtsp")  ? 554
                 : scheme == QLatin1String("rtmp")  ? 1935
                 : (scheme == QLatin1String("http") || scheme == QLatin1String("https")) ? 80
                 : -1;
        }
        QString path = u.path();
        if (!u.query().isEmpty()) {
            path += QLatin1Char('?') + u.query();
        }
        const QString valias = e.videoAlias.isEmpty() ? e.callsign : e.videoAlias;
        const QString vuid   = e.uid + QStringLiteral("-video");
        w.writeStartElement(QStringLiteral("__video"));
        w.writeAttribute(QStringLiteral("uid"), vuid);
        w.writeAttribute(QStringLiteral("url"), e.videoUrl);
        w.writeStartElement(QStringLiteral("ConnectionEntry"));
        w.writeAttribute(QStringLiteral("uid"), vuid);
        w.writeAttribute(QStringLiteral("alias"), valias);
        w.writeAttribute(QStringLiteral("address"), u.host());
        w.writeAttribute(QStringLiteral("port"), QString::number(port));
        w.writeAttribute(QStringLiteral("path"), path);
        w.writeAttribute(QStringLiteral("protocol"), scheme);
        w.writeAttribute(QStringLiteral("networkTimeout"), QStringLiteral("12000"));
        w.writeAttribute(QStringLiteral("bufferTime"), QStringLiteral("-1"));
        w.writeAttribute(QStringLiteral("roverPort"), QStringLiteral("-1"));
        w.writeAttribute(QStringLiteral("rtspReliable"), QStringLiteral("0"));
        w.writeAttribute(QStringLiteral("ignoreEmbeddedKLV"), QStringLiteral("false"));
        w.writeEndElement(); // ConnectionEntry
        w.writeEndElement(); // __video
    }

    w.writeStartElement(QStringLiteral("remarks"));
    w.writeCharacters(e.remark.isEmpty() ? QStringLiteral("RotorLab %1").arg(e.callsign) : e.remark);
    w.writeEndElement();

    w.writeEndElement(); // detail
    w.writeEndElement(); // event
    w.writeEndDocument();

    return xml.toUtf8();
}

Marker parse(const QByteArray &xml)
{
    Marker m;
    QXmlStreamReader r(xml);
    bool haveEvent = false;
    bool havePoint = false;

    while (!r.atEnd() && !r.hasError()) {
        const auto tok = r.readNext();
        if (tok != QXmlStreamReader::StartElement) {
            continue;
        }
        const QStringView name = r.name();
        if (name == QLatin1String("event")) {
            haveEvent = true;
            const auto a = r.attributes();
            m.uid   = a.value(QLatin1String("uid")).toString();
            m.type  = a.value(QLatin1String("type")).toString();
            m.how   = a.value(QLatin1String("how")).toString();
            m.time  = QDateTime::fromString(a.value(QLatin1String("time")).toString(), Qt::ISODateWithMs);
            m.stale = QDateTime::fromString(a.value(QLatin1String("stale")).toString(), Qt::ISODateWithMs);
        } else if (name == QLatin1String("takv") || name == QLatin1String("__group")) {
            // Only a TAK device's own report carries these (its position
            // report, PLI). A placed marker does not.
            m.isEud = true;
        } else if (name == QLatin1String("point")) {
            const auto a = r.attributes();
            bool okLat = false, okLon = false;
            const double lat = a.value(QLatin1String("lat")).toDouble(&okLat);
            const double lon = a.value(QLatin1String("lon")).toDouble(&okLon);
            if (okLat && okLon) {
                m.lat = lat;
                m.lon = lon;
                havePoint = true;
            }
        } else if (name == QLatin1String("contact")) {
            const QString cs = r.attributes().value(QLatin1String("callsign")).toString();
            if (!cs.isEmpty() && m.callsign.isEmpty()) {
                m.callsign = cs;
            }
        } else if (name == QLatin1String("dest")) {
            const QString cs = r.attributes().value(QLatin1String("callsign")).toString();
            if (!cs.isEmpty() && m.destCallsign.isEmpty()) {
                m.destCallsign = cs;
            }
        } else if (name == QLatin1String("link")) {
            // The p-p (parent) link names the EUD that placed the marker. ATAK
            // stamps it with the device uid and the operator's callsign. The
            // sender comes from the first p-p link only: a link of any other
            // relation names some other entity, never the creator (G44).
            const auto a = r.attributes();
            const bool isParent = a.value(QLatin1String("relation")) == QLatin1String("p-p");
            if (isParent && !m.hasParentLink) {
                m.hasParentLink  = true;
                m.senderUid      = a.value(QLatin1String("uid")).toString();
                m.senderCallsign = a.value(QLatin1String("parent_callsign")).toString();
            }
        }
    }

    m.valid = haveEvent && havePoint && !r.hasError();
    return m;
}

} // namespace CoT

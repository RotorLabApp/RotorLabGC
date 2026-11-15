/****************************************************************************
 *
 * RotorLab GC TAK integration - field command grammar.
 *
 ****************************************************************************/

#include "TakCommand.h"

#include <QtCore/QHash>
#include <QtCore/QRegularExpression>

namespace Tak {

static Verb verbFromToken(const QString &tok)
{
    static const QHash<QString, Verb> table = {
        { QStringLiteral("GOTO"), Verb::Goto },   { QStringLiteral("GO"), Verb::Goto },
        { QStringLiteral("VEC"), Verb::Goto },    { QStringLiteral("VECTOR"), Verb::Goto },
        { QStringLiteral("SEND"), Verb::Goto },
        { QStringLiteral("TKOFF"), Verb::Takeoff }, { QStringLiteral("TAKEOFF"), Verb::Takeoff },
        { QStringLiteral("LAUNCH"), Verb::Takeoff }, { QStringLiteral("UP"), Verb::Takeoff },
        { QStringLiteral("RTL"), Verb::Rtl },     { QStringLiteral("HOME"), Verb::Rtl },
        { QStringLiteral("RETURN"), Verb::Rtl },
        { QStringLiteral("LAND"), Verb::Land },   { QStringLiteral("DOWN"), Verb::Land },
        { QStringLiteral("HOLD"), Verb::Hold },   { QStringLiteral("LOITER"), Verb::Hold },
        { QStringLiteral("HOVER"), Verb::Hold },  { QStringLiteral("STOP"), Verb::Hold },
        { QStringLiteral("GUIDED"), Verb::Guided }, { QStringLiteral("TAKE"), Verb::Guided },
        { QStringLiteral("CONTROL"), Verb::Guided }, { QStringLiteral("ASSUME"), Verb::Guided },
        { QStringLiteral("ARM"), Verb::Arm },
        { QStringLiteral("DISARM"), Verb::Disarm }, { QStringLiteral("SAFE"), Verb::Disarm },
        { QStringLiteral("ALTHOLD"), Verb::AltHold }, { QStringLiteral("HOLDALT"), Verb::AltHold },
        { QStringLiteral("SETALT"), Verb::AltHold }, { QStringLiteral("GOALT"), Verb::AltHold },
    };
    return table.value(tok, Verb::Invalid);
}

QString verbName(Verb v)
{
    switch (v) {
    case Verb::Goto:    return QStringLiteral("GOTO");
    case Verb::Takeoff: return QStringLiteral("TKOFF");
    case Verb::Rtl:     return QStringLiteral("RTL");
    case Verb::Land:    return QStringLiteral("LAND");
    case Verb::Hold:    return QStringLiteral("HOLD");
    case Verb::Guided:  return QStringLiteral("GUIDED");
    case Verb::Arm:     return QStringLiteral("ARM");
    case Verb::Disarm:  return QStringLiteral("DISARM");
    case Verb::AltHold: return QStringLiteral("ALTHOLD");
    case Verb::Invalid: break;
    }
    return QStringLiteral("INVALID");
}

bool parseCommand(const QString &text, Command &out, QString &err)
{
    out = Command();
    if (text.trimmed().isEmpty()) {
        err = QStringLiteral("empty");
        return false;
    }

    // Normalise exactly like the original Python parser: upper-case, whitespace/hyphens -> '_',
    // collapse repeats, split, drop empties. Lets "GOTO 2", "goto-2",
    // "GOTO_2_ALT_150" all parse the same.
    QString t = text.trimmed().toUpper();
    t.replace(QRegularExpression(QStringLiteral("[\\s\\-]+")), QStringLiteral("_"));
    t.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    const QStringList toks = t.split(QLatin1Char('_'), Qt::SkipEmptyParts);
    if (toks.isEmpty()) {
        err = QStringLiteral("empty");
        return false;
    }

    out.verb = verbFromToken(toks.at(0));
    if (out.verb == Verb::Invalid) {
        err = QStringLiteral("unknown verb ") + toks.at(0);
        return false;
    }
    if (toks.size() < 2) {
        err = QStringLiteral("missing track id");
        return false;
    }

    const QString idt = toks.at(1);
    if (idt == QLatin1String("ALL")) {
        out.target = Target::All;
    } else if (idt == QLatin1String("SEL")) {
        out.target = Target::Sel;
    } else if (idt.contains(QLatin1Char(','))) {
        out.target = Target::Ids;
        const QStringList parts = idt.split(QLatin1Char(','), Qt::SkipEmptyParts);
        if (parts.isEmpty()) {
            err = QStringLiteral("bad track list ") + idt;
            return false;
        }
        for (const QString &p : parts) {
            bool ok = false;
            const int v = p.toInt(&ok);
            if (!ok) {
                err = QStringLiteral("bad track list ") + idt;
                return false;
            }
            out.ids.append(v);
        }
    } else {
        bool ok = false;
        const int v = idt.toInt(&ok);
        if (!ok) {
            err = QStringLiteral("bad track id ") + idt;
            return false;
        }
        out.target = Target::Ids;
        out.ids.append(v);
    }

    // Trailing ALT <n> / SPD <n> pairs, in any order.
    int i = 2;
    while (i < toks.size()) {
        const QString key = toks.at(i);
        if ((key == QLatin1String("ALT") || key == QLatin1String("SPD"))
            && i + 1 < toks.size()) {
            bool ok = false;
            const int val = toks.at(i + 1).toInt(&ok);
            if (!ok) {
                err = QStringLiteral("bad token ") + key;
                return false;
            }
            if (key == QLatin1String("ALT")) {
                out.altFt = val;
            } else {
                out.spd = qBound(1, val, 10);
            }
            i += 2;
        } else {
            err = QStringLiteral("bad token ") + key;
            return false;
        }
    }
    return true;
}

double spdToMetersPerSecond(int spd, double spdMin, double spdMax)
{
    if (spd < 1) {
        return -1.0;
    }
    const int s = qBound(1, spd, 10);
    return spdMin + (s - 1) / 9.0 * (spdMax - spdMin);
}

} // namespace Tak

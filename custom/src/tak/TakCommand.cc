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

bool isReservedWord(const QString &upperWord)
{
    static const QStringList others = { QStringLiteral("ALL"), QStringLiteral("SEL"), QStringLiteral("ALT"),
                                        QStringLiteral("SPD"), QStringLiteral("GCS") };
    return verbFromToken(upperWord) != Verb::Invalid || others.contains(upperWord);
}

QString normalizeName(const QString &text)
{
    static const QRegularExpression seps(QStringLiteral("[\\s\\p{Z}_\\-]+"),
                                         QRegularExpression::UseUnicodePropertiesOption);
    QString t = text.toUpper();
    t.replace(seps, QStringLiteral("-"));
    while (t.startsWith(QLatin1Char('-'))) {
        t.remove(0, 1);
    }
    while (t.endsWith(QLatin1Char('-'))) {
        t.chop(1);
    }
    return t;
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
    //
    // A minus used as a sign is kept (G35): a '-' at the start or after a
    // separator (space, '_', '-' or ',') and before a digit, as in "ALT -50",
    // "ALT_-50" or "ALT--50". It used to be read as a separator, so "ALT -50"
    // became ALT 50.
    //
    // G100: after ALT or SPD, any minus sign or hyphen between the word and
    // its number makes the number negative, so "ALT-50" and "GOTO-2-ALT-150"
    // are refused instead of read as positive. A hyphen between the verb and
    // the aircraft number ("GOTO-2") is still a separator: aircraft numbers
    // are never negative. Unicode minus signs and dashes count as a minus.
    static const QChar kSign(0xE000);   // private-use placeholder while normalising
    QString t = text.trimmed().toUpper();
    // Every Unicode white space (no-break space U+00A0, U+202F, U+3000, NEL
    // U+0085, ...) is a plain space here, so the sign rules below see it as a
    // separator. Without this, "ALT -<U+00A0>150" slipped past them and
    // QString::toInt trimmed the no-break space and read 150.
    static const QRegularExpression unicodeSpace(
        QStringLiteral("[\\s\\p{Z}\\x{0085}]"), QRegularExpression::UseUnicodePropertiesOption);
    t.replace(unicodeSpace, QStringLiteral(" "));
    static const QRegularExpression unicodeMinus(
        QStringLiteral("[\\x{2010}-\\x{2015}\\x{2212}\\x{FE58}\\x{FE63}\\x{FF0D}]"));
    t.replace(unicodeMinus, QStringLiteral("-"));
    {
        static const QRegularExpression valueKey(
            QStringLiteral("(?<=^|[\\s_,\\-])(ALT|SPD)([\\s_,\\-]*)(?=\\d)"));
        QString marked;
        qsizetype last = 0;
        QRegularExpressionMatchIterator it = valueKey.globalMatch(t);
        while (it.hasNext()) {
            const QRegularExpressionMatch mo = it.next();
            if (!mo.captured(2).contains(QLatin1Char('-'))) {
                continue;
            }
            marked += t.mid(last, mo.capturedStart() - last);
            marked += mo.captured(1) + QLatin1Char('_') + kSign;
            last = mo.capturedEnd();
        }
        if (last > 0) {
            t = marked + t.mid(last);
        }
    }
    t.replace(QRegularExpression(QStringLiteral("(?<=^|[\\s_,\\-])-\\s*(?=\\d)")), QString(kSign));
    t.replace(QRegularExpression(QStringLiteral("[\\s\\-]+")), QStringLiteral("_"));
    t.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    QStringList toks = t.split(QLatin1Char('_'), Qt::SkipEmptyParts);
    for (QString &tok : toks) {
        tok.replace(kSign, QLatin1Char('-'));
    }
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

    // The target (G47, G48): any name words, then N, N,N, ALL or SEL; or a
    // name alone. The name ends at the target word, at ALT or SPD, or at the
    // end. A word with a digit list or a signed number is a target word, so
    // "GOTO -2" and "RTL 1,-2" are refused as before.
    static const QRegularExpression numberLike(QStringLiteral("^[+-]?[0-9]+$"));
    QStringList nameWords;
    int i = 1;
    bool haveTarget = false;
    while (i < toks.size()) {
        const QString idt = toks.at(i);
        if (idt == QLatin1String("ALT") || idt == QLatin1String("SPD")) {
            break;
        }
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
                const int v = numberLike.match(p).hasMatch() ? p.toInt(&ok) : -1;
                if (!ok || v < 0) {
                    err = QStringLiteral("bad track list ") + idt;
                    return false;
                }
                out.ids.append(v);
            }
        } else if (numberLike.match(idt).hasMatch()) {
            bool ok = false;
            const int v = idt.toInt(&ok);
            if (!ok || v < 0) {
                err = QStringLiteral("bad track id ") + idt;
                return false;
            }
            out.target = Target::Ids;
            out.ids.append(v);
        } else {
            nameWords << idt;
            if (nameWords.size() > kMaxNameWords) {
                err = QStringLiteral("name has more than %1 words").arg(kMaxNameWords);
                return false;
            }
            ++i;
            continue;
        }
        haveTarget = true;
        ++i;
        break;
    }
    if (!haveTarget) {
        if (nameWords.isEmpty()) {
            err = QStringLiteral("bad track id ") + toks.at(1);
            return false;
        }
        out.target = Target::Name;
    }
    out.name = nameWords.join(QLatin1Char('-'));
    if (out.name.size() > kMaxNameChars) {
        err = QStringLiteral("name longer than %1 characters").arg(kMaxNameChars);
        return false;
    }

    // Trailing ALT <n> / SPD <n> pairs, in any order.
    while (i < toks.size()) {
        const QString key = toks.at(i);
        if ((key == QLatin1String("ALT") || key == QLatin1String("SPD"))
            && i + 1 < toks.size()) {
            const QString valTok = toks.at(i + 1);
            // A negative value is refused with a reason, never turned
            // positive or clamped (G35). Any minus sign counts, "-0" too
            // (G100), and so does a '+' anywhere in the value: "ALT -+150"
            // leaves "+150" here, which toInt would read as 150.
            if (valTok.contains(QLatin1Char('-')) || valTok.contains(QLatin1Char('+'))) {
                // One reason, true for a minus, a hyphen and a plus alike
                // (RotorLab 5.3.0 review, M3).
                err = (key == QLatin1String("ALT"))
                    ? QStringLiteral("ALT value %1 refused: a minus sign or hyphen before the number makes it "
                                     "negative, and a plus sign is not accepted. ALT is feet above launch; write "
                                     "the number with no sign, as in ALT 150 or ALT_150").arg(valTok)
                    : QStringLiteral("SPD value %1 refused: a minus sign or hyphen before the number makes it "
                                     "negative, and a plus sign is not accepted. SPD is a step from 1 to 10 on "
                                     "this station's speed scale (TAK settings); write the number with no sign, "
                                     "as in SPD 5 or SPD_5").arg(valTok);
                return false;
            }
            // Only plain ASCII digits are a value: toInt also trims white
            // space and accepts signs, which must never turn into a number.
            static const QRegularExpression digitsOnly(QStringLiteral("^[0-9]+$"));
            bool ok = false;
            const int val = digitsOnly.match(valTok).hasMatch() ? valTok.toInt(&ok) : 0;
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

/****************************************************************************
 *
 * RotorLab GC TAK integration - field command grammar.
 *
 * Ported from the original Python field-command parser (parse_field_command /
 * FIELD_VERBS). Pure and
 * side-effect free so it can be unit-tested offline. The grammar:
 *
 *   Verbs:     GOTO (GO/VEC/VECTOR/SEND), TKOFF, RTL, LAND, HOLD,
 *              GUIDED, ARM, DISARM, ALTHOLD  (+ aliases)
 *   Target:    [NAME] N | [NAME] N,N,N | [NAME] ALL | [NAME] SEL | NAME
 *   Modifiers: ALT <ft>, SPD <1-10>
 *   Example:   GOTO ROTOR-2 ALT 400 SPD 5
 *
 * NAME is the station's callsign prefix (ROTOR-2, ROTOR-1,3, ROTOR-ALL) or an
 * aircraft's Fleet callsign (HAWK). The parser only reads it; the station
 * decides whether the name is its own (G47). A target with no name still
 * parses, so the station can say why it is not run. SEL parses so the
 * station can refuse it with its own reason (G48).
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QList>
#include <QtCore/QString>

namespace Tak {

enum class Verb {
    Invalid,
    Goto,
    Takeoff,
    Rtl,
    Land,
    Hold,
    Guided,
    Arm,
    Disarm,
    AltHold,
};

enum class Target {
    Ids,        ///< explicit sysid list in `ids`
    All,        ///< ALL
    Sel,        ///< SEL: never run by the station (G48)
    Name,       ///< a name alone, no number (a Fleet callsign such as HAWK)
};

struct Command {
    Verb    verb = Verb::Invalid;
    Target  target = Target::Ids;
    QList<int> ids;             ///< populated when target == Ids
    /// The name before the target, upper case, words joined by '-'
    /// ("ROTOR" in GOTO ROTOR-2, "HAWK" in GOTO HAWK). Empty when the
    /// marker names no station.
    QString name;
    /// Filled by the station, never by the parser: the normalized name of
    /// one of this station's aircraft the marker was also sent to with
    /// <marti><dest>, or empty (G47).
    QString sentTo;
    int     sentToId = -1;      ///< the system id `sentTo` names; -1 = none
    int     altFt = -1;        ///< -1 = unset
    int     spd = -1;           ///< 1..10, -1 = unset
};

/// Most words a name may have, and its longest normalized length (G47).
inline constexpr int kMaxNameWords = 4;
inline constexpr int kMaxNameChars = 40;

/// True for a word the grammar reads as a command word: every verb and alias,
/// and ALL, SEL, ALT, SPD and GCS. Upper case expected.
bool isReservedWord(const QString &upperWord);

/// A name as the station compares it: upper case, runs of white space,
/// '_' and '-' turned into one '-', no '-' at either end. "rotor 2" and
/// "ROTOR_2" both become "ROTOR-2".
QString normalizeName(const QString &text);

/// Canonical verb name for logs/UI (e.g. Verb::Goto -> "GOTO").
QString verbName(Verb v);

/// Parse a command string (typically an ATAK marker callsign). Returns true on
/// success and fills `out`; on failure returns false and sets `err`.
bool parseCommand(const QString &text, Command &out, QString &err);

/// Map SPD step 1..10 to m/s over [spdMin,spdMax]; returns -1 for unset (default speed).
double spdToMetersPerSecond(int spd, double spdMin, double spdMax);

} // namespace Tak

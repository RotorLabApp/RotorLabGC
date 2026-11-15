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
 *   Target:    N | N,N,N | ALL | SEL
 *   Modifiers: ALT <ft>, SPD <1-10>
 *   Example:   GOTO 2 ALT 400 SPD 5
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
    All,        ///< every known track
    Sel,        ///< current UI selection
};

struct Command {
    Verb    verb = Verb::Invalid;
    Target  target = Target::Ids;
    QList<int> ids;             ///< populated when target == Ids
    int     altFt = -1;         ///< -1 = unset
    int     spd = -1;           ///< 1..10, -1 = unset
};

/// Canonical verb name for logs/UI (e.g. Verb::Goto -> "GOTO").
QString verbName(Verb v);

/// Parse a command string (typically an ATAK marker callsign). Returns true on
/// success and fills `out`; on failure returns false and sets `err`.
bool parseCommand(const QString &text, Command &out, QString &err);

/// Map SPD step 1..10 to m/s over [spdMin,spdMax]; returns -1 for unset (default speed).
double spdToMetersPerSecond(int spd, double spdMin, double spdMax);

} // namespace Tak

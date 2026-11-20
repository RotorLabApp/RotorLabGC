/****************************************************************************
 *
 * RotorLab GC Plan view 3D (G64, MG2) - PLAN-CONTRACT.md section 4 colors.
 *
 * The clearance states (section 4, with CL-1a ground contact) and their colors, shared by every block. Red
 * means a strike and nothing else (CL-1); violet means unknown (CL-2).
 *
 ****************************************************************************/

#pragma once

#include "Plan3DMath.h"

#include <QtGui/QColor>

namespace Plan3D {

inline QColor strikeColor()                 { return QColor(0xff, 0x55, 0x44); }   // #ff5544
inline QColor noStrikeColor()               { return QColor(0x2d, 0xd4, 0xbf); }   // #2dd4bf
inline QColor unknownColor(bool darkTheme)  { return darkTheme ? QColor(0xb0, 0x7c, 0xff) : QColor(0x7c, 0x3a, 0xed); }   // #b07cff / #7c3aed
/// CL-1a (rev 5): ground contact of a takeoff or landing. Neutral slate, never red.
inline QColor groundContactColor(bool darkTheme) { return darkTheme ? QColor(0x94, 0xa3, 0xb8) : QColor(0x64, 0x74, 0x8b); }   // #94a3b8 / #64748b

inline QColor clearanceColor(Clearance c, bool darkTheme)
{
    switch (c) {
    case Clearance::Strike:   return strikeColor();
    case Clearance::NoStrike: return noStrikeColor();
    case Clearance::GroundContact: return groundContactColor(darkTheme);
    case Clearance::Unknown:  break;
    }
    return unknownColor(darkTheme);
}

} // namespace Plan3D

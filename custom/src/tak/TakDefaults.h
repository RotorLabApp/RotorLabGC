/****************************************************************************
 *
 * RotorLab GC TAK integration - defaults for a station with no saved TAK
 * settings.
 *
 * The one place these live. TakManager's members start from this table and
 * load() falls back to it for any key not saved yet; a saved value is always
 * kept. Change a default here and nowhere else.
 *
 ****************************************************************************/

#pragma once

namespace TakDefaults {

// "Verify server certificate": on. The CoT stream checks the server
// certificate against the system trust store, any imported CA and any
// accepted (pinned) fingerprint.
inline constexpr bool verifyPeer = true;

// "Send commands only from listed senders": on. A marker is sent only when its
// reported sender is on the station's list; an empty list sends nothing.
inline constexpr bool approvedOnly = true;

// Command By ATAK: every verb is opt in, per verb, on the TAK page.
// Movement and recovery (GOTO, RTL, LAND, HOLD, ALTHOLD) ship off as well as
// launch and seize (GUIDED, ARM, DISARM, TKOFF).
inline constexpr bool verbGoto    = false;
inline constexpr bool verbRtl     = false;
inline constexpr bool verbLand    = false;
inline constexpr bool verbHold    = false;
inline constexpr bool verbAltHold = false;
inline constexpr bool verbGuided  = false;
inline constexpr bool verbArm     = false;
inline constexpr bool verbDisarm  = false;
inline constexpr bool verbTakeoff = false;

// "Commanded altitude warning above", feet above the launch point (the datum
// the marker grammar's ALT uses). 0 = no warning. A command above it still
// goes to the aircraft; the station alert adds a warning line.
inline constexpr double altWarnAboveFt = 0.0;

} // namespace TakDefaults

# Compatibility

"Tested with" means someone ran it and saw it work, as described on this page. "Built for" means the release is made and meant for it, but nobody ran that setup for this release. If you run a setup marked "Built for", an issue saying it worked (or did not) helps everyone.

## Platforms

RotorLab GC 5.4.0, like 5.3.0, is published for Windows 10 and 11 (x64), Android on arm64-v8a, and Linux x86_64 as an AppImage built on Ubuntu 22.04, which needs glibc 2.35 or later (Ubuntu 22.04 or later, or another distribution of that age). Its signed offline activation needs RotorLab 3.7.0 or later. The table below is for 5.1.1.

| Platform | RotorLab GC 5.1.1 |
|---|---|
| Windows 11 (x64) | Tested with. The release was built and its test suites run on Windows 11 (121 passed, 0 failed, 5 skipped), and the signed installer and checksums were verified there. |
| Windows 10 (x64) | Built for. Not run for this release. |
| Android, arm64-v8a phones and tablets | Built for. The Device Key store was tested on an Android 11 (API 30) emulator during 5.1 development. The 5.1.1 APK was not run on an Android device for this release, and installing it side by side with a stock QGroundControl was not tested. |
| Other platforms (Linux, macOS, iOS, 32-bit Android) | Not released. QGroundControl's code for them is in this tree, but RotorLab does not build or publish GC for them. |

## Autopilots and aircraft

| Autopilot | Status |
|---|---|
| ArduPilot Copter | Tested with, in SITL over TCP, during 5.1 development: flight filing, the offline queue, a flight through RotorLab Server, and ATAK commands. |
| ArduPilot Plane, QuadPlane and VTOL | Built for (QGroundControl's ArduPilot support). Not run for this release. |
| PX4 | Built for (QGroundControl's PX4 support). Not run for this release. |

GC connects over USB, a telemetry radio or a network link (UDP or TCP), as QGroundControl does.

## RotorLab

| RotorLab | What GC does with it |
|---|---|
| RotorLab 3.5.6 | Tested with, against a local RotorLab during 5.1 development: license activation, the pilot list, Device Key Verify, flight filing, a flight flown while RotorLab was unreachable and filed once afterward, and a flight handed on by RotorLab Server 2.0 and recorded once. |
| 3.5.4 or later | Planned crew and remote pilot in command periods. GC reads RotorLab's version to decide. |
| 3.5.2 or later | A flight with no pilot named files marked "Pilot not named". |
| 3.5.1 or later | Readiness words and open work orders on the crew screen. |
| Older than 3.5.4 | GC files one pilot per flight, the pilot at takeoff, and says so on the RotorLab Connector page. |

## RotorLab Server (Fleet Server role)

| RotorLab Server | Status |
|---|---|
| 2.0 | Tested with, during 5.1 development: enrollment with a 16-character code, the roster through the server, and a flight handed on to RotorLab once. |

RotorLab Server is in early access testing and not on sale today.

## TAK

| TAK server | Status |
|---|---|
| OpenTAKServer 1.8.3 | Tested with, during 5.1 development: certificate enrollment, air tracks, and every station check for ATAK commands, including an airborne RTL in SITL. |

The [ATAK and TAK Field Guide](https://github.com/RotorLabApp/rotorlab-gc-tak-guide), chapter 9, covers TAK compatibility in full.

## Build

| Tool | Version used for 5.1.1 |
|---|---|
| Qt | 6.8.3 (msvc2022_64 and android_arm64_v8a) |
| CMake | 3.25 or later, with Ninja |
| Windows compiler | Visual Studio 2022 |
| Android | NDK 26.1.10909125, JDK 17 |

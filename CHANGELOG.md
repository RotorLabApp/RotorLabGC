# RotorLab GC changelog

RotorLab GC is built on QGroundControl. RotorLab's releases come first; the history inherited from upstream QGroundControl follows, unchanged, under [Upstream QGroundControl history](#upstream-qgroundcontrol-history).

## [RotorLab GC] 5.4.0

RotorLab GC is a modified version of QGroundControl. Changes since 5.3.0:

- **Before you upgrade a TAK / ATAK station**: after the upgrade, a station reads no commands from ATAK markers, and "Commands to aircraft on" cannot be turned on, until its TAK callsign prefix is set or confirmed on the TAK / OpenTAKServer page. ROTOR, the shared default in earlier versions, can no longer be a prefix: a station that used it is offered its own prefix, made from "RL" and four characters of its station ID. Markers must now name the station and the aircraft, so markers such as `GOTO 2`, `HOLD 1` or `RTL ALL` no longer reach any aircraft. SEL is refused. Station position sharing to the TAK server is now off by default.
- **TAK / ATAK commands name their station**: a marker names the aircraft as this station publishes it to TAK, for example `HOLD ALPHA-1`, `ALPHA-1,3`, `ALPHA-ALL` or the aircraft's Fleet callsign. A marker that names no station, names SEL, names a station or aircraft not seen on the TAK server, or names this station but no aircraft, is not sent to any aircraft, and the Remote commands list shows why. GOTO still reaches one aircraft only. Changing the prefix turns "Commands to aircraft on" off. When another station on the TAK server uses the same prefix, this station sends no ATAK commands to any aircraft until about 2 minutes after the other station is last seen. Addressing is routing, not authentication: the name in a marker chooses which station reads it, does not show who placed it, and anyone who can post to the TAK server can write any name. ATAK operators never see a refusal, duplicate-prefix detection is best effort, and addressing applies only between stations on 5.4.0 or later. Give each station its own prefix.
- **TAK / ATAK station position**: new setting "Share this station's position with the TAK server" on the GCS location card, off by default for new and upgraded installs. With it off, the station still announces itself to the TAK server, with no location.
- **Plan view**: legs on the 2D map are drawn in the same terrain colors as the 3D view, with tick marks on red parts and dashes on violet parts, and the terrain profile shows the same colors and the number of legs in each state. Legs that use terrain-relative altitude are now checked in 2D. Obstacles, buildings and wires are not checked.
- **Network video**: GC shows network video again, with its own video receiver: H.264 and H.265 over UDP (RTP), RTSP, and MPEG-TS over UDP or TCP, on Windows, Android and Linux.
- **Video recording**: a new "Video recording and relay" settings page records the main video stream to this computer as received: Off, While armed (the default) or Always while video is received. When this station is enrolled with a Fleet Server, GC can upload each recording to it; the recording stays on this computer.
- **Live video to the Fleet Server**: when this station is enrolled with a Fleet Server, GC can send the live video to it for viewing in the server's console, at Full, Medium or Low quality (Medium and Low on Windows and Android with a suitable video encoder; Linux sends Full only).
- **License notices**: the Windows installer, the Android app and the Linux AppImage each list every library they include in NOTICE, with its version and license, and include each library's license file. The Linux AppImage now lists the Ubuntu 22.04 libraries, ICU 73 and the Qt libraries it carries, with each Ubuntu library's copyright file. The Windows install folder and the AppImage no longer include files used only to build software.
- **Fixes**: fixed a crash that could happen when a vehicle was removed while GC was still waiting for its camera to answer a request for camera information.
- **Known limit**: text added by RotorLab GC appears in English in every language setting.

## [RotorLab GC] 5.3.0

RotorLab GC is a modified version of QGroundControl. Changes since 5.1.1:

- **3D view of the plan**: in Plan view, the 3D button shows the plan over the terrain, and you can edit it there (move a point, change its height, and on supported vehicles set spline legs, heading and gimbal pitch). Colors show what GC's terrain data says about each leg; obstacles, buildings and wires are not checked. Windows and Linux only. The old Fly view 3D viewer is removed.
- **Linux**: a Linux x86_64 AppImage, for Ubuntu 22.04 or later, or another distribution with glibc 2.35 or later.
- **Signed offline activation**: GC opens without a connection only with an activation that RotorLab signed, checked against a RotorLab public key built into GC (RotorLab 3.7.0 or later). The License page shows the date until which GC can open without a connection. After upgrading from 5.1.1, start GC once with internet: until this device is activated, GC does not connect to vehicles. Once GC is open, the license gate never closes it or covers the screen while a vehicle is connected.
- **Offline terrain heights**: terrain heights saved for offline use by earlier versions are removed, because some were wrong. Open each offline area while online and select Resume Download. Saved map imagery is not affected.
- **Mission planning**: more reliable plan uploads to PX4, structure scans that ArduPilot accepts, a read-only breach return height with a note on how your autopilot uses it, terrain-based altitudes kept as saved, and clearer messages for plan files GC cannot open.
- **TAK / ATAK**: commands are read only from markers that the sending device reports as placed by a person (GC cannot verify this); GOTO needs a valid target and one aircraft; signed altitude and speed values (such as ALT -50 or ALT +150) are refused; TKOFF does not arm while ARM is off; a marker is never run twice; the Fly view shows when command markers were not sent to the aircraft; the TAK password is kept in the system key store.
- **Stored keys**: the Device Key is never saved in the settings file. On Windows, the Credential Manager entries are named "RotorLab GC - <account>". On Linux without a desktop keyring, the Device Key is kept for the session only.
- **Vehicle Setup**: the setup pages use the same page frame as the Application Settings pages.
- **Known limit**: text added by RotorLab GC appears in English in every language setting.

## [RotorLab GC] 5.1.1

RotorLab GC is a modified version of QGroundControl. Changes in this release:

- **Android app id**: the Android app is now `com.rotorlab.gc`, so it installs next to a stock QGroundControl on the same device. If you installed the 5.1.0 APK, 5.1.1 installs as a separate app, and its settings do not carry over from 5.1.0.
- **Android icon, splash screen and name**: the Android app now shows RotorLab GC's own icon, splash screen and name. 5.1.0 showed QGroundControl's.
- **Android USB permission**: the request for permission to use a USB device is now private to GC, so it is not shared with QGroundControl or another app on the device.
- **Windows installer**: the installer no longer includes Microsoft's debug runtime libraries or Qt's QML debugger plugins.
- **Licenses**: the Windows installer and the Android APK now include NOTICE and the license texts (Windows: next to bin/; Android: assets/licenses/).

## [RotorLab GC] 5.1.0

RotorLab GC is a modified version of QGroundControl. Changes in this release:

- **TAK / ATAK**: every command received from ATAK is listed on the TAK page and in a station command log, and ARM, TAKEOFF and GOTO also show a banner over the fly view. Every Command By ATAK verb is off until checked, "Send commands only from listed senders" is on, and "Verify server certificate" is on. An optional altitude warning level. TAK enrollment sends the username and password only over a verified connection.
- **RotorLab Connector**: flights file to RotorLab with RotorLab's own flight types, readiness words and open work orders on the crew screen, planned crew first, remote pilot in command changes recorded at the station and filed with the flight, and a queue that saves each flight at landing and files it once.
- **Device Key, license and start-up**: the Device Key is kept in the platform's key store where one exists, and is never touched while an aircraft is armed. The license is re-checked every 24 hours.
- **Settings**: every settings page fills the window and reflows to its width.

## Upstream QGroundControl history

> **Note:** This file only contains high‑level features or important fixes.

## [5.0] – Daily Build

- **New UI**: Combined compass + attitude instrument for enhanced navigation.
- **Instrument Selection**: Click on desktop or long‑press on mobile to switch instruments.
- **MAVLink Actions**:
  - Fly View & Joystick custom actions renamed from “Custom Actions” to **Mavlink Actions** (move your JSON files accordingly).
  - Support for setting individual MAVLink message rates in the Inspector.
  - Enabled MAVLink 2 signing.
- **Battery Display**: Dynamic bars with configurable thresholds (100%, Config 1, Config 2, Low, Critical).

---

<details>
<summary><strong>4.1</strong></summary>

### [4.1.2] – Not yet released
- **Bugfix**: Radio setup – double‑send of `MAV_CMD_PREFLIGHT_CALIBRATION` causing “Unable to send command.”

### [4.1.1] – Stable
- **Fix**: TCP link communications.

### [4.1.0]
- **Camera**: Support simple cameras (only `DIGICAM_CONTROL`) in Photo/Video control.
- **Parameters**:
  - Load from file even if missing on vehicle.
  - Diff dialog + selective param upload.
- **Video Streaming**: Capture individual images from the stream.
- **Fly**: Long‑press arm = Force Arm; click again to arm.
- **VTOL**:
  - Transition‑distance setting for takeoff/landing patterns.
  - Improved VTOL support throughout.
- **Maps**: Zoom up to level 23 (even without tiles).
- **Settings/Mavlink**: Forward traffic to specified UDP port.
- **Terrain Protocol**: Query GCS for terrain data (`TERRAIN_FRAME`) in mission planning.
- **Plan**:
  - VTOL Landing Pattern.
  - KML export improvements for 3D verification.
  - Terrain Profile with collision indications.
- **Fly**: Rearchitected view & controls for custom builds.

</details>

---

<details>
<summary><strong>4.0</strong></summary>

### [4.0.9] – Not yet released
- Don’t auto‑connect to second Cube Orange/Yellow composite port.
- **Plan**: Fix mission commands with altitude but no lat/lon.
- Fix view switching break after altitude‑mode warning.

### [4.0.8] – Stable
- **iOS**: Update file storage for Files app.
- **Mobile**: Fix Log Replay status‑bar file selection.

### [4.0.7] – Stable
- Fix video page sizing.
- **Virtual Joystick**:
  - Right‑stick centering fix.
  - Rover/sub reverse‑throttle support.
- Fix display of multiple ADSB vehicles.

### [4.0.6] – Stable
- **Analyze/Log Download**: Fix mobile download.
- **Fly**: Continue Mission & Change Altitude now available after pause.
- **PX4 Flow**: Video display fix.

### [4.0.5] – Stable
- **Solo**: Fix mission upload failures.
- **Plan**: Crash fix for Create Plan → Survey (fixed‑wing).

### [4.0.4]
- **Mobile File Save**: Incorrect extension fix.
- **Radio Setup**: Spektrum bind fix.
- **Plan/Fly**: Restore waypoint number display.

### [4.0.3]
- **Plan**:
  - Optional takeoff item.
  - Enforce takeoff before other items.
- **Video**: Low‑latency mode option.
- **ArduPilot**: Firmware list generation fix.

### [4.0.2]
- Fix MAVLink V2 negotiation via capability bits.
- Fix `AUTOPILOT_VERSION` response wait.
- **ArduPilot**: More reliable fence/rally support.

### [4.0.1]
- Fix ArduPilot mission‑item tracking in Fly view.
- Fix ADSB display.
- Fix Plan view map positioning.
- Fix Windows `0xCC000007B` startup error (VC++ runtimes).

### [4.0.0]
- **Flight**: ROI option + Cancel ROI toggle + ROI‑affected path color.
- **Windows**: 64‑bit builds, Qt 5.12.5.
- **ADSB**: SBS server & USB SDR dongle support.
- **Toolbar**: Scrollable on small screens.
- **Plan View**: New initial‑plan UI.
- **Editing Tools**: Corridor & Polygon click‑trace.
- **Performance**: No mobile path‑length limit.
- **ArduPilot**:
  - Motor Test page.
  - Copter: Simple/Super‑Simple modes, advanced tuning, 3.5+ support.
  - Plane: 3.8+ support.
  - Rover: Frame setup & 3.4+ support.
  - Airframe UI overhaul.
- **Plan/Pattern**: Named presets (Survey).
- **ChibiOS**: Improved bootloader support.
- **Misc**:
  - Camera API open to all firmwares.
  - Configurable MAVLink stream rates.
  - Structure Scan rewrite (old plans must be recreated).
  - Object‑avoidance, joystick action modes.
  - UDP RTP H.265, English‑only Linux TTS.
  - Automated Crowdin localization.
  - Korean & Chinese font improvements.
  - New QtQuick MAVLink Inspector.

</details>

---

<details>
<summary><strong>3.5</strong></summary>

#### [3.5.5]
- Fix MAVLink `memset` causing wrong ArduPilot GotoLocation commands.
- Disable Pause when fixed‑wing is landing.

#### [3.5.4]
- Update Windows drivers.
- Add FMUK66 flashing support.
- Guard against null GStreamer geometry.
- `.apj` file‑selection for custom flash.

#### [3.5.3]
- RTK Survey‑In limit → 0.01 m.
- Windows driver‑detection logic fix.
- GeoFence vertex crash fix.
- **PX4:** Add `MC_YAW_FF` to PID Tuning.
- **ArduPilot:** Fix bad chars in param‑file save.

#### [3.5.2]
- Fix Ubuntu AppImage startup.

#### [3.5.1]
- Update Windows USB drivers.
- Add CubeBlack Service‑Bulletin check.
- Fix PX4/ArduPilot logo in toolbar.
- OfflineMaps tile‑set count fix.

#### [3.5.0]
- **Plan GeoFence:** Fix loading from 3.4.
- **Structure Scan:** Height loading fix.
- **ArduPilot:** Home‑position fix (Issue #6840).
- Multi‑component param loading fix.
- Mobile file‑dialog delete fix.
- Fixed RTK station setting.
- Airmap integration.
- Add `ESTIMATOR_STATUS` FactGroup.
- Chinese/Turkish + partial German localization.
- Distance‑to‑GCS & Heading‑to‑Home instruments.
- Position dialog on polygon vertices.
- **Fixed‑Wing Landing**: Stop photo/video support.
- SHP polygon loading.
- Settings version bump (reset defaults).
- Orbit rotation direction toggle.
- Taisync 2.4 GHz ViUlinx HD link.
- NMEA GPS UDP port option.

</details>

---

<details>
<summary><strong>3.4</strong></summary>

#### [3.4.4]
- Notify desktop if newer version available.
- Fix Multi‑Vehicle Start/Pause (Issue #6864).

#### [3.4.3]
- Resume Mission display fix (Issue #6835).
- Home‑Position altitude fix (Issue #6846).

#### [3.4.2]
- Fix new mission items altitude = 0 bug (Issue #6823).

#### [3.4.1]
- Crash on quick terrain‑follow move fix.
- Terrain‑follow rate fields swapped fix.

</details>

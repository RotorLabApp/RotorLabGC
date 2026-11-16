# Setup, upgrade and uninstall

From a fresh install to a station that files its flights to RotorLab. Each step names the labels GC 5.1.1 shows on screen.

## What you need

- A RotorLab account. Your RotorLab GC license key is on your organization's licenses in RotorLab.
- Windows 10 or 11 (x64), or an Android phone or tablet on arm64. [Compatibility](compatibility.md) says what this release was tested on.
- An ArduPilot or PX4 aircraft, or SITL, connected over USB, a telemetry radio or a network link.
- To file flights: an organization admin who can create a Device Key in RotorLab.
- For TAK: your TAK server's address and certificates. The [ATAK and TAK Field Guide](https://github.com/RotorLabApp/rotorlab-gc-tak-guide) covers that setup.

## 1. Download and check GC

Download GC from your RotorLab account (start at [app.rotorlab.app/buy/gc](https://app.rotorlab.app/buy/gc)) or from this repository's [Releases](https://github.com/RotorLabApp/RotorLabGC/releases/latest) page. Check the signature and checksum before you install: the commands and their expected output are in the README, [Verify a release](../README.md#verify-a-release).

## 2. Install

**Windows.** Run `RotorLabGC-v5.1.1-windows-x64-installer.exe`. In PowerShell, `Get-AuthenticodeSignature .\RotorLabGC-v5.1.1-windows-x64-installer.exe` should show `Valid` first.

**Android.** Install `RotorLabGC-v5.1.1-android-arm64-v8a.apk` on the device. From 5.1.1 the app id is `com.rotorlab.gc`, so GC installs next to a stock QGroundControl.

## 3. Activate the license

GC opens on "Activate this device to continue". Enter your license key and press "Activate". Activating needs a connection to RotorLab.

Once a device is activated, GC opens on its stored activation when it is offline. It re-checks the license every 24 hours and when the network comes back; a re-check while GC is running never closes it or interrupts a flight, and the next start applies its result. The license page is Application Settings, License.

## 4. Connect an aircraft

Connect the aircraft as in QGroundControl. A USB or radio link is usually found on its own. For SITL or a network link, open Application Settings, Comm Links, add a link (for example TCP, address `127.0.0.1`, port `5760` for ArduPilot SITL) and connect.

## 5. Turn on the RotorLab Connector

1. **In RotorLab**, an organization admin opens Organization, Integrations, Devices, enters a name for this station under "What is this device" and clicks "Enroll device". The Device Key is shown once: copy it.
2. **In GC**, open Application Settings, RotorLab Connector, and check "Enable RotorLab Connector".
3. Paste the key under "Device Key" and press "Verify". The status reads "Checking the key…" and then "Connected".
4. Under "Behavior", choose whether to "File flights automatically when a flight ends", "Upload the flight log with the flight" and "Ask who is flying at start-up". All three are on by default.
5. Optional: under "How this operation files", set the jurisdiction, operating category and visual mode your flights are flown under. Leave anything you are not sure of unset.

The key is not saved while an aircraft is armed: enter it again after landing. Every option is described in [options](options.md).

## 6. Fly

With "Ask who is flying at start-up" on, the crew screen asks "Who is flying?". Pick the pilot (the day's planned crew comes first with RotorLab 3.5.4 or later), check the aircraft, and press "Start flying". You can skip it and fly.

When the flight ends, GC saves it to the station's queue and files it. The RotorLab Connector page lists anything still "Waiting to file"; "Nothing waiting: everything this station recorded has been filed." means it is all in RotorLab.

## 7. Optional: TAK and ATAK

Set up the TAK server on Application Settings, TAK / ATAK. Every command verb starts off. Follow the [ATAK and TAK Field Guide](https://github.com/RotorLabApp/rotorlab-gc-tak-guide), chapters 1 and 2, for a conservative setup.

## Optional: RotorLab Server (Fleet Server role)

If your organization runs a RotorLab Server in its Fleet Server role, open Application Settings, Fleet Connector, enter the "RotorLab Server address" and the "Enrollment code" your operator gives you, and press "Enroll". Codes work once and expire after 15 minutes. Paste the code exactly as the server console shows it, including any pin after a dot. The Fleet Connector page is hidden while the RotorLab Connector is on. RotorLab Server is in early access testing and not on sale today.

## Upgrade

1. Let the queue empty first: the RotorLab Connector page should read "Nothing waiting: everything this station recorded has been filed."
2. Download the new release and check it as in [Verify a release](../README.md#verify-a-release).
3. **Windows:** run the new installer. As written in `deploy/windows/nullsoft_installer.nsi`, it removes the previous version and keeps GC's data folder, then installs the new one. **Android:** install the new APK over the old one.

Read [CHANGELOG.md](../CHANGELOG.md) first: each release says what changed.

**From 5.1.0 on Android:** 5.1.1 has a new app id (`com.rotorlab.gc`), so it installs as a separate app, and its settings do not carry over from 5.1.0. Activate the license and add the Device Key again in 5.1.1. Before you remove 5.1.0, let its queue empty and press "Deactivate this device" on its License page, so the key is free to activate in 5.1.1.

## Uninstall

1. **Let the queue empty**, or the flights still waiting are lost with the station's data.
2. **Release the license:** Application Settings, License, "Deactivate this device". Without a connection, an organization admin can release it in RotorLab under Organization, Billing, Product licenses.
3. **Remove the Device Key:** Application Settings, RotorLab Connector, "Disconnect". This removes the key and cached lists from this computer. It does not revoke the key in RotorLab: have an organization admin revoke it under Organization, Integrations, Devices.
4. **Remove GC.**
   - **Windows:** in Settings, Apps, uninstall RotorLab GC. As written in `deploy/windows/nullsoft_installer.nsi`, the uninstaller removes the program folder and GC's data folder (`%APPDATA%\RotorLab`), which holds the station queue, the remote pilot in command log and the TAK command log. Copy any log you want to keep before you uninstall.
   - **Android:** uninstall RotorLab GC from the device's app settings. Android removes the app's data with it.

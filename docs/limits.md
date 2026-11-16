# Limits

What RotorLab GC 5.1.1 does not do, and the limits a team should plan around. Each one comes from the source or from what this release was tested on.

## What GC is not

- **Not an authority.** GC records and warns. It never authorizes, approves or clears a flight, and it makes no compliance determination. Readiness words, currency warnings and a green state are RotorLab's records, shown as they are.
- **Not a safety system.** Nothing in RotorLab's features holds up arming, a launch or video. The crew screen and the checklist offers can always be skipped. The remote pilot in command stays responsible for the aircraft.
- **Not a check on altitude, airspace or terrain.** An ATAK `ALT` value is sent as given. The optional altitude warning informs the station; it does not hold or change a command. See the [ATAK and TAK Field Guide](https://github.com/RotorLabApp/rotorlab-gc-tak-guide), chapter 5.

## Filing

- **Flights file when they end**, not live. GC saves each flight to disk when the aircraft disarms and files it from the queue.
- **One route at a time.** A station files either straight to RotorLab (RotorLab Connector) or through a RotorLab Server (Fleet Connector), not both.
- **No log download while armed.** GC collects a flight log only once every aircraft has disarmed, and the aircraft must be connected and active for it.
- **Log size.** A log larger than RotorLab takes for one file (512 MB unless RotorLab says otherwise) is not sent; the queue says where the file is so you can attach it in RotorLab by hand.
- **Feature levels follow RotorLab's version.** Remote pilot in command periods and planned crew need RotorLab 3.5.4 or later; older versions record one pilot per flight. See [compatibility](compatibility.md#rotorlab).
- **Retries slow down.** Items in the queue are retried after 1 s, 5 s, 30 s, 2 min, 10 min, then every 10 minutes. "Retry now" sends at once.
- **An uninstall on Windows removes the queue.** Let it empty first; see [setup](setup.md#uninstall).

## Keys and the license

- **Activation needs a connection.** After that, GC opens offline on its stored activation.
- **One license key per install.** Deactivate it on one device before you activate it on another, or have an organization admin release it in RotorLab.
- **The Device Key cannot be changed while an aircraft is armed.**
- **Disconnect does not revoke.** It removes the key from the station only. Revoke it in RotorLab, under Organization, Integrations, Devices.

## ATAK and TAK

- **A sender cannot be proven.** Callsigns and device UIDs are self-reported. Your TAK server's group membership is the control.
- **TLS only.** GC connects to a TAK server over TLS.
- **The command log is a station record**, with a hash chain, not a tamper-proof record.

The full list is in the [ATAK and TAK Field Guide](https://github.com/RotorLabApp/rotorlab-gc-tak-guide), chapter 9.

## Platforms

- **Released for Windows x64 and Android arm64 only.** Other platforms in QGroundControl's tree are not built or published by RotorLab.
- **Video through Qt Multimedia.** The released builds do not include GStreamer, so some stream types QGroundControl plays with GStreamer may not play. Test your video source before you rely on it.
- **What was not run for 5.1.1:** the APK on an Android device, PX4 aircraft, and Windows 10. See [compatibility](compatibility.md).

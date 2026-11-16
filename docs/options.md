# Options

The settings RotorLab adds to GC, with their on-screen labels and defaults in GC 5.1.1. QGroundControl's own settings (Fly View, Plan View, Video, Comm Links, Maps and the rest) work as they do in QGroundControl. All of these pages are under Application Settings.

## RotorLab Connector

Source: `custom/res/rotorlab/RotorLabConnectorSettings.qml`, `custom/src/rotorlab/`.

"Nothing here can hold up a launch." The page says so, and the connector is designed that way: arming, flight and video do not wait on it.

### RotorLab Connector

| Option | Default | What it does |
|---|---|---|
| Enable RotorLab Connector | Off | Turns on filing to your RotorLab organization. While it is on, the Fleet Connector page is hidden. If the station is enrolled with a RotorLab Server, GC asks first: "Switch to filing directly to RotorLab?" Switching pauses reporting to that server and keeps the enrollment. |

### Device Key

| Item | What it shows or does |
|---|---|
| Status | One of "Not configured", "Checking the key…", "Connected", "Offline, still filing to the queue", "This key was rejected", "The account needs attention" or "RotorLab unavailable, still filing to the queue". See [troubleshooting](troubleshooting.md#rotorlab-connector). |
| Key, Name in RotorLab, Stored in, Server | The masked key, the device's name in RotorLab, the key store that holds it (for example "Windows Credential Manager") and the RotorLab address. |
| Device Key field, Show or Hide, Verify | Paste the key (it begins with `rl_`) and press Verify. The key is not saved while an aircraft is armed. |
| Replace key | Paste a new key. "Re-keying never needs an outage": have an admin create a second key, paste it here, then have them revoke the old one. |
| Disconnect | Removes the Device Key and cached lists from this computer. Anything still waiting to file is kept and files after you reconnect. It does not revoke the key in RotorLab. |

### Behavior

| Option | Default | What it does |
|---|---|---|
| File flights automatically when a flight ends | On | Files each flight from the queue when it ends. |
| Upload the flight log with the flight | On | Collects the aircraft's log after the flight, once no aircraft is armed, and files it with the flight. A log larger than RotorLab takes for one file (512 MB unless RotorLab says otherwise) is not sent, and the queue says how to attach it by hand. |
| Ask who is flying at start-up | On | Opens the crew screen when GC starts. |
| Change remote pilot in command | | Opens the crew screen at any time, in flight too. The label follows your organization's term ("Change remote pilot in command", "Change remote pilot" or "Change pilot"). With RotorLab 3.5.4 or later, a change during a flight is filed with its time; an older RotorLab files the flight under the pilot at takeoff. |

### How this operation files

| Option | Default | What it does |
|---|---|---|
| Jurisdiction, Operating category, Visual mode | (not set) | Sent with every flight so the record says which framework it was flown under. Nothing checks these, so leave anything you are not sure of unset. |

Whether anyone was overflown, and whether dangerous goods were carried, are never sent from here.

### Checklists

Shown only when your organization has published checklists. The pre-flight is offered when an aircraft connects, before its first arm, and the post-flight after landing. Each offer can be ignored and never holds the aircraft. "Run now" runs and files any checklist at any time.

### Missions from RotorLab

Missions planned in RotorLab by the pilot on the crew screen. "Open in Plan" opens one as an ordinary plan; editing and flying work as for a plan you built yourself. Nothing is sent back to RotorLab. "Refresh" fetches the list again.

### Waiting to file

The station queue. "Retry now" sends everything waiting at once. An item that "needs attention" says why and offers "Try again" and "Discard". Try again sends a flight again under the same reference, so it is designed not to be filed twice. Discard removes the item from this computer, and it is not filed; this cannot be undone.

## License

Source: `custom/res/license/LicenseSettings.qml`, `custom/src/license/`.

| Item | What it does |
|---|---|
| License status | "Not activated", "Activated for this session" or "Activated", with the latest check result. |
| License key, Activate | Activates this install. One key per install. Needs a connection to RotorLab. |
| Re-check now | Checks the license with RotorLab now. GC also re-checks every 24 hours and when the network comes back. |
| Deactivate this device | Releases the key so it can be activated on another device. Without a connection, an organization admin can release it in RotorLab under Organization, Billing, Product licenses. |

## Fleet Connector (RotorLab Server, Fleet Server role)

Source: `custom/res/fleet/FleetConnectorSettings.qml`, `custom/src/fleet/`. Hidden while the RotorLab Connector is on.

| Option | Default | What it does |
|---|---|---|
| RotorLab Server address, Enrollment code, Enroll | empty | Enrolls the station with your RotorLab Server. The code is 16 characters, or a code with a pin after a dot; it works once and expires after 15 minutes. A code with a pin needs an `https://` address, and GC checks the server's key against the pin before it sends anything. |
| Refresh now, Disconnect | | Refreshes the connection, or removes the enrollment. |
| Change pilot | | Sets who is flying for the station's flights through the server. |
| Download the log from the aircraft after a flight | On | Collects the aircraft's log after a flight. Never while the aircraft is armed, and stopped at once if it arms. |
| Switch to filing directly to RotorLab… | | Moves the station to the RotorLab Connector. The enrollment is kept, so switching back needs nothing re-entered. |

RotorLab Server is in early access testing and not on sale today.

## Fleet

"Fleet: per-vehicle ATAK metadata". For each aircraft (by MAVLink system ID): a "Callsign", a "Video feed URL" and a "Tail number", used for its TAK track. "Add / pre-stage a sysid" adds an aircraft before it connects.

## TAK / ATAK

Every TAK and ATAK setting, its default, and what each switch does are in the [RotorLab GC ATAK and TAK Field Guide](https://github.com/RotorLabApp/rotorlab-gc-tak-guide), chapters 2 and 5. In short: every command verb starts off, "Send commands only from listed senders" and "Verify server certificate" start on, and the altitude warning starts off (0 ft).

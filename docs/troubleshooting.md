# Troubleshooting

Each message below is shown by RotorLab GC 5.1.1, word for word, with what it means and what to do. `%1` stands for a value GC fills in, such as a file path or an aircraft. The source file is named for each group, so you can read exactly when GC shows it.

For TAK and ATAK messages (the TAK page list, refusals, certificates and enrollment), see the [ATAK and TAK Field Guide, Troubleshooting](https://github.com/RotorLabApp/rotorlab-gc-tak-guide/blob/main/guide/08-troubleshooting.md).

None of these messages holds up arming or a flight. GC records and warns; it never authorizes a flight.

## Starting GC

| Message | What it means and what to do |
|---|---|
| `A second instance of RotorLabGC is already running. Please close the other instance and try again.` | GC is already open for this user. Close the other window, or end it in Task Manager. (`src/main.cc`) |
| `Activate this device to continue` | This install has no license activation yet. Enter your license key and press Activate, with a connection to RotorLab. (`custom/res/license/LicenseGate.qml`) |

## License

Source: `custom/src/license/LicenseManager.cc`.

| Message | What it means and what to do |
|---|---|
| `Enter a license key` | The key field was empty when you pressed Activate. |
| `Could not reach the license server. Check your connection.` | Activating needs a connection to RotorLab. Connect and try again. |
| `This key has reached its activation limit` | The key is active on another install. Deactivate it there, or have an organization admin release it in RotorLab under Organization, Billing, Product licenses. |
| `This license key has expired` | Renew it in your RotorLab account, or ask your organization admin. |
| `This license key has been revoked` | Ask your organization admin for a key. |
| `Too many license requests. Try again later.` | RotorLab is limiting license requests from this install. Wait and try again. |
| `Offline: using stored activation` | GC could not reach RotorLab for a re-check and keeps running on its stored activation. Nothing to do; it checks again when the network comes back. |
| `License check inconclusive: using stored activation` | The re-check got no clear answer. GC keeps running on its stored activation. |
| `License expired (re-check at %1). RotorLab GC keeps running for this session; the next start checks the license again.` (also `License revoked` or `License is no longer valid for this device`) | A re-check while GC was running found a problem. GC does not close and the flight is not interrupted. Sort the license out before the next start. |
| `Could not reach the license server to deactivate. Without a connection, an organization admin can release this key in RotorLab: Organization, Billing, Product licenses, then Release on this device's activation.` | Deactivating needs a connection. Do it later, or have an admin release the key in RotorLab. |
| `No active license to release` | This install has no activation to deactivate. |

## RotorLab Connector

Source: `custom/src/rotorlab/RotorLabConnector.cc` and `custom/res/rotorlab/RotorLabConnectorSettings.qml`.

| Message | What it means and what to do |
|---|---|
| `That does not look like a Device Key. Device Keys begin with rl_ and are created by an organization admin in RotorLab under Organization, Integrations, Devices.` | The pasted text is not a Device Key. A license key is not a Device Key. Copy the Device Key whole. |
| `The key is not saved while an aircraft is armed. Enter it again after landing; the current key stays in use.` | GC never touches the key store while an aircraft is armed. Land, disarm and paste the key again. |
| `The key could not be saved to the %1.` | The platform key store (for example Windows Credential Manager) refused the key. GC does not fall back to a less safe place. Check that your Windows account can use Credential Manager, then try again. |
| `RotorLab rejected this Device Key. It may have been revoked by an organization admin under Organization, Integrations, Devices.` | Status "This key was rejected". Ask an admin for a new key and use Replace key. Flights keep going to the queue. |
| `This account cannot file right now.` (or RotorLab's own sentence) | Status "The account needs attention". RotorLab refused the filing for an account reason, such as the plan. Flights keep going to the queue and file when the account is sorted out. |
| `RotorLab answered with a server error (HTTP %1). Flights keep filing to the queue and send when it is back.` | Status "RotorLab unavailable, still filing to the queue". A RotorLab outage. Nothing to do; the queue retries. |
| `Offline, still filing to the queue` | GC cannot reach RotorLab (no network, DNS or a firewall). Flights stay in the queue on this computer until they are filed. Check that the station can reach `https://app.rotorlab.app` on port 443. |

## Waiting to file (the queue)

Source: `custom/src/rotorlab/RotorLabConnector.cc`, `FlightSpool.cc` and `FilingRules.cc`.

| Message | What it means and what to do |
|---|---|
| `Waiting until no aircraft is armed. GC collects this log once every aircraft has disarmed.` | GC does not download a log while any aircraft is armed. It collects the log when every aircraft has disarmed. |
| `Not collected yet: %1 is not connected. Connect it and make it the active aircraft, and GC collects this log once it is disarmed.` | The aircraft that flew is not connected. Connect it, make it active, and the log is collected. |
| `Not collected yet: %1 is not the active aircraft. Make it the active aircraft and GC collects this log once it is disarmed.` | Select that aircraft in the fly view. |
| `Not collected: %1.` | The aircraft could not send its log, for the reason shown. The flight files without it. Press Try again, or attach the file in RotorLab by hand. |
| `This log is %1 and RotorLab takes up to %2 for one file, so it was not sent. Attach it to the flight in RotorLab by hand: %3` | The log is larger than RotorLab takes for one file. Upload it to the flight in RotorLab yourself, from the path shown. |
| `RotorLab refused this log as larger than it takes for one file. Attach it to the flight in RotorLab by hand: %1` | As above, when RotorLab refuses the size. |
| `The log file is no longer at %1. It was moved or deleted. Put it back and press Try again, or Discard this log.` | The log file was moved after the flight. Put it back at that path and press Try again, or Discard. |
| `This flight was voided in RotorLab. Nothing new was filed.` | Someone voided the flight in RotorLab. GC files nothing more for it. |
| `This flight filed with no pilot named. Name the pilot on the flight in RotorLab.` | The crew screen was skipped. Name the pilot on the flight in RotorLab. |
| `%n item(s) need attention. Each one says why. Flying is never held up by them.` | Read each item's reason, then press Try again or Discard. |

## Crew screen

Source: `custom/res/fleet/CrewScreen.qml`.

| Message | What it means and what to do |
|---|---|
| `Could not load the roster. Tap to retry` | GC could not fetch your roster from RotorLab. Check the connection, then tap. You can still skip and fly. |
| `No pilots on the roster` | Your organization has no pilots GC can list. Add pilots in RotorLab. |
| `currency lapsed` or `currency due soon` | RotorLab's records for that pilot. They inform; they do not stop anyone from being chosen or from flying. |

## RotorLab Server (Fleet Server role)

Source: `custom/src/fleet/FleetConnector.cc`.

| Message | What it means and what to do |
|---|---|
| `That code is wrong or has expired. Codes work once and expire after 15 minutes. Ask your operator for a new one.` | Get a new code from the server console and paste it whole. |
| `Too many wrong codes from this computer. Wait %1 seconds, then try again.` | The server is limiting tries. Wait, then use a fresh code. |
| `The server at %1 does not match the pin in the enrollment code, so nothing was sent. Check the address, or ask your operator for a new code.` | The server's key does not match the pin in the code. Check the address with your operator before you try again. |
| `This code carries a server pin, so the address must start with https://. Nothing was sent.` | Use the `https://` address. |
| `That does not look like a server address.` | Check the address your operator gave you. |
| `Could not reach %1` (sometimes with the reason after a colon) | The station cannot reach the server. Check the address and the network. |
| `This RotorLab Server has no Fleet Server license, so it cannot enroll ground stations. Ask your administrator.` | The server needs its Fleet Server role. Talk to your operator. |
| `This ground station was removed` | The server no longer recognizes this station. Ask your operator for a new enrollment code, then disconnect and enroll again. |
| `The offline telemetry queue is full; new samples cannot be saved.` | The station has been away from the server for a long time. Reconnect it. |

## Still stuck

Open an issue with the bug template and quote the message word for word. Leave out keys, passwords, flight logs and anything personal. For account or license questions, use [rotorlab.app/contact](https://rotorlab.app/contact).

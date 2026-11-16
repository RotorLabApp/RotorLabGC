# FAQ

### Does GC decide whether a flight may go?

No. GC records, reminds and warns, and shows what RotorLab's records say. It never authorizes, approves or clears a flight. Readiness words, currency warnings and a green state are information, not permission to fly. The decision stays with your remote pilot in command and your organization.

### Can anything in GC stop my aircraft from arming or taking off?

RotorLab's features are designed not to. The RotorLab Connector, the crew screen, the checklist offers and the queue never hold up arming, a launch or video, and the crew screen can always be skipped. A license re-check while GC is running never closes it or interrupts a flight. QGroundControl's own checks and your autopilot's own checks work as they always do.

### Do I need RotorLab to use GC?

GC needs a license key from your RotorLab account, and activating it needs a connection to RotorLab. After that, GC opens and flies on its stored activation when it is offline. The RotorLab Connector, the crew screen and the filing queue work with your RotorLab organization; flying, planning and ATAK do not need a connection to it.

### What happens with no network at the field?

GC keeps flying as normal. Each flight is saved to disk when it ends and waits in the station's queue. When RotorLab is reachable again, the queue files it, once.

### What is the difference between the license key and the Device Key?

The license key lets GC open on one install. The Device Key lets one station file to your RotorLab organization; an organization admin creates it under Organization, Integrations, Devices. Device Keys begin with `rl_`.

### Can I change who is flying during a flight?

Yes. "Change remote pilot in command" on the RotorLab Connector page opens the crew screen at any time, in flight too, and you can add an optional reason. With RotorLab 3.5.4 or later, the flight is filed with each pilot's period.

### Does GC send anything when the RotorLab Connector is off?

GC checks its license with RotorLab when it starts, every 24 hours and when the network comes back. Flights, logs, checklist runs and crew changes are sent only when the RotorLab Connector is on (or, when a station is enrolled, to your own RotorLab Server). The full list is in the [security model](security-model.md).

### Where are GC's own records kept?

On the station: the queue, a log of remote pilot in command changes, and the TAK command log. See [security model](security-model.md#what-stays-on-the-station).

### Which aircraft and autopilots?

MAVLink aircraft on ArduPilot or PX4: multirotor, VTOL, fixed-wing and quadplane, over USB, a telemetry radio or a network link. What this release was tested with is in [compatibility](compatibility.md).

### Does GC work with ATAK and my TAK server?

Yes. GC publishes your aircraft as air tracks and can act on ATAK markers, behind per-verb switches that all start off. The [ATAK and TAK Field Guide](https://github.com/RotorLabApp/rotorlab-gc-tak-guide) covers it in full, including chapter 9, FAQ, compatibility and limits.

### Can GC prove who sent an ATAK command?

No. The sender's callsign and device UID are what the sending device reports. "Send commands only from listed senders" narrows who is heard; it does not prove who sent a marker. The control on who can reach the station is your TAK server's group membership.

### Can I run GC next to QGroundControl?

On Android, 5.1.1 has its own app id (`com.rotorlab.gc`), so it installs next to a stock QGroundControl. Side by side on one device was not tested for this release. On Windows, GC installs in its own folder.

### Can I build GC myself?

Yes, from the tagged source. See the README, [Build from source](../README.md#build-from-source). Your build will not be byte-identical to our signed release and carries no RotorLab signature.

### Can I contribute?

Issues are welcome. For now we do not take pull requests; see [CONTRIBUTING.md](../CONTRIBUTING.md).

### Where do I get help?

Account, license and billing: [rotorlab.app/contact](https://rotorlab.app/contact). Using GC: this `docs/` folder, the [RotorLab docs](https://docs.rotorlab.app/) and [RotorLab Academy](https://academy.rotorlab.app/). Security: [SECURITY.md](../SECURITY.md).

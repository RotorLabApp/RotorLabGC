# RotorLab GC documentation

Documentation for RotorLab GC 5.1.1. It describes what that release is designed to do, and names the source so you can check it.

RotorLab GC 5.3.0 and 5.4.0 change some of what these pages say, most of all how GC opens without a connection (a signed offline activation, and no vehicle connection until the device is activated), the new Linux AppImage, the 3D view of the plan, network video and recording, and how ATAK command markers name their station (5.4.0). [CHANGELOG.md](../CHANGELOG.md) lists every change in 5.3.0 and 5.4.0; where it differs from these pages, it is the one that describes those releases.

| Page | What it covers |
|---|---|
| [Concepts](concepts.md) | Station, license key, Device Key, RotorLab Connector, queue, crew screen, sortie, remote pilot in command periods, RotorLab Server |
| [Setup, upgrade and uninstall](setup.md) | From a fresh install to a station that files its flights, then upgrading and removing GC |
| [Options](options.md) | Every setting RotorLab adds, with its label and default |
| [Troubleshooting](troubleshooting.md) | GC's own messages, word for word, with what to do |
| [FAQ](faq.md) | Common questions |
| [Security model](security-model.md) | What GC sends, where and when; what stays on the station; what it trusts and what it cannot protect you from |
| [Compatibility](compatibility.md) | Platforms, autopilots, RotorLab and TAK versions, and what this release was tested with |
| [Limits](limits.md) | What GC does not do, and the limits to plan around |

For everything about ATAK and TAK, see the [RotorLab GC ATAK and TAK Field Guide](https://github.com/RotorLabApp/rotorlab-gc-tak-guide). For RotorLab itself, see [docs.rotorlab.app](https://docs.rotorlab.app/).

GC records and warns. It never authorizes a flight.

[Back to the README](../README.md)

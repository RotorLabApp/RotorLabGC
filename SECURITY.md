# Security

## Reporting a vulnerability

Please report a security issue in RotorLab GC privately, by either:

- email to [security@rotorlab.app](mailto:security@rotorlab.app), or
- GitHub private vulnerability reporting, from this repository's **Security** tab ([Report a vulnerability](https://github.com/RotorLabApp/RotorLabGC/security/advisories/new)).

Do not open a public issue, pull request or discussion for it.

Please include:

- the GC version (Settings, General, or the installer name);
- the platform (Windows or Android) and its version;
- what you found and how to reproduce it;
- what an attacker could do with it, as far as you know.

Leave out real keys, passwords, flight logs and personal data. If we need a sample, we will ask for one and agree a safe way to send it. If a Device Key may have been exposed, have an organization admin revoke it in RotorLab under Organization, Integrations, Devices, and create a new one.

## What happens next

We will confirm that we received your report and keep you informed while we look into it. When a fix ships, it is listed in the release notes, and we are glad to credit you if you would like us to.

We do not run a bug bounty program, and a report does not create a right to a payment.

## Supported versions

| Version | Supported |
|---|---|
| 5.1.1 | Yes |
| 5.1.0 and earlier | No, update to 5.1.1 |

Security fixes are made in the latest RotorLab GC release. Please check that an issue is still present in the latest release before reporting it.

## Scope

In scope: RotorLab's changes in this repository and the releases built from them: the Windows installer, the Android app, the TAK and ATAK command handling, the RotorLab Connector and its queue, the RotorLab Server (Fleet Server role) connection, the license check and Device Key storage. Removing or changing the license check in a build you make yourself is your right under the GNU GPL v3 and is not a vulnerability; a way to use another organization's license key or Device Key is. Of most interest is anything that could let a command reach an aircraft past the station's switches, expose a Device Key or license key, or weaken TLS. [docs/security-model.md](docs/security-model.md) describes what GC trusts and the limits it documents.

A problem in QGroundControl itself that is not specific to RotorLab GC is welcome here too. With your agreement, we will pass it on to the QGroundControl project, or you may report it to them directly. The RotorLab service at app.rotorlab.app is outside this repository's scope; report a problem with it to security@rotorlab.app too. Test only against your own installation, SITL or an aircraft you are entitled to fly, and your own RotorLab organization. Do not access other people's data, stations or aircraft, and do not run load or denial-of-service tests against RotorLab's services.

## Verifying releases

RotorLab signs its GC releases from 5.1.1. README.md, "Verify a release", shows how to check a download against RotorLab's published release key.

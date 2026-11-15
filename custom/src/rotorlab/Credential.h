/****************************************************************************
 *
 * RotorLab GC: where the RotorLab Device Key is kept, per platform.
 *
 * A RotorLab device key can file into a customer's compliance record. It is
 * therefore deliberately NOT kept the way the Fleet Connector keeps its station
 * token: that one is a DPAPI-sealed blob inside the application's own ini,
 * which is good, but the ini is still a file that lands in a support bundle or
 * a backup by accident.
 *
 *   Windows  Credential Manager (CredWrite/CredRead), bound to the Windows
 *            account. The key never appears in the settings file.
 *   Linux    Secret Service (the desktop keyring, through libsecret, loaded at
 *            run time) when a Secret Service answers. Otherwise a settings file
 *            holding the key base64-encoded, which is NOT encryption: anyone who
 *            can read the user's files can read the key.
 *   Android  App-private settings, holding the key encrypted (AES-GCM) with a
 *            key kept in the Android Keystore under the alias
 *            "RotorLabGC.DeviceKey". The Keystore key never leaves it.
 *   macOS    A settings file holding the key base64-encoded, NOT encrypted.
 *   (other)  The same settings file.
 *
 * A key found in the older base64 settings value on a platform that now has a
 * store is moved into the store once, and the settings value is removed after
 * the stored copy reads back.
 *
 * The key is read from the platform store once per run and then served from
 * memory. The platform store is never touched while any vehicle is armed (a
 * Secret Service lookup can raise a keyring unlock prompt): a first read then
 * returns nothing until no vehicle is armed, a save is refused, and an erase
 * clears the memory copy at once and the store when no vehicle is armed.
 *
 * storeName() names the storage in use, plainly, for the settings page.
 *
 * Whatever the platform, the rules are the same and they are the point:
 * never in a log line, never in a URL, never in a crash report, and redacted
 * to `rl_...` in anything resembling a diagnostics bundle.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QString>

namespace Credential {

/// Store `secret` under `account`, replacing anything already there.
/// Returns false if the platform store refused it, or a vehicle is armed. The
/// caller must then treat the key as unavailable; there is no fallback to a
/// weaker store.
bool store(const QString &account, const QString &secret);

/// Read back what `store` wrote, or an empty string when there is nothing.
/// Served from memory after the first read.
QString load(const QString &account);

/// Forget it. Safe to call when nothing is stored.
void erase(const QString &account);

/// True when the secret is kept by an OS facility (Credential Manager, Secret
/// Service, or encrypted with an Android Keystore key) rather than as base64
/// in a settings file.
bool inOsStore();

/// Plain name of the storage in use on this platform, for "Stored in" and
/// "could not be saved to the ..." on the settings page.
QString storeName();

/// Tests: replace the "is any vehicle armed" check (nullptr restores it).
using ArmedProbe = bool (*)();
void setArmedProbe(ArmedProbe probe);

/// Tests: forget the in-memory copies so the next load reads the store.
void dropCache();

} // namespace Credential

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
 *            account, under the target name "RotorLab GC - <account>". The
 *            key never appears in the settings file. GC 5.1.x and earlier
 *            used "RotorLab GC" + em dash + account; such an entry is copied
 *            to the new name at the first read, and deleted only after the
 *            new entry is written and reads back the same.
 *   Linux    Secret Service (the desktop keyring, through libsecret, loaded at
 *            run time) when a Secret Service answers. Otherwise nothing: the
 *            key is not saved (see below).
 *   Android  App-private settings, holding the key encrypted (AES-GCM) with a
 *            key kept in the Android Keystore under the alias
 *            "RotorLabGC.DeviceKey". The Keystore key never leaves it.
 *   macOS    Nothing (see below).
 *   (other)  Nothing.
 *
 * No store (G82): GC never writes a key to the settings file. store() refuses;
 * the caller may keepForSession() so the key works until GC quits, and tells
 * the user it is not saved, the same way for the Device Key and the TAK
 * password.
 *
 * Older builds kept the key base64-encoded (NOT encrypted) in the settings
 * file on Linux with no Secret Service, on macOS, and on Android before the
 * Keystore. At the first read such a copy is moved into the store when one is
 * available now (removed from the file after the stored copy reads back). On
 * every platform, when there is no store, or the store does not accept it
 * (the Secret Service refuses, or Android Keystore encryption fails), the
 * copy is removed from the file and NOT used: the user enters the key again.
 * fileCopy() says which happened, for the settings page.
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

/// Keep `secret` in memory for this run only: nothing is written anywhere.
/// For when inOsStore() is false. erase() forgets it.
bool keepForSession(const QString &account, const QString &secret);

/// What the first read did with a copy an older build left in the settings
/// file: nothing found, moved into the store, or removed without being used
/// (the user has to enter the key again). Cleared by store, keepForSession and
/// erase.
enum class FileCopy { None, Moved, Removed };
FileCopy fileCopy(const QString &account);

/// Forget it. Safe to call when nothing is stored.
void erase(const QString &account);

/// True when the secret is kept by an OS facility (Credential Manager, Secret
/// Service, or encrypted with an Android Keystore key). False means there is
/// no store: store() refuses and nothing is saved.
bool inOsStore();

/// Plain name of the storage in use on this platform, for "Stored in" and
/// "could not be saved in ..." on the settings page.
QString storeName();

/// Tests: replace the "is any vehicle armed" check (nullptr restores it).
using ArmedProbe = bool (*)();
void setArmedProbe(ArmedProbe probe);

/// Tests: forget the in-memory copies so the next load reads the store.
void dropCache();

/// G105, unit test builds: erase every "Unit test ..." and "unittest-..."
/// account this run saved in the OS store. runTests() calls it when the
/// tests end (a test run never enters the event loop, so aboutToQuit does
/// not fire). Does nothing in other builds.
void eraseUnitTestAccounts();

/// Tests: behave as if no system key store were available (Linux with no
/// Secret Service), on any platform. false restores the real check.
void setNoOsStoreForTests(bool on);

/// Tests: run the settings-file copy rule (the one Linux and Android use) with
/// a fake store that accepts the copy or not. Returns the key the read would
/// use. storedCopy gets what the fake store was given; storeCopyDropped says
/// whether an older store copy would have been cleared.
QString resolveFileCopyForTests(const QString &account, bool storeAccepts, bool *storeCopyDropped,
                                QString *storedCopy);

/// True while the platform store is off limits because a vehicle is armed:
/// a store() refused now was refused for that reason. Same check (and test
/// probe) as store() itself.
bool heldWhileArmed();

/// The label the Linux Secret Service shows for `account` (display only;
/// lookups use the "account" attribute). The TAK password entry is
/// "RotorLab GC TAK password"; Device Key entries keep
/// "RotorLab GC Device Key (<account>)". No label has an em dash. The
/// Windows target name is a lookup key and is not derived from this.
QString entryLabel(const QString &account);

} // namespace Credential

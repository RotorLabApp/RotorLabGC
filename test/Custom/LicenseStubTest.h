#pragma once

#include "UnitTest.h"

/// GC activates against a local signing test stub (RotorLab's, not part of
/// this repository) through its own flow: LicenseManager::activate, recheck and
/// deactivate over HTTP on 127.0.0.1, and the next start opening offline from
/// the stored signed record.
///
/// Standalone: runs only when asked for (--unittest:LicenseStubTest), in a
/// Daily build, with ROTORLAB_BASE_URL on 127.0.0.1 and
/// ROTORLAB_LICENSE_PUBKEYS set to that stub. Otherwise it
/// skips. It never talks to any other host.
class LicenseStubTest : public UnitTest
{
    Q_OBJECT

protected slots:
    void init() override;
    void cleanup() override;

private slots:
    void initTestCase();
    void _activateThroughGcFlow();
    void _nextStartOpensOfflineFromSignedRecord();
    void _recheckRefreshesRecord();
    void _unknownKeyRefused();
    void _deactivateClearsRecord();

private:
    static void _clearSettings();
};

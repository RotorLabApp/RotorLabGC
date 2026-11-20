#pragma once

#include "UnitTest.h"

class LicenseManager;

/// License re-check while running (license/LicenseManager.h): a re-check after
/// the launch check only notes what it finds and never closes the session,
/// and the deactivate screen names the admin Release path for offline use.
/// Replies are fed straight in; nothing is sent to a license server.
class LicenseRecheckTest : public UnitTest
{
    Q_OBJECT

private slots:
    void cleanup() override;

    void _inSessionProblemKeepsSession();
    void _inSessionOfflineKeepsSession();
    void _inSessionInconclusiveIsNotAProblem();
    void _inSessionValidClearsNote();
    void _firstVerifyIsTheLaunchCheck();
    void _activationOpensSession();
    void _timerEvery24Hours();
    void _networkRegainRechecksSpaced();
    void _offlineDeactivateNamesRelease();
    void _lateLaunchReplyWhileConnectedOnlyNotes();

private:
    /// Activated and past the launch check, as during a flight.
    static void _openSession(LicenseManager &lm);
    /// A good reply carrying a record signed by a key generated for this test
    /// and pinned in `lm` (G89: a good answer counts only with one).
    static QByteArray _goodReply(LicenseManager &lm, const QString &state);
};

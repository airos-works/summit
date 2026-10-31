#pragma once
#include "core/UpdateCheck.h"
#include <Handler.h>
#include <Messenger.h>
#include <Rect.h>
#include <nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <string>

class BMessageRunner;

namespace summit {
// The Summit that runs: its packages, their versions and the commit it was
// built from, worked out once (packagefs attributes and `package list -i`).
const InstalledBuild& CurrentBuild();
// The package lines of About Summit.
std::string BuildDescription();

// File › Check for Updates… and the daily check of Preferences › General.
// Asks GitHub for the release this build updates from (core/UpdateCheck.h)
// on a thread of its own; a newer build opens the update window, which
// downloads, checks and installs both packages with pkgman and then asks the
// application to restart (kUpdateRestart). Runs on the application's looper.
//
// For tests: SUMMIT_UPDATE_INSTALLED pretends a build is installed (see
// ParseSimulatedBuild) and SUMMIT_UPDATE_DRY_RUN=1 replaces the pkgman
// transaction by unpacking both packages into a scratch folder; a pretended
// build always runs dry. SUMMIT_UPDATE_API replaces
// https://api.github.com/repos/airos-works/summit.
class UpdateChecker final : public BHandler {
public:
    struct Settings {
        bool enabled = true;
        int64_t lastCheck = 0;
    };
    // settings reads Preferences; checked records that GitHub answered, at
    // that time; frontFrame is the front browser window's, for placing the
    // update window (invalid when there is none).
    UpdateChecker(std::function<Settings()> settings, std::function<void(int64_t)> checked, std::function<BRect()> frontFrame);
    ~UpdateChecker() override;
    // Once the handler is on its looper: looks every hour whether the daily check is due.
    void Start();
    // manual (File › Check for Updates…): always says what was found;
    // otherwise only a newer build is shown.
    void Check(bool manual);
    // The application did not restart after all (a page kept its window open).
    void RestartCancelled();
    void MessageReceived(BMessage*) override;
    // The MCP server's updates tool: what runs, the last result, the window.
    nlohmann::json State() const;
    // Presses a button of the update window: "install", "later" or "changes";
    // "cancel" is the Cancel of the Checking for updates window.
    bool Answer(const std::string& button);
    struct Job;
    struct WindowState;

private:
    void Finished();
    // The "Checking for updates…" window of a manual check.
    void ShowChecking();
    void ShowWindow();
    std::function<Settings()> fSettings;
    std::function<void(int64_t)> fChecked;
    std::function<BRect()> fFrontFrame;
    std::unique_ptr<BMessageRunner> fFirst, fHourly;
    std::shared_ptr<Job> fJob;
    // The last finished check.
    std::shared_ptr<Job> fLast;
    BMessenger fWindow;
    std::shared_ptr<WindowState> fWindowState;
    BMessenger fChecking;
};
}

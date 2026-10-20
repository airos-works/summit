#pragma once
#if SUMMIT_MODERN_WEBKIT
#include "ExtensionController.h"
#include <Handler.h>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>

class BMessageRunner;
class BWebKitContext;
namespace summit {
// Keeps the extensions from the Chrome Web Store and Firefox Add-ons up to
// date. It looks for new versions a minute after launch, once a day and when
// asked, downloads and checks them as an installation from the store does,
// and installs them in place of the running version, which keeps its
// settings and data. A version that asks for more access waits for the
// user's approval. Attach to BApplication next to the controller.
class ExtensionUpdater final : public BHandler {
public:
    ExtensionUpdater(std::shared_ptr<BWebKitContext>, std::filesystem::path catalogRoot, ExtensionController&,
        std::function<void()> changed);
    ~ExtensionUpdater() override;
    void Start();
    // Check for Updates: every extension that has a store, even with its
    // automatic updates off.
    void CheckNow();
    // Install the waiting version of an extension that asks for more access.
    void Approve(const std::string& identifier);
    void Shutdown();
    bool HasPendingWork() const;
    bool IsChecking() const { return fWork != nullptr || !fQueue.empty() || fPreparing || fApplying; }
    // For the Extensions window: what it is doing or found, overall and for
    // one extension, and whether a version waits for approval.
    const std::string& Status() const { return fStatus; }
    std::string Note(const std::string& identifier) const;
    bool IsWaiting(const std::string& identifier) const { return fWaiting.count(identifier) > 0; }
    void MessageReceived(BMessage*) override;
private:
    struct Candidate { std::string identifier, name, version, source; };
    struct Found {
        Candidate candidate;
        std::string version, error;
        std::unique_ptr<StagedExtensionPackage> package;
        // Approved access beyond the installed version's; not on Firefox Add-ons.
        bool approved = false, unlisted = false;
    };
    struct Work;
    void Tick();
    void Round(bool manual, bool launch);
    void Collect();
    void PrepareNext();
    void Prepared(const BMessage&);
    void Apply();
    void Changed();
    std::shared_ptr<BWebKitContext> fContext;
    ExtensionCatalog fCatalog;
    std::filesystem::path fDownloadRoot;
    ExtensionController& fController;
    std::function<void()> fChanged;
    std::unique_ptr<BMessageRunner> fTimer;
    std::shared_ptr<Work> fWork;
    std::thread fThread;
    // Downloads to prepare, one at a time, then the prepared update that
    // waits for the controller.
    std::deque<Found> fQueue;
    std::unique_ptr<Found> fPreparing;
    std::unique_ptr<ExtensionController::Update> fReady;
    bool fApplying = false;
    // Versions that ask for more access, until approved.
    std::map<std::string, Found> fWaiting;
    std::map<std::string, std::string> fNotes;
    std::string fStatus;
    bigtime_t fStarted = 0;
    uint64 fRequest = 0;
    bool fLaunchChecked = false, fManual = false, fStopping = false;
    unsigned fUpdated = 0, fFailed = 0;
};
}
#endif

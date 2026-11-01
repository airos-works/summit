#pragma once
#include "core/SafeBrowsing.h"
#include <GroupView.h>
#include <SupportDefs.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

class BButton;
class BCheckBox;
class BMessageRunner;
class BStringView;

namespace summit {
// Fraudulent and malicious website warnings (#150). Two free block lists are
// downloaded in the background every few hours and kept in the profile's
// SafeBrowsing folder; every main-frame navigation is checked against them on
// this computer (nothing about the pages visited leaves it):
// - phishing: Phishing Army's domain list (phishing.army, free for personal
//   use; it aggregates PhishTank, OpenPhish, CERT.PL and others);
// - malware: abuse.ch URLhaus's online malware addresses, as malware-filter
//   republishes them (CC0) now that URLhaus's own downloads need an account.
// Google Safe Browsing (the Update API with the user's own key) would be a
// third source: a list whose hashes come from its hash prefixes, filled by
// the same updater thread; not done yet.
//
// Engines with BWebKitSetNavigationCheck (B_WEBKIT_HAS_NAVIGATION_CHECK) ask
// before a request is made; with older engines the browser windows stop a
// listed page as soon as it starts loading.
class SafeBrowsingService {
public:
    static SafeBrowsingService& Shared();
    // Reads the settings and lists kept in directory, puts the check in front
    // of the engine's navigations and starts the updater. Once.
    void Start(const std::filesystem::path& directory);
    // Before the engine goes: no more checks, the updater stops.
    void Stop();
    // The list url is on, None when it is on none, checking is off, or the
    // user chose to visit its host anyway. Any thread, a few microseconds.
    safebrowsing::Threat Check(std::string_view url) const;
    // "Visit This Site Anyway": url's host is not checked again until Summit quits.
    void AllowHost(std::string_view url);
    bool Enabled() const { return fEnabled.load(); }
    void SetEnabled(bool);
    // Preferences › Security › Update Now.
    void UpdateNow();
    // Whether the engine checks navigations itself.
    static bool EngineChecks();
    struct Status {
        bool enabled = true, updating = false;
        // Seconds since the epoch of the last time every list was up to date (0: never).
        int64 lastUpdate = 0;
        size_t phishing = 0, malware = 0;
        // Why the last update failed, empty when it did not.
        std::string error;
    };
    Status CurrentStatus() const;
private:
    struct Feed {
        safebrowsing::Threat threat;
        const char* name;
        std::vector<std::string> urls;
        bool custom = false;
        std::string etag, lastModified;
        int64 updated = 0;
    };
    SafeBrowsingService() = default;
    void Run();
    // Brings one list up to date; false (and error) when it could not.
    bool UpdateFeed(Feed&, std::string& error);
    void LoadSettings();
    void SaveSettings();
    std::filesystem::path fDirectory;
    safebrowsing::Database fDatabase;
    std::atomic<bool> fEnabled { true };
    std::atomic<bool> fStopping { false };
    std::atomic<bool> fUpdating { false };
    mutable std::mutex fMutex;
    std::condition_variable fWake;
    bool fUpdateRequested = false;
    int64 fLastUpdate = 0;
    std::string fLastError;
    std::vector<Feed> fFeeds;
    std::thread fThread;
    bool fStarted = false;
};

// Preferences › Security: the setting, when the lists were last brought up to
// date and Update Now.
class SecurityPreferences : public BGroupView {
public:
    SecurityPreferences();
    ~SecurityPreferences() override;
    void AttachedToWindow() override;
    void DetachedFromWindow() override;
    void MessageReceived(BMessage*) override;
private:
    void ShowStatus();
    BCheckBox* fEnabled;
    BStringView* fStatus;
    BStringView* fCounts;
    BButton* fUpdate;
    std::unique_ptr<BMessageRunner> fRefresh;
};
}

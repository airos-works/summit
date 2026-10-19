#pragma once
#include "core/Profile.h"
#include <Messenger.h>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace summit {
// The profile all browser windows share: bookmarks, history, settings and the
// saved session. Every window runs on its own thread, so all access goes
// through this object, and changes are announced to the windows with a
// kProfileChanged message ("changes": the bits below, "sender": the window
// that made the change, "icon_url" for kIconChanged).
class SharedProfile {
public:
    enum : uint32 {
        kHistoryChanged = 1 << 0,
        kHistoryCleared = 1 << 1,
        kBookmarksChanged = 1 << 2,
        kSettingsChanged = 1 << 3,
        kIconChanged = 1 << 4,
        // A site's zoom changed: "zoom_key" names it, "private" says whether
        // for private windows (which keep their own, unsaved, zoom levels).
        kZoomChanged = 1 << 5,
        // Site icons were deleted; windows drop the ones they hold.
        kIconsCleared = 1 << 6,
        // An extension's action was put on or taken off the toolbar.
        kExtensionsPinnedChanged = 1 << 7,
        // An extension was installed, removed, turned on or off.
        kExtensionsInstalledChanged = 1 << 8,
        // When Summit last asked GitHub for a new build (Profile::lastUpdateCheck);
        // nothing to show again.
        kUpdateCheckChanged = 1 << 9,
    };
    // An extension in the Extensions window, for the cards on store pages.
    struct InstalledExtensionInfo {
        std::string name;
        bool enabled = true;
        bool operator==(const InstalledExtensionInfo& other) const { return name == other.name && enabled == other.enabled; }
    };
    explicit SharedProfile(std::filesystem::path path);
    // Writes what has not been saved yet.
    ~SharedProfile();
    const std::filesystem::path& Path() const { return fPath; }
    // Why the saved profile could not be read (it is then left untouched).
    const std::string& LoadError() const { return fLoadError; }
    // The windows saved when Summit last quit.
    std::vector<WindowSession> SavedWindows() const;

    void AddListener(const BMessenger&);
    void RemoveListener(const BMessenger&);

    // Runs function(const Profile&) under the lock and returns its result.
    template<typename Function> auto Read(Function&& function) const
    {
        std::lock_guard lock(fMutex);
        return function(static_cast<const Profile&>(fProfile));
    }
    // Runs function(Profile&) under the lock. It returns the change bits (0
    // when nothing changed); a change is announced to every window and saved,
    // at once or (saveNow false, for visits) with the next session save.
    void Change(const std::function<uint32(Profile&)>& function, const BMessenger& sender = BMessenger(),
        bool saveNow = true);
    void IconChanged(const std::string& pageURL, const BMessenger& sender);

    // The zoom remembered for a site (ZoomKey()), 1 when there is none.
    // Private windows see their own changes over the saved ones; theirs are
    // forgotten when the last private window closes (ClearPrivateSession).
    double SiteZoom(const std::string& key, bool privateBrowsing) const;
    void SetSiteZoom(const std::string& key, double zoom, bool privateBrowsing, const BMessenger& sender);
    void ClearPrivateSession();
    // Forgets every visited page: the history list, the History page written
    // from it and the icons of sites that are not bookmarked.
    void ClearHistory(const BMessenger& sender);

    // Each window publishes its tabs under its own key; windows are saved in
    // key order. A window closed while Summit keeps running is removed.
    void SetWindowSession(uint64 key, const WindowSession& session);
    void RemoveWindowSession(uint64 key);
    // The page a new tab opens when an enabled extension overrides it
    // (chrome_url_overrides.newtab); empty for the home page. Not saved.
    void SetNewTabOverride(std::string url)
    {
        std::lock_guard lock(fMutex);
        fNewTabOverride = std::move(url);
    }
    std::string NewTabOverride() const
    {
        std::lock_guard lock(fMutex);
        return fNewTabOverride;
    }
    // The installed extensions by identifier, which the application keeps up
    // to date (a change is announced as kExtensionsInstalledChanged). Not saved.
    void SetInstalledExtensions(std::map<std::string, InstalledExtensionInfo>);
    std::map<std::string, InstalledExtensionInfo> InstalledExtensions() const
    {
        std::lock_guard lock(fMutex);
        return fInstalledExtensions;
    }
    // Writes the profile if anything changed since the last save.
    // Saves now, on the caller's thread.
    bool Save(std::string& error);
    // Saves on the profile's own thread a moment from now, with whatever else
    // changes meanwhile: writing the profile (history, bookmarks, sessions)
    // and waiting for the disk took up to 100 ms on the window thread.
    void SaveSoon();
    // Why the last save on the profile's thread failed, or nothing.
    std::string SaveError() const
    {
        std::lock_guard lock(fMutex);
        return fSaveError;
    }

private:
    void SaveLoop();
    void Announce(uint32 changes, const BMessenger& sender, const std::string& iconURL = std::string(),
        const BMessage* details = nullptr);
    mutable std::mutex fMutex;
    std::mutex fSaveMutex;
    const std::filesystem::path fPath;
    Profile fProfile;
    std::vector<WindowSession> fSavedWindows;
    std::string fLoadError;
    std::string fNewTabOverride;
    std::map<std::string, InstalledExtensionInfo> fInstalledExtensions;
    std::map<std::string, double> fPrivateZoom;
    bool fWritable = true;
    std::map<uint64, WindowSession> fSessions;
    std::vector<BMessenger> fListeners;
    uint64 fRevision = 1, fSavedRevision = 0;
    std::thread fSaver;
    std::condition_variable fSaveWanted;
    bool fSaveRequested = false, fStopping = false;
    std::string fSaveError;
};
}

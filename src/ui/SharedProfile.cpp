#include "SharedProfile.h"
#include "StallTrace.h"
#include "Messages.h"
#include "core/Favicon.h"
#include "core/Zoom.h"
#include <set>
#include <Message.h>
#include <algorithm>
#include <cstdio>

namespace summit {
static bool SameSession(const WindowSession& a, const WindowSession& b)
{
    if (a.selected != b.selected || a.tabs.size() != b.tabs.size()
        || !std::equal(std::begin(a.frame), std::end(a.frame), std::begin(b.frame))) return false;
    for (size_t i = 0; i < a.tabs.size(); ++i)
        if (a.tabs[i].url != b.tabs[i].url || a.tabs[i].title != b.tabs[i].title) return false;
    return true;
}

SharedProfile::SharedProfile(std::filesystem::path path) : fPath(std::move(path))
{
    fProfile = Profile::Load(fPath, fLoadError);
    // An unreadable profile is preserved: nothing is written over it.
    fWritable = fLoadError.empty();
    fSavedWindows = std::move(fProfile.windows);
    fProfile.windows.clear();
}

SharedProfile::~SharedProfile()
{
    {
        std::lock_guard lock(fMutex);
        fStopping = true;
    }
    fSaveWanted.notify_all();
    if (fSaver.joinable()) fSaver.join();
    std::string error;
    if (!Save(error)) std::fprintf(stderr, "Summit: could not save the profile: %s\n", error.c_str());
}

void SharedProfile::SaveSoon()
{
    {
        std::lock_guard lock(fMutex);
        if (fStopping || !fWritable || fRevision == fSavedRevision) return;
        fSaveRequested = true;
        if (!fSaver.joinable()) fSaver = std::thread([this] { SaveLoop(); });
    }
    fSaveWanted.notify_all();
}

void SharedProfile::SaveLoop()
{
    rename_thread(find_thread(nullptr), "profile saver");
    std::unique_lock lock(fMutex);
    while (true) {
        fSaveWanted.wait(lock, [this] { return fSaveRequested || fStopping; });
        if (fStopping) return;
        // A page load changes history twice or more within a moment, and a
        // window's session with it: one write takes them all.
        fSaveWanted.wait_for(lock, std::chrono::milliseconds(250), [this] { return fStopping; });
        if (fStopping) return;
        fSaveRequested = false;
        lock.unlock();
        std::string error;
        Save(error);
        lock.lock();
        fSaveError = error;
    }
}

std::vector<WindowSession> SharedProfile::SavedWindows() const
{
    std::lock_guard lock(fMutex);
    return fSavedWindows;
}

void SharedProfile::AddListener(const BMessenger& listener)
{
    std::lock_guard lock(fMutex);
    fListeners.push_back(listener);
}

void SharedProfile::RemoveListener(const BMessenger& listener)
{
    std::lock_guard lock(fMutex);
    std::erase(fListeners, listener);
}

void SharedProfile::Change(const std::function<uint32(Profile&)>& function, const BMessenger& sender, bool saveNow)
{
    uint32 changes;
    {
        std::lock_guard lock(fMutex);
        changes = function(fProfile);
        if (changes) ++fRevision;
    }
    if (!changes) return;
    if (saveNow) SaveSoon();
    Announce(changes, sender);
}

void SharedProfile::IconChanged(const std::string& pageURL, const BMessenger& sender)
{
    Announce(kIconChanged, sender, pageURL);
}

double SharedProfile::SiteZoom(const std::string& key, bool privateBrowsing) const
{
    if (key.empty()) return 1;
    std::lock_guard lock(fMutex);
    if (privateBrowsing)
        if (auto found = fPrivateZoom.find(key); found != fPrivateZoom.end()) return found->second;
    auto found = fProfile.siteZoom.find(key);
    return found == fProfile.siteZoom.end() ? 1 : found->second;
}

void SharedProfile::SetSiteZoom(const std::string& key, double zoom, bool privateBrowsing, const BMessenger& sender)
{
    if (key.empty()) return;
    {
        std::lock_guard lock(fMutex);
        if (privateBrowsing) fPrivateZoom[key] = zoom;
        else {
            auto& zooms = fProfile.siteZoom;
            if (IsDefaultZoom(zoom)) zooms.erase(key);
            else if (zooms.size() < 5000 || zooms.contains(key)) zooms[key] = zoom;
            // Saved with the next session save: a wheel turns through many steps.
            ++fRevision;
        }
    }
    BMessage details;
    details.AddString("zoom_key", key.c_str());
    details.AddBool("private", privateBrowsing);
    Announce(kZoomChanged, sender, std::string(), &details);
}

void SharedProfile::ClearPrivateSession()
{
    std::lock_guard lock(fMutex);
    fPrivateZoom.clear();
}

void SharedProfile::ClearHistory(const BMessenger& sender)
{
    std::set<std::string> keep;
    {
        std::lock_guard lock(fMutex);
        fProfile.history.clear();
        for (const auto& bookmark : fProfile.bookmarks.Pages()) {
            const auto key = FaviconKey(bookmark.url);
            keep.insert(key);
            keep.insert(key.rfind("www.", 0) == 0 ? key.substr(4) : "www." + key);
        }
        ++fRevision;
    }
    std::string error;
    Save(error);
    const auto directory = fPath.parent_path();
    std::error_code ignored;
    // The History page is rewritten whenever it is opened; until then it
    // would still list what was just cleared.
    std::filesystem::remove(directory / "Pages" / "history.html", ignored);
    for (auto entry = std::filesystem::directory_iterator(directory / "Favicons", ignored);
        !ignored && entry != std::filesystem::directory_iterator(); entry.increment(ignored)) {
        const auto name = entry->path().filename().string();
        if (name.size() > 4 && name.ends_with(".png") && !keep.contains(name.substr(0, name.size() - 4)))
            std::filesystem::remove(entry->path(), ignored);
    }
    Announce(kHistoryChanged | kHistoryCleared | kIconsCleared, sender);
}

void SharedProfile::Announce(uint32 changes, const BMessenger& sender, const std::string& iconURL, const BMessage* details)
{
    std::vector<BMessenger> listeners;
    {
        std::lock_guard lock(fMutex);
        std::erase_if(fListeners, [](const BMessenger& listener) { return !listener.IsValid(); });
        listeners = fListeners;
    }
    BMessage message(details ? *details : BMessage());
    message.what = kProfileChanged;
    message.AddUInt32("changes", changes);
    message.AddMessenger("sender", sender);
    if (!iconURL.empty()) message.AddString("icon_url", iconURL.c_str());
    for (const auto& listener : listeners) listener.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
}

void SharedProfile::SetInstalledExtensions(std::map<std::string, InstalledExtensionInfo> extensions)
{
    {
        std::lock_guard lock(fMutex);
        if (extensions == fInstalledExtensions) return;
        fInstalledExtensions = std::move(extensions);
    }
    Announce(kExtensionsInstalledChanged, BMessenger());
}

void SharedProfile::SetWindowSession(uint64 key, const WindowSession& session)
{
    std::lock_guard lock(fMutex);
    auto found = fSessions.find(key);
    if (found != fSessions.end() && SameSession(found->second, session)) return;
    fSessions[key] = session;
    ++fRevision;
}

void SharedProfile::RemoveWindowSession(uint64 key)
{
    std::lock_guard lock(fMutex);
    if (fSessions.erase(key)) ++fRevision;
}

bool SharedProfile::Save(std::string& error)
{
    summit::StallScope scope("profile save", 'save');
    error.clear();
    std::lock_guard saving(fSaveMutex);
    Profile snapshot;
    uint64 revision;
    {
        std::lock_guard lock(fMutex);
        if (!fWritable || fRevision == fSavedRevision) return true;
        snapshot = fProfile;
        for (const auto& [key, session] : fSessions)
            if (!session.tabs.empty()) snapshot.windows.push_back(session);
        revision = fRevision;
    }
    if (!snapshot.Save(fPath, error)) return false;
    std::lock_guard lock(fMutex);
    fSavedRevision = std::max(fSavedRevision, revision);
    return true;
}
}

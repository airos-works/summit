#pragma once
#include "Bookmarks.h"
#include "PageRecord.h"
#include "Protocol.h"
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace summit {
// One browser window of a saved session.
struct WindowSession {
    std::vector<PageRecord> tabs;
    size_t selected = 0;
    // Window frame in screen coordinates (left, top, right, bottom); all zero
    // when the window was never placed.
    float frame[4] = {0, 0, 0, 0};
    // The first `pinned` tabs are pinned (small, icon-only tabs).
    size_t pinned = 0;
    bool HasFrame() const { return frame[2] > frame[0] && frame[3] > frame[1]; }
};
// A server certificate the user chose to trust for a host although it failed
// verification: typically a router or NAS with a self-signed certificate.
struct TrustedCertificate {
    std::string host;
    std::string sha256; // lower-case hex SHA-256 of the DER leaf certificate
    std::string subject;
    int64_t added = 0;  // seconds since the epoch
};
// A website installed as an app (#75): its own window and launcher.
struct WebApp {
    std::string id;       // letters and digits, unique
    std::string name;
    std::string startURL;
    std::string scope;    // pages under this address belong to the app
    std::string launcher; // the launcher file (Deskbar and Tracker open it)
};
struct ProfileLoad;
struct Profile {
    // Open windows, front to back as they were created. Older builds only
    // read "tabs" and "selected", which hold the first window.
    std::vector<WindowSession> windows;
    // In folders: the bookmarks bar and Other Bookmarks. profile.json keeps
    // the folders in "bookmarkTree" and every bookmark in "bookmarks" too, the
    // flat list older builds read (they ignore the folders).
    Bookmarks bookmarks;
    std::vector<PageRecord> history;
    // Where the Home button goes; empty means the built-in start page.
    std::string homeURL;
    bool showBookmarksBar = true;
    // Preferences › Browsing › Spelling: the dictionary to check with ("de_DE");
    // empty for the system's language.
    std::string spellingLanguage;
    // View › Zoom Text Only: zooming changes the text's size, not the page's.
    bool zoomTextOnly = false;
    // Accessibility aids (#118): View › Caret Browsing (F7), Reduce Motion
    // (prefers-reduced-motion) and Increase Contrast (prefers-contrast), and
    // Preferences › Browsing's minimum font size (CSS pixels, 0 for none).
    bool caretBrowsing = false, reduceMotion = false, increaseContrast = false;
    int minimumFontSize = 0;
    // Preferences › Browsing: unload long-unseen background tabs when memory
    // is low (#116).
    bool unloadWhenMemoryIsLow = true;
    // Whether Summit starts with the windows and tabs open when it last quit
    // (or the system shut down), where they were; otherwise with one window.
    bool restoreSession = true;
    // "haiku" draws the window like other Haiku applications (real buttons and
    // tabs); "safari" is the flat, Safari-like look.
    std::string interfaceStyle = "haiku";
    // The engine that searches text typed in the address field (SearchEngines()).
    std::string searchEngine = "duckduckgo";
    // Preferences › Compatibility: a UserAgentPresets() identifier.
    std::string userAgent = "default";
    // Page zoom remembered per site (ZoomKey()); sites at 100% are not listed.
    std::map<std::string, double> siteZoom;
    // Extensions whose action button is not on the toolbar (their actions
    // are in the toolbar's extensions menu), by installation identifier.
    std::set<std::string> unpinnedExtensions;
    // Certificates trusted despite failing verification, by host.
    std::vector<TrustedCertificate> trustedCertificates;
    // What sites may do, as the user answered when they asked: permission
    // ("notifications", "geolocation") -> origin ("https://host[:port]") ->
    // allowed. Sites not listed ask again.
    std::map<std::string, std::map<std::string, bool>> sitePermissions;
    std::map<std::string, ProtocolHandler> protocolHandlers;
    // A declined site registration is not prompted again until removed in Preferences.
    std::map<std::string, std::set<std::string>> declinedProtocolHandlers;
    // Summit's built-in debugging server (MCP) for tools such as Claude Code:
    // off unless turned on in Preferences › Developer. The token is made when
    // it is first turned on.
    bool mcpEnabled = false;
    int mcpPort = 7790;
    bool mcpLocalOnly = false;
    std::string mcpToken;
    // Saved passwords (#67, kept in the Haiku key store, not here): whether
    // Summit offers to save them, and sites it never offers for.
    bool savePasswords = true;
    std::set<std::string> passwordNever;
    // Spelling is checked while typing in pages (#66).
    bool spellChecking = true;
    // Media that start by themselves (#70): "allow", "block-audio" (only
    // without sound) or "block" (not before the user interacts). Sites can
    // have their own choice ("autoplay" in sitePermissions).
    std::string autoplay = "block-audio";
    std::vector<WebApp> webApps;
    const WebApp* FindWebApp(const std::string& id) const;
    // Preferences › General: look for a new Summit build once a day (File ›
    // Check for Updates… looks at once), and when GitHub last answered that
    // question, in seconds since the epoch (0: never).
    bool checkForUpdates = true;
    int64_t lastUpdateCheck = 0;
    bool SetProtocolHandler(const std::string& scheme, const ProtocolHandler&);
    bool RemoveProtocolHandler(const std::string& scheme);
    static Profile Load(const std::filesystem::path& path, std::string& error);
    // Load(), and when the file is damaged, the newest of its copies that
    // can be read (Backups()), so that a power cut that zero-filled the
    // file costs at most the last changes (#93). The damaged file is kept
    // beside it as profile.damaged-DATE.json.
    static ProfileLoad LoadOrRestore(const std::filesystem::path& path, int64_t now = std::time(nullptr));
    // Writes the profile (a new file, then renamed over the old one). The
    // file it replaces becomes path.1, and the first save of a day is also
    // kept as profile-backup-DATE.json, for the last kDailyBackups days.
    bool Save(const std::filesystem::path& path, std::string& error, int64_t now = std::time(nullptr)) const;
    static constexpr size_t kDailyBackups = 3;
    // The copies Save() keeps, newest first.
    static std::vector<std::filesystem::path> Backups(const std::filesystem::path& path);
    // Records a visit to an http(s) page; returns false for other pages.
    bool Visit(const PageRecord& page);
    bool Visit(const PageRecord& page, int64_t now);
    // A download is a history visit of its original URL, but does not add to
    // the page's visit count. Keep an existing title; otherwise use filename.
    // The caller supplies the start time in microseconds, once per download.
    bool VisitDownload(const PageRecord& page, int64_t startedMicros);
    // The destination name may arrive later. Never recreate cleared history.
    bool SetDownloadTitle(const std::string& url, int64_t startedMicros, const std::string& filename);
    // Forgets the visits from since (seconds since the epoch) on, and the
    // pages visited only then; returns how many visits went.
    size_t ForgetVisitsSince(int64_t since);
    // Adds the page to the bar or Other Bookmarks, or moves an existing
    // bookmark of its address onto the bar (bar true).
    void AddBookmark(const PageRecord& page, bool bar);
    bool RemoveBookmark(const std::string& url) { return bookmarks.RemoveURL(url); }
    // Adds pages visited in another browser that history does not have yet,
    // keeping the most recent 2000 pages; returns how many were added.
    size_t ImportHistory(const std::vector<PageRecord>& pages, bool* changed = nullptr);
    // Remembers a trusted certificate (host names compare without case); false
    // when that host already had it.
    bool TrustCertificate(const TrustedCertificate&);
    // Records (allowed or denied) or forgets (nullopt) a site's permission;
    // false when nothing changed or the names are not usable.
    bool SetSitePermission(const std::string& permission, const std::string& origin, std::optional<bool> allowed);
};
// What Profile::LoadOrRestore() found.
struct ProfileLoad {
    Profile profile;
    // Why the profile itself could not be read; empty when it was.
    std::string error;
    // The copy loaded instead, and when it was written (seconds since the
    // epoch); empty when none could be read (the profile starts afresh).
    std::filesystem::path restoredFrom;
    int64_t restoredTime = 0;
    // Where the unreadable file was moved.
    std::filesystem::path damagedCopy;
    // Whether the profile may be saved. Not when the file is a newer
    // Summit's (an unknown version) or could not be moved aside: it is then
    // left as it is.
    bool writable = true;
};
// The permissions sites can ask for, in the order the Preferences list them.
const std::vector<std::string>& SitePermissionNames();
}

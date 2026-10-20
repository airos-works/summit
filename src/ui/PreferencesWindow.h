#pragma once
#include <Messenger.h>
#include <Window.h>
#include <filesystem>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

class BButton;
class BMessageRunner;
class BColumnListView;
class BTabView;
class BCheckBox;
class BMenuField;
class BRadioButton;
class BStringView;
class BTextControl;

namespace summit {
// What the Preferences window starts with.
struct PreferencesState {
    std::string homeURL;
    bool showBookmarksBar = true;
    std::string interfaceStyle;
    std::string searchEngine;
    size_t historyCount = 0;
    // The profile's WebKit folder, whose Cache folder's size is shown.
    std::filesystem::path webKitDirectory;
    // Certificates the user chose to trust although they failed verification.
    size_t trustedCertificates = 0;
    bool restoreSession = true;
    // Preferences › Developer: the built-in debugging server (McpService.h).
    bool mcpEnabled = false;
    int mcpPort = 7790;
    bool mcpLocalOnly = false;
    // Preferences › General › Updates (UpdateChecker.h).
    bool checkForUpdates = true;
    int64 lastUpdateCheck = 0;
    // Preferences › Browsing (BrowsingPreferences.h).
    std::string autoplay = "block-audio";
    bool spellChecking = true;
    bool savePasswords = true;
    // The dictionary chosen (empty: the system's language) and those installed.
    std::string spellingLanguage;
    std::vector<std::string> dictionaries;
    // Installed web apps: name and start address, by identifier.
    std::vector<std::tuple<std::string, std::string, std::string>> webApps;
};
// Edit › Preferences…. One window for the application; changes apply to every
// browser window as they are made and are saved in the profile.
class PreferencesWindow final : public BWindow {
public:
    PreferencesWindow(BMessenger owner, const PreferencesState& state);
    void MessageReceived(BMessage*) override;
    bool QuitRequested() override;
private:
    void ShowHomeHint();
    void ShowHistoryCount(size_t count);
    void ShowTrustedCertificates(size_t count);
    void ShowLastUpdateCheck(int64 when);
    // Measures the cache on another thread; kCacheMeasured brings the result.
    void MeasureCache();
    struct SiteEntry {
        std::string permission, origin;
        bool allowed = false;
    };
    void ShowSitePermissions(const std::vector<SiteEntry>&);
    void ShowProtocolHandlers(const std::vector<std::pair<std::string, std::string>>&);
    void SendMcpPort();
    BCheckBox* fMcpEnabled;
    BCheckBox* fMcpLocalOnly;
    BTextControl* fMcpPort;
    BStringView* fMcpStatus;
    BStringView* fMcpEndpoint;
    BButton* fMcpCopy;
    BButton* fMcpNewToken;
    std::string fMcpConfiguration;
    std::unique_ptr<BMessageRunner> fMcpRefresh;
    BColumnListView* fProtocols;
    BButton* fProtocolRemove;
    std::vector<std::pair<std::string, std::string>> fProtocolEntries;
    BTabView* fTabs;
    BColumnListView* fSites;
    // Preferences › History and Data › Site data: sites and their sizes.
    BColumnListView* fSiteData;
    BStringView* fSiteDataTotal;
    BButton* fSiteDataRemove;
    void RequestSiteData();
    BButton* fSiteAllow;
    BButton* fSiteBlock;
    BButton* fSiteRemove;
    // The rows of fSites, in order.
    std::vector<SiteEntry> fSiteEntries;
    BMessenger fOwner;
    BStringView* fDefaultInfo;
    BButton* fMakeDefault;
    BTextControl* fHome;
    BStringView* fHint;
    BMenuField* fSearchEngine;
    BCheckBox* fRestoreSession;
    BCheckBox* fCheckForUpdates;
    BStringView* fUpdateInfo;
    BCheckBox* fBookmarksBar;
    BRadioButton* fHaikuStyle;
    BRadioButton* fSafariStyle;
    BStringView* fHistoryInfo;
    BStringView* fCertificatesInfo;
    BButton* fForgetCertificates;
    BStringView* fCacheInfo;
    BStringView* fDataStatus;
    // What the three Clear buttons take: the last this many seconds, 0 all time.
    BMenuField* fClearRange;
    int64 ClearSince() const;
    std::string ClearRangeText() const;
    std::filesystem::path fWebKitDirectory;
};
}

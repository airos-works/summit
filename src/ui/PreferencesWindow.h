#pragma once
#include <Messenger.h>
#include <Window.h>
#include <filesystem>
#include <string>
#include <vector>

class BButton;
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
    // Measures the cache on another thread; kCacheMeasured brings the result.
    void MeasureCache();
    struct SiteEntry {
        std::string permission, origin;
        bool allowed = false;
    };
    void ShowSitePermissions(const std::vector<SiteEntry>&);
    void ShowProtocolHandlers(const std::vector<std::pair<std::string, std::string>>&);
    BColumnListView* fProtocols;
    BButton* fProtocolRemove;
    std::vector<std::pair<std::string, std::string>> fProtocolEntries;
    BTabView* fTabs;
    BColumnListView* fSites;
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
    BCheckBox* fBookmarksBar;
    BRadioButton* fHaikuStyle;
    BRadioButton* fSafariStyle;
    BStringView* fHistoryInfo;
    BStringView* fCertificatesInfo;
    BButton* fForgetCertificates;
    BStringView* fCacheInfo;
    BStringView* fDataStatus;
    std::filesystem::path fWebKitDirectory;
};
}

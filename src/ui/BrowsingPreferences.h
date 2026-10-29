#pragma once
#include <GroupView.h>
#include <Messenger.h>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

class BCheckBox;
class BColumnListView;
class BFilePanel;
class BMenuField;
class BButton;
class BStringView;

namespace summit {
// Preferences › Browsing › Accessibility (#118; the first three are also in
// the View menu) and Memory (#116).
struct BrowsingAids {
    bool caretBrowsing = false, reduceMotion = false, increaseContrast = false;
    int minimumFontSize = 0;
    bool unloadWhenMemoryIsLow = true;
    bool overrideColors = false;
};

// Preferences › Browsing (#66, #67, #70): media that play by themselves,
// spell checking, and saved passwords (PasswordStore.h) with import and
// export, and Summit's passkeys (#111), which can be removed. Changes go to the application as kPreferencesChanged.
class BrowsingPreferences : public BGroupView {
public:
    // spellingLanguage: the dictionary chosen, empty for the system's
    // language; dictionaries: those installed.
    BrowsingPreferences(BMessenger owner, const std::string& autoplay, bool spellChecking, const std::string& spellingLanguage,
        const std::vector<std::string>& dictionaries, bool savePasswords,
        const std::vector<std::tuple<std::string, std::string, std::string>>& webApps, const BrowsingAids& aids);
    ~BrowsingPreferences() override;
    // The application's kPreferencesState: settings changed elsewhere (View menu).
    void ShowState(const BMessage& state);
    void AttachedToWindow() override;
    void MessageReceived(BMessage* message) override;
private:
    void ShowAccounts();
    void Send(const char* name, bool value);
    BMessenger fOwner;
    BMenuField* fAutoplay;
    BCheckBox* fSpelling;
    BMenuField* fSpellingLanguage;
    BStringView* fSpellingInfo;
    BCheckBox* fSavePasswords;
    BColumnListView* fAccounts;
    BButton* fShow;
    BButton* fRemove;
    BColumnListView* fApps;
    BButton* fRemoveApp;
    BCheckBox* fCaretBrowsing;
    BCheckBox* fReduceMotion;
    BCheckBox* fIncreaseContrast;
    BCheckBox* fOverrideColors;
    BMenuField* fMinimumFontSize;
    BCheckBox* fUnload;
    std::vector<std::string> fAppIDs;
    // The list's passkey rows (PasskeyStore): row, site and credential.
    struct PasskeyRow {
        const void* row;
        std::string site, credential, account;
    };
    std::vector<PasskeyRow> fPasskeyRows;
    std::unique_ptr<BFilePanel> fPanel;
};
}

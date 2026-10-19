#pragma once
#include <GroupView.h>
#include <Messenger.h>
#include <memory>
#include <string>

class BCheckBox;
class BColumnListView;
class BFilePanel;
class BMenuField;
class BButton;
class BStringView;

namespace summit {
// Preferences › Browsing (#66, #67, #70): media that play by themselves,
// spell checking, and saved passwords (PasswordStore.h) with import and
// export. Changes go to the application as kPreferencesChanged.
class BrowsingPreferences : public BGroupView {
public:
    BrowsingPreferences(BMessenger owner, const std::string& autoplay, bool spellChecking, bool savePasswords);
    ~BrowsingPreferences() override;
    void AttachedToWindow() override;
    void MessageReceived(BMessage* message) override;
private:
    void ShowAccounts();
    void Send(const char* name, bool value);
    BMessenger fOwner;
    BMenuField* fAutoplay;
    BCheckBox* fSpelling;
    BStringView* fSpellingInfo;
    BCheckBox* fSavePasswords;
    BColumnListView* fAccounts;
    BButton* fShow;
    BButton* fRemove;
    std::unique_ptr<BFilePanel> fPanel;
};
}

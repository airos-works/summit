#pragma once
#if SUMMIT_MODERN_WEBKIT
#include <Window.h>
#include <Messenger.h>
#include <atomic>
#include <map>
#include <memory>
#include <string>

class BBitmap;
class BButton;
class BCheckBox;
class BFilePanel;
class BGroupView;
class BListView;
class BStringView;
class BTextView;
class BTextControl;
namespace summit {
class ExtensionManager final : public BWindow {
public:
    ExtensionManager(BMessenger owner, std::shared_ptr<std::atomic<unsigned>> windows);
    ~ExtensionManager() override;
    bool QuitRequested() override;
    void MessageReceived(BMessage*) override;
private:
    void Render();
    void Send(BMessage&);
    void Apply(const BMessage&);
    BMessenger fOwner;
    std::shared_ptr<std::atomic<unsigned>> fWindows;
    std::unique_ptr<BFilePanel> fPanel;
    BListView* fList;
    BStringView* fHeading;
    BTextView* fDetails;
    BStringView* fStatus;
    BButton *fAdd, *fToggle, *fRemove, *fInstall, *fCancel;
    BButton* fStoreInstall;
    BTextControl* fStoreURL;
    BCheckBox* fFiles;
    // Of the selected extension: its toolbar button, and private windows.
    BCheckBox* fPinned;
    BCheckBox* fPrivate;
    BGroupView* fApproval;
    std::string fDraftName, fDraftBody, fStatusText;
    // Icons of the listed extensions, by identifier and package.
    std::map<std::string, std::shared_ptr<BBitmap>> fIcons;
    // An extension to select once the list has it (Manage Extension).
    std::string fSelect;
    uint64 fGeneration = 0;
    bool fBusy = false, fReady = false, fApprovalReady = false;
};
}
#endif

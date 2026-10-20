#pragma once
#if SUMMIT_MODERN_WEBKIT
#include "core/ExtensionShortcut.h"
#include <Messenger.h>
#include <Window.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

class BButton;
class BColumnListView;
class BStringView;
namespace summit {
// Extensions › Keyboard Shortcuts…: every command of the running extensions
// with its keys, which the user can change, clear or set back to the
// extension's own. The engine keeps them with the extension's state, as
// commands.update() does, so they survive a restart.
class ExtensionShortcutsWindow final : public BWindow {
public:
    ExtensionShortcutsWindow(BMessenger owner, std::shared_ptr<std::atomic<unsigned>> windows);
    ~ExtensionShortcutsWindow() override;
    bool QuitRequested() override;
    void DispatchMessage(BMessage*, BHandler*) override;
    void MessageReceived(BMessage*) override;
private:
    void Refresh();
    void Render();
    void Capture(const BMessage& keyDown);
    void Send(BMessage&);
    const ExtensionCommandBinding* Selected() const;
    BMessenger fOwner;
    std::shared_ptr<std::atomic<unsigned>> fWindows;
    BColumnListView* fList;
    BButton *fChange, *fClear, *fReset;
    BStringView* fStatus;
    std::vector<ExtensionCommandBinding> fBindings;
    // Waiting for the new keys of the selected command.
    bool fCapturing = false;
};
}
#endif

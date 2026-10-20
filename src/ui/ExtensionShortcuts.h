#pragma once
#if SUMMIT_MODERN_WEBKIT
#include "core/ExtensionShortcut.h"
#include <SupportDefs.h>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class BMenu;
class BMessage;
namespace summit {
// The keyboard shortcuts of the loaded extensions' commands, for every
// window: the application sets them from the engine, windows look up the
// keys pressed in them. Any thread.
class ExtensionShortcuts final {
public:
    static ExtensionShortcuts& Default();
    // The engine's commands, earliest installed extension first.
    void Set(std::vector<ExtensionCommandBinding>);
    std::vector<ExtensionCommandBinding> Bindings() const;
    // The command a B_KEY_DOWN runs, if any.
    std::optional<ExtensionCommandBinding> Find(const BMessage& keyDown) const;
    // Summit's own shortcuts and what they do (a browser window's menus):
    // they win over extensions'.
    void SetBrowserShortcuts(std::vector<std::pair<ExtensionShortcutKeys, std::string>>);
    std::string BrowserUses(const ExtensionShortcutKeys&) const;
    // The keys as this keyboard names them ("Shift+Ctrl+D"), and the names
    // of its Command and Control keys.
    static std::string Label(const ExtensionShortcutKeys&);
    static std::string CommandKeyName();
    static std::string ControlKeyName();
private:
    void Resolve();
    mutable std::mutex fLock;
    std::vector<ExtensionCommandBinding> fBindings;
    std::vector<std::pair<ExtensionShortcutKeys, std::string>> fBrowser;
};
// The keys of a B_KEY_DOWN, when they can be an extension command's.
std::optional<ExtensionShortcutKeys> ExtensionShortcutKeysFor(const BMessage& keyDown);
// A menu's shortcuts (with B_COMMAND_KEY, as windows add them) and their
// items' labels, with those of its submenus.
void CollectMenuShortcuts(BMenu*, std::vector<std::pair<ExtensionShortcutKeys, std::string>>&);
}
#endif

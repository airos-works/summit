#include "ExtensionShortcuts.h"
#if SUMMIT_MODERN_WEBKIT
#include <InterfaceDefs.h>
#include <Menu.h>
#include <MenuItem.h>
#include <Message.h>
#include <cctype>

namespace summit {
ExtensionShortcuts& ExtensionShortcuts::Default()
{
    static ExtensionShortcuts shortcuts;
    return shortcuts;
}
void ExtensionShortcuts::Set(std::vector<ExtensionCommandBinding> bindings)
{
    std::lock_guard lock(fLock);
    fBindings = std::move(bindings);
    Resolve();
}
std::vector<ExtensionCommandBinding> ExtensionShortcuts::Bindings() const
{
    std::lock_guard lock(fLock);
    return fBindings;
}
void ExtensionShortcuts::SetBrowserShortcuts(std::vector<std::pair<ExtensionShortcutKeys, std::string>> shortcuts)
{
    std::lock_guard lock(fLock);
    fBrowser = std::move(shortcuts);
    Resolve();
}
std::string ExtensionShortcuts::BrowserUses(const ExtensionShortcutKeys& keys) const
{
    std::lock_guard lock(fLock);
    for (const auto& [browser, label] : fBrowser)
        if (browser == keys) return label;
    return {};
}
void ExtensionShortcuts::Resolve()
{
    ResolveExtensionShortcuts(fBindings, [this](const ExtensionShortcutKeys& keys) {
        for (const auto& [browser, label] : fBrowser)
            if (browser == keys) return label;
        return std::string();
    });
}
std::optional<ExtensionCommandBinding> ExtensionShortcuts::Find(const BMessage& keyDown) const
{
    const auto keys = ExtensionShortcutKeysFor(keyDown);
    if (!keys) return std::nullopt;
    std::lock_guard lock(fLock);
    if (const auto* binding = FindExtensionShortcut(fBindings, *keys)) return *binding;
    return std::nullopt;
}
// Haiku's keyboard mode has Command on Alt and Control on Ctrl; the
// Windows/Linux mode swaps them (Keymap preferences).
static std::string KeyName(uint32 modifier, const char* fallback)
{
    uint32 key = 0;
    get_modifier_key(modifier, &key);
    return key == 0x5c ? std::string("Ctrl") : key == 0x5d ? std::string("Alt") : key == 0x66 ? std::string("Win") : std::string(fallback);
}
std::string ExtensionShortcuts::CommandKeyName() { return KeyName(B_LEFT_COMMAND_KEY, "Alt"); }
std::string ExtensionShortcuts::ControlKeyName() { return KeyName(B_LEFT_CONTROL_KEY, "Ctrl"); }
std::string ExtensionShortcuts::Label(const ExtensionShortcutKeys& keys)
{
    return ExtensionShortcutLabel(keys, CommandKeyName(), ControlKeyName());
}

static unsigned ShortcutModifiers(uint32 modifiers)
{
    unsigned result = 0;
    if (modifiers & B_COMMAND_KEY) result |= ExtensionShortcutKeys::Command;
    if (modifiers & B_CONTROL_KEY) result |= ExtensionShortcutKeys::Control;
    if (modifiers & B_SHIFT_KEY) result |= ExtensionShortcutKeys::Shift;
    return result;
}
static unsigned ShortcutKey(uint32 character)
{
    switch (character) {
        case B_INSERT: return ExtensionShortcutKeys::Insert;
        case B_DELETE: return ExtensionShortcutKeys::Delete;
        case B_HOME: return ExtensionShortcutKeys::Home;
        case B_END: return ExtensionShortcutKeys::End;
        case B_PAGE_UP: return ExtensionShortcutKeys::PageUp;
        case B_PAGE_DOWN: return ExtensionShortcutKeys::PageDown;
        case B_UP_ARROW: return ExtensionShortcutKeys::Up;
        case B_DOWN_ARROW: return ExtensionShortcutKeys::Down;
        case B_LEFT_ARROW: return ExtensionShortcutKeys::Left;
        case B_RIGHT_ARROW: return ExtensionShortcutKeys::Right;
        case ',': case '.': case ' ': return character;
    }
    if (character < 128 && std::isalnum(static_cast<unsigned char>(character)))
        return static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(character)));
    return 0;
}
std::optional<ExtensionShortcutKeys> ExtensionShortcutKeysFor(const BMessage& keyDown)
{
    const uint32 modifiers = keyDown.GetInt32("modifiers", 0);
    // The Option (Windows) key is none of the keys extensions name.
    if (modifiers & B_OPTION_KEY) return std::nullopt;
    const char* bytes = keyDown.GetString("bytes", "");
    ExtensionShortcutKeys keys;
    keys.modifiers = ShortcutModifiers(modifiers);
    if (bytes[0] == B_FUNCTION_KEY) {
        const int32 key = keyDown.GetInt32("key", 0);
        if (key < B_F1_KEY || key > B_F12_KEY) return std::nullopt;
        keys.key = ExtensionShortcutKeys::F1 + (key - B_F1_KEY);
    } else {
        // The key's own character, which modifiers do not change.
        keys.key = ShortcutKey(keyDown.GetInt32("raw_char", static_cast<unsigned char>(bytes[0])));
    }
    if (!keys.key || ExtensionShortcutText(keys).empty()) return std::nullopt;
    return keys;
}
void CollectMenuShortcuts(BMenu* menu, std::vector<std::pair<ExtensionShortcutKeys, std::string>>& shortcuts)
{
    if (!menu) return;
    for (int32 i = 0; auto* item = menu->ItemAt(i); ++i) {
        if (auto* submenu = item->Submenu()) {
            CollectMenuShortcuts(submenu, shortcuts);
            continue;
        }
        uint32 modifiers = 0;
        const char character = item->Shortcut(&modifiers);
        if (!character) continue;
        ExtensionShortcutKeys keys { ShortcutKey(static_cast<unsigned char>(character)), ShortcutModifiers(modifiers | B_COMMAND_KEY) };
        if (keys.key) shortcuts.emplace_back(keys, item->Label() ? item->Label() : "");
    }
}
}
#endif

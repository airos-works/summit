#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace summit {
// The keys of an extension command (manifest "commands"). The engine names
// them as Chrome does ("Ctrl+Shift+Y", BWebKitContext::GetExtensionCommands).
// Ctrl is Summit's shortcut key, Command, as the browser's own Ctrl shortcuts
// are (New Tab is Command-T), and Alt is the other one, Control.
struct ExtensionShortcutKeys {
    enum Modifier : unsigned { Command = 1, Control = 2, Shift = 4 };
    // A lower-case letter or digit, ',', '.' or ' ', or one of these.
    enum Key : unsigned {
        F1 = 0x100, // to F19 = 0x112
        Insert = 0x200, Delete, Home, End, PageUp, PageDown, Up, Down, Left, Right,
        MediaNextTrack, MediaPlayPause, MediaPrevTrack, MediaStop
    };
    unsigned key = 0;
    unsigned modifiers = 0;
    bool operator==(const ExtensionShortcutKeys& other) const { return key == other.key && modifiers == other.modifiers; }
    bool operator!=(const ExtensionShortcutKeys& other) const { return !(*this == other); }
};
// Keys from the engine's text, or nothing when it is no valid shortcut.
std::optional<ExtensionShortcutKeys> ParseExtensionShortcut(const std::string&);
// The engine's text for keys (commands.update()); empty when they cannot be
// a shortcut: letters, digits and the like need Ctrl or Alt, and at most two
// modifiers go with a key.
std::string ExtensionShortcutText(const ExtensionShortcutKeys&);
// The keys as the user presses them: commandName and controlName name the
// keyboard's keys for Command and Control ("Alt" and "Ctrl" in Haiku's
// keyboard mode).
std::string ExtensionShortcutLabel(const ExtensionShortcutKeys&, const std::string& commandName, const std::string& controlName);

// One extension command and the keys it ends up with.
struct ExtensionCommandBinding {
    std::string extension, extensionName, command, description;
    // The engine's text of its keys now, and of the manifest's.
    std::string shortcut, suggested;
    std::optional<ExtensionShortcutKeys> keys;
    // Why its keys do nothing (another extension or the browser has them);
    // empty when they work.
    std::string conflict;
};
// Gives each command its keys, in order: the browser's own shortcuts come
// first (browserUses names the browser command with those keys, or is empty),
// then the commands earlier in the list.
void ResolveExtensionShortcuts(std::vector<ExtensionCommandBinding>&,
    const std::function<std::string(const ExtensionShortcutKeys&)>& browserUses);
// The command those keys run, or nullptr.
const ExtensionCommandBinding* FindExtensionShortcut(const std::vector<ExtensionCommandBinding>&, const ExtensionShortcutKeys&);
}

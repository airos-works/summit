#include "ExtensionShortcut.h"
#include <cctype>
#include <cstring>

namespace summit {
namespace {
struct NamedKey { const char* name; unsigned key; };
constexpr NamedKey namedKeys[] = {
    { "Comma", ',' }, { "Period", '.' }, { "Space", ' ' },
    { "Insert", ExtensionShortcutKeys::Insert }, { "Delete", ExtensionShortcutKeys::Delete },
    { "Home", ExtensionShortcutKeys::Home }, { "End", ExtensionShortcutKeys::End },
    { "PageUp", ExtensionShortcutKeys::PageUp }, { "PageDown", ExtensionShortcutKeys::PageDown },
    { "Up", ExtensionShortcutKeys::Up }, { "Down", ExtensionShortcutKeys::Down },
    { "Left", ExtensionShortcutKeys::Left }, { "Right", ExtensionShortcutKeys::Right },
    { "MediaNextTrack", ExtensionShortcutKeys::MediaNextTrack }, { "MediaPlayPause", ExtensionShortcutKeys::MediaPlayPause },
    { "MediaPrevTrack", ExtensionShortcutKeys::MediaPrevTrack }, { "MediaStop", ExtensionShortcutKeys::MediaStop },
};
bool isMedia(unsigned key) { return key >= ExtensionShortcutKeys::MediaNextTrack && key <= ExtensionShortcutKeys::MediaStop; }
bool isFunction(unsigned key) { return key >= ExtensionShortcutKeys::F1 && key <= ExtensionShortcutKeys::F1 + 18; }
std::string keyName(unsigned key)
{
    if (isFunction(key)) return "F" + std::to_string(key - ExtensionShortcutKeys::F1 + 1);
    for (const auto& named : namedKeys)
        if (named.key == key) return named.name;
    if (key < 128 && std::isalnum(static_cast<unsigned char>(key))) return std::string(1, char(std::toupper(static_cast<unsigned char>(key))));
    return {};
}
bool valid(const ExtensionShortcutKeys& keys)
{
    if (keyName(keys.key).empty() || (keys.modifiers & ~7u)) return false;
    if (isMedia(keys.key)) return !keys.modifiers;
    const unsigned count = !!(keys.modifiers & ExtensionShortcutKeys::Command) + !!(keys.modifiers & ExtensionShortcutKeys::Control)
        + !!(keys.modifiers & ExtensionShortcutKeys::Shift);
    if (count > 2) return false;
    return isFunction(keys.key) || (keys.modifiers & (ExtensionShortcutKeys::Command | ExtensionShortcutKeys::Control));
}
}

std::optional<ExtensionShortcutKeys> ParseExtensionShortcut(const std::string& text)
{
    if (text.empty() || text.size() > 64) return std::nullopt;
    std::vector<std::string> parts;
    for (size_t start = 0;;) {
        const auto plus = text.find('+', start);
        parts.push_back(text.substr(start, plus == std::string::npos ? plus : plus - start));
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    ExtensionShortcutKeys keys;
    const auto& name = parts.back();
    if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0])))
        keys.key = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(name[0])));
    else if (name.size() >= 2 && name.size() <= 3 && name[0] == 'F' && std::isdigit(static_cast<unsigned char>(name[1]))
        && (name.size() == 2 || std::isdigit(static_cast<unsigned char>(name[2]))) && name[1] != '0') {
        const int number = std::stoi(name.substr(1));
        if (number < 1 || number > 19) return std::nullopt;
        keys.key = ExtensionShortcutKeys::F1 + number - 1;
    } else {
        for (const auto& named : namedKeys)
            if (name == named.name) keys.key = named.key;
        if (!keys.key) return std::nullopt;
    }
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        unsigned modifier = 0;
        if (parts[i] == "Ctrl") modifier = ExtensionShortcutKeys::Command;
        else if (parts[i] == "Alt") modifier = ExtensionShortcutKeys::Control;
        else if (parts[i] == "Shift") modifier = ExtensionShortcutKeys::Shift;
        if (!modifier || (keys.modifiers & modifier)) return std::nullopt;
        keys.modifiers |= modifier;
    }
    if (!valid(keys)) return std::nullopt;
    return keys;
}

std::string ExtensionShortcutText(const ExtensionShortcutKeys& keys)
{
    if (!valid(keys)) return {};
    std::string text;
    if (keys.modifiers & ExtensionShortcutKeys::Command) text += "Ctrl+";
    if (keys.modifiers & ExtensionShortcutKeys::Control) text += "Alt+";
    if (keys.modifiers & ExtensionShortcutKeys::Shift) text += "Shift+";
    return text + keyName(keys.key);
}

std::string ExtensionShortcutLabel(const ExtensionShortcutKeys& keys, const std::string& commandName, const std::string& controlName)
{
    if (!valid(keys)) return {};
    std::string text;
    // In the order Haiku's menus show them.
    if (keys.modifiers & ExtensionShortcutKeys::Shift) text += "Shift+";
    if (keys.modifiers & ExtensionShortcutKeys::Control) text += controlName + "+";
    if (keys.modifiers & ExtensionShortcutKeys::Command) text += commandName + "+";
    return text + keyName(keys.key);
}

void ResolveExtensionShortcuts(std::vector<ExtensionCommandBinding>& bindings,
    const std::function<std::string(const ExtensionShortcutKeys&)>& browserUses)
{
    for (size_t i = 0; i < bindings.size(); ++i) {
        auto& binding = bindings[i];
        binding.keys = binding.shortcut.empty() ? std::nullopt : ParseExtensionShortcut(binding.shortcut);
        binding.conflict.clear();
        if (!binding.keys) continue;
        if (auto browser = browserUses ? browserUses(*binding.keys) : std::string(); !browser.empty()) {
            binding.conflict = "Summit uses these keys for " + browser + ".";
            continue;
        }
        for (size_t j = 0; j < i; ++j) {
            const auto& earlier = bindings[j];
            if (earlier.keys && earlier.conflict.empty() && *earlier.keys == *binding.keys) {
                binding.conflict = (earlier.extension == binding.extension ? std::string("Another of its commands") : earlier.extensionName)
                    + " uses these keys.";
                break;
            }
        }
    }
}

const ExtensionCommandBinding* FindExtensionShortcut(const std::vector<ExtensionCommandBinding>& bindings, const ExtensionShortcutKeys& keys)
{
    for (const auto& binding : bindings)
        if (binding.keys && binding.conflict.empty() && *binding.keys == keys) return &binding;
    return nullptr;
}
}

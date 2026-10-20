#include "core/ExtensionShortcut.h"
#include "core/ExtensionUpdate.h"
#include <iostream>

static unsigned checks, failures;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::cerr << __LINE__ << ": " #value "\n"; } } while (0)
int main()
{
    using namespace summit;
    using Keys = ExtensionShortcutKeys;
    // Ctrl is Summit's shortcut key (Command), Alt the Control key.
    auto keys = ParseExtensionShortcut("Ctrl+Shift+Y");
    CHECK(keys && keys->key == 'y' && keys->modifiers == (Keys::Command | Keys::Shift));
    keys = ParseExtensionShortcut("Alt+Shift+D");
    CHECK(keys && keys->key == 'd' && keys->modifiers == (Keys::Control | Keys::Shift));
    CHECK(ExtensionShortcutText(*keys) == "Alt+Shift+D");
    CHECK(ExtensionShortcutLabel(*keys, "Alt", "Ctrl") == "Shift+Ctrl+D");
    CHECK(ExtensionShortcutLabel(*ParseExtensionShortcut("Ctrl+Period"), "Alt", "Ctrl") == "Alt+Period");
    keys = ParseExtensionShortcut("F5");
    CHECK(keys && keys->key == Keys::F1 + 4 && !keys->modifiers && ExtensionShortcutText(*keys) == "F5");
    CHECK(ParseExtensionShortcut("Ctrl+F19") && !ParseExtensionShortcut("F20") && !ParseExtensionShortcut("F0"));
    CHECK(ParseExtensionShortcut("Ctrl+Alt+Left") && ExtensionShortcutText(*ParseExtensionShortcut("Ctrl+Alt+Left")) == "Ctrl+Alt+Left");
    CHECK(ParseExtensionShortcut("MediaPlayPause") && !ParseExtensionShortcut("Ctrl+MediaPlayPause"));
    // Letters need Ctrl or Alt; three modifiers, doubled or unknown ones fail.
    CHECK(!ParseExtensionShortcut("Shift+Y") && !ParseExtensionShortcut("Y") && !ParseExtensionShortcut(""));
    CHECK(!ParseExtensionShortcut("Ctrl+Alt+Shift+Y") && !ParseExtensionShortcut("Ctrl+Ctrl+Y"));
    CHECK(!ParseExtensionShortcut("Command+Y") && !ParseExtensionShortcut("Ctrl+") && !ParseExtensionShortcut("Ctrl+Tab"));
    CHECK(ExtensionShortcutText(Keys { 'y', Keys::Shift }).empty());
    CHECK(ExtensionShortcutText(Keys { ',', Keys::Command }) == "Ctrl+Comma");

    std::vector<ExtensionCommandBinding> bindings {
        { "a", "Alpha", "_execute_action", "Open", "Ctrl+Shift+Y", "Ctrl+Shift+Y", {}, {} },
        { "a", "Alpha", "fill", "Fill", "Ctrl+T", "", {}, {} },
        { "b", "Beta", "toggle", "Toggle", "Ctrl+Shift+Y", "", {}, {} },
        { "b", "Beta", "other", "Other", "", "", {}, {} },
        { "b", "Beta", "broken", "Broken", "Shift+Q", "", {}, {} },
    };
    ResolveExtensionShortcuts(bindings, [](const Keys& keys) {
        return keys == Keys { 't', Keys::Command } ? std::string("New Tab") : std::string();
    });
    CHECK(bindings[0].conflict.empty() && bindings[0].keys);
    CHECK(bindings[1].conflict == "Summit uses these keys for New Tab.");
    CHECK(bindings[2].conflict == "Alpha uses these keys.");
    CHECK(!bindings[3].keys && bindings[3].conflict.empty());
    CHECK(!bindings[4].keys);
    auto* found = FindExtensionShortcut(bindings, Keys { 'y', Keys::Command | Keys::Shift });
    CHECK(found && found->extension == "a" && found->command == "_execute_action");
    CHECK(!FindExtensionShortcut(bindings, Keys { 't', Keys::Command }));

    // Update sources and versions.
    const std::string chrome = "eimadpbcbfnmbkopoojfekhnkhdbieeh";
    nlohmann::json manifest = {{"update_url", "https://clients2.google.com/service/update2/crx"}};
    CHECK(ExtensionUpdateSource(manifest, chrome, chrome) == "chrome");
    CHECK(ExtensionUpdateSource(manifest, chrome, "").empty());
    CHECK(ExtensionUpdateSource(nlohmann::json::object(), chrome, chrome).empty());
    manifest = {{"browser_specific_settings", {{"gecko", {{"id", "addon@darkreader.org"}}}}}};
    CHECK(ExtensionUpdateSource(manifest, "addon@darkreader.org", "") == "firefox");
    CHECK(ExtensionUpdateSource(manifest, "summit-pkg-abcdef", "").empty());
    CHECK(CompareExtensionVersions("4.9.110", "4.9.99") > 0 && CompareExtensionVersions("1.62.0", "1.62") == 0);
    CHECK(CompareExtensionVersions("1.0a1", "1.0") < 0 && CompareExtensionVersions("2", "10") < 0);
    CHECK(CompareExtensionVersions("1.2.3", "1.2.3") == 0 && CompareExtensionVersions("1.10", "1.9") > 0);
    CHECK(ChromeUpdateCheckURL(chrome, "4.9.1").find("x=id%3D" + chrome + "%26v%3D4.9.1%26uc") != std::string::npos);
    const std::string response = "<?xml version='1.0' encoding='UTF-8'?><gupdate xmlns='http://www.google.com/update2/response' protocol='2.0'>"
        "<app appid='other' status='ok'><updatecheck status='ok' version='9'/></app>"
        "<app appid='" + chrome + "' status='ok'><updatecheck codebase='https://clients2.googleusercontent.com/crx/blobs/x.crx' "
        "fp='1.x' hash_sha256='ab' size='851022' status='ok' version='4.9.110'/></app></gupdate>";
    CHECK(ChromeUpdateCheckVersion(response, chrome) == std::string("4.9.110"));
    CHECK(!ChromeUpdateCheckVersion("<gupdate><app appid=\"" + chrome + "\" status=\"ok\"><updatecheck status=\"noupdate\"/></app></gupdate>", chrome));
    CHECK(!ChromeUpdateCheckVersion(response, "nothere"));
    CHECK(FirefoxAddonURL("uBlock0@raymondhill.net") == "https://addons.mozilla.org/api/v5/addons/addon/uBlock0%40raymondhill.net/");
    std::cout << checks - failures << "/" << checks << " extension shortcut and update checks passed\n";
    return failures ? 1 : 0;
}

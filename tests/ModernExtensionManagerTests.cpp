// Exercise actual native controls and the real installer/extension runtime.
#define main SummitCloseHarnessEntry
#include "ModernCloseTests.cpp"
#undef main
#include <Bitmap.h>
#include <Screen.h>

static BMessenger NamedWindow(const BMessenger& app, const char* title)
{
    for (int32 i = 0; i < 16; ++i) {
        auto candidate = Window(app, i);
        if (!candidate.IsValid()) break;
        if (String(Property(candidate, "Title"), "result") == title) return candidate;
    }
    return {};
}
static BMessenger Manager(const BMessenger& app) { return NamedWindow(app, "Extensions — Summit"); }
static BMessenger Picker(const BMessenger& app) { return NamedWindow(app, "Choose extension package — Summit"); }
static json ReadJSON(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) return json();
    return json::parse(input, nullptr, false);
}
static int Entries(const BMessenger& manager)
{
    return Property(View(manager, "extension-list"), "Item", B_COUNT_PROPERTIES).GetInt32("result", -1);
}
static bool Enabled(const BMessenger& manager, const char* name)
{
    return Property(View(manager, name), "Enabled").GetBool("result", false);
}
static std::string Text(const BMessenger& manager, const char* name)
{
    const auto view = View(manager, name);
    if (std::string(name) == "extension-details") {
        // BTextView exposes Text through a byte range, unlike BStringView.
        const auto length = Property(view, "Text", B_COUNT_PROPERTIES).GetInt32("result", -1);
        Require(length >= 0 && length <= 65536, "native permission/detail text has a bounded byte length");
        BMessage request(B_GET_PROPERTY);
        request.AddSpecifier("Text", int32(0), length);
        return String(Query(view, request), "result");
    }
    return String(Property(view, "Text"), "result");
}
static void Button(const BMessenger& manager, const char* name)
{
    Require(Wait([&] { return Enabled(manager, name); }), std::string("native control enabled: ") + name);
    Key(View(manager, name), " ", 0x5e);
}

static BMessenger ActionPopup(const BMessenger& app)
{
    for (int32 i = 0; i < 16; ++i) {
        auto window = Window(app, i);
        if (!window.IsValid()) break;
        if (View(window, "extension-popup-content").IsValid()) return window;
    }
    return {};
}
static void FocusWindow(const BMessenger& window)
{
    BMessage activate(B_SET_PROPERTY);
    activate.AddSpecifier("Active");
    activate.AddBool("data", true);
    Require(Query(window, activate).GetInt32("error", B_ERROR) == B_OK, "activate real native browser window");
    Require(Wait([&] { return Property(window, "Active").GetBool("result", false); }), "browser owns native input focus");
}
static BMessage ActionState(const BMessenger& window)
{
    auto state = State(window);
    BMessage action;
    if (state.GetUInt64("extension_action_snapshot", 0) && state.FindMessage("extension_action", &action) == B_OK)
        action.AddUInt64("snapshot", state.GetUInt64("extension_action_snapshot", 0));
    return action;
}

static void BadgeColors(const BMessenger& browser, uint32 background, uint32 foreground)
{
    Require(Wait([&] { const auto action = ActionState(browser);
        return action.GetUInt32("badge_background_rgba", 0) == background
            && action.GetUInt32("badge_text_rgba", 0) == foreground;
    }), "effective badge colors reach the native toolbar snapshot");
}

static void BadgePixels(const BMessenger& browser, const char* name)
{
    auto target = View(browser, name);
    BRect windowFrame;
    Require(Property(browser, "Frame").FindRect("result", &windowFrame) == B_OK, "browser has native screen bounds for badge capture");
    BRect frame;
    std::function<bool(const BMessenger&, BPoint, int)> find = [&](const BMessenger& parent, BPoint origin, int depth) {
        if (depth > 12) return false;
        const auto count = Property(parent, "View", B_COUNT_PROPERTIES).GetInt32("result", 0);
        for (int32 i = 0; i < count; ++i) {
            auto child = Child(parent, i);
            BRect local;
            if (Property(child, "Frame").FindRect("result", &local) != B_OK) continue;
            local.OffsetBy(origin);
            if (child == target) { frame = local; return true; }
            if (find(child, local.LeftTop(), depth + 1)) return true;
        }
        return false;
    };
    Require(find(browser, windowFrame.LeftTop(), 0), "locate actual badge control through native view hierarchy");
    BScreen screen;
    Require(frame.Width() >= 12 && frame.Height() >= 13 && screen.Frame().Contains(frame),
        "badge capture has valid dimensions and lies inside the native screen");
    BBitmap bitmap(BRect(0, 0, frame.Width(), frame.Height()), B_RGBA32);
    Require(Wait([&] {
        if (bitmap.InitCheck() != B_OK || screen.ReadBitmap(&bitmap, false, &frame) != B_OK) return false;
        bool background = false, foreground = false;
        for (int y = 2; y <= 12; ++y) for (int x = int(frame.Width()) - 11; x < int(frame.Width()); ++x) {
            auto* pixel = static_cast<const uint8*>(bitmap.Bits()) + y * bitmap.BytesPerRow() + x * 4;
            background |= pixel[2] == 24 && pixel[1] == 72 && pixel[0] == 120;
            foreground |= pixel[2] > 200 && pixel[1] > 170 && pixel[0] > 90 && pixel[0] < 180;
        }
        return background && foreground;
    }), "actual toolbar pixels render the extension's blue badge and yellow text");
}

#ifndef SUMMIT_EXTENSION_MANAGER_HELPERS_ONLY
int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--check-executable-unused") return SummitCloseHarnessEntry(argc, argv);
    if (argc != 9 && argc != 10) return 2;
    const bool watch = argc == 10 && std::string(argv[9]) == "watch-actions";
    const bool actions = watch || (argc == 10 && std::string(argv[9]) == "actions");
    status_t status;
    BApplication application("application/x-vnd.Kunanyi-Summit-extension-manager-tests", &status);
    if (status != B_OK) return 1;
    const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    const std::filesystem::path profile(argv[3]), package(argv[4]), observations(argv[5]);
    const std::string nonce(argv[6]), phase(argv[7]), identity(argv[8]);
    const auto catalogPath = profile / "Extensions/catalog.json";
    BMessenger app, manager;
    bool verified = false;
    try {
        app_info info;
        Require(Wait([&] {
            app = BMessenger(nullptr, team);
            return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK;
        }), "owned browser registers with native roster");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "browser executable matches frozen bundle");
        verified = true;
        const auto reports = [&]() {
            auto value = ReadJSON(observations);
            json result = json::array();
            if (value.is_object() && value.contains("reports"))
                for (const auto& item : value["reports"]) if (!item.contains("kind")) result.push_back(item);
            return result;
        };
        const auto actionEvents = [&]() {
            auto value = ReadJSON(observations);
            json result = json::array();
            if (value.is_object() && value.contains("reports"))
                for (const auto& item : value["reports"]) if (item.contains("kind") && item.value("kind", "") != "badge-colors") result.push_back(item);
            return result;
        };
        const auto count = [&]() { return reports().size(); };
        const auto boot = [&](int number) {
            Require(Wait([&] {
                auto list = reports();
                return list.size() == size_t(number) && list.back().value("boot", 0) == number
                    && list.back().value("nonce", "") == nonce && list.back().value("id", "") == identity
                    && list.back().value("granted", false) && !list.back().value("optionalGranted", true);
            }), "actual background reports boot " + std::to_string(number) + " with approved but no optional grants");
        };
        BMessage openManager(B_EXECUTE_PROPERTY);
        openManager.AddSpecifier("MenuItem", "Extensions…");
        openManager.AddSpecifier("Menu", "Window");
        openManager.AddSpecifier("View", "menu");
        openManager.AddSpecifier("Window", int32(0));
        Require(Query(app, openManager).GetInt32("error", B_ERROR) == B_OK, "open extension manager through browser Window menu");
        Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-add"); }), "native extension manager is ready");
        const auto browser = Window(app, 0);
        const auto actionName = "extension-action-" + identity;
        const auto actionTitle = [&]() { return String(Property(View(browser, actionName.c_str()), "Label"), "result"); };
        const auto actionClick = [&]() {
            FocusWindow(browser);
            Button(browser, actionName.c_str());
        };
        const auto popup = [&](int number) {
            BMessenger window;
            Require(Wait([&] {
                window = ActionPopup(app);
                auto events = actionEvents();
                return window.IsValid() && !Property(window, "Hidden").GetBool("result", true)
                    && !events.empty() && events.back().value("kind", "") == "popup"
                    && events.back().value("count", 0) == number && events.back().value("storedNonce", "") == nonce;
            }), "real popup " + std::to_string(number) + " loads HTML, runs extension JavaScript and shares saved storage");
            BRect frame;
            Require(Property(window, "Frame").FindRect("result", &frame) == B_OK && frame.Width() >= 300 && frame.Width() <= 800
                && frame.Height() >= 175 && frame.Height() <= 600, "native popup is visible at a bounded content size");
            BadgeColors(browser, 0x502878FF, 0xFFFFFFFF);
            if (watch) snooze(2500000);
            return window;
        };
        BMessage originalAction;
        const auto openPicker = [&]() {
            Button(manager, "extension-add");
            BMessenger picker;
            Require(Wait([&] { picker = Picker(app); return picker.IsValid() && !Property(picker, "Hidden").GetBool("result", true)
                && View(picker, "ActualPoseView").IsValid(); }), "Add extension opens native file picker");
            return picker;
        };
        const auto selectPackage = [&](const std::filesystem::path& selectedPath) {
            const auto picker = openPicker();
            auto poses = View(picker, "ActualPoseView");
            BEntry parent(selectedPath.parent_path().c_str()), entry(selectedPath.c_str());
            entry_ref parentRef, ref;
            Require(parent.GetRef(&parentRef) == B_OK && entry.GetRef(&ref) == B_OK, "fixture package and parent have native entry references");
            // Tracker's native PoseView scripting 'sref' specifier opens the
            // directory inside the real picker, just like a folder activation.
            BMessage specifier('sref');
            specifier.AddString("property", "Entry");
            specifier.AddRef("refs", &parentRef);
            BMessage navigate(B_EXECUTE_PROPERTY);
            navigate.AddSpecifier(&specifier);
            Require(Query(poses, navigate).GetInt32("error", B_OK) == B_OK, "navigate native picker to package parent");
            Require(Wait([&] {
                poses = View(picker, "ActualPoseView");
                entry_ref actual;
                return Property(poses, "Path").FindRef("result", &actual) == B_OK && actual == parentRef;
            }), "native picker shows requested package directory");
            Require(Wait([&] {
                BMessage selection(B_SET_PROPERTY);
                selection.AddSpecifier("Selection");
                selection.AddRef("data", &ref);
                Query(poses, selection);
                entry_ref actual;
                return Property(poses, "Selection").FindRef("result", &actual) == B_OK && actual == ref;
            }), "select package in actual native file list");
            Button(picker, "default button");
            Require(Wait([&] { return Property(picker, "Hidden").GetBool("result", false); }), "Review extension accepts picker selection and hides picker");
        };
        const auto catalog = [&]() { return ReadJSON(catalogPath); };
        if (phase == "install") {
            Require(Entries(manager) == 0 && count() == 0, "fresh profile has no installation or extension execution");
            auto picker = openPicker();
            Button(picker, "cancel button");
            Require(Wait([&] { return Property(picker, "Hidden").GetBool("result", false); }) && Enabled(manager, "extension-add")
                && count() == 0 && !std::filesystem::exists(catalogPath), "cancelling file picker leaves importer and profile untouched");
            selectPackage(package.parent_path() / "package.xpi");
            Require(Wait([&] { return Enabled(manager, "extension-install"); }), "package preparation reaches native consent");
            const auto text = Text(manager, "extension-details");
            Require(text.find("storage") != std::string::npos && text.find("http://10.0.2.2/*") != std::string::npos
                && text.find("summitUnknownPermission") != std::string::npos && text.find("tabs") != std::string::npos,
                "consent displays recognized, unknown, host and optional requirements");
            Require(count() == 0 && !std::filesystem::exists(catalogPath), "review does not execute or install the package");
            BMessage stale(summit::kExtensionApprove);
            stale.AddMessenger("window", manager);
            stale.AddUInt64("generation", UINT64_MAX);
            stale.AddBool("allow_files", true);
            Require(app.SendMessage(&stale, static_cast<BHandler*>(nullptr), 500000) == B_OK, "submit stale approval generation");
            snooze(200000);
            Require(Enabled(manager, "extension-install") && count() == 0 && !std::filesystem::exists(catalogPath), "stale approval cannot activate the current package");
            Button(manager, "extension-cancel");
            Require(Wait([&] { return Enabled(manager, "extension-add") && !Enabled(manager, "extension-install"); }), "cancel dismisses consent and releases importer");
            Require(count() == 0 && std::filesystem::is_empty(profile / "Extensions/packages"), "cancelled import leaves no package or execution");
            selectPackage(package);
            Send(manager, B_QUIT_REQUESTED);
            Require(Wait([&] { return !manager.IsValid(); }), "manager can close immediately after selecting a package");
            Send(app, summit::kShowExtensions);
            Require(Wait([&] { manager = Manager(app); return manager.IsValid() && Enabled(manager, "extension-add"); }), "manager close cancels even an import whose first snapshot was pending");
            Require(count() == 0 && std::filesystem::is_empty(profile / "Extensions/packages"), "closing during import releases owned files without running code");
            selectPackage(package.parent_path() / "package.xpi");
            Require(Wait([&] { return Enabled(manager, "extension-install"); }), "XPI import can be reviewed after cancellation");
            Button(manager, "extension-install");
            boot(1);
            Require(Wait([&] { return Entries(manager) == 1 && Enabled(manager, "extension-toggle"); }), "approved extension appears in native manager");
            auto saved = catalog();
            Require(saved.is_object() && saved["extensions"].size() == 1
                && saved["extensions"][0]["identifier"] == identity
                && saved["extensions"][0]["enabled"] == true, "approval commits stable identity and enabled installation");
            Require(saved["extensions"][0]["allow_file_urls"] == false && saved["extensions"][0]["allow_private_browsing"] == false,
                "file and private access stay disabled by default");
            if (actions) {
                FocusWindow(browser);
                Require(Wait([&] { return actionTitle() == "Summit popup ready" && Enabled(browser, actionName.c_str()); }),
                    "live background title and enabled state reach the native toolbar");
                originalAction = ActionState(browser);
                BadgeColors(browser, 0x184878FF, 0xFFE080FF);
                BadgePixels(browser, actionName.c_str());
                const void* pixels = nullptr;
                ssize_t length = 0;
                Require(originalAction.GetUInt64("load_identifier", 0) && originalAction.GetUInt64("page_identifier", 0)
                    && String(originalAction, "badge") == "R" && originalAction.GetBool("has_popup", false),
                    "toolbar snapshot carries actual badge, popup, load and active page identities");
                Require(originalAction.FindData("icon_bgra", B_RAW_TYPE, &pixels, &length) == B_OK && length == 1024
                    && static_cast<const uint8*>(pixels)[(8 * 16 + 8) * 4] == 106
                    && static_cast<const uint8*>(pixels)[(8 * 16 + 8) * 4 + 3] == 255,
                    "toolbar receives decoded extension icon pixels in public BGRA format");
                const auto firstTab = Selected(State(browser));
                actionClick();
                auto firstPopup = popup(1);
                Send(firstPopup, B_QUIT_REQUESTED);
                Require(Wait([&] { return !ActionPopup(app).IsValid(); }), "closing popup destroys its native host");
                actionClick();
                auto secondPopup = popup(2);
                Click(View(secondPopup, "extension-popup-content"), BPoint(100, 128));
                Require(Wait([&] { auto events = actionEvents(); return events.size() == 3
                    && events.back().value("kind", "") == "popup-button" && !ActionPopup(app).IsValid()
                    && actionTitle() == "Summit click ready"; }), "native pointer input executes popup DOM control and changing popup path dismisses it");
                actionClick();
                Require(Wait([&] { auto events = actionEvents(); return events.size() == 4
                    && events.back().value("kind", "") == "clicked" && events.back().value("click", 0) == 1
                    && events.back().value("tabId", 0) > 0 && events.back().value("url", "").find("/page?run=" + nonce) != std::string::npos
                    && actionTitle() == "Summit clicked tab" && !Enabled(browser, actionName.c_str()); }),
                    "action without a popup dispatches onClicked with the real active tab and applies its disabled state");
                BMessage firstState = ActionState(browser);
                BadgeColors(browser, 0xC03040FF, 0xFFFFFFFF);
                BMessage currentTab;
                State(browser).FindMessage("tab", &currentTab);
                Send(browser, summit::kNewTab, -1, String(currentTab, "url") + "&second=1");
                Require(Wait([&] { return Count(State(browser)) == 2 && Selected(State(browser)) != firstTab
                    && actionTitle() == "Summit click ready" && Enabled(browser, actionName.c_str()); }),
                    "new active tab inherits default action instead of another tab's disabled override");
                const auto secondTab = Selected(State(browser));
                BadgeColors(browser, 0x502878FF, 0xFFFFFFFF);
                auto current = ActionState(browser);
                auto oldTabClick = firstState;
                oldTabClick.what = summit::kActivateExtensionAction;
                oldTabClick.ReplaceUInt64("snapshot", current.GetUInt64("snapshot", 0));
                auto previousResult = State(browser).GetUInt64("extension_action_result_identifier", 0);
                Query(browser, oldTabClick);
                Require(Wait([&] { auto state = State(browser); return state.GetUInt64("extension_action_result_identifier", 0) > previousResult
                    && state.GetInt32("extension_action_result_error", B_OK) == B_CANCELED; }), "SDK rejects old-page activation and refreshes controls");
                Require(actionEvents().size() == 4 && !ActionPopup(app).IsValid(), "old-page action grants no invocation to the newly selected tab");
                actionClick();
                Require(Wait([&] { auto events = actionEvents(); return events.size() == 5
                    && events.back().value("kind", "") == "clicked" && events.back().value("click", 0) == 2
                    && actionTitle() == "Summit popup again"; }), "second tab's real action can restore its popup");
                actionClick();
                popup(3);
                SelectTab(browser, firstTab);
                Require(Wait([&] { return !ActionPopup(app).IsValid() && actionTitle() == "Summit clicked tab"
                    && !Enabled(browser, actionName.c_str()); }), "tab switch closes popup and restores the first tab's title and disabled state");
                BadgeColors(browser, 0xC03040FF, 0xFFFFFFFF);
                SelectTab(browser, secondTab);
                actionClick();
                auto fourthPopup = popup(4);
                Send(fourthPopup, B_QUIT_REQUESTED);
                Require(Wait([&] { return !ActionPopup(app).IsValid(); }), "reopened popup closes cleanly");
                SelectTab(browser, firstTab);
                Send(browser, summit::kNavigate, -1, String(currentTab, "url") + "&badge-reset=1");
                Require(Wait([&] { return actionTitle() == "Summit popup again" && Enabled(browser, actionName.c_str()); }),
                    "committed navigation clears the original tab's action overrides");
                BadgeColors(browser, 0x502878FF, 0xFFFFFFFF);
                SelectTab(browser, secondTab);
                FocusWindow(manager);
            }
            selectPackage(package);
            Require(Wait([&] { return Enabled(manager, "extension-add") && Text(manager, "extension-status").find("already installed") != std::string::npos; }),
                "duplicate declared identity is rejected before consent or activation");
            Require(count() == 1 && catalog() == saved, "duplicate import preserves runtime and saved installation");
            Button(manager, "extension-toggle");
            Require(Wait([&] {
                auto state = catalog();
                return Enabled(manager, "extension-toggle") && String(Property(View(manager, "extension-toggle"), "Label"), "result") == "Enable"
                    && state.is_object() && state["extensions"][0]["enabled"] == false;
            }), "disable unloads the runtime and persists disabled state");
            if (actions) Require(Wait([&] { return !View(browser, actionName.c_str()).IsValid(); }), "unloading an extension removes its toolbar control");
            Button(manager, "extension-toggle");
            boot(2);
            Require(Wait([&] { return Enabled(manager, "extension-toggle") && catalog()["extensions"][0]["enabled"] == true; }),
                "enable reloads the approved package and preserves its storage");
            if (actions) {
                FocusWindow(browser);
                Require(Wait([&] { return actionTitle() == "Summit popup ready" && Enabled(browser, actionName.c_str()); }), "reloaded extension publishes fresh action state");
                auto current = ActionState(browser);
                Require(current.GetUInt64("load_identifier", 0) != originalAction.GetUInt64("load_identifier", 0), "reload has a distinct action load identity");
                auto oldLoadClick = current;
                oldLoadClick.what = summit::kActivateExtensionAction;
                oldLoadClick.ReplaceUInt64("load_identifier", originalAction.GetUInt64("load_identifier", 0));
                auto previousResult = State(browser).GetUInt64("extension_action_result_identifier", 0);
                Query(browser, oldLoadClick);
                Require(Wait([&] { auto state = State(browser); return state.GetUInt64("extension_action_result_identifier", 0) > previousResult
                    && state.GetInt32("extension_action_result_error", B_OK) == B_CANCELED; }), "SDK rejects activation from a previous extension load");
                Require(actionEvents().size() == 7 && !ActionPopup(app).IsValid(), "stale load cannot open a replacement extension's popup");
            }
        } else if (phase == "popup-quit" && actions) {
            boot(3);
            Require(Wait([&] { return actionTitle() == "Summit popup ready"; }), "action restores before shutdown test");
            actionClick();
            popup(5);
            // Deliberately keep the real WebKit popup open for app shutdown.
        } else if (phase == "remove") {
            boot(actions ? 4 : 3);
            Require(Entries(manager) == 1 && Text(manager, "extension-details").find("Running") != std::string::npos,
                "UI-installed extension restores automatically in another browser process");
            if (actions) {
                Require(Wait([&] { return actionTitle() == "Summit popup ready"; }), "action restores after actual browser restart");
                actionClick();
                popup(6);
            }
            Button(manager, "extension-remove");
            BMessenger confirmation;
            Require(Wait([&] { confirmation = NamedWindow(app, "Remove extension"); return confirmation.IsValid(); }),
                "removal asks for confirmation before deleting extension data");
            const auto beforeRemoval = catalog();
            Require(Entries(manager) == 1 && beforeRemoval["extensions"].size() == 1,
                "opening removal confirmation preserves the installed extension");
            auto cancel = View(confirmation, "_b0_");
            Require(String(Property(cancel, "Label"), "result") == "Cancel", "native removal dialog has a Cancel button");
            Key(cancel, " ", 0x5e);
            Require(Wait([&] { return !confirmation.IsValid(); }) && Entries(manager) == 1 && catalog() == beforeRemoval,
                "cancelling removal preserves the extension and saved installation");
            Button(manager, "extension-remove");
            Require(Wait([&] { confirmation = NamedWindow(app, "Remove extension"); return confirmation.IsValid(); }),
                "removal can be confirmed after cancellation");
            auto remove = View(confirmation, "_b1_");
            Require(String(Property(remove, "Label"), "result") == "Remove", "native removal dialog has a Remove button");
            Key(remove, " ", 0x5e);
            Require(Wait([&] { return !confirmation.IsValid(); }), "confirmed removal dismisses its native dialog");
            Require(Wait([&] { auto state = catalog(); return Entries(manager) == 0 && Enabled(manager, "extension-add")
                && state.is_object() && state["extensions"].empty(); }), "remove unloads extension and removes its startup record");
            if (actions) Require(Wait([&] { return !ActionPopup(app).IsValid() && !View(browser, actionName.c_str()).IsValid(); }),
                "removal destroys the open extension popup and toolbar control");
        } else if (phase == "after-removal") {
            Require(Entries(manager) == 0 && catalog()["extensions"].empty(), "removed installation stays absent after another browser restart");
            snooze(300000);
            Require(count() == (actions ? 4 : 3), "removed extension does not start again");
            openPicker();
        } else throw std::runtime_error("unknown test phase");
        Send(app, B_QUIT_REQUESTED);
        Require(Wait([&] { team_info info; return get_team_info(team, &info) != B_OK; }), "browser closes with extension manager and any file picker still open");
        std::printf("EXTENSION_MANAGER_RESULT PASS phase=%s checks=%d\n", phase.c_str(), checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "EXTENSION_MANAGER_RESULT FAIL %s\n", error.what());
        if (verified) Cleanup(app, team);
        return 1;
    }
}
#endif

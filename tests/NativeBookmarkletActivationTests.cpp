// Real toolbar, keyword and editor actions in an explicitly owned browser.
#define main SummitCloseHarnessEntry
#include "ModernCloseTests.cpp"
#undef main

int main(int argc, char** argv)
{
    if (argc != 6) return 2;
    status_t status;
    BApplication app("application/x-vnd.Summit-BookmarkletActivationTests", &status);
    if (status != B_OK) return 1;
    try {
        const team_id team = std::strtol(argv[1], nullptr, 10);
        BMessenger browser(nullptr, team), window;
        app_info info;
        Require(team > 0 && browser.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK,
            "explicit browser team registered");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]),
            "browser belongs to the exact owned executable");
        Require(Wait([&] {
            for (int32 i = 0; i < 12; ++i) {
                auto candidate = Window(browser, i);
                if (!candidate.IsValid()) break;
                if (String(State(candidate), "backend") == "modern") { window = candidate; return true; }
            }
            return false;
        }), "modern browser window found");
        auto page = [&](int64 id) {
            const auto state = State(window);
            BMessage tab; state.FindMessage("tab", Index(state, id), &tab); return tab;
        };
        const auto manager = Selected(State(window));
        const auto scriptID = std::strtoll(argv[4], nullptr, 10);
        BMessage move(summit::kMoveBookmark);
        move.AddInt64("id", scriptID); move.AddInt64("folder", 1); move.AddInt64("index", 0);
        Require(window.SendMessage(&move) == B_OK, "move imported bookmarklet onto the bar");
        auto open = [&](const std::string& path) {
            const auto before = Selected(State(window));
            Send(window, summit::kNewTab, -1, std::string(argv[3]) + path);
            Require(Wait([&] {
                const auto state = State(window);
                return Selected(state) != before && String(page(Selected(state)), "url") == std::string(argv[3]) + path
                    && String(page(Selected(state)), "loadOutcome") == "succeeded";
            }, 30000000), "new fixture tab loads");
            return Selected(State(window));
        };
        const auto target = open("/target"), other = open("/other");
        SelectTab(window, target);
        const auto generation = page(target).GetUInt64("loadGeneration", 0);
        const auto count = Count(State(window));
        const auto bar = View(window, "bookmarks-bar");
        Require(bar.IsValid(), "native bookmarks bar available");
        Click(bar, BPoint(20, 12));
        Require(Wait([&] { return String(page(target), "title") == "Bookmarklet activated 1"; }),
            "real toolbar click executes the saved bookmarklet once");
        Require(String(page(other), "title") == "Bookmarklet fixture", "background tab is unchanged");
        Require(Count(State(window)) == count && page(target).GetUInt64("loadGeneration", 0) == generation,
            "activation keeps the current tab and navigation generation");
        Send(window, summit::kFocusAddress);
        const auto address = Child(View(window, "address"), 0);
        Key(address, "mark");
        Require(Wait([&] { return State(window).GetBool("suggestions_showing", false) && String(State(window), "suggestion_url") == "mark"; }),
            "bookmarklet keyword appears as an explicit shortcut");
        Key(address, std::string(1, B_ENTER), 0x47);
        Require(Wait([&] { return String(page(target), "title") == "Bookmarklet activated 2"; }),
            "real address keys activate the keyword bookmarklet");
        Require(String(State(window), "address") == std::string(argv[3]) + "/target"
            && !State(window).GetBool("address_focused", true), "keyword activation restores the page address and focus");
        Send(window, summit::kOpenBookmark, -1, "javascript:void(document.title='Unsaved script')");
        snooze(200000);
        Require(String(page(target), "title") == "Bookmarklet activated 2", "unsaved script in a navigation message is rejected");
        SelectTab(window, manager);
        Click(bar, BPoint(20, 12));
        snooze(200000);
        Require(String(page(manager), "url") == "summit:bookmarks" && String(page(manager), "title") != "Bookmarklet activated 1",
            "toolbar activation cannot run inside the privileged manager");
        SelectTab(window, target);
        Send(window, summit::kEditBookmark, scriptID);
        BMessenger editor;
        Require(Wait([&] {
            for (int32 i = 0; i < 16; ++i) {
                auto candidate = Window(browser, i);
                if (!candidate.IsValid()) break;
                if (String(Property(candidate, "Title"), "result") == "Edit Bookmark") { editor = candidate; return true; }
            }
            return false;
        }), "native bookmarklet editor opens");
        const auto source = Child(View(editor, "address"), 0), save = View(editor, "save");
        Require(source.IsValid() && save.IsValid(), "native source field and Save available");
        BMessage select(B_SELECT_ALL); Require(source.SendMessage(&select) == B_OK, "select bookmarklet source");
        Key(source, "javascript:void(document.title='Edited bookmarklet')");
        Key(save, " ", 0x5e);
        Require(Wait([&] { return !editor.IsValid(); }), "native editor saves and closes");
        Click(bar, BPoint(20, 12));
        Require(Wait([&] { return String(page(target), "title") == "Edited bookmarklet"; }), "edited source executes from the real toolbar");
        const auto beforeBulk = Count(State(window));
        Send(window, summit::kOpenBookmarkFolder, std::strtoll(argv[5], nullptr, 10));
        Require(Wait([&] { return Count(State(window)) == beforeBulk + 1; }), "Open All adds only the ordinary page");
        snooze(200000);
        Require(Count(State(window)) == beforeBulk + 1 && String(page(target), "title") == "Edited bookmarklet",
            "Open All neither opens nor runs the folder's bookmarklet");
        std::printf("BOOKMARKLET_ACTIVATION_RESULT PASS checks=%d\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "BOOKMARKLET_ACTIVATION_FAILURE %s\n", error.what());
        return 1;
    }
}

// Native menu, keyword and editor actions in an explicitly owned browser.
#define main SummitCloseHarnessEntry
#include "ModernCloseTests.cpp"
#undef main

int main(int argc, char** argv)
{
    if (argc != 6) return 2;
    status_t status;
    BApplication app("application/x-vnd.Summit-BookmarkQueryTests", &status);
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
        auto active = [&] {
            auto state = State(window); BMessage tab;
            state.FindMessage("tab", Index(state, Selected(state)), &tab); return tab;
        };
        const auto id = std::strtoll(argv[4], nullptr, 10);
        const std::string mode = argv[3];
        if (mode == "menu") {
            BMessage move(summit::kMoveBookmark); move.AddInt64("id", id); move.AddInt64("folder", 1); move.AddInt64("index", 0);
            Require(window.SendMessage(&move) == B_OK, "move the saved smart folder onto the bar");
            const auto bar = View(window, "bookmarks-bar");
            Require(bar.IsValid(), "native bookmarks bar available");
            Click(bar, BPoint(20, 12));
            BMessenger menu;
            Require(Wait([&] {
                // Haiku excludes BMenuWindow from the application's Windows
                // property. Loopers includes those real popup windows too.
                auto loopers = Property(browser, "Loopers");
                BMessenger candidate;
                for (int32 i = 0; loopers.FindMessenger("result", i, &candidate) == B_OK; ++i) {
                    auto view = View(candidate, "bookmark-folder");
                    if (view.IsValid()) { menu = view; return true; }
                }
                return false;
            }), "real toolbar click opens the query's native menu");
            BMessage label(B_GET_PROPERTY); label.AddSpecifier("Label"); label.AddSpecifier("MenuItem", int32(0));
            Require(!String(Query(menu, label), "result").empty(), "native query has a first result");
            BMessage invoke(B_EXECUTE_PROPERTY); invoke.AddSpecifier("MenuItem", int32(0));
            Require(Query(menu, invoke).GetInt32("error", B_ERROR) == B_OK, "invoke the real first query menu item");
            Key(menu, std::string(1, B_ESCAPE), 0x01);
            Require(Wait([&] { return String(active(), "url") == argv[5] && String(active(), "loadOutcome") == "succeeded"; }),
                "query menu activation loads its first page");
        } else if (mode == "keyword") {
            Send(window, summit::kFocusAddress);
            const auto address = Child(View(window, "address"), 0);
            Key(address, argv[5]); Key(address, std::string(1, B_ENTER), 0x47);
            Require(Wait([&] { return String(active(), "url") == "summit:bookmarks" && String(active(), "title").find("Most visited") != std::string::npos; }),
                "real address keys open the saved smart-folder keyword");
        } else if (mode == "edit") {
            Send(window, summit::kEditBookmark, id);
            BMessenger editor;
            Require(Wait([&] {
                for (int32 i = 0; i < 16; ++i) {
                    auto candidate = Window(browser, i);
                    if (!candidate.IsValid()) break;
                    if (String(Property(candidate, "Title"), "result") == "Edit Bookmark") { editor = candidate; return true; }
                }
                return false;
            }), "native smart-folder editor opens");
            const auto address = Child(View(editor, "address"), 0), save = View(editor, "save");
            Require(address.IsValid() && save.IsValid(), "native query address and Save are available");
            BMessage select(B_SELECT_ALL); Require(address.SendMessage(&select) == B_OK, "select saved query source");
            Key(address, argv[5]); Key(save, " ", 0x5e);
            Require(Wait([&] { return !editor.IsValid(); }), "native query edit saves and closes");
        } else if (mode == "open-all") {
            const auto before = Count(State(window));
            Send(window, summit::kOpenBookmarkFolder, id);
            Require(Wait([&] { return Count(State(window)) == before + std::atoi(argv[5]); }),
                "Open All evaluates current query results");
        } else if (mode == "clear-history") {
            BMessage clear(summit::kClearHistoryReply); clear.AddInt32("which", 1);
            Require(window.SendMessage(&clear) == B_OK, "deliver confirmed native clear-history action for owned profile");
        } else if (mode == "back") {
            Send(window, summit::kBack);
            Require(Wait([&] { return String(active(), "title") == argv[5]; }), "native Back restores the preceding bookmarks view");
        } else return 2;
        std::printf("BOOKMARK_QUERY_UI_RESULT PASS checks=%d\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "BOOKMARK_QUERY_UI_FAILURE %s\n", error.what());
        return 1;
    }
}

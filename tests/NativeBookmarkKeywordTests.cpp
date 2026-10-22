// Address-field and editor integration through an explicitly owned browser.
#define main SummitCloseHarnessEntry
#include "ModernCloseTests.cpp"
#undef main

int main(int argc, char** argv)
{
    if (argc != 4) return 2;
    status_t status;
    BApplication application("application/x-vnd.Summit-BookmarkKeywordTests", &status);
    if (status != B_OK) return 1;
    try {
        const team_id team = std::strtol(argv[1], nullptr, 10);
        BMessenger app(nullptr, team), window;
        app_info info;
        Require(team > 0 && app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK,
            "explicit browser team registered with native roster");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]),
            "browser belongs to the exact test executable");
        Require(Wait([&] {
            for (int32 i = 0; i < 12; ++i) {
                auto candidate = Window(app, i);
                if (!candidate.IsValid()) break;
                if (String(State(candidate), "backend") == "modern") { window = candidate; return true; }
            }
            return false;
        }), "browser exposes native modern state");
        auto pageState = [&] {
            const auto state = State(window);
            BMessage page;
            state.FindMessage("tab", Index(state, Selected(state)), &page);
            return page;
        };
        auto enter = [&](const std::string& text, const std::string& path, bool selectSuggestion = false) {
            const auto generation = pageState().GetUInt64("loadGeneration", 0);
            Send(window, summit::kFocusAddress);
            const auto address = Child(View(window, "address"), 0);
            Require(address.IsValid(), "native address editor available");
            Key(address, text);
            Require(Wait([&] {
                const auto state = State(window);
                return state.GetBool("suggestions_showing", false) && String(state, "suggestion_url") == text;
            }), "first suggestion retains the complete shortcut input");
            if (selectSuggestion) {
                Key(address, std::string(1, B_DOWN_ARROW));
                Require(Wait([&] { return State(window).GetInt32("suggestion_selected", -1) == 0; }),
                    "down arrow selects the shortcut suggestion");
            }
            Key(address, std::string(1, B_ENTER), 0x47);
            Require(Wait([&] {
                const auto page = pageState();
                return page.GetUInt64("loadGeneration", 0) > generation
                    && String(page, "url") == std::string(argv[3]) + path
                    && String(page, "loadOutcome") == "succeeded";
            }, 30000000), selectSuggestion ? "selected bookmark shortcut navigates successfully" : "typed bookmark shortcut navigates successfully");
        };
        enter("shortcut", "/fixed");
        enter("find café & tea", "/get?q=caf%C3%A9%20%26%20tea");
        enter("post café & tea", "/post");
        enter("raw a+b&c", "/post", true);
        enter("latin café", "/latin?q=caf%E9");
        enter("post before editing", "/post");
        Send(window, summit::kEditBookmark);
        BMessenger editor;
        Require(Wait([&] {
            for (int32 i = 0; i < 16; ++i) {
                const auto candidate = Window(app, i);
                if (!candidate.IsValid()) break;
                if (String(Property(candidate, "Title"), "result") == "Edit Bookmark") { editor = candidate; return true; }
            }
            return false;
        }), "real native bookmark editor opens");
        const auto keywords = Child(View(editor, "keywords"), 0);
        const auto save = View(editor, "save");
        Require(keywords.IsValid() && save.IsValid(), "native editor exposes shortcut field and Save");
        BMessage select(B_SELECT_ALL);
        Require(keywords.SendMessage(&select) == B_OK, "select native shortcut names");
        Key(keywords, "post, raw, native");
        Key(save, " ", 0x5e);
        Require(Wait([&] { return !editor.IsValid(); }), "native Save closes the editor");
        enter("post after editing", "/post", true);
        enter("native extra", "/post");
        std::printf("BOOKMARK_KEYWORD_RESULT PASS checks=%d\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "BOOKMARK_KEYWORD_FAILURE %s\n", error.what());
        return 1;
    }
}

// Invoke the real Language menu item and type in an explicitly owned browser.
#define main SummitCloseHarnessEntry
#include "ModernCloseTests.cpp"
#undef main

int main(int argc, char** argv)
{
    if (argc != 7) return 2;
    status_t status;
    BApplication app("application/x-vnd.Summit-SpellingPreferencesTests", &status);
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
        BMessage show(summit::kShowPreferences); show.AddString("tab", "browsing");
        Require(browser.SendMessage(&show) == B_OK, "open native Browsing preferences");
        BMessenger preferences;
        Require(Wait([&] {
            for (int32 i = 0; i < 16; ++i) {
                auto candidate = Window(browser, i);
                if (!candidate.IsValid()) break;
                if (String(Property(candidate, "Title"), "result") == "Preferences — Summit") { preferences = candidate; return true; }
            }
            return false;
        }), "native Preferences window opens");
        const auto field = View(preferences, "spelling-language"), bar = Child(field, 0);
        Require(field.IsValid() && bar.IsValid(), "native spelling Language menu is available");
        BMessage choose(B_EXECUTE_PROPERTY);
        choose.AddSpecifier("MenuItem", argv[3]);
        choose.AddSpecifier("Menu", int32(0));
        Require(Query(bar, choose).GetInt32("error", B_ERROR) == B_OK, "invoke the real spelling Language menu item");
        Require(Wait([&] {
            BMessage marked(B_GET_PROPERTY);
            marked.AddSpecifier("Mark"); marked.AddSpecifier("MenuItem", argv[3]); marked.AddSpecifier("Menu", int32(0));
            return Query(bar, marked).GetBool("result", false);
        }), "selected language is marked in the native menu");
        Require(Wait([&] {
            std::ifstream stream(argv[5]);
            auto profile = json::parse(stream, nullptr, false);
            return profile.is_object() && profile.value("spellingLanguage", "missing") == argv[6];
        }), "native menu choice reaches the saved profile");
        Require(preferences.SendMessage(B_QUIT_REQUESTED) == B_OK, "close native Preferences window");
        Require(Wait([&] { return !preferences.IsValid(); }), "Preferences closes normally");
        const auto selected = Selected(State(window));
        const auto page = Page(window, selected);
        Require(page.IsValid(), "selected page is available for native typing");
        Click(page, BPoint(35, 40));
        Key(page, argv[4]); Key(page, " ");
        Require(Wait([&] {
            auto state = State(window); BMessage tab;
            if (state.FindMessage("tab", Index(state, selected), &tab) != B_OK) return false;
            auto title = String(tab, "title");
            if (!title.starts_with("SPELL ")) return false;
            auto data = json::parse(title.substr(6), nullptr, false);
            return data.is_object() && data.value("value", "") == std::string(argv[4]) + " ";
        }), "real keys reach the selected editor after changing the language preference");
        std::printf("SPELLING_PREFERENCES_RESULT PASS checks=%d\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SPELLING_PREFERENCES_FAILURE %s\n", error.what());
        return 1;
    }
}

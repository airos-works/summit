// Reuse the native scripting/input helpers and exact-executable guard.
#define main SummitCloseHarnessEntry
#include "ModernCloseTests.cpp"
#undef main
#include <FindDirectory.h>

static BMessenger DownloadPrompt(const BMessenger& app)
{
    for (int32 index = 0; index < 16; ++index) {
        auto window = Window(app, index);
        if (!window.IsValid()) break;
        if (String(Property(window, "Title"), "result") == "Downloads"
            && String(Property(View(window, "_b0_"), "Label"), "result") == "Keep Browsing"
            && String(Property(View(window, "_b1_"), "Label"), "result") == "Quit") return window;
    }
    return {};
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--check-executable-unused")
        return SummitCloseHarnessEntry(argc, argv);
    if (argc != 6) return 2;
    status_t status;
    BApplication application("application/x-vnd.Kunanyi-Summit-browser-download-tests", &status);
    if (status != B_OK) return 1;
    BMessenger app;
    team_id team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    bool verified = false;
    try {
        Require(team > 0, "explicit browser team supplied");
        app_info info;
        Require(Wait([&] {
            app = BMessenger(nullptr, team);
            return app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK;
        }), "test browser is registered with the native roster");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]),
            "target belongs to the exact frozen browser executable");
        verified = true;
        BMessenger window;
        Require(Wait([&] {
            for (int32 index = 0; index < 8; ++index) {
                auto candidate = Window(app, index);
                if (!candidate.IsValid()) break;
                if (String(State(candidate), "backend") == "modern") { window = candidate; return true; }
            }
            return false;
        }), "native browser window exposes its state");
        Require(Property(View(window, "downloads"), "Enabled").GetBool("result", false),
            "native Downloads toolbar button is enabled");
        BPath home;
        Require(find_directory(B_USER_DIRECTORY, &home) == B_OK, "locate the real native user download directory");
        const std::filesystem::path directory = std::filesystem::path(home.Path()) / "Downloads";
        const std::string prefix = "summit-test-" + std::string(argv[4]);
        const auto complete = directory / (prefix + "-ui-complete.txt");
        const auto partial = directory / (prefix + "-ui-cancel.txt");
        Require(!std::filesystem::exists(complete) && !std::filesystem::exists(partial),
            "unique test download names do not replace existing files");
        const std::string downloadURL = argv[3];
        const auto origin = downloadURL.substr(0, downloadURL.find('/', downloadURL.find("://") + 3));
        Send(window, summit::kNavigate, -1, origin + "/native-close.html?run=" + argv[4] + "&name=A&guard=0");
        Require(Wait([&] { return Has(Document(State(window), Selected(State(window))), "name", "A"); }),
            "real HTML download link loads in the browser");
        Click(Page(window, Selected(State(window))), BPoint(45, 335));
        Require(Wait([&] {
            auto state = State(window);
            return std::filesystem::exists(complete) && state.GetInt32("download_count", -1) == 0
                && String(state, "status").find("Download complete") != std::string::npos;
        }), "real browser saves the attachment and reports completion");
        {
            std::ifstream stream(complete, std::ios::binary);
            std::string body { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
            Require(body == "summit download ui-complete\n", "browser output matches the exact fixture body");
        }
        {
            auto state = State(window);
            BMessage page;
            state.FindMessage("tab", Index(state, Selected(state)), &page);
            Require(!page.GetBool("loadError", true), "HTML download handoff does not display a navigation error");
        }
        Send(window, summit::kNavigate, -1, std::string(argv[3]) + "?case=ui-cancel");
        Require(Wait([&] {
            return State(window).GetInt32("download_count", -1) == 1
                && std::filesystem::exists(partial) && std::filesystem::file_size(partial) > 0;
        }), "slow download is active and has written a real partial file");
        Require(Wait([&] {
            auto state = State(window);
            BMessage page;
            state.FindMessage("tab", Index(state, Selected(state)), &page);
            return String(page, "loadOutcome") == "cancelled" && !page.GetBool("loadError", true)
                && !page.GetBool("loading", true);
        }), "attachment navigation finishes as cancellation while the download continues");
        const auto selected = Selected(State(window));
        const auto linkDocument = Document(State(window), selected);
        Send(window, summit::kNavigate, -1, origin + "/native-close.html?run=" + argv[4] + "&name=A&guard=1");
        Require(Wait([&] {
            auto document = Document(State(window), selected);
            return Has(document, "name", "A") && document.contains("instance")
                && !Has(document, "instance", linkDocument.value("instance", std::string()));
        }),
            "guarded page loads while the independent download continues");
        Click(Page(window, selected), BPoint(40, 40));
        Require(Wait([&] { return Has(Document(State(window), selected), "armed", true); }),
            "native click activates beforeunload during the download");
        Key(Page(window, selected), "X", 0x4d);
        Require(Wait([&] { return Has(Document(State(window), selected), "value", "X"); }),
            "native edit remains live before provisional page approval");
        const auto original = Document(State(window), selected);
        auto address = Child(View(window, "address"), 0);
        Draft(window, address, "unfinished download address draft");
        Send(app, B_QUIT_REQUESTED);
        Decide(Prompt(app, window, selected, 1), true);
        BMessenger dialog;
        Require(Wait([&] { dialog = DownloadPrompt(app); return dialog.IsValid(); }),
            "quit presents native download cancellation controls");
        Key(View(dialog, "_b0_"), " ", 0x5e);
        Require(Wait([&] { return !dialog.IsValid() && !State(window).GetBool("closing", true); }),
            "Keep Browsing cancels window closure");
        CheckDraft(window, address, selected, "unfinished download address draft");
        SettledAttempts(window, selected, 1);
        Require(Has(Document(State(window), selected), "instance", original["instance"])
            && Has(Document(State(window), selected), "value", "X"),
            "Keep Browsing preserves the edited document after provisional page approval");
        Require(State(window).GetInt32("download_count", -1) == 1,
            "Keep Browsing leaves the active download running");
        Send(app, B_QUIT_REQUESTED);
        Decide(Prompt(app, window, selected, 2), true);
        Require(Wait([&] { dialog = DownloadPrompt(app); return dialog.IsValid(); }),
            "later quit requires a fresh native download decision");
        Key(View(dialog, "_b1_"), " ", 0x5e);
        Require(Wait([&] { team_info current; return get_team_info(team, &current) != B_OK; }),
            "confirmed quit drains cancellation and exits the exact browser");
        Require(!std::filesystem::exists(partial), "confirmed quit removes the partial download");
        {
            std::ifstream saved(std::filesystem::path(argv[5]) / "profile.json");
            const json profile = json::parse(saved, nullptr, false);
            Require(profile.is_object() && profile.contains("history"), "download test saves native browsing history");
            size_t downloadVisits = 0;
            for (const auto& page : profile["history"]) {
                const auto url = page.value("url", std::string());
                if (url.find("/download?") == std::string::npos) continue;
                Require(page.value("visits", -1) == 0 && page.value("visitCountKnown", false),
                    "downloads do not increment the successful page visit count");
                Require(page.contains("visitTransitions") && page["visitTransitions"].size() == 1
                    && page["visitTransitions"][0][1] == 7, "download start retains its distinct history transition");
                ++downloadVisits;
            }
            Require(downloadVisits == 2, "completed and cancelled downloads each retain one start visit");
        }
        // Remove only our completed fixture after verifying its content again.
        std::ifstream stream(complete, std::ios::binary);
        const std::string body { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
        stream.close();
        Require(body == "summit download ui-complete\n" && std::filesystem::remove(complete),
            "remove only the unchanged completed test fixture");
        std::printf("DOWNLOAD_UI_RESULT PASS checks=%d\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "DOWNLOAD_UI_RESULT FAIL: %s\n", error.what());
        if (verified) {
            const auto deadline = system_time() + 15000000;
            while (system_time() < deadline) {
                team_info current;
                if (get_team_info(team, &current) != B_OK) break;
                BMessage quit(B_QUIT_REQUESTED);
                app.SendMessage(&quit, static_cast<BHandler*>(nullptr), 100000);
                try {
                    if (auto dialog = DownloadPrompt(app); dialog.IsValid()) Key(View(dialog, "_b1_"), " ", 0x5e);
                    else if (auto dialog = Dialog(app); dialog.IsValid()) Key(View(dialog, "dialog-accept"), " ", 0x5e);
                } catch (...) { }
                snooze(100000);
            }
        }
        return 1;
    }
}

// Context ownership and history commands against an explicitly owned browser.
#define main SummitCloseHarnessEntry
#include "ModernCloseTests.cpp"
#undef main
int main(int argc, char** argv)
{
    if (argc != 5) return 2;
    status_t status;
    BApplication application("application/x-vnd.Kunanyi-Summit-download-visit-browser-tests", &status);
    if (status != B_OK) return 1;
    try {
        const auto team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
        BMessenger app(nullptr, team);
        app_info info;
        Require(team > 0 && app.IsValid() && be_roster->GetRunningAppInfo(team, &info) == B_OK, "owned browser is registered");
        BPath executable(&info.ref);
        Require(std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]), "exact owned browser executable");
        const std::string mode = argv[3];
        if (mode == "clear-history") {
            Send(app, summit::kClearHistoryRequest);
        } else if (mode == "private-close") {
            BMessenger window;
            for (int32 i = 0; i < 12; ++i) {
                auto candidate = Window(app, i);
                if (!candidate.IsValid()) break;
                if (State(candidate).GetBool("private", false)) { window = candidate; break; }
            }
            Require(window.IsValid(), "private source window found");
            Send(window, B_QUIT_REQUESTED);
            Require(Wait([&] { return !window.IsValid(); }), "private source window closes");
        } else {
            Require(mode == "private-open" || mode == "normal-detach", "recognized native window operation");
            std::vector<BMessenger> before;
            for (int32 i = 0; i < 12; ++i) { auto window = Window(app,i); if (!window.IsValid()) break; before.push_back(window); }
            BMessage create(summit::kNewWindow);
            create.AddBool("private", mode == "private-open");
            create.AddString("url", "about:blank");
            Require(app.SendMessage(&create) == B_OK, "create isolated source window");
            BMessenger window;
            Require(Wait([&] {
                for (int32 i = 0; i < 12; ++i) {
                    auto candidate = Window(app,i);
                    if (!candidate.IsValid()) break;
                    if (std::find(before.begin(),before.end(),candidate) == before.end()
                        && String(State(candidate),"backend") == "modern") { window=candidate; return true; }
                }
                return false;
            }), "new source window is available");
            Require(State(window).GetBool("private", false) == (mode == "private-open"), "source window uses requested privacy context");
            if (std::string(argv[4]) != "-") {
                Send(window, summit::kDownloadRetry, -1, argv[4]);
                Require(Wait([&] { return State(window).GetInt32("download_count", 0) > 0; }), "source window starts a real download");
            }
            if (mode == "normal-detach") {
                Send(window, B_QUIT_REQUESTED);
                Require(Wait([&] { return !window.IsValid(); }), "public source window closes during transfer");
            }
        }
        std::printf("DOWNLOAD_VISIT_UI_RESULT PASS checks=%d\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"DOWNLOAD_VISIT_UI_RESULT FAIL: %s\n",error.what());
        return 1;
    }
}

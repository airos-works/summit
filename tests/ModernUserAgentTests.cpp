// Native user-agent compatibility: initial requests, DOM values and explicit overrides.
#define SUMMIT_PDF_HELPERS_ONLY
#include "ModernPDFTests.cpp"

namespace {
const std::string haikuUA = "Mozilla/5.0 (Macintosh; Intel Haiku/R1) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";
const std::string macUA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";
const std::string customUA = "Summit-Explicit-UA-Test/1.0";
struct Panel { BWindow* window; BWebKitView* view; };

class UserAgentTests final : public BApplication {
public:
    UserAgentTests(char** argv) : BApplication("application/x-vnd.Summit-UserAgentTests"),
        profile(argv[1]), bundle(argv[2]), gate(argv[3]), baseline(std::string(argv[4]) == "baseline") { }
    int Result() const { return failed ? 1 : 0; }
    Panel Create(const std::string& agent)
    {
        BMessage settings;
        if (!agent.empty()) settings.AddString("userAgent", agent.c_str());
        auto* window = new BWindow(BRect(80, 80, 719, 559), "User agent tests", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
        auto* view = new BWebKitView(window->Bounds(), "ua-page", BMessenger(this), settings, B_FOLLOW_ALL, context);
        window->AddChild(view);
        Require(view->InitCheck() == B_OK, "native view initializes with its initial settings");
        window->Show(); panels.push_back({window, view}); return panels.back();
    }
    void ReadyToRun() override
    {
        SetPulseRate(100000);
        try {
            Require(BWebKitInitialize() == B_OK, "initialize native WebKit");
            context = std::make_shared<BWebKitContext>(profile.c_str());
            Require(context->InitCheck() == B_OK, "create isolated user-agent context");
            Create(""); Create(customUA); Create(haikuUA);
            worker = std::thread([this] {
                try { RunChecks(); }
                catch (const std::exception& error) { std::fprintf(stderr, "USER_AGENT_FAILURE %s\n", error.what()); failed = true; }
                done = true;
            });
        } catch (const std::exception& error) {
            std::fprintf(stderr, "USER_AGENT_FAILURE %s\n", error.what()); failed = done = true;
        }
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == B_WEBKIT_INSPECTOR_MESSAGES) return;
        if (message->what == B_WEBKIT_STATE_CHANGED) {
            if (!message->GetBool("loading", true)) {
                std::lock_guard lock(stateLock); loaded.insert(message->GetString("url", ""));
            }
            return;
        }
        BApplication::MessageReceived(message);
    }
    void Pulse() override
    {
        if (!done) return;
        if (!closing) {
            closing = system_time(); if (worker.joinable()) worker.join();
            for (auto panel : panels) if (panel.window->Lock()) panel.window->Quit();
            panels.clear(); context.reset();
        }
        if (!HasHelpers(bundle) && !BWebKitHasPendingNativeUI()) {
            std::printf("USER_AGENT_RESULT %s checks=%u\n", failed ? "FAIL" : "PASS", checks);
            finished = true; PostMessage(B_QUIT_REQUESTED);
        } else if (system_time() - closing > 30000000) {
            std::fputs("USER_AGENT_FAILURE helper teardown deadline\n", stderr); _Exit(1);
        }
    }
    bool QuitRequested() override { return finished; }
private:
    void Wait(const std::function<bool()>& condition, const std::string& label, bigtime_t timeout = 20000000)
    {
        const auto deadline = system_time() + timeout;
        while (system_time() < deadline) { if (condition()) { Require(true, label.c_str()); return; } snooze(50000); }
        Require(false, label.c_str());
    }
    std::shared_ptr<BWebKitInspectorSession> Attach(Panel panel, const std::string& url)
    {
        panel.view->LoadURL(url.c_str());
        Wait([&] { std::lock_guard lock(stateLock); return loaded.contains(url); }, "initial fixture loads before Inspector attaches");
        Require(panel.window->Lock(), "lock view for Inspector attachment");
        auto session = BWebKitInspectorSession::Create(*panel.view, BMessenger(this)); panel.window->Unlock();
        Require(bool(session), "native Inspector attaches"); return session;
    }
    void Check(Inspector& inspector, const std::string& url, const std::string& expected, bool navigation = true)
    {
        Wait([&] { return inspector.Evaluate("location.href === " + JSON(url).dump() + " && document.readyState === 'complete'") == true; }, "fixture commits: " + url);
        Wait([&] { return inspector.Evaluate("navigator.userAgent") == expected; }, "DOM user agent: " + url);
        if (navigation)
            Require(inspector.Evaluate("window.requestUserAgent") == expected, ("initial HTTP user agent: " + url).c_str());
        inspector.Evaluate("window.echoUA=null;fetch('/echo?case='+encodeURIComponent(location.pathname)).then(r=>r.json()).then(r=>window.echoUA=r);true");
        Wait([&] { return inspector.Evaluate("window.echoUA && window.echoUA.user_agent") == expected; }, "subresource HTTP user agent: " + url);
        std::printf("USER_AGENT_CASE %s\n", JSON{{"url", url}, {"expected", expected}, {"navigation", navigation}}.dump().c_str());
    }
    void Navigate(Panel panel, Inspector& inspector, const std::string& url, const std::string& expected)
    {
        panel.view->LoadURL(url.c_str()); Check(inspector, url, expected);
    }
    void RunChecks()
    {
        const auto zoomUA = baseline ? haikuUA : macUA;
        auto aSession = Attach(panels[0], "http://example.test/control"); Inspector a(aSession);
        Check(a, "http://example.test/control", haikuUA);
        std::puts("USER_AGENT_READY"); std::fflush(stdout);
        Wait([&] { return std::filesystem::exists(gate); }, "runtime inventory gate", 180000000);
        for (const auto& [host, path, agent] : std::vector<std::tuple<std::string, std::string, std::string>> {
            {"zoom.us", "default", zoomUA}, {"app.zoom.us", "subdomain", zoomUA},
            {"foo.app.zoom.us", "nested", zoomUA}, {"notzoom.us", "lookalike", haikuUA},
            {"zoom.us.example.test", "suffix", haikuUA}, {"foozoom.us", "prefix", haikuUA},
            {"zoom.us", "return", zoomUA}})
            Navigate(panels[0], a, "http://" + host + "/" + path, agent);

        // Even an explicit string identical to the engine default suppresses
        // quirks. The DOM must update without a reload when only this flag changes.
        panels[0].view->SetCustomUserAgent(haikuUA.c_str());
        Check(a, "http://zoom.us/return", haikuUA, false);
        Navigate(panels[0], a, "http://zoom.us/explicit-default", haikuUA);
        panels[0].view->SetCustomUserAgent("");
        Check(a, "http://zoom.us/explicit-default", zoomUA, false);
        Navigate(panels[0], a, "http://app.zoom.us/cleared", zoomUA);
        panels[0].view->SetCustomUserAgent(customUA.c_str());
        Check(a, "http://app.zoom.us/cleared", customUA, false);
        Navigate(panels[0], a, "http://app.zoom.us/live-custom", customUA);
        Navigate(panels[0], a, "http://example.test/live-custom", customUA);
        panels[0].view->SetCustomUserAgent("");
        Check(a, "http://example.test/live-custom", haikuUA, false);
        Navigate(panels[0], a, "http://zoom.us/reenabled", zoomUA);

        auto bSession = Attach(panels[1], "http://zoom.us/initial-custom"); Inspector b(bSession);
        Check(b, "http://zoom.us/initial-custom", customUA);
        Navigate(panels[1], b, "http://example.test/initial-custom", customUA);
        panels[1].view->SetCustomUserAgent("");
        Navigate(panels[1], b, "http://app.zoom.us/initial-custom-cleared", zoomUA);

        auto cSession = Attach(panels[2], "http://zoom.us/initial-default"); Inspector c(cSession);
        Check(c, "http://zoom.us/initial-default", haikuUA);
        panels[2].view->SetCustomUserAgent("");
        Check(c, "http://zoom.us/initial-default", zoomUA, false);
        Navigate(panels[2], c, "http://app.zoom.us/initial-default-cleared", zoomUA);
        Check(a, "http://zoom.us/reenabled", zoomUA, false);
    }
    std::string profile, bundle, gate;
    bool baseline, finished {false};
    std::atomic<bool> failed {false}, done {false};
    bigtime_t closing {0};
    std::thread worker;
    std::shared_ptr<BWebKitContext> context;
    std::vector<Panel> panels;
    std::mutex stateLock;
    std::set<std::string> loaded;
};
}
int main(int argc, char** argv)
{
    if (argc != 5) { std::fputs("Usage: ModernUserAgentTests PROFILE BUNDLE GATE baseline|candidate\n", stderr); return 2; }
    UserAgentTests app(argv); app.Run(); return app.Result();
}

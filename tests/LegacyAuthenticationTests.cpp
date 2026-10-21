// Native legacy authentication lifecycle: held replies, navigation and view destruction.
#define SUMMIT_PDF_HELPERS_ONLY
#include "ModernPDFTests.cpp"
#include <WebPage.h>
#include <WebSettings.h>
#include <WebView.h>
#include <WebViewConstants.h>

namespace {
constexpr uint32 kDestroyView = 'adsv';
std::mutex challengeLock;
std::vector<std::unique_ptr<BMessage>> challenges;

class AuthWindow final : public BWindow {
public:
    AuthWindow() : BWindow(BRect(80, 80, 679, 479), "Legacy authentication lifecycle",
        B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS) { }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == AUTHENTICATION_CHALLENGE) {
            std::printf("LEGACY_AUTH_CHALLENGE %s\n", message->GetString("text", ""));
            std::fflush(stdout);
            std::lock_guard guard(challengeLock);
            challenges.emplace_back(DetachCurrentMessage());
            return;
        }
        BWindow::MessageReceived(message);
    }
    bool QuitRequested() override { return false; }
};

class AuthProbe final : public BApplication {
public:
    AuthProbe(char** argv) : BApplication("application/x-vnd.Summit-LegacyAuthenticationTests"),
        base(argv[1]), profile(argv[2]), bundle(argv[3]), gate(argv[4]) { }
    int Result() const { return failed ? 1 : 0; }
    void ReadyToRun() override
    {
        // BApplication rounds pulse rates down to 100 ms granularity.
        SetPulseRate(100000);
        BWebPage::InitializeOnce();
        BWebSettings::SetPersistentStoragePath(profile.c_str());
        window = new AuthWindow();
        view = new BWebView("legacy-auth");
        view->ResizeTo(window->Bounds().Width(), window->Bounds().Height());
        view->SetResizingMode(B_FOLLOW_ALL);
        window->AddChild(view);
        window->Show();
        worker = std::thread([this] {
            try { RunChecks(); }
            catch (const std::exception& error) {
                std::fprintf(stderr, "LEGACY_AUTH_FAILURE %s\n", error.what());
                failed = true;
            }
            done = true;
        });
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == kDestroyView) {
            if (window && window->Lock()) {
                window->Quit();
                window = nullptr;
                view = nullptr;
            }
            destroyed = true;
            return;
        }
        BApplication::MessageReceived(message);
    }
    void Pulse() override
    {
        ++pulses;
        unsigned handlers = 0;
        BMessenger replyTarget;
        for (int32 i = 0; i < CountHandlers(); ++i) {
            if (HandlerAt(i)->Name() && std::string(HandlerAt(i)->Name()) == "legacy authentication reply") {
                ++handlers;
                replyTarget = BMessenger(HandlerAt(i));
            }
        }
        {
            std::lock_guard guard(pendingLock);
            pendingReply = replyTarget;
        }
        pending = handlers;
        if (handlers != lastPending) {
            lastPending = handlers;
            std::printf("LEGACY_AUTH_PENDING %u\n", handlers);
            std::fflush(stdout);
        }
        if (!done)
            return;
        if (!closing) {
            closing = system_time();
            if (worker.joinable())
                worker.join();
            if (window && window->Lock()) {
                window->Quit();
                window = nullptr;
                view = nullptr;
            }
            BWebPage::ShutdownOnce();
        }
        if (system_time() - closing > 500000 && !HasHelpers(bundle) && !BWebKitHasPendingNativeUI()) {
            std::printf("LEGACY_AUTH_RESULT %s checks=%u\n", failed ? "FAIL" : "PASS", checks);
            finished = true;
            PostMessage(B_QUIT_REQUESTED);
        } else if (system_time() - closing > 30000000) {
            std::fputs("LEGACY_AUTH_FAILURE helper teardown deadline\n", stderr);
            _Exit(1);
        }
    }
    bool QuitRequested() override { return finished; }
private:
    void Wait(const std::function<bool()>& predicate, const char* description,
        bigtime_t timeout = 15000000)
    {
        auto deadline = system_time() + timeout;
        while (system_time() < deadline) {
            if (predicate()) { Require(true, description); return; }
            snooze(50000);
        }
        Require(false, description);
    }
    void Navigate(const char* path)
    {
        Require(window->Lock(), "lock native legacy view for navigation");
        view->LoadURL((base + path).c_str());
        window->Unlock();
    }
    std::string Title() { return view->MainFrameTitle().String(); }
    std::unique_ptr<BMessage> Challenge()
    {
        Wait([] { std::lock_guard guard(challengeLock); return !challenges.empty(); },
            "legacy listener receives authentication challenge");
        std::lock_guard guard(challengeLock);
        auto challenge = std::move(challenges.front());
        challenges.erase(challenges.begin());
        return challenge;
    }
    void Reply(BMessage& challenge, const char* kind)
    {
        BMessage reply;
        reply.AddString("user", "legacy-test-user");
        reply.AddString("password", (std::string("legacy-test-password-") + kind).c_str());
        reply.AddBool("rememberCredentials", false);
        // A withdrawn handler may already be gone; delivery failure is valid.
        challenge.SendReply(&reply);
    }
    void RunChecks()
    {
        Require(window->Lock(), "lock legacy view for engine initialization check");
        auto* engineView = dynamic_cast<BWebKitView*>(view->ChildAt(0));
        status_t engineStatus = engineView ? engineView->InitCheck() : B_NO_INIT;
        window->Unlock();
        Require(engineStatus == B_OK, "legacy child engine initializes successfully");
        Navigate("/basic");
        auto basic = Challenge();
        std::puts("LEGACY_AUTH_READY");
        std::fflush(stdout);
        Wait([&] { return std::filesystem::exists(gate); }, "runtime inventory gate", 180000000);
        Require(basic->GetUInt32("failureCount", 999) == 0, "initial challenge has no previous failures");
        Reply(*basic, "basic");
        Wait([&] { return Title() == "Legacy child auth basic"; }, "legacy credentials load authenticated page");
        Wait([&] { return pending == 0; }, "answered challenge releases reply handler");

        Navigate("/hold");
        auto abandoned = Challenge();
        Wait([&] { return pending == 1; }, "held challenge retains one reply handler");
        BMessenger replyTarget;
        {
            std::lock_guard guard(pendingLock);
            replyTarget = pendingReply;
        }
        BMessage query(B_GET_PROPERTY), queryReply;
        query.AddSpecifier("InternalName");
        Require(replyTarget.SendMessage(&query, &queryReply, 1000000, 1000000) == B_OK
                && std::string(queryReply.GetString("result", "")) == "legacy authentication reply",
            "native handler query is not mistaken for an authentication answer");
        auto before = pulses.load();
        Wait([&] { return pulses > before + 2; }, "application thread runs while a listener holds its reply");
        Navigate("/after");
        Wait([&] { return Title() == "Legacy child auth after"; }, "navigation abandons unanswered challenge");
        Wait([&] { return pending == 0; }, "withdrawn challenge releases reply handler");

        Navigate("/fresh");
        auto fresh = Challenge();
        Wait([&] { return pending == 1; }, "new challenge has its own reply handler");
        Reply(*abandoned, "hold");
        snooze(500000);
        Require(pending == 1 && Title() == "Legacy child auth after",
            "stale reply cannot answer a newer challenge");
        Reply(*fresh, "fresh");
        Wait([&] { return Title() == "Legacy child auth fresh"; }, "new challenge accepts its own reply");
        Wait([&] { return pending == 0; }, "new reply handler is released");

        Navigate("/destroy");
        auto destroyedReply = Challenge();
        Wait([&] { return pending == 1; }, "pending challenge before view destruction");
        PostMessage(kDestroyView);
        Wait([&] { return destroyed.load(); }, "host destroys view with a pending challenge");
        Wait([&] { return pending == 0; }, "destroyed page releases its reply handlers");
        Reply(*destroyedReply, "destroy");
        before = pulses.load();
        Wait([&] { return pulses > before + 2; }, "late reply after destruction leaves application responsive");
    }
    std::string base, profile, bundle, gate;
    AuthWindow* window {nullptr};
    BWebView* view {nullptr};
    std::thread worker;
    std::atomic<bool> done {false}, failed {false}, destroyed {false};
    std::atomic<unsigned> pending {0}, pulses {0};
    std::mutex pendingLock;
    BMessenger pendingReply;
    unsigned lastPending {~0u};
    bigtime_t closing {0};
    bool finished {false};
};
}

int main(int argc, char** argv)
{
    if (argc != 5)
        return 2;
    AuthProbe app(argv);
    app.Run();
    return app.Result();
}

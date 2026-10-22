// Native certificate approvals are bound to the actual failed navigation.
#define SUMMIT_PDF_HELPERS_ONLY
#include "ModernPDFTests.cpp"

namespace {
constexpr uint32 kRecreate = 'tcrc';
class CertificateTests final : public BApplication {
public:
    CertificateTests(char** argv, status_t& status)
        : BApplication("application/x-vnd.Summit-CertificateDecisionTests", &status),
          plain(argv[1]), secure(argv[2]), bundle(argv[3]), gate(argv[4]),
          fingerprintA(argv[5]), fingerprintB(argv[6]) { }
    int Result() const { return failed ? 1 : 0; }
    void ReadyToRun() override
    {
        SetPulseRate(100000);
        try {
            Require(BWebKitInitialize() == B_OK, "initialize native engine");
            Create(true);
            worker = std::thread([this] {
                try { RunChecks(); }
                catch (const std::exception& error) {
                    std::fprintf(stderr, "CERTIFICATE_DECISION_FAILURE %s\n", error.what());
                    failed = true;
                }
                done = true;
            });
        } catch (const std::exception& error) {
            std::fprintf(stderr, "CERTIFICATE_DECISION_FAILURE %s\n", error.what());
            failed = done = true;
        }
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == B_WEBKIT_STATE_CHANGED) {
            BMessenger sender;
            if (message->FindMessenger("view", &sender) == B_OK && sender == BMessenger(view)) {
                std::lock_guard guard(mutex);
                state = *message;
            }
            return;
        }
        if (message->what == kRecreate) {
            try {
                if (auto generation = message->GetUInt64("generation", 0))
                    view->ContinueAfterCertificateError(generation);
                // On the engine thread: the queued approval above cannot run
                // until after destruction has marked the old view closed.
                if (window && window->Lock()) window->Quit();
                window = nullptr; view = nullptr;
                { std::lock_guard guard(mutex); state = BMessage(); }
                Create(message->GetBool("freshContext", false));
            } catch (const std::exception& error) {
                std::fprintf(stderr, "CERTIFICATE_DECISION_FAILURE %s\n", error.what());
                failed = true;
            }
            ++recreated;
            return;
        }
        BApplication::MessageReceived(message);
    }
    void Pulse() override
    {
        if (!done) return;
        if (!closing) {
            closing = system_time();
            if (worker.joinable()) worker.join();
            if (window && window->Lock()) window->Quit();
            window = nullptr; view = nullptr;
            context.reset();
        }
        if (system_time() - closing > 500000 && !HasHelpers(bundle) && !BWebKitHasPendingNativeUI()) {
            std::printf("CERTIFICATE_DECISION_RESULT %s checks=%u\n", failed ? "FAIL" : "PASS", checks);
            finished = true; PostMessage(B_QUIT_REQUESTED);
        } else if (system_time() - closing > 30000000) {
            std::fputs("CERTIFICATE_DECISION_FAILURE teardown deadline\n", stderr); _Exit(1);
        }
    }
    bool QuitRequested() override { return finished; }
private:
    void Create(bool fresh)
    {
        if (fresh) context = std::make_shared<BWebKitContext>(nullptr, true);
        Require(context && context->InitCheck() == B_OK, "private context initializes");
        window = new BWindow(BRect(60, 60, 659, 459), "Certificate decision tests",
            B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
        view = new BWebKitView(window->Bounds(), "certificate-page", BMessenger(this), B_FOLLOW_ALL, context);
        window->AddChild(view);
        Require(view->InitCheck() == B_OK, "native certificate view initializes");
        window->Show();
    }
    void Wait(const std::function<bool()>& condition, const char* label, bigtime_t timeout = 25000000)
    {
        auto deadline = system_time() + timeout;
        while (system_time() < deadline) {
            if (condition()) { Require(true, label); return; }
            snooze(50000);
        }
        Require(false, label);
    }
    BMessage State() { std::lock_guard guard(mutex); return state; }
    uint64 Failure(const std::string& url, const std::string& fingerprint, const char* formData = nullptr)
    {
        if (formData) Require(view->LoadForm(url.c_str(), formData) == B_OK, "native form request queued");
        else view->LoadURL(url.c_str());
        Wait([&] {
            auto snapshot = State();
            return std::string(snapshot.GetString("loadOutcome", "")) == "failed"
                && std::string(snapshot.GetString("loadErrorURL", "")) == url;
        }, "untrusted TLS navigation fails");
        auto snapshot = State();
        Require(snapshot.GetBool("loadErrorCertificate", false)
            && std::string(snapshot.GetString("loadErrorCertificateSHA256", "")) == fingerprint,
            "failure contains the exact current leaf fingerprint");
        Require(snapshot.GetUInt64("loadGeneration", 0) != 0, "failed navigation has a generation");
        return snapshot.GetUInt64("loadGeneration", 0);
    }
    void Loaded(const std::string& url)
    {
        Wait([&] {
            auto snapshot = State();
            return std::string(snapshot.GetString("loadOutcome", "")) == "succeeded"
                && std::string(snapshot.GetString("loadSuccessURL", "")) == url;
        }, "expected page loads successfully");
    }
    void Recreate(bool fresh, uint64 approve = 0)
    {
        auto before = recreated.load();
        BMessage message(kRecreate);
        message.AddBool("freshContext", fresh);
        message.AddUInt64("generation", approve);
        PostMessage(&message);
        Wait([&] { return recreated > before; }, "view replacement completes");
        Require(!failed, "view replacement succeeds");
    }
    void RunChecks()
    {
        auto generation = Failure(secure + "/initial", fingerprintA);
        std::puts("CERTIFICATE_DECISION_READY"); std::fflush(stdout);
        Wait([&] { return std::filesystem::exists(gate); }, "runtime inventory gate", 180000000);
        view->ContinueAfterCertificateError(0);
        view->ContinueAfterCertificateError(generation + 100);
        snooze(300000);
        Require(std::string(State().GetString("loadOutcome", "")) == "failed", "invalid generations do not approve TLS");

        view->LoadURL((plain + "/plain").c_str());
        view->ContinueAfterCertificateError(generation);
        Loaded(plain + "/plain");
        generation = Failure(secure + "/after-navigation", fingerprintA);
        view->Stop();
        view->ContinueAfterCertificateError(generation);
        generation = Failure(secure + "/after-stop", fingerprintA);
        view->Reload();
        view->ContinueAfterCertificateError(generation);
        // Reload returns to the last committed document after a provisional
        // TLS failure. It still invalidates approval of that failed request.
        Loaded(plain + "/plain");
        Wait([&] {
            return State().GetUInt64("loadGeneration", 0) > generation;
        }, "reload invalidates a queued approval of the previous failure");
        generation = Failure(secure + "/after-reload", fingerprintA);
        view->ContinueAfterCertificateError(generation);
        Loaded(secure + "/after-reload");
        view->LoadURL((secure + "/reuse").c_str()); Loaded(secure + "/reuse");
        view->LoadURL((plain + "/duplicate").c_str());
        view->ContinueAfterCertificateError(generation);
        Loaded(plain + "/duplicate");

        std::string otherHost = secure;
        otherHost.replace(otherHost.find("127.0.0.1"), 9, "localhost");
        generation = Failure(otherHost + "/host-scope", fingerprintA);
        view->ContinueAfterCertificateError(generation); Loaded(otherHost + "/host-scope");
        view->LoadURL((plain + "/rotate").c_str()); Loaded(plain + "/rotate");
        generation = Failure(secure + "/rotated", fingerprintB);
        view->ContinueAfterCertificateError(generation); Loaded(secure + "/rotated");
        generation = Failure(otherHost + "/closed", fingerprintB);
        Recreate(false, generation);
        generation = Failure(otherHost + "/after-closed", fingerprintB);
        view->ContinueAfterCertificateError(generation); Loaded(otherHost + "/after-closed");
        Recreate(true);
        Failure(otherHost + "/fresh-context", fingerprintB);
        Require(view->LoadForm("file:///boot/home/test", "q=x") == B_BAD_VALUE,
            "form navigation rejects non-HTTP destinations");
        Require(view->LoadForm(nullptr, "q=x") == B_BAD_VALUE && view->LoadForm(plain.c_str(), nullptr) == B_BAD_VALUE,
            "form navigation rejects missing arguments");
        const std::string oversized(1024 * 1024 + 1, 'x');
        Require(view->LoadForm(plain.c_str(), oversized.c_str()) == B_BAD_VALUE, "form navigation bounds request data");
        generation = Failure(secure + "/post-approval", fingerprintB, "q=caf%C3%A9%20%26%20tea&fixed=1");
        view->ContinueAfterCertificateError(generation);
        Loaded(secure + "/post-approval");
        Require(view->LoadForm((plain + "/post-empty").c_str(), "") == B_OK, "empty form remains a POST");
        Loaded(plain + "/post-empty");
        Require(view->LoadForm((plain + "/post-redirect307").c_str(), "keep=exact%2Bbytes") == B_OK, "307 form navigation queued");
        Loaded(plain + "/post-redirected307");
        Require(view->LoadForm((plain + "/post-redirect303").c_str(), "drop=body") == B_OK, "303 form navigation queued");
        Loaded(plain + "/post-redirected303");
    }
    std::string plain, secure, bundle, gate, fingerprintA, fingerprintB;
    std::shared_ptr<BWebKitContext> context;
    BWindow* window = nullptr;
    BWebKitView* view = nullptr;
    std::thread worker;
    std::mutex mutex;
    BMessage state;
    std::atomic<bool> done {false}, failed {false};
    std::atomic<unsigned> recreated {0};
    bigtime_t closing = 0;
    bool finished = false;
};
}

int main(int argc, char** argv)
{
    if (argc != 7) return 2;
    status_t status;
    CertificateTests app(argv, status);
    if (status != B_OK) return 1;
    app.Run();
    return app.Result();
}

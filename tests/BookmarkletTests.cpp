// Native bookmarklet semantics and stale-document rejection on owned pages.
#define SUMMIT_PDF_HELPERS_ONLY
#include "ModernPDFTests.cpp"

namespace {
constexpr uint32 kReplaceBeforeBookmarklet = 'bkrc';
class BookmarkletTests final : public BApplication {
public:
    BookmarkletTests(char** argv, status_t& status)
        : BApplication("application/x-vnd.Summit-BookmarkletTests", &status),
          base(argv[1]), bundle(argv[2]), gate(argv[3]) { }
    int Result() const { return failed ? 1 : 0; }
    void ReadyToRun() override
    {
        SetPulseRate(100000);
        try {
            Require(BWebKitInitialize() == B_OK, "initialize native engine");
            context = std::make_shared<BWebKitContext>(nullptr, true);
            Require(context->InitCheck() == B_OK, "private context initializes");
            window = new BWindow(BRect(60, 60, 659, 459), "Bookmarklet tests", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
            view = new BWebKitView(window->Bounds(), "page", BMessenger(this), B_FOLLOW_ALL, context);
            window->AddChild(view);
            Require(view->InitCheck() == B_OK, "native view initializes");
            window->Show();
            worker = std::thread([this] {
                try { RunChecks(); }
                catch (const std::exception& error) {
                    std::fprintf(stderr, "BOOKMARKLET_FAILURE %s\n", error.what()); failed = true;
                }
                done = true;
            });
        } catch (const std::exception& error) {
            std::fprintf(stderr, "BOOKMARKLET_FAILURE %s\n", error.what()); failed = done = true;
        }
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == B_WEBKIT_STATE_CHANGED) {
            BMessenger sender;
            if (message->FindMessenger("view", &sender) == B_OK && sender == BMessenger(view)) {
                std::lock_guard guard(mutex); state = *message;
            }
            return;
        }
        if (message->what == B_WEBKIT_JAVASCRIPT_RESULT) {
            std::lock_guard guard(mutex);
            answers[message->GetUInt64("identifier", 0)] = *message;
            return;
        }
        if (message->what == kReplaceBeforeBookmarklet) {
            // Queue both operations before the native loop can observe the
            // renderer's new document. The renderer must check its identity.
            view->RunBookmarklet("javascript:'<p id=newdoc>New document</p>'", BMessenger(this), 0, message->GetUInt64("generation", 0));
            view->RunBookmarklet("javascript:void(window.raced=true)", BMessenger(this), message->GetUInt64("identifier", 0),
                message->GetUInt64("generation", 0));
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
            session.reset();
            if (window && window->Lock()) window->Quit();
            window = nullptr; view = nullptr; context.reset();
        }
        if (system_time() - closing > 500000 && !HasHelpers(bundle) && !BWebKitHasPendingNativeUI()) {
            std::printf("BOOKMARKLET_RESULT %s checks=%u\n", failed ? "FAIL" : "PASS", checks);
            finished = true; PostMessage(B_QUIT_REQUESTED);
        } else if (system_time() - closing > 30000000) {
            std::fputs("BOOKMARKLET_FAILURE teardown deadline\n", stderr); _Exit(1);
        }
    }
    bool QuitRequested() override { return finished; }
private:
    void Wait(const std::function<bool()>& predicate, const char* label, bigtime_t timeout = 25000000)
    {
        auto deadline = system_time() + timeout;
        while (system_time() < deadline) {
            if (predicate()) { Require(true, label); return; }
            snooze(30000);
        }
        Require(false, label);
    }
    BMessage State() { std::lock_guard guard(mutex); return state; }
    BMessage Answer(uint64 identifier)
    {
        Wait([&] { std::lock_guard guard(mutex); return answers.contains(identifier); }, "bookmarklet reply arrives");
        std::lock_guard guard(mutex);
        auto answer = answers.at(identifier); answers.erase(identifier); return answer;
    }
    BMessage Invoke(const char* source, uint64 generation)
    {
        const auto id = ++request;
        view->RunBookmarklet(source, BMessenger(this), id, generation);
        return Answer(id);
    }
    void Execute(const char* source)
    {
        const auto answer = Invoke(source, State().GetUInt64("loadGeneration", 0));
        if (answer.HasString("error")) std::fprintf(stderr, "BOOKMARKLET_ERROR %s\n", answer.GetString("error", ""));
        Require(!answer.HasString("error") && answer.GetBool("result", false), "bookmarklet accepted on its current document");
    }
    void Load(const std::string& url)
    {
        const auto generation = State().GetUInt64("loadGeneration", 0);
        view->LoadURL(url.c_str());
        Wait([&] {
            auto snapshot = State();
            return snapshot.GetUInt64("loadGeneration", 0) > generation && !snapshot.GetBool("loading", true)
                && std::string(snapshot.GetString("loadOutcome", "")) == "succeeded"
                && std::string(snapshot.GetString("loadSuccessURL", "")) == url;
        }, "expected document loads successfully");
    }
    void RunChecks()
    {
        Load(base + "/plain");
        Require(window->Lock(), "lock native view for inspector");
        session = BWebKitInspectorSession::Create(*view, BMessenger(this)); window->Unlock();
        Require(bool(session), "test inspector attaches");
        Inspector inspector(session);
        Wait([&] { return inspector.Evaluate("document.readyState") == "complete"; }, "inspector sees current document");
        std::puts("BOOKMARKLET_READY"); std::fflush(stdout);
        Wait([&] { return std::filesystem::exists(gate); }, "runtime inventory gate", 180000000);
        const auto generation = State().GetUInt64("loadGeneration", 0);
        Execute("JaVaScRiPt:void(window.testValue='caf%C3%A9%20%26%20%2525')");
        Require(inspector.Evaluate("window.testValue") == "café & %25", "source percent-decodes exactly once as UTF-8");
        Execute("javascript:void(document.body.dataset.mark='selected')");
        Require(inspector.Evaluate("document.body.dataset.mark") == "selected", "bookmarklet changes the main document");
        Execute("javascript:void(window.activation=navigator.userActivation.isActive)");
        Require(inspector.Evaluate("window.activation") == true, "explicit bookmark activation supplies a user gesture");
        Execute("javascript:window.counter=41;window.counter+1");
        Require(inspector.Evaluate("window.counter") == 41, "numeric completion retains the page's main-world globals");
        Require(State().GetUInt64("loadGeneration", 0) == generation && !State().GetBool("loading", true),
            "scripts without string completion do not start a navigation");
        Execute("javascript:throw new Error('bookmarklet fixture exception')");
        Require(inspector.Evaluate("window.counter") == 41 && !State().GetBool("loading", true), "script exceptions leave the current page usable");
        Require(Invoke(nullptr, generation).HasString("error"), "null source rejected");
        Require(Invoke("https://example.invalid/", generation).HasString("error"), "non-script address rejected");
        const auto oversized = "javascript:" + std::string(65536, 'x');
        Require(Invoke(oversized.c_str(), generation).HasString("error"), "oversized source rejected");
        Require(Invoke("javascript:void(window.bad=1)", 0).HasString("error"), "zero generation rejected");
        Load(base + "/next");
        Require(Invoke("javascript:void(window.bad=1)", generation).HasString("error"), "stale navigation generation rejected");
        Require(inspector.Evaluate("typeof window.bad") == "undefined", "stale script does not reach the next page");

        Load(base + "/csp");
        Require(inspector.Evaluate("typeof window.inlineRan") == "undefined", "page inline script is blocked by its CSP");
        Execute("javascript:void(window.cspRan=1)");
        Require(inspector.Evaluate("window.cspRan") == 1, "explicit bookmarklet works under script-src none");
        Execute("javascript:void(document.getElementById('blocked').click())");
        Require(inspector.Evaluate("typeof window.linkRan") == "undefined", "page javascript links still obey CSP during bookmarklet execution");
        Execute("javascript:void((()=>{let s=document.createElement('script');s.textContent='window.injected=1';document.body.append(s)})())");
        Require(inspector.Evaluate("typeof window.injected") == "undefined", "bookmarklet does not disable the document's policy for injected scripts");
        const auto history = inspector.Evaluate("history.length");
        Execute("javascript:'<!doctype html><title>Replacement</title><p id=done>caf%C3%A9</p>'");
        Wait([&] { return inspector.Evaluate("document.getElementById('done')?.textContent") == "café"; }, "string completion replaces the document with HTML");
        Require(inspector.Evaluate("location.href") == base + "/csp" && inspector.Evaluate("history.length") == history,
            "replacement retains the page URL without adding history");
        Require(!State().GetBool("loading", true), "replacement settles the native load state");

        Load(base + "/race");
        const auto race = ++request;
        BMessage replace(kReplaceBeforeBookmarklet);
        replace.AddUInt64("identifier", race);
        replace.AddUInt64("generation", State().GetUInt64("loadGeneration", 0));
        PostMessage(&replace);
        Require(Answer(race).HasString("error"), "document replacement rejects an already queued bookmarklet");
        Require(inspector.Evaluate("typeof window.raced") == "undefined", "replacement document never runs the old bookmarklet");
        Load("about:blank");
        Require(Invoke("javascript:void(window.bad=1)", State().GetUInt64("loadGeneration", 0)).HasString("error"),
            "non-content document rejected by renderer");
        Load(base + "/sandbox");
        Require(Invoke("javascript:void(window.bad=1)", State().GetUInt64("loadGeneration", 0)).HasString("error"),
            "script-disabled sandbox rejected");
    }
    std::string base, bundle, gate;
    std::shared_ptr<BWebKitContext> context;
    std::shared_ptr<BWebKitInspectorSession> session;
    BWindow* window = nullptr;
    BWebKitView* view = nullptr;
    std::thread worker;
    std::mutex mutex;
    BMessage state;
    std::map<uint64, BMessage> answers;
    uint64 request = 0;
    std::atomic<bool> done {false}, failed {false};
    bigtime_t closing = 0;
    bool finished = false;
};
}

int main(int argc, char** argv)
{
    if (argc != 4) return 2;
    status_t status;
    BookmarkletTests app(argv, status);
    if (status != B_OK) return 1;
    app.Run();
    return app.Result();
}

// Native acceptance for browser-owned persistent inspection worlds. Link to
// the modern WebKit API and run with an isolated engine's LIBRARY_PATH and
// WEBKIT_EXEC_PATH. No account, network or existing browser is used.
#include <WebKit/WebKitView.h>
#include <WebKit/WebKitContext.h>
#include <WebKit/WebKitEmbedding.h>
#include <Application.h>
#include <Message.h>
#include <Window.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

struct Step {
    const char* label;
    const char* source;
    const char* expected;
    int view = 0;
    bool isolated = true, persistent = true, stale = false;
};

class TestApp final : public BApplication {
public:
    TestApp() : BApplication("application/x-vnd.Summit-IsolatedWorldTests") { }
    void ReadyToRun() override
    {
        if (BWebKitInitialize() != B_OK) { Fail("initialize"); return; }
        fContext = std::make_shared<BWebKitContext>("", true);
        fWindow = new BWindow(BRect(0, 0, 200, 100), "Summit isolated world tests", B_TITLED_WINDOW, 0);
        for (int i = 0; i < 2; ++i) {
            fViews[i] = new BWebKitView(fWindow->Bounds(), "test", BMessenger(this), B_FOLLOW_ALL, fContext);
            fWindow->AddChild(fViews[i]);
            fViews[i]->Hide();
            fMessengers[i] = BMessenger(fViews[i]);
            fViews[i]->SetURLSchemeHandler("summit-isolated", BMessenger(this), B_WEBKIT_SCHEME_SECURE);
            fViews[i]->LoadURL("summit-isolated://test/start");
        }
        fWindow->Run();
        fStarted = system_time();
        SetPulseRate(100000);
    }

    void MessageReceived(BMessage* message) override
    {
        if (message->what == B_WEBKIT_URL_SCHEME_REQUEST) {
            static const char html[] = "<!doctype html><title>original</title><p id='text'>Guten Morgen</p><div id='destination'></div>";
            BWebKitView::RespondToURLSchemeRequest(message->GetUInt64("identifier", 0), 200,
                "text/html", nullptr, html, sizeof(html) - 1);
            return;
        }
        if (message->what == B_WEBKIT_STATE_CHANGED) {
            BMessenger view;
            message->FindMessenger("view", &view);
            for (int i = 0; i < 2; ++i) {
                if (view != fMessengers[i]) continue;
                if (!message->GetBool("loading", true) && std::strcmp(message->GetString("loadOutcome", ""), "succeeded") == 0)
                    fGenerations[i] = message->GetUInt64("loadGeneration", 0);
            }
            if (!fRunning && fGenerations[0] && fGenerations[1]) {
                fRunning = true;
                fSteps = {
                    {"isolated call has no user activation", "String(navigator.userActivation.isActive || navigator.userActivation.hasBeenActive)", "false"},
                    {"retain node and private globals", "globalThis.saved = document.getElementById('text').firstChild; globalThis.marker = 'private'; saved.data", "Guten Morgen"},
                    {"website cannot read private globals", "String(typeof globalThis.saved) + ':' + typeof globalThis.marker", "undefined:undefined", 0, false},
                    {"website globals and prototype overrides", "globalThis.marker='website'; Array.prototype.map=()=>{throw Error('website override')}; 'ready'", "ready", 0, false},
                    {"private globals and builtins survive", "marker + ':' + [1,2].map(n=>n+1).join(',')", "private:2,3"},
                    {"fresh API remains fresh", "typeof globalThis.marker", "undefined", 0, true, false},
                    {"views have distinct persistent worlds", "typeof globalThis.marker", "undefined", 1},
                    {"website reorders DOM", "document.getElementById('destination').appendChild(document.getElementById('text')); 'moved'", "moved", 0, false},
                    {"retained reference updates original node", "saved.data='Good morning'; document.getElementById('destination').textContent", "Good morning"},
                    {"restore using retained reference", "saved.data='Guten Morgen'; document.getElementById('destination').textContent", "Guten Morgen"},
                    {"stale generation rejected", "document.title='stale script ran'; 'bad'", "error", 0, true, true, true},
                    {"stale script did not execute", "document.title", "original"}
                };
                Next();
            } else if (fAwaitingNavigation && fGenerations[0] > fOldGeneration) {
                fAwaitingNavigation = false;
                fSteps = {
                    {"new document has fresh private globals", "typeof globalThis.marker + ':' + typeof globalThis.saved", "undefined:undefined"},
                    {"old generation rejected after navigation", "document.title='previous document'; 'bad'", "error", 0, true, true, true},
                    {"new document unaffected by stale call", "document.title", "original"},
                    {"new document can retain its own state", "globalThis.marker='new'; marker", "new"},
                    {"second view still independent", "typeof globalThis.marker", "undefined", 1},
                    {"new state survives another call", "marker", "new"}
                };
                fStep = 0;
                Next();
            }
            return;
        }
        if (message->what == B_WEBKIT_JAVASCRIPT_RESULT && !fEnding) {
            if (fStep >= fSteps.size() || message->GetUInt64("identifier", 0) != fRequest) { Fail("unexpected reply"); return; }
            const auto& step = fSteps[fStep];
            const std::string value = message->GetString("result", "");
            const bool error = message->HasString("error");
            if (error != step.stale || (!error && value != step.expected)) {
                std::fprintf(stderr, "FAIL %s: value=%s error=%s\n", step.label, value.c_str(), message->GetString("error", ""));
                Fail(step.label); return;
            }
            std::printf("PASS %s\n", step.label); std::fflush(stdout);
            ++fChecks; ++fStep; Next(); return;
        }
        if (message->what == B_WEBKIT_PROCESS_EXITED && !fEnding) { Fail("content process exited"); return; }
        BApplication::MessageReceived(message);
    }
    void Pulse() override
    {
        if (fEnding) PostMessage(B_QUIT_REQUESTED);
        else if (system_time() - fStarted > 45000000) Fail("timeout");
    }
    bool QuitRequested() override
    {
        fEnding = true;
        if (fWindow) {
            if (!fWindow->Lock()) return false;
            auto* window = fWindow; fWindow = nullptr; window->Quit(); fContext.reset();
            PostMessage(B_QUIT_REQUESTED); return false;
        }
        if (BWebKitHasPendingNativeUI()) return false;
        if (!fDrained) { fDrained = true; PostMessage(B_QUIT_REQUESTED); return false; }
        SetPulseRate(0); return true;
    }
    int Result() const { return fResult; }
private:
    void Fail(const char* reason)
    {
        std::fprintf(stderr, "FAIL isolated world test: %s\n", reason);
        fResult = 1; fEnding = true; PostMessage(B_QUIT_REQUESTED);
    }
    void Next()
    {
        if (fStep == fSteps.size()) {
            if (!fOldGeneration) {
                fOldGeneration = fGenerations[0]; fAwaitingNavigation = true;
                fViews[0]->LoadURL("summit-isolated://test/next"); return;
            }
            std::printf("%u isolated world checks passed\n", fChecks);
            fResult = 0; fEnding = true; PostMessage(B_QUIT_REQUESTED); return;
        }
        const auto& step = fSteps[fStep];
        const uint64 generation = step.stale ? fGenerations[step.view] - 1 : fGenerations[step.view];
        ++fRequest;
        if (!step.isolated) fViews[step.view]->EvaluateJavaScript(step.source, BMessenger(this), fRequest);
        else if (step.persistent) fViews[step.view]->EvaluateJavaScriptIsolated(step.source, BMessenger(this), fRequest, generation, true);
        else fViews[step.view]->EvaluateJavaScriptIsolated(step.source, BMessenger(this), fRequest, generation);
    }
    BWindow* fWindow = nullptr;
    BWebKitView* fViews[2] { };
    BMessenger fMessengers[2];
    std::shared_ptr<BWebKitContext> fContext;
    uint64 fGenerations[2] { }, fOldGeneration = 0, fRequest = 0;
    std::vector<Step> fSteps;
    size_t fStep = 0;
    unsigned fChecks = 0;
    bigtime_t fStarted = 0;
    bool fRunning = false, fAwaitingNavigation = false, fEnding = false, fDrained = false;
    int fResult = 1;
};
int main() { TestApp app; app.Run(); return app.Result(); }

// Native legacy SSL_CERT_ERROR dialog and delayed listener replies.
#define SUMMIT_PDF_HELPERS_ONLY
#include "ModernPDFTests.cpp"
#include <WebPage.h>
#include <WebSettings.h>
#include <WebView.h>
#include <WebViewConstants.h>
#include <WebWindow.h>
#include <Alert.h>
#include <Button.h>
#include <TextView.h>

namespace {
constexpr uint32 kDestroy = 'lcdv';
std::atomic<bool> hold {false};
std::atomic<unsigned> questionCount {0}, genericErrors {0};
std::mutex questionsLock;
std::vector<std::unique_ptr<BMessage>> questions;
class CertificateWindow final : public BWebWindow {
public:
    CertificateWindow() : BWebWindow(BRect(80, 80, 679, 479), "Legacy certificate tests",
        B_TITLED_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL, B_ASYNCHRONOUS_CONTROLS) { }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == SSL_CERT_ERROR) {
            ++questionCount;
            if (hold) {
                std::lock_guard guard(questionsLock);
                questions.emplace_back(DetachCurrentMessage());
                return;
            }
        }
        BWebWindow::MessageReceived(message);
    }
    void MainDocumentError(const BString&, const BString&, BWebView*) override { ++genericErrors; }
    bool QuitRequested() override { return false; }
};

class CertificateProbe final : public BApplication {
public:
    CertificateProbe(char** argv) : BApplication("application/x-vnd.Summit-LegacyCertificateTests"),
        plain(argv[1]), secure(argv[2]), profile(argv[3]), bundle(argv[4]), gate(argv[5]),
        fingerprint(argv[6]) { }
    int Result() const { return failed ? 1 : 0; }
    void ReadyToRun() override
    {
        SetPulseRate(100000);
        BWebPage::InitializeOnce();
        BWebSettings::SetPersistentStoragePath(profile.c_str());
        window = new CertificateWindow();
        view = new BWebView("legacy-certificate");
        window->AddChild(view);
        window->SetCurrentWebView(view);
        window->Show();
        worker = std::thread([this] {
            try { RunChecks(); }
            catch (const std::exception& error) {
                std::fprintf(stderr, "LEGACY_CERTIFICATE_FAILURE %s\n", error.what());
                failed = true;
            }
            done = true;
        });
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == kDestroy) {
            if (window && window->Lock()) window->Quit();
            window = nullptr; view = nullptr;
            destroyed = true;
            return;
        }
        BApplication::MessageReceived(message);
    }
    void Pulse() override
    {
        ++pulses;
        unsigned count = 0;
        BMessenger replyTarget;
        for (int32 i = 0; i < CountHandlers(); ++i) {
            auto* handler = HandlerAt(i);
            if (handler->Name() && std::string(handler->Name()) == "legacy certificate reply") {
                ++count;
                replyTarget = BMessenger(handler);
            }
        }
        {
            std::lock_guard guard(uiLock);
            pendingReply = replyTarget;
            alert = BMessenger();
            for (int32 i = 0; i < CountWindows(); ++i) {
                auto* candidate = dynamic_cast<BAlert*>(WindowAt(i));
                if (!candidate || !candidate->Lock()) continue;
                alert = BMessenger(candidate);
                defaultStop = candidate->DefaultButton() == candidate->ButtonAt(1);
                alertText = candidate->TextView()->Text();
                candidate->Unlock();
                break;
            }
        }
        pending = count;
        if (!done) return;
        if (!closing) {
            closing = system_time();
            if (worker.joinable()) worker.join();
            // A failed assertion must not leave the synchronous host alert
            // blocking its window's teardown.
            { std::lock_guard guard(uiLock); AnswerAlertLocked(1); }
            if (window && window->Lock()) window->Quit();
            window = nullptr; view = nullptr;
            BWebPage::ShutdownOnce();
        }
        if (system_time() - closing > 500000 && !HasHelpers(bundle) && !BWebKitHasPendingNativeUI()) {
            std::printf("LEGACY_CERTIFICATE_RESULT %s checks=%u\n", failed ? "FAIL" : "PASS", checks);
            finished = true; PostMessage(B_QUIT_REQUESTED);
        } else if (system_time() - closing > 30000000) {
            std::fputs("LEGACY_CERTIFICATE_FAILURE teardown deadline\n", stderr); _Exit(1);
        }
    }
    bool QuitRequested() override { return finished; }
private:
    void Wait(const std::function<bool()>& condition, const char* label, bigtime_t timeout = 25000000)
    {
        auto deadline = system_time() + timeout;
        while (system_time() < deadline) {
            if (condition()) { Require(true, label); return; }
            snooze(50000);
        }
        Require(false, label);
    }
    void Navigate(const std::string& url)
    {
        Require(window->Lock(), "lock legacy view for navigation");
        view->LoadURL(url.c_str());
        window->Unlock();
    }
    void Loaded(const char* path)
    {
        Wait([&] { return view->MainFrameTitle() == (std::string("Certificate fixture ") + path).c_str(); },
            "legacy view displays the expected page");
    }
    void AnswerAlertLocked(int32 button)
    {
        if (!alert.IsValid()) return;
        BMessage answer('ALTB'); answer.AddInt32("which", button);
        alert.SendMessage(&answer);
    }
    std::unique_ptr<BMessage> Question()
    {
        Wait([] { std::lock_guard guard(questionsLock); return !questions.empty(); },
            "legacy listener receives a certificate question");
        std::lock_guard guard(questionsLock);
        auto question = std::move(questions.front()); questions.erase(questions.begin());
        return question;
    }
    void Reply(BMessage& question, bool approved)
    {
        BMessage answer; answer.AddBool("continue", approved); question.SendReply(&answer);
    }
    void RunChecks()
    {
        Navigate(secure + "/ui-stop");
        Wait([&] { std::lock_guard guard(uiLock); return alert.IsValid(); },
            "inherited BWebWindow displays the native certificate alert");
        std::puts("LEGACY_CERTIFICATE_READY"); std::fflush(stdout);
        Wait([&] { return std::filesystem::exists(gate); }, "runtime inventory gate", 180000000);
        {
            std::lock_guard guard(uiLock);
            Require(defaultStop, "native certificate dialog defaults to Stop");
            Require(alertText.find(fingerprint) != std::string::npos
                && alertText.find("127.0.0.1") != std::string::npos
                && alertText.find("Summit certificate fixture A") != std::string::npos
                && alertText.find("UTC") != std::string::npos
                && alertText.find("application session") != std::string::npos,
                "dialog identifies host, certificate, dates, fingerprint and session scope");
        }
        auto before = pulses.load();
        Wait([&] { return pulses > before + 2; }, "engine application thread runs while native dialog waits");
        { std::lock_guard guard(uiLock); AnswerAlertLocked(1); }
        Wait([&] { return pending == 0; }, "Stop releases the certificate reply handler");
        Navigate(secure + "/ui-continue");
        Wait([&] { std::lock_guard guard(uiLock); return questionCount == 2 && alert.IsValid(); },
            "Stop leaves the certificate untrusted and a later load prompts again");
        { std::lock_guard guard(uiLock); AnswerAlertLocked(0); }
        Loaded("/ui-continue");
        Navigate(secure + "/ui-reuse"); Loaded("/ui-reuse");
        Require(questionCount == 2 && !genericErrors, "session reuse needs no dialog and TLS errors have no duplicate generic alert");

        hold = true;
        std::string otherHost = secure;
        otherHost.replace(otherHost.find("127.0.0.1"), 9, "localhost");
        Navigate(otherHost + "/abandoned"); auto abandoned = Question();
        Wait([&] { return pending == 1; }, "held listener retains one certificate reply handler");
        BMessenger target;
        { std::lock_guard guard(uiLock); target = pendingReply; }
        BMessage query(B_GET_PROPERTY), queryReply; query.AddSpecifier("InternalName");
        Require(target.SendMessage(&query, &queryReply, 1000000, 1000000) == B_OK
            && std::string(queryReply.GetString("result", "")) == "legacy certificate reply",
            "native handler scripting is not mistaken for certificate approval");
        Navigate(plain + "/plain"); Loaded("/plain");
        Wait([&] { return pending == 0; }, "navigation releases the old reply handler");
        Navigate(otherHost + "/fresh"); auto fresh = Question();
        Reply(*abandoned, true);
        before = pulses.load();
        Wait([&] { return pulses > before + 3; }, "late abandoned response is processed without blocking");
        Require(pending == 1 && view->MainFrameTitle() == "Certificate fixture /plain",
            "late approval cannot trust the newer question");
        Reply(*fresh, false);
        Wait([&] { return pending == 0; }, "negative listener reply releases its handler");
        Navigate(otherHost + "/held-continue"); auto approved = Question();
        Reply(*approved, true); Loaded("/held-continue");
        Navigate(plain + "/rotate"); Loaded("/rotate");
        Navigate(secure + "/destroyed"); auto destroyedQuestion = Question();
        Wait([&] { return pending == 1; }, "replacement certificate needs a new question");
        PostMessage(kDestroy);
        Wait([&] { return destroyed.load() && pending == 0; }, "destroying view releases pending certificate handler");
        Reply(*destroyedQuestion, true);
        before = pulses.load();
        Wait([&] { return pulses > before + 3; }, "late approval after destruction leaves the application responsive");
    }
    std::string plain, secure, profile, bundle, gate, fingerprint;
    CertificateWindow* window = nullptr; BWebView* view = nullptr;
    std::thread worker; std::mutex uiLock;
    BMessenger alert, pendingReply; bool defaultStop = false; std::string alertText;
    std::atomic<bool> done {false}, failed {false}, destroyed {false};
    std::atomic<unsigned> pending {0}, pulses {0};
    bigtime_t closing = 0; bool finished = false;
};
}
int main(int argc, char** argv)
{
    if (argc != 7) return 2;
    CertificateProbe app(argv); app.Run(); return app.Result();
}

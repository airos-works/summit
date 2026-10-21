// Native legacy settings: committed defaults, page overrides and popup isolation.
#define SUMMIT_PDF_HELPERS_ONLY
#include "ModernPDFTests.cpp"
#include <WebPage.h>
#include <WebSettings.h>
#include <WebView.h>
#include <WebViewConstants.h>
#include <Font.h>
#include <cmath>

namespace {
constexpr uint32 kNewPanel = 'lspn';
class SettingsWindow final : public BWindow {
public:
    SettingsWindow() : BWindow(BRect(60, 60, 819, 639), "Legacy settings tests",
        B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS) { }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == NEW_PAGE_CREATED) { be_app->PostMessage(message); return; }
        BWindow::MessageReceived(message);
    }
    bool QuitRequested() override { return false; }
};
struct Panel { SettingsWindow* window; BWebView* legacy; BWebKitView* engine; };

class SettingsProbe final : public BApplication {
public:
    SettingsProbe(char** argv) : BApplication("application/x-vnd.Summit-LegacySettingsTests"),
        base(argv[1]), profile(argv[2]), bundle(argv[3]), gate(argv[4]) { }
    int Result() const { return failed ? 1 : 0; }
    Panel Create(BWebView* view)
    {
        auto* window = new SettingsWindow();
        view->ResizeTo(window->Bounds().Width(), window->Bounds().Height());
        view->SetResizingMode(B_FOLLOW_ALL);
        window->AddChild(view);
        Panel panel {window, view, dynamic_cast<BWebKitView*>(view->ChildAt(0))};
        Require(panel.engine && panel.engine->InitCheck() == B_OK, "native legacy engine view initializes");
        { std::lock_guard guard(panelsLock); panels.push_back(panel); }
        window->Show();
        return panel;
    }
    void ReadyToRun() override
    {
        SetPulseRate(100000);
        try {
            BWebPage::InitializeOnce(); BWebSettings::SetPersistentStoragePath(profile.c_str());
#ifndef SUMMIT_LEGACY_SETTINGS_BASELINE
            BMessage invalid; invalid.AddInt32("standardFontSize", 0);
            auto* invalidView = new BWebKitView(BRect(0, 0, 99, 99), "invalid-settings", BMessenger(this), invalid);
            Require(invalidView->InitCheck() == B_BAD_VALUE, "invalid initial settings cannot initialize a page");
            delete invalidView;
#endif
            font_family family; font_style style;
            be_plain_font->GetFamilyAndStyle(&family, &style); plainFont = family;
            be_fixed_font->GetFamilyAndStyle(&family, &style); fixedFont = family;
            Require(plainFont != fixedFont, "native plain and fixed font families differ");
            auto* defaults = BWebSettings::Default();
            defaults->SetStandardFont(*be_plain_font); defaults->SetSerifFont(*be_plain_font);
            defaults->SetSansSerifFont(*be_plain_font); defaults->SetFixedFont(*be_fixed_font);
            defaults->SetDefaultStandardFontSize(20); defaults->SetDefaultFixedFontSize(17);
            defaults->SetJavascriptEnabled(true);
#ifndef SUMMIT_LEGACY_SETTINGS_BASELINE
            defaults->SetUserAgent("Summit-Settings-Global-1");
#endif
            defaults->Apply();
            first = Create(new BWebView("settings-a")); second = Create(new BWebView("settings-b"));
            worker = std::thread([this] {
                try { RunChecks(); }
                catch (const std::exception& error) {
                    std::fprintf(stderr, "LEGACY_SETTINGS_FAILURE %s\n", error.what()); failed = true;
                }
                done = true;
            });
        } catch (const std::exception& error) {
            std::fprintf(stderr, "LEGACY_SETTINGS_FAILURE %s\n", error.what()); failed = done = true;
        }
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == B_WEBKIT_INSPECTOR_MESSAGES) return;
        if (message->what == kNewPanel) { Create(new BWebView("settings-new")); ++created; return; }
        if (message->what == NEW_PAGE_CREATED) {
            BWebView* view = nullptr;
            if (message->FindPointer("view", reinterpret_cast<void**>(&view)) == B_OK && view) {
                Create(view); ++opened;
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
            std::vector<Panel> old;
            { std::lock_guard guard(panelsLock); old.swap(panels); }
            for (auto panel : old) if (panel.window->Lock()) panel.window->Quit();
            // Queued settings notifications must not touch the deleted child
            // views while their BWebPage shutdown messages drain.
            BWebSettings::Default()->Apply();
            BWebPage::ShutdownOnce();
        }
        if (system_time() - closing > 500000 && !HasHelpers(bundle) && !BWebKitHasPendingNativeUI()) {
            unsigned pages = 0;
            for (int32 i = 0; i < CountHandlers(); ++i)
                if (HandlerAt(i)->Name() && std::string(HandlerAt(i)->Name()) == "BWebPage") ++pages;
            if (!pages) {
                std::printf("LEGACY_SETTINGS_RESULT %s checks=%u\n", failed ? "FAIL" : "PASS", checks);
                finished = true; PostMessage(B_QUIT_REQUESTED);
            }
        }
        if (!finished && system_time() - closing > 30000000) {
            std::fputs("LEGACY_SETTINGS_FAILURE teardown deadline\n", stderr); _Exit(1);
        }
    }
    bool QuitRequested() override { return finished; }
private:
    void Wait(const std::function<bool()>& condition, const char* label, bigtime_t timeout = 25000000)
    {
        auto deadline = system_time() + timeout;
        while (system_time() < deadline) { if (condition()) { Require(true, label); return; } snooze(50000); }
        Require(false, label);
    }
    void Navigate(Panel panel, const char* path)
    {
        Require(panel.window->Lock(), "lock native legacy view for navigation");
        panel.legacy->LoadURL((base + path).c_str()); panel.window->Unlock();
    }
    std::shared_ptr<BWebKitInspectorSession> Attach(Panel panel)
    {
        Wait([&] { return panel.legacy->MainFrameTitle().FindFirst("Settings fixture") == 0; },
            "legacy document commits before attaching inspection");
        Require(panel.window->Lock(), "lock native view for inspection");
        auto session = BWebKitInspectorSession::Create(*panel.engine, BMessenger(this));
        panel.window->Unlock(); Require(bool(session), "native Inspector attaches"); return session;
    }
    void Ready(Inspector& inspector, const char* path)
    {
        Wait([&] { return inspector.Evaluate("location.pathname===" + JSON(path).dump()
            + " && document.readyState==='complete'") == true; }, "fixture document finishes loading");
    }
    void Font(Inspector& inspector, const char* id, const std::string& family, int size)
    {
        const std::string script = "(()=>{const e=document.getElementById(" + JSON(id).dump()
            + ");const s=getComputedStyle(e);const r=document.createElement('span');r.textContent=e.textContent;"
              "r.style.fontSize=s.fontSize;r.style.fontFamily=" + JSON(family).dump()
            + ";document.body.append(r);const a=[parseFloat(s.fontSize),e.getBoundingClientRect().width,r.getBoundingClientRect().width];r.remove();return a})()";
        Wait([&] {
            auto metrics = inspector.Evaluate(script);
            return metrics.is_array() && metrics.size() == 3 && metrics[0] == size
                && std::abs(metrics[1].get<double>() - metrics[2].get<double>()) < 0.5;
        }, (std::string("rendered ") + id + " uses " + family + " at " + std::to_string(size) + "px").c_str());
    }
    void Click(Panel panel, Inspector& inspector, const char* id)
    {
        auto point = inspector.Evaluate("(()=>{const r=document.getElementById(" + JSON(id).dump()
            + ").getBoundingClientRect();return [r.x+r.width/2,r.y+r.height/2]})()");
        Require(point.is_array(), "find the native input target");
        BPoint where(point[0].get<float>(), point[1].get<float>());
        for (uint32 what : {B_MOUSE_DOWN, B_MOUSE_UP}) {
            BMessage event(what); event.AddPoint("where", where); event.AddPoint("be:view_where", where);
            event.AddInt32("buttons", what == B_MOUSE_DOWN ? B_PRIMARY_MOUSE_BUTTON : 0);
            event.AddInt32("clicks", 1); event.AddInt32("modifiers", 0); event.AddInt64("when", system_time());
            Require(BMessenger(panel.engine).SendMessage(&event) == B_OK, "send native input gesture");
        }
    }
    void RunChecks()
    {
        auto* local = first.legacy->WebPage()->Settings();
#ifdef SUMMIT_LEGACY_SETTINGS_BASELINE
        local->SetDefaultStandardFontSize(40); local->SetJavascriptEnabled(false); local->Apply();
        Navigate(first, "/baseline"); auto session = Attach(first); Inspector a(session); Ready(a, "/baseline");
        std::puts("LEGACY_SETTINGS_READY"); std::fflush(stdout);
        Wait([&] { return std::filesystem::exists(gate); }, "runtime inventory gate", 180000000);
        Require(a.Evaluate("document.documentElement.dataset.ran==='yes'") == true,
            "baseline ignores disabling JavaScript");
        Require(a.Evaluate("getComputedStyle(document.getElementById('standard')).fontSize==='40px'") == false,
            "baseline ignores the requested font size");
#else
        local->SetStandardFont(*be_fixed_font); local->SetSerifFont(*be_fixed_font);
        local->SetDefaultStandardFontSize(24); local->SetJavascriptEnabled(false);
        local->SetUserAgent("Summit-Settings-Page-A"); local->Apply();
        Navigate(first, "/a1"); Navigate(second, "/b1");
        auto aSession = Attach(first), bSession = Attach(second); Inspector a(aSession), b(bSession);
        Ready(a, "/a1"); Ready(b, "/b1");
        std::puts("LEGACY_SETTINGS_READY"); std::fflush(stdout);
        Wait([&] { return std::filesystem::exists(gate); }, "runtime inventory gate", 180000000);
        Require(a.Evaluate("document.documentElement.dataset.ran==='yes'") == false,
            "disabled page never executes its parser script");
        Require(b.Evaluate("document.documentElement.dataset.ran==='yes'") == true,
            "another page keeps application-default JavaScript enabled");
        Require(a.Evaluate("navigator.userAgent") == "Summit-Settings-Page-A"
            && b.Evaluate("navigator.userAgent") == "Summit-Settings-Global-1", "page and application user agents stay distinct");
        Font(a, "standard", fixedFont, 24); Font(b, "standard", plainFont, 20);
        Font(a, "serif", fixedFont, 24); Font(b, "serif", plainFont, 20);
        Font(a, "sans", plainFont, 24); Font(b, "fixed", fixedFont, 17);

        local->SetStandardFont(*be_plain_font); local->SetDefaultStandardFontSize(40);
        auto* defaults = BWebSettings::Default();
        defaults->SetDefaultStandardFontSize(32); defaults->SetDefaultFixedFontSize(27);
        defaults->SetSansSerifFont(*be_fixed_font); defaults->SetFixedFont(*be_plain_font);
        defaults->SetJavascriptEnabled(false); defaults->SetUserAgent("Summit-Settings-Global-2"); defaults->Apply();
        Font(a, "standard", fixedFont, 24); Font(b, "standard", plainFont, 32);
        Font(a, "sans", fixedFont, 24); Font(b, "fixed", plainFont, 27);
        local->SetJavascriptEnabled(true); local->Apply();
        Font(a, "standard", plainFont, 40); Font(b, "standard", plainFont, 32);
        // WebCore does not create inline event listeners while scripting is
        // disabled. Enabling it cannot restore handlers skipped during parsing.
        Require(a.Evaluate("document.getElementById('click').onclick===null") == true,
            "enabling scripts does not retroactively install skipped inline handlers");
        Navigate(first, "/a2"); Ready(a, "/a2");
        Click(first, a, "click");
        Wait([&] { return a.Evaluate("document.documentElement.dataset.clicks") == "1"; },
            "enabled JavaScript permits native input handlers after navigation");
        Click(second, b, "click"); snooze(300000);
        Require(b.Evaluate("document.documentElement.dataset.clicks||'0'") == "0",
            "disabling JavaScript blocks existing page input handlers");

        BMessage invalid; invalid.AddBool("javascriptEnabled", true); invalid.AddInt32("standardFontSize", 0);
        Require(second.engine->SetPageSettings(invalid) == B_BAD_VALUE, "invalid setting rejects the whole engine update");
        BMessage unknown; unknown.AddString("unexpected", "value");
        Require(second.engine->SetPageSettings(unknown) == B_BAD_VALUE, "unknown engine setting is rejected");
        BMessage duplicate; duplicate.AddBool("javascriptEnabled", true); duplicate.AddBool("javascriptEnabled", false);
        Require(second.engine->SetPageSettings(duplicate) == B_BAD_VALUE, "duplicate setting values are rejected");
        BMessage wrongType; wrongType.AddString("javascriptEnabled", "false");
        Require(second.engine->SetPageSettings(wrongType) == B_BAD_VALUE, "wrong setting types are rejected");
        Navigate(second, "/b2"); Ready(b, "/b2");
        Require(a.Evaluate("document.documentElement.dataset.ran==='yes'") == true
            && b.Evaluate("document.documentElement.dataset.ran==='yes'") == false,
            "committed script settings precede the next navigation");
        Require(a.Evaluate("navigator.userAgent") == "Summit-Settings-Page-A"
            && b.Evaluate("navigator.userAgent") == "Summit-Settings-Global-2", "default user-agent changes preserve page overrides");

        PostMessage(kNewPanel); Wait([&] { return created == 1; }, "new page created after changing defaults");
        Panel third; { std::lock_guard guard(panelsLock); third = panels.back(); }
        Navigate(third, "/c1"); auto cSession = Attach(third); Inspector c(cSession); Ready(c, "/c1");
        Require(c.Evaluate("document.documentElement.dataset.ran==='yes'") == false
            && c.Evaluate("navigator.userAgent") == "Summit-Settings-Global-2", "new page inherits current application defaults");
        Font(c, "standard", plainFont, 32); Font(c, "fixed", plainFont, 27);

        Click(first, a, "popup"); Wait([&] { return opened == 1; }, "native gesture opens a legacy popup");
        Panel popup; { std::lock_guard guard(panelsLock); popup = panels.back(); }
        auto popupSession = Attach(popup); Inspector child(popupSession); Ready(child, "/popup");
        Require(child.Evaluate("!!window.opener") == true, "settings preserve the popup's opener");
        Require(child.Evaluate("document.documentElement.dataset.ran==='yes'") == false,
            "opened page starts with application-default JavaScript disabled");
        Font(child, "standard", plainFont, 32);
        popup.legacy->WebPage()->Settings()->SetDefaultStandardFontSize(9);
        popup.legacy->WebPage()->Settings()->Apply(); Font(child, "standard", plainFont, 9);
        Font(a, "standard", plainFont, 40);
        local->SetUserAgent(""); local->Apply(); Navigate(first, "/reset"); Ready(a, "/reset");
        auto reset = a.Evaluate("navigator.userAgent");
        Require(reset.is_string() && !reset.get<std::string>().empty()
            && reset.get<std::string>().find("Summit-Settings-") == std::string::npos,
            "empty page user agent restores the engine default");
#endif
    }
    std::string base, profile, bundle, gate, plainFont, fixedFont;
    Panel first {}, second {}; std::vector<Panel> panels; std::mutex panelsLock;
    std::thread worker; std::atomic<bool> done {false}, failed {false};
    std::atomic<unsigned> opened {0}, created {0}; bigtime_t closing = 0; bool finished = false;
};
}
int main(int argc, char** argv)
{
    if (argc != 5) return 2;
    SettingsProbe app(argv); app.Run(); return app.Result();
}

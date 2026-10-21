// Exercise HTML datalist through the real engine, native input and popup.
#define SUMMIT_PDF_HELPERS_ONLY
#include "ModernPDFTests.cpp"
#include <ListView.h>

class DataListTests final : public BApplication {
public:
    DataListTests(char** argv, status_t& status)
        : BApplication("application/x-vnd.Summit-DataListTests", &status), m_url(argv[1]), m_bundle(argv[2]) { }
    int result() const { return m_failed || !m_finished; }
    void ReadyToRun() override
    {
        SetPulseRate(100000);
        try {
            Require(BWebKitInitialize() == B_OK, "initialize datalist engine");
            m_context = std::make_shared<BWebKitContext>(nullptr, true);
            Require(m_context->InitCheck() == B_OK, "create private datalist context");
            m_window = new BWindow(BRect(100, 100, 949, 799), "Summit datalist tests", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
            m_view = new BWebKitView(m_window->Bounds(), "datalist-page", BMessenger(this), B_FOLLOW_ALL, m_context);
            m_window->AddChild(m_view);
            Require(m_view->InitCheck() == B_OK, "create datalist web view");
            m_view->MakeFocus(); m_window->Show();
            m_worker = std::thread([this] {
                try { runTests(); }
                catch (const std::exception& error) { std::fprintf(stderr, "DATALIST_FAILURE %s\n", error.what()); m_failed = true; }
                m_done = true;
            });
        } catch (const std::exception& error) {
            std::fprintf(stderr, "DATALIST_FAILURE %s\n", error.what()); m_failed = true; m_done = true;
        }
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == B_WEBKIT_INSPECTOR_MESSAGES) return;
        if (message->what == B_WEBKIT_STATE_CHANGED) { std::lock_guard lock(m_mutex); m_state = *message; return; }
        BApplication::MessageReceived(message);
    }
    void Pulse() override
    {
        if (!m_done) return;
        if (!m_closing) {
            m_closing = system_time();
            if (m_worker.joinable()) m_worker.join();
            m_session.reset();
            if (m_window && m_window->Lock()) { m_window->Quit(); m_window = nullptr; m_view = nullptr; }
            m_context.reset();
        }
        if (!HasHelpers(m_bundle) && !BWebKitHasPendingNativeUI()) {
            std::printf("DATALIST_RESULT %s checks=%u\n", m_failed ? "FAIL" : "PASS", checks);
            std::fflush(nullptr); m_finished = true; PostMessage(B_QUIT_REQUESTED);
        } else if (system_time() - m_closing > 30000000) {
            std::fputs("DATALIST_FAILURE native/helper shutdown deadline\n", stderr); std::_Exit(1);
        }
    }
    bool QuitRequested() override { return m_finished; }
private:
    void wait(const std::function<bool()>& predicate, const char* description, bigtime_t duration = 10000000)
    {
        const auto deadline = system_time() + duration;
        while (system_time() < deadline) { if (predicate()) { Require(true, description); return; } snooze(50000); }
        Require(false, description);
    }
    void key(const char* text, int32 code = 0x3c)
    {
        for (uint32 what : { B_KEY_DOWN, B_KEY_UP }) {
            BMessage message(what); message.AddString("bytes", text); message.AddInt32("key", code);
            message.AddInt32("modifiers", 0); message.AddInt64("when", system_time());
            Require(BMessenger(m_view).SendMessage(&message) == B_OK, "send native datalist key");
        }
    }
    void special(char byte, int32 code) { const char text[] = { byte, 0 }; key(text, code); }
    void click(BMessenger view, BPoint where)
    {
        for (uint32 what : { B_MOUSE_DOWN, B_MOUSE_UP }) {
            BMessage message(what); message.AddPoint("where", where); message.AddPoint("be:view_where", where);
            message.AddInt32("buttons", what == B_MOUSE_DOWN ? B_PRIMARY_MOUSE_BUTTON : 0);
            message.AddInt32("clicks", 1); message.AddInt32("modifiers", 0); message.AddInt64("when", system_time());
            Require(view.SendMessage(&message) == B_OK, "send native datalist pointer event");
        }
    }
    BMessenger findPopup()
    {
        BMessage request(B_GET_PROPERTY), reply;
        request.AddSpecifier("Windows");
        if (BMessenger(this).SendMessage(&request, &reply, 500000, 500000) != B_OK) return { };
        BMessenger candidate;
        for (int32 i = 0; reply.FindMessenger("result", i, &candidate) == B_OK; ++i) {
            if (candidate.LockTargetWithTimeout(0) != B_OK) continue;
            BLooper* looper = nullptr;
            auto* window = dynamic_cast<BWindow*>(candidate.Target(&looper));
            const bool found = window && !window->IsHidden() && window->FindView("datalist");
            if (looper) looper->Unlock();
            if (found) return candidate;
        }
        return { };
    }
    int32 rows()
    {
        if (!m_popup.IsValid()) m_popup = findPopup();
        if (m_popup.LockTargetWithTimeout(0) != B_OK) return -1;
        BLooper* looper = nullptr;
        auto* window = dynamic_cast<BWindow*>(m_popup.Target(&looper));
        auto* list = window ? dynamic_cast<BListView*>(window->FindView("datalist")) : nullptr;
        auto count = list && !window->IsHidden() ? list->CountItems() : -1;
        if (looper) looper->Unlock();
        return count;
    }
    void closed() { wait([&] { return !m_popup.IsValid(); }, "native popup closes"); m_popup = { }; }
    void open(Inspector& inspector)
    {
        Require(inspector.Evaluate("(()=>{fruit.blur(); fruit.value=''; log.length=0; scrollTo(0,0); document.querySelector('#fruits').innerHTML='<option value=Apple label=Red><option value=Apricot label=Orange><option value=Banana label=Yellow><option value=Cherry label=Stone><option value=Date label=Sweet>'; return true})()") == true,
            "reset fixture and restore five suggestions");
        Require(m_window->Lock(), "lock input owner");
        m_view->MakeFocus(); m_window->Activate(); m_window->Unlock();
        auto rect = inspector.Evaluate("(()=>{const r=fruit.getBoundingClientRect();return [r.x,r.y,r.width,r.height]})()");
        Require(rect.is_array(), "find HTML input rectangle");
        click(BMessenger(m_view), BPoint(rect[0].get<float>() + 24, rect[1].get<float>() + rect[3].get<float>() / 2));
        wait([&] { return rows() == 5; }, "click opens five native datalist options");
        Require(inspector.Evaluate("document.activeElement===fruit && document.hasFocus()") == true, "HTML input retains keyboard focus");
        Require(m_popup.LockTargetWithTimeout(500000) == B_OK, "lock popup for anchor check");
        BLooper* looper = nullptr;
        auto* window = dynamic_cast<BWindow*>(m_popup.Target(&looper));
        BRect frame = window->Frame(); bool active = window->IsActive(); looper->Unlock();
        Require(m_window->Lock(), "lock owner for anchor check");
        const BPoint anchor = m_view->ConvertToScreen(BPoint(rect[0].get<float>(), rect[1].get<float>() + rect[3].get<float>()));
        m_window->Unlock();
        Require(!active && std::abs(frame.left - anchor.x) <= 1 && std::abs(frame.top - anchor.y - 1) <= 1,
            "popup stays below the field in logical screen coordinates without taking focus");
    }
    void accepted(Inspector& inspector, const char* value)
    {
        closed();
        const auto expected = JSON(value).dump();
        wait([&] { return inspector.Evaluate("fruit.value===" + expected + " && log.length===2 && log[0].type==='input' && log[1].type==='change' && log.every(e=>e.trusted && e.value===" + expected + ")") == true; },
            "selection sets the value and fires exactly one trusted input/change pair");
    }
    void runTests()
    {
        Require(m_window->Lock(), "lock web view for navigation");
        m_view->LoadURL(m_url.c_str()); m_window->Unlock();
        wait([&] { std::lock_guard lock(m_mutex); return std::string(m_state.GetString("url", "")) == m_url && !m_state.GetBool("loading", true); }, "datalist fixture loads", 45000000);
        Require(m_window->Lock(), "lock view for inspector");
        m_session = BWebKitInspectorSession::Create(*m_view, BMessenger(this)); m_window->Unlock();
        Inspector inspector(m_session);
        wait([&] { return inspector.Evaluate("typeof datalistResult==='function'") == true; }, "datalist fixture is ready");
        std::printf("DATALIST dpr=%s\n", inspector.Evaluate("devicePixelRatio").dump().c_str());

        open(inspector); key("a"); key("p");
        wait([&] { return rows() == 2; }, "typing filters Apple and Apricot");
        inspector.Evaluate("log.length=0");
        special(B_DOWN_ARROW, 0x62); special(B_DOWN_ARROW, 0x62); special(B_ENTER, 0x47);
        accepted(inspector, "Apricot");

        open(inspector);
        for (const char c : std::string("yellow")) { char text[] = { c, 0 }; key(text); }
        wait([&] { return rows() == 1; }, "typing matches an option label");
        inspector.Evaluate("log.length=0");
        Require(m_popup.LockTargetWithTimeout(500000) == B_OK, "lock popup for row click");
        BLooper* looper = nullptr;
        auto* popup = dynamic_cast<BWindow*>(m_popup.Target(&looper));
        auto* list = dynamic_cast<BListView*>(popup->FindView("datalist"));
        BMessenger target(list); BPoint point = list->ItemFrame(0).LeftTop() + BPoint(10, 10); looper->Unlock();
        click(target, point); accepted(inspector, "Banana");

        open(inspector); special(B_ESCAPE, 0x01); closed();
        Require(inspector.Evaluate("fruit.value==='' && log.length===0") == true, "Escape leaves input and events unchanged");
        open(inspector); click(BMessenger(m_view), BPoint(700, 450)); closed();
        open(inspector); inspector.Evaluate("scrollTo(0,300)"); closed();
        open(inspector);
        Require(m_window->Lock(), "lock owner for movement"); m_window->MoveBy(12, 10); m_window->Unlock(); closed();
        open(inspector);
        Require(m_window->Lock(), "lock view for tab switch"); m_view->Hide(); m_window->Unlock(); closed();
        Require(m_window->Lock(), "restore switched tab"); m_view->Show(); m_window->Unlock();
        open(inspector);
        m_session.reset();
        Require(m_window->Lock(), "close tab with live suggestions"); m_view->RemoveSelf(); delete m_view; m_view = nullptr; m_window->Unlock(); closed();
    }
    std::string m_url, m_bundle;
    std::shared_ptr<BWebKitContext> m_context;
    std::shared_ptr<BWebKitInspectorSession> m_session;
    BWindow* m_window { nullptr };
    BWebKitView* m_view { nullptr };
    BMessenger m_popup;
    std::thread m_worker;
    std::mutex m_mutex;
    BMessage m_state;
    std::atomic<bool> m_done { false }, m_failed { false };
    bigtime_t m_closing { 0 };
    bool m_finished { false };
};

int main(int argc, char** argv)
{
    if (argc != 3) { std::fprintf(stderr, "Usage: %s FIXTURE_URL BUNDLE\n", argv[0]); return 2; }
    int result;
    {
        status_t status;
        DataListTests app(argv, status);
        if (status != B_OK) return 1;
        app.Run(); result = app.result();
    }
    std::_Exit(result);
}

// Native typing and spelling menus with isolated, distinguishable dictionaries.
#define SUMMIT_PDF_HELPERS_ONLY
#include "ModernPDFTests.cpp"

namespace {
class PageSpellingTests final : public BApplication {
public:
    PageSpellingTests(char** argv, status_t& status)
        : BApplication("application/x-vnd.Summit-PageSpellingTests", &status),
          base(argv[1]), bundle(argv[2]), gate(argv[3]), dictionaries(argv[4]), output(argv[5]),
          verifyLearned(std::string(argv[6]) == "verify-learned") { }
    int Result() const { return failed ? 1 : 0; }
    void ReadyToRun() override
    {
        SetPulseRate(100000);
        try {
            Require(BWebKitInitialize() == B_OK, "initialize native engine");
            BWebKitSetSpellChecking(true, "en_US", dictionaries.c_str(), (dictionaries + "/words.txt").c_str());
            context = std::make_shared<BWebKitContext>(nullptr, true);
            Require(context->InitCheck() == B_OK, "private context initializes");
            window = new BWindow(BRect(30, 60, 1339, 519), "Page spelling tests", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
            for (unsigned i = 0; i < 2; ++i) {
                views[i] = new BWebKitView(BRect(i * 660, 0, i * 660 + 639, 459), "page", BMessenger(this), 0, context);
                window->AddChild(views[i]);
                Require(views[i]->InitCheck() == B_OK, "native view initializes");
            }
            window->Show();
            worker = std::thread([this] {
                try { RunChecks(); }
                catch (const std::exception& error) { std::fprintf(stderr, "PAGE_SPELLING_FAILURE %s\n", error.what()); failed = true; }
                done = true;
            });
        } catch (const std::exception& error) {
            std::fprintf(stderr, "PAGE_SPELLING_FAILURE %s\n", error.what()); failed = done = true;
        }
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == B_WEBKIT_STATE_CHANGED || message->what == B_WEBKIT_CONTEXT_MENU) {
            BMessenger sender;
            if (message->FindMessenger("view", &sender) != B_OK) return;
            std::lock_guard guard(mutex);
            for (unsigned i = 0; i < 2; ++i) {
                if (sender != BMessenger(views[i])) continue;
                if (message->what == B_WEBKIT_STATE_CHANGED) states[i] = *message;
                else { menus[i] = *message; ++menuCounts[i]; }
            }
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
            for (auto& session : sessions) session.reset();
            if (window && window->Lock()) window->Quit();
            window = nullptr; views[0] = views[1] = nullptr; context.reset();
        }
        if (system_time() - closing > 500000 && !HasHelpers(bundle) && !BWebKitHasPendingNativeUI()) {
            std::printf("PAGE_SPELLING_RESULT %s checks=%u\n", failed ? "FAIL" : "PASS", checks);
            finished = true; PostMessage(B_QUIT_REQUESTED);
        } else if (system_time() - closing > 30000000) {
            std::fputs("PAGE_SPELLING_FAILURE teardown deadline\n", stderr); _Exit(1);
        }
    }
    bool QuitRequested() override { return finished; }
private:
    void Wait(const std::function<bool()>& predicate, const char* label, bigtime_t timeout = 25000000)
    {
        const auto deadline = system_time() + timeout;
        while (system_time() < deadline) {
            if (predicate()) { Require(true, label); return; }
            snooze(30000);
        }
        Require(false, label);
    }
    BMessage State(unsigned index) { std::lock_guard guard(mutex); return states[index]; }
    void Load(unsigned index, const std::string& path)
    {
        const auto url = base + path;
        const auto generation = State(index).GetUInt64("loadGeneration", 0);
        views[index]->LoadURL(url.c_str());
        Wait([&] {
            const auto state = State(index);
            return state.GetUInt64("loadGeneration", 0) > generation && !state.GetBool("loading", true)
                && std::string(state.GetString("loadSuccessURL", "")) == url;
        }, "expected document loads");
    }
    void Click(unsigned index, BPoint point, int32 buttons = B_PRIMARY_MOUSE_BUTTON)
    {
        for (uint32 what : { B_MOUSE_DOWN, B_MOUSE_UP }) {
            BMessage event(what);
            event.AddPoint("where", point); event.AddPoint("be:view_where", point);
            event.AddInt32("buttons", what == B_MOUSE_DOWN ? buttons : 0);
            event.AddInt32("clicks", 1); event.AddInt32("modifiers", 0); event.AddInt64("when", system_time());
            Require(BMessenger(views[index]).SendMessage(&event, static_cast<BHandler*>(nullptr), 500000) == B_OK, "deliver native mouse event");
        }
    }
    void Type(unsigned index, const std::string& text)
    {
        for (uint32 what : { B_KEY_DOWN, B_KEY_UP }) {
            BMessage event(what);
            event.AddString("bytes", text.c_str()); event.AddInt32("raw_char", text.empty() ? 0 : text[0]);
            event.AddInt32("key", 0); event.AddInt32("modifiers", 0); event.AddInt64("when", system_time());
            Require(BMessenger(views[index]).SendMessage(&event, static_cast<BHandler*>(nullptr), 500000) == B_OK, "deliver native key event");
        }
    }
    BMessage Word(unsigned index, Inspector& inspector, const std::string& word, bool misspelled,
        const char* label, const std::string& target = "document.getElementById('edit')", BPoint point = BPoint(35, 40), const char* snapshot = nullptr)
    {
        inspector.Evaluate("(()=>{let e=" + target + ";if('value' in e)e.value='';else e.textContent='';e.focus();return true})()");
        Click(index, point);
        Type(index, word);
        Type(index, " ");
        Wait([&] { return inspector.Evaluate("(()=>{let e=" + target + ";return 'value' in e?e.value:e.textContent})()") == word + " "; }, "real typing reaches the intended editor");
        // Opening the word's context menu changes selection and can erase its
        // marker on non-Cocoa ports. Observe as-you-type underlining first.
        if (snapshot) Snapshot(inspector, snapshot, misspelled);
        unsigned previous;
        { std::lock_guard guard(mutex); previous = menuCounts[index]; }
        Click(index, point, B_SECONDARY_MOUSE_BUTTON);
        Wait([&] { std::lock_guard guard(mutex); return menuCounts[index] > previous; }, "native spelling context menu arrives");
        BMessage menu;
        { std::lock_guard guard(mutex); menu = menus[index]; }
        Require(menu.GetBool("editable", false), "context menu targets editable content");
        Require(menu.GetBool("misspelled", false) == misspelled, label);
        return menu;
    }
    void Guess(const BMessage& menu, const char* expected)
    {
        const char* guess = nullptr;
        bool found = false;
        for (int32 i = 0; menu.FindString("spelling_guess", i, &guess) == B_OK; ++i)
            found |= std::string(guess) == expected;
        Require(found, "suggestions come from the selected language dictionary");
    }
    void Snapshot(Inspector& inspector, const char* name, bool misspelled = false)
    {
        auto snapshot = inspector.Call("Page.snapshotRect", {{"x",0},{"y",0},{"width",600},{"height",90},{"coordinateSystem","Viewport"}});
        Require(snapshot.contains("dataURL"), "WebCore captures the editor");
        std::ofstream(output + "/" + name + ".json") << snapshot.dump();
        // Inspector snapshots intentionally omit document markers. Read the
        // actual presented frame, including underlines, and require a unique
        // painted pixel to rule out a frame from before the current edit.
        const auto blue = ++paintGate;
        inspector.Evaluate("(()=>{let m=document.getElementById('paint-gate');if(!m){m=document.createElement('div');m.id='paint-gate';document.body.append(m)}m.style.cssText='position:fixed;right:0;bottom:0;width:8px;height:8px;background:rgb(17,77," + std::to_string(blue) + ")';return true})()");
        std::unique_ptr<BBitmap> frame;
        Wait([&] {
            frame.reset(views[0]->CopyPresentedFrame());
            if (!frame || (frame->ColorSpace() != B_RGBA32 && frame->ColorSpace() != B_RGB32)) return false;
            const auto width = frame->Bounds().IntegerWidth() + 1, height = frame->Bounds().IntegerHeight() + 1;
            auto* pixel = static_cast<uint8*>(frame->Bits()) + (height - 2) * frame->BytesPerRow() + (width - 2) * 4;
            return pixel[2] == 17 && pixel[1] == 77 && pixel[0] == blue;
        }, "presented frame contains the current edit's paint gate");
        const auto width = frame->Bounds().IntegerWidth() + 1;
        const auto height = frame->Bounds().IntegerHeight() + 1;
        unsigned red = 0;
        std::ofstream image(output + "/" + name + "-presented.ppm", std::ios::binary);
        image << "P6\n" << width << ' ' << height << "\n255\n";
        for (int32 y = 0; y < height; ++y) for (int32 x = 0; x < width; ++x) {
            auto* pixel = static_cast<uint8*>(frame->Bits()) + y * frame->BytesPerRow() + x * 4;
            const char rgb[] { char(pixel[2]), char(pixel[1]), char(pixel[0]) };
            image.write(rgb, 3);
            if (pixel[2] > 140 && pixel[2] > pixel[1] * 1.5 && pixel[2] > pixel[0] * 1.5) ++red;
        }
        Require(image.good(), "save actual editor pixels");
        std::printf("SPELLING_MARKER_PIXELS %s red=%u\n", name, red);
        Require(misspelled ? red > 10 : red == 0, misspelled ? "typed typo has a visible spelling underline" : "accepted word has no spelling underline");
    }
    void RunChecks()
    {
        Load(0, "/en"); Load(1, "/en");
        Require(window->Lock(), "lock native views for inspectors");
        for (unsigned i = 0; i < 2; ++i) sessions[i] = BWebKitInspectorSession::Create(*views[i], BMessenger(this));
        window->Unlock();
        Require(bool(sessions[0]) && bool(sessions[1]), "inspectors attach to both views");
        Inspector first(sessions[0]), second(sessions[1]);
        Wait([&] { return std::string(BWebKitSpellCheckingLanguage()) == "en_US"; }, "fixture English fallback dictionary loads");
        std::puts("PAGE_SPELLING_READY"); std::fflush(stdout);
        Wait([&] { return std::filesystem::exists(gate); }, "runtime inventory gate", 180000000);
        if (verifyLearned) {
            Word(1, second, "browserword", false, "learned word survives a fresh browser process in English");
            Load(0, "/de");
            Word(0, first, "browserword", false, "learned word survives a fresh browser process in German");
            Word(0, first, "temporaryword", true, "ignored word does not survive a fresh browser process");
            return;
        }
        Word(0, first, "hello", false, "English control accepts its own word");
        Load(0, "/de");
        Word(0, first, "hallo", false, "German document language selects its dictionary",
            "document.getElementById('edit')", BPoint(35, 40), "german-correct");
        Guess(Word(0, first, "halllo", true, "German typo is marked",
            "document.getElementById('edit')", BPoint(35, 40), "german-typo"), "hallo");
        Word(1, second, "hello", false, "English view retains its own dictionary");
        Word(0, first, "hello", true, "German view does not inherit the other view's English dictionary");
        Word(1, second, "hallo", true, "English view does not inherit the German dictionary");
        Load(0, "/field");
        Word(0, first, "bonjour", false, "field language overrides English document language",
            "document.getElementById('edit')", BPoint(35, 40), "french-field-correct");
        Guess(Word(0, first, "bonjouur", true, "French field typo is marked",
            "document.getElementById('edit')", BPoint(35, 40), "french-field-typo"), "bonjour");
        Load(0, "/ancestor");
        Word(0, first, "bonjour", false, "field inherits an ancestor's language");
        Load(0, "/empty");
        Word(0, first, "hello", false, "empty field language uses system fallback");
        Load(0, "/frame");
        Word(0, first, "hallo", false, "subframe uses its own document language",
            "document.getElementById('child').contentDocument.getElementById('edit')", BPoint(35, 150), "german-frame-correct");
        Guess(Word(0, first, "halllo", true, "subframe typo is marked",
            "document.getElementById('child').contentDocument.getElementById('edit')", BPoint(35, 150), "german-frame-typo"), "hallo");
        Word(0, first, "hallo", true, "parent document remains English");
        Load(0, "/editable");
        Word(0, first, "bonjour", false, "contenteditable uses inherited language");
        Load(0, "/input");
        Word(0, first, "hallo", false, "text input uses document language");
        Load(0, "/missing");
        Word(0, first, "hello", false, "missing page dictionary uses system fallback");
        Load(0, "/invalid");
        Word(0, first, "hello", false, "invalid page language uses system fallback");
        Load(0, "/de");
        first.Evaluate("document.documentElement.lang='fr'");
        Word(0, first, "bonjour", false, "changed document language applies to subsequent typing");
        Load(0, "/disabled");
        Word(0, first, "halllo", false, "spellcheck false still suppresses checking");
        Load(0, "/password");
        Word(0, first, "halllo", false, "password field still suppresses checking");
#if SUMMIT_PAGE_SPELLING
        BWebKitSetSpellChecking(true, "en_US", dictionaries.c_str(), (dictionaries + "/words.txt").c_str(), false);
        Load(0, "/de");
        Word(0, first, "hello", false, "explicit dictionary preference overrides page language");
        Word(0, first, "hallo", true, "explicit preference does not silently switch to German");
        BWebKitSetSpellChecking(true, "en_US", dictionaries.c_str(), (dictionaries + "/words.txt").c_str(), true);
#endif
        Word(0, first, "hallo", false, "automatic page dictionary resumes after preference change");
        Word(0, first, "temporaryword", true, "unknown word is initially misspelled");
        views[0]->PerformSpellingCommand(B_WEBKIT_SPELLING_IGNORE);
        Word(0, first, "temporaryword", false, "Ignore Spelling applies to page dictionary");
        Word(1, second, "temporaryword", false, "ignored words are shared across dictionaries and views");
        Word(0, first, "browserword", true, "new personal word is initially misspelled");
        views[0]->PerformSpellingCommand(B_WEBKIT_SPELLING_LEARN);
        Word(0, first, "browserword", false, "learned word applies to loaded page dictionary");
        Word(1, second, "browserword", false, "learned word applies to the other view's dictionary");
        Require(Read(dictionaries + "/words.txt").find("browserword\n") != std::string::npos, "learned word reaches the personal word file");
        Snapshot(first, "learned-word");
    }
    std::string base, bundle, gate, dictionaries, output;
    bool verifyLearned;
    std::shared_ptr<BWebKitContext> context;
    std::shared_ptr<BWebKitInspectorSession> sessions[2];
    BWindow* window = nullptr;
    BWebKitView* views[2] { nullptr, nullptr };
    std::thread worker;
    std::mutex mutex;
    BMessage states[2], menus[2];
    unsigned menuCounts[2] { 0, 0 };
    unsigned paintGate { 100 };
    std::atomic<bool> done {false}, failed {false};
    bigtime_t closing = 0;
    bool finished = false;
};
}

int main(int argc, char** argv)
{
    if (argc != 7) return 2;
    status_t status;
    PageSpellingTests app(argv, status);
    if (status != B_OK) return 1;
    app.Run();
    return app.Result();
}

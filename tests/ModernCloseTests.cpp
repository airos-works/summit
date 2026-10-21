// Actual full Summit UI integration. No engine or dialog callbacks are mocked.
#include "ui/Messages.h"
#include <Application.h>
#include <Entry.h>
#include <InterfaceDefs.h>
#include <image.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <Path.h>
#include <Roster.h>
#include <View.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;
static int checks;
static constexpr bigtime_t timeout = 20000000;
static void Require(bool value, const std::string& label)
{
    ++checks;
    std::printf("%s %s\n", value ? "PASS" : "FAIL", label.c_str());
    std::fflush(stdout);
    if (!value) throw std::runtime_error(label);
}
static bool Wait(const std::function<bool()>& predicate, bigtime_t duration = timeout)
{
    const bigtime_t deadline = system_time() + duration;
    do {
        if (predicate()) return true;
        snooze(100000);
    } while (system_time() < deadline);
    return false;
}
static BMessage Query(const BMessenger& target, BMessage message)
{
    BMessage reply;
    if (target.SendMessage(&message, &reply, 500000, 500000) != B_OK) return {};
    return reply;
}
static BMessage Property(const BMessenger& target, const char* name, uint32 what = B_GET_PROPERTY)
{
    BMessage request(what);
    request.AddSpecifier(name);
    return Query(target, request);
}
static BMessenger View(const BMessenger& parent, const char* name)
{
    BMessage request(B_GET_PROPERTY);
    request.AddSpecifier("Messenger");
    request.AddSpecifier("View", name);
    BMessenger result;
    Query(parent, request).FindMessenger("result", &result);
    return result;
}
static BMessenger Child(const BMessenger& parent, int32 index)
{
    BMessage request(B_GET_PROPERTY);
    request.AddSpecifier("Messenger");
    request.AddSpecifier("View", index);
    BMessenger result;
    Query(parent, request).FindMessenger("result", &result);
    return result;
}
static BMessenger Window(const BMessenger& app, int32 index)
{
    BMessage request(B_GET_PROPERTY);
    request.AddSpecifier("Window", index);
    BMessenger result;
    Query(app, request).FindMessenger("result", &result);
    return result;
}
static BMessage State(const BMessenger& window) { return Query(window, BMessage(summit::kBrowserState)); }
static int32 Count(const BMessage& state) { return state.GetInt32("count", -1); }
static int64 Selected(const BMessage& state) { return state.GetInt64("selected", -1); }
static std::string String(const BMessage& message, const char* name)
{
    const char* result = nullptr;
    return message.FindString(name, &result) == B_OK ? result : "";
}
static int32 Index(const BMessage& state, int64 id)
{
    BMessage tab;
    for (int32 index = 0; state.FindMessage("tab", index, &tab) == B_OK; ++index)
        if (tab.GetInt64("id", -1) == id) return index;
    return -1;
}
static json Document(const BMessage& state, int64 id)
{
    BMessage tab;
    int32 index = Index(state, id);
    if (index < 0 || state.FindMessage("tab", index, &tab) != B_OK || tab.GetBool("loading", true)) return {};
    const std::string title = String(tab, "title");
    if (title.rfind("CLOSE ", 0)) return {};
    return json::parse(title.substr(6), nullptr, false);
}
static bool Has(const json& document, const char* key, const json& value)
{
    return document.is_object() && document.contains(key) && document[key] == value;
}
static void Send(const BMessenger& target, uint32 what, int64 id = -1, const std::string& url = {})
{
    BMessage message(what);
    if (id >= 0) message.AddInt64("id", id);
    if (!url.empty()) message.AddString("url", url.c_str());
    Require(target.IsValid() && target.SendMessage(&message, static_cast<BHandler*>(nullptr), 500000) == B_OK,
        "deliver native browser command " + std::to_string(what));
}
static void SelectTab(const BMessenger& window, int64 id)
{
    Send(window, summit::kSelectTab, id);
    // Selection is queued on the window, whereas quit goes to BApplication.
    // Observe completion before issuing a command to the other looper.
    Require(Wait([&] { return Selected(State(window)) == id; }),
        "native tab selection completes before the next operation");
}
static void Key(const BMessenger& target, const std::string& bytes, int32 code = 0)
{
    for (uint32 what : { B_KEY_DOWN, B_KEY_UP }) {
        BMessage event(what);
        event.AddString("bytes", bytes.c_str());
        event.AddInt32("raw_char", bytes.empty() ? 0 : bytes[0]);
        event.AddInt32("key", code);
        event.AddInt32("modifiers", bytes == "X" ? B_SHIFT_KEY : 0);
        event.AddInt64("when", system_time());
        const auto result = target.SendMessage(&event, static_cast<BHandler*>(nullptr), 500000);
        // A dialog button may destroy its window between key down and key up.
        if (what == B_KEY_UP && !target.IsValid()) return;
        Require(result == B_OK, what == B_KEY_DOWN ? "deliver native key down" : "deliver native key up");
    }
}
static void Click(const BMessenger& target, BPoint point)
{
    for (uint32 what : { B_MOUSE_DOWN, B_MOUSE_UP }) {
        BMessage event(what);
        event.AddPoint("where", point);
        event.AddPoint("be:view_where", point);
        event.AddInt32("buttons", what == B_MOUSE_DOWN ? B_PRIMARY_MOUSE_BUTTON : 0);
        event.AddInt32("clicks", 1);
        event.AddInt32("modifiers", 0);
        event.AddInt64("when", system_time());
        Require(target.SendMessage(&event, static_cast<BHandler*>(nullptr), 500000) == B_OK,
            what == B_MOUSE_DOWN ? "deliver native mouse down" : "deliver native mouse up");
    }
}
static BMessenger Page(const BMessenger& window, int64 id)
{
    return Child(View(window, "pages"), Index(State(window), id));
}
static BMessenger Dialog(const BMessenger& app)
{
    // Native Window name scripting uses the looper name ("w>..."), not
    // its title. Enumerate only the explicit team and inspect the public title.
    for (int32 index = 0; index < 16; ++index) {
        auto dialog = Window(app, index);
        if (!dialog.IsValid()) break;
        if (String(Property(dialog, "Title"), "result") != "Web page dialog") continue;
        if (String(Property(View(dialog, "dialog-cancel"), "Label"), "result") == "Stay"
            && String(Property(View(dialog, "dialog-accept"), "Label"), "result") == "Leave") return dialog;
    }
    return {};
}
static BMessenger Prompt(const BMessenger& app, const BMessenger& window, int64 id, int attempts)
{
    BMessenger result;
    const auto document = Document(State(window), id);
    Require(document.is_object() && document.contains("instance") && Has(document, "armed", true),
        "pending close belongs to an activated live fixture");
    const bool ready = Wait([&] {
        result = Dialog(app);
        const auto state = State(window);
        const auto current = Document(state, id);
        // Document::updateTitle queues a DOM task. A synchronous beforeunload
        // dialog can hold that task until the user decides. Observe the real
        // native prompt now; check the exact new counter after cancellation.
        return result.IsValid() && Selected(state) == id
            && Has(current, "instance", document["instance"])
            && (Has(current, "attempts", attempts - 1) || Has(current, "attempts", attempts));
    });
    if (!ready) {
        const auto state = State(window);
        std::fprintf(stderr, "PROMPT_STATE dialog=%d expectedTab=%lld selected=%lld document=%s\n",
            result.IsValid(), static_cast<long long>(id), static_cast<long long>(Selected(state)),
            Document(state, id).dump().c_str());
        state.PrintToStream();
    }
    Require(ready, "actual beforeunload dialog selects expected live tab, attempt " + std::to_string(attempts));
    return result;
}
static void SettledAttempts(const BMessenger& window, int64 id, int attempts)
{
    Require(Wait([&] { return Has(Document(State(window), id), "attempts", attempts); }),
        "resumed document reports exact beforeunload count " + std::to_string(attempts));
}
static void Decide(const BMessenger& dialog, bool leave)
{
    auto button = View(dialog, leave ? "dialog-accept" : "dialog-cancel");
    Require(button.IsValid(), leave ? "native Leave control exists" : "native Stay control exists");
    // A real native button key event invokes its installed action.
    Key(button, " ", 0x5e);
    Require(Wait([&] { return !dialog.IsValid(); }), "native beforeunload dialog is destroyed");
}
static std::pair<int32, int32> Selection(const BMessenger& text)
{
    BMessage reply = Property(text, "selection");
    int32 start = -1, end = -1;
    reply.FindInt32("result", 0, &start);
    reply.FindInt32("result", 1, &end);
    return { start, end };
}
static void SelectText(const BMessenger& text, int32 start, int32 end)
{
    BMessage request(B_SET_PROPERTY), specifier(B_DIRECT_SPECIFIER);
    specifier.AddString("property", "selection");
    specifier.AddInt32("index", start);
    specifier.AddInt32("range", end - start);
    request.AddSpecifier(&specifier);
    Require(Query(text, request).GetInt32("error", B_ERROR) == B_OK, "set native address text selection");
}
static void Draft(const BMessenger& window, const BMessenger& address, const std::string& draft)
{
    Send(window, summit::kFocusAddress);
    Key(address, draft);
    Require(Wait([&] { return String(State(window), "address") == draft; }), "address contains unsubmitted draft");
    SelectText(address, 2, 7);
}
static void CheckDraft(const BMessenger& window, const BMessenger& address, int64 selected, const std::string& draft)
{
    Require(Selected(State(window)) == selected && String(State(window), "address") == draft,
        "background close preserves selected tab and address draft");
    Require(Selection(address) == std::pair<int32, int32>(2, 7), "background close preserves native text selection");
    Send(window, B_SELECT_ALL);
    Require(Wait([&] { return Selection(address) == std::pair<int32, int32>(0, static_cast<int32>(draft.size())); }),
        "browser editing command still targets focused address field");
}
static void Session(const std::filesystem::path& profile, const std::vector<std::string>& urls, size_t selected)
{
    // SharedProfile batches saves on a worker for 250 ms. The native close
    // reply settles the UI transaction before that write necessarily finishes.
    json data;
    Require(Wait([&] {
        std::ifstream stream(profile / "profile.json");
        data = json::parse(stream, nullptr, false);
        if (!data.is_object() || !data.contains("tabs") || !data["tabs"].is_array()
            || data["tabs"].size() != urls.size() || !data.contains("selected")
            || data["selected"] != selected)
            return false;
        for (size_t i = 0; i < urls.size(); ++i)
            if (!data["tabs"][i].is_object() || !data["tabs"][i].contains("url")
                || data["tabs"][i]["url"] != urls[i])
                return false;
        return true;
    }, 5000000), "cancelled session reaches the saved profile within five seconds");
    Require(data.is_object() && data.contains("tabs") && data["tabs"].size() == urls.size(),
        "saved session retains every original tab after cancellation");
    Require(data.contains("selected") && data["selected"] == selected,
        "saved session retains the intended selected tab");
    for (size_t i = 0; i < urls.size(); ++i)
        Require(data["tabs"][i]["url"] == urls[i], "saved session retains original tab order and URL");
}

static void Cleanup(const BMessenger& app, team_id team)
{
    // Failed assertions remain failures. Give only our verified test team the
    // same queued close/Leave sequence before the runner resorts to termination.
    const bigtime_t deadline = system_time() + 15000000;
    while (system_time() < deadline) {
        team_info current;
        if (get_team_info(team, &current) != B_OK) return;
        BMessage quit(B_QUIT_REQUESTED);
        app.SendMessage(&quit, static_cast<BHandler*>(nullptr), 100000);
        try {
            if (auto dialog = Dialog(app); dialog.IsValid())
                Key(View(dialog, "dialog-accept"), " ", 0x5e);
        } catch (...) { }
        snooze(100000);
    }
}

int main(int argc, char** argv)
{
    if (argc == 4 && std::string(argv[1]) == "--close-browser") {
        BApplication application("application/x-vnd.Summit-close-owned-browser");
        const auto team = static_cast<team_id>(std::strtol(argv[2], nullptr, 10));
        app_info info;
        BMessenger app(nullptr, team);
        if (team <= 0 || !app.IsValid() || be_roster->GetRunningAppInfo(team, &info) != B_OK) return 1;
        BPath executable(&info.ref);
        std::error_code error;
        auto actual = std::filesystem::canonical(executable.Path(), error);
        if (error) return 1;
        auto expected = std::filesystem::canonical(argv[3], error);
        if (error || actual != expected) return 1;
        return app.SendMessage(B_QUIT_REQUESTED) == B_OK ? 0 : 1;
    }
    if (argc == 3 && std::string(argv[1]) == "--check-executable-unused") {
        const auto expected = std::filesystem::canonical(argv[2]);
        int32 cookie = 0;
        team_info team;
        while (get_next_team_info(&cookie, &team) == B_OK) {
            int32 imageCookie = 0;
            image_info image;
            while (get_next_image_info(team.team, &imageCookie, &image) == B_OK) {
                if (image.type != B_APP_IMAGE) continue;
                std::error_code error;
                if (std::filesystem::canonical(image.name, error) == expected && !error) {
                    std::fprintf(stderr, "FAIL frozen executable already belongs to team %ld; refusing to launch or target it\n", static_cast<long>(team.team));
                    return 1;
                }
            }
        }
        std::puts("PASS frozen executable has no existing team");
        return 0;
    }
    if (argc != 6) {
        std::fprintf(stderr, "Usage: ModernCloseTests TEAM BROWSER_EXECUTABLE BASE_URL RUN_TOKEN PROFILE\n");
        return 2;
    }
    status_t applicationStatus;
    BApplication application("application/x-vnd.Kunanyi-Summit-modern-close-tests", &applicationStatus);
    if (applicationStatus != B_OK) {
        std::fprintf(stderr, "FAIL initialize native close harness: %ld\n", static_cast<long>(applicationStatus));
        return 1;
    }
    BMessenger app;
    const team_id team = static_cast<team_id>(std::strtol(argv[1], nullptr, 10));
    bool verifiedTeam = false;
    try {
        Require(team > 0, "explicit browser team supplied");
        app_info info;
        Require(Wait([&] { app = BMessenger(nullptr, team); return be_roster->GetRunningAppInfo(team, &info) == B_OK && app.IsValid(); }),
            "explicit browser team registered with native roster");
        BPath executable(&info.ref);
        Require(executable.InitCheck() == B_OK && std::filesystem::canonical(executable.Path()) == std::filesystem::canonical(argv[2]),
            "target team belongs to the exact frozen Summit executable");
        verifiedTeam = true;
        BMessenger window;
        Require(Wait([&] {
            // Only enumerate windows belonging to the explicitly supplied team.
            for (int32 i = 0; i < 8; ++i) {
                auto candidate = Window(app, i);
                if (!candidate.IsValid()) break;
                if (String(State(candidate), "backend") == "modern") { window = candidate; return true; }
            }
            return false;
        }), "full modern browser window responds through native scripting");
        Require(window.Team() == team && Count(State(window)) == 1, "isolated test profile starts with one tab");
        const std::string prefix = std::string(argv[3]) + "/native-close.html?run=" + argv[4];
        const std::string urlA = prefix + "&name=A&guard=1";
        const std::string urlB = prefix + "&name=B&guard=1";
        const std::string urlC = prefix + "&name=C&guard=0";
        const auto a = Selected(State(window));
        Send(window, summit::kFocusAddress);
        auto initialAddress = Child(View(window, "address"), 0);
        Require(initialAddress.IsValid(), "native address editor is available");
        Key(initialAddress, urlA);
        Key(initialAddress, std::string(1, B_ENTER), 0x47);
        Require(Wait([&] { return Has(Document(State(window), a), "name", "A"); }),
            "Enter in the native address editor loads the real HTTP/JavaScript fixture");
        Click(Page(window, a), BPoint(40, 40));
        Require(Wait([&] { return Has(Document(State(window), a), "armed", true); }), "native click activates A beforeunload protection");
        Key(Page(window, a), "X", 0x4d);
        Require(Wait([&] { return Has(Document(State(window), a), "value", "X"); }), "native typing creates live editable state and an undo item");
        const auto originalA = Document(State(window), a);
        Send(window, summit::kNewTab, -1, urlB);
        Require(Wait([&] { return Count(State(window)) == 2 && Has(Document(State(window), Selected(State(window))), "name", "B"); }), "page B loads independently");
        const auto b = Selected(State(window));
        Click(Page(window, b), BPoint(40, 40));
        Require(Wait([&] { return Has(Document(State(window), b), "armed", true); }), "native click activates B beforeunload protection");
        const auto originalB = Document(State(window), b);
        SelectTab(window, a);
        Send(app, B_QUIT_REQUESTED);
        Decide(Prompt(app, window, a, 1), true);
        auto second = Prompt(app, window, b, 1);
        Require(Count(State(window)) == 2 && Has(Document(State(window), a), "instance", originalA["instance"])
            && Has(Document(State(window), a), "value", "X"), "earlier approved tab remains the same live edited document");
        Decide(second, false);
        Require(Wait([&] { return Count(State(window)) == 2 && Selected(State(window)) == a
            && !State(window).GetBool("closing", true); }), "window cancellation retains both original live tabs and selection");
        SettledAttempts(window, a, 1);
        SettledAttempts(window, b, 1);
        Require(Has(Document(State(window), b), "instance", originalB["instance"]), "cancelling tab retains its original document instance");
        Session(argv[5], { urlA, urlB }, 0);
        Send(window, B_UNDO);
        Require(Wait([&] { return Has(Document(State(window), a), "value", "seed"); }), "earlier provisional approval preserved native WebKit undo state");
        Send(app, B_QUIT_REQUESTED);
        Decide(Prompt(app, window, a, 2), false);
        Require(Wait([&] { return Count(State(window)) == 2 && !State(window).GetBool("closing", true); }),
            "reset approval runs beforeunload again and permits another cancellation");
        SettledAttempts(window, a, 2);

        Send(window, summit::kNewTab, -1, urlC);
        Require(Wait([&] { return Count(State(window)) == 3 && Has(Document(State(window), Selected(State(window))), "name", "C"); }), "unguarded background-close fixture loads");
        auto c = Selected(State(window));
        SelectTab(window, a);
        auto address = Child(View(window, "address"), 0);
        Require(address.IsValid(), "native address text view is scriptable");
        const std::string draft = "unsubmitted close-test draft";
        Draft(window, address, draft);
        Send(window, summit::kCloseTab, c);
        Require(Wait([&] { return Count(State(window)) == 2; }), "unguarded background tab closes without a prompt");
        Require(!Dialog(app).IsValid(), "unguarded background close creates no dialog");
        CheckDraft(window, address, a, draft);

        SelectText(address, 2, 7);
        Send(window, summit::kCloseTab, b);
        Decide(Prompt(app, window, b, 2), false);
        Require(Wait([&] { return Selected(State(window)) == a; }), "cancelled background prompt restores the prior selected tab");
        SettledAttempts(window, b, 2);
        CheckDraft(window, address, a, draft);
        Send(window, summit::kNewTab, -1, urlC);
        Require(Wait([&] { return Count(State(window)) == 3 && Has(Document(State(window), Selected(State(window))), "name", "C"); }), "newer-selection fixture loads");
        c = Selected(State(window));
        SelectTab(window, a);
        Send(window, summit::kCloseTab, b);
        auto background = Prompt(app, window, b, 3);
        SelectTab(window, c);
        Require(Wait([&] { return Selected(State(window)) == c; }), "newer tab selection takes effect while close decision is pending");
        Decide(background, false);
        Require(Wait([&] { return !State(window).GetBool("closing", true); }), "background close cancellation reaches the browser");
        Require(Selected(State(window)) == c && Count(State(window)) == 3,
            "close cancellation does not override newer user tab selection");
        SettledAttempts(window, b, 3);

        // C has no beforeunload handler: it approves first without a prompt.
        // Navigation of that provisionally approved tab must invalidate quit
        // while A's outstanding close decision settles.
        SelectTab(window, c);
        Send(app, B_QUIT_REQUESTED);
        auto navigationPending = Prompt(app, window, a, 3);
        SelectTab(window, c);
        const std::string urlD = prefix + "&name=D&guard=0";
        Send(window, summit::kNavigate, -1, urlD);
        Require(Wait([&] { return Has(Document(State(window), c), "name", "D"); }),
            "provisionally approved tab navigates to a real replacement document");
        const auto replacement = Document(State(window), c);
        Decide(navigationPending, true);
        Require(Wait([&] { return Count(State(window)) == 3 && Selected(State(window)) == c
            && !State(window).GetBool("closing", true); }),
            "navigation invalidates whole-window quit after its pending decision settles");
        Require(!Dialog(app).IsValid() && Has(Document(State(window), c), "instance", replacement["instance"]),
            "replacement document survives the stale close approval without another tab prompt");
        SettledAttempts(window, a, 3);
        Session(argv[5], { urlA, urlB, urlD }, 2);

        SelectTab(window, a);
        Send(app, B_QUIT_REQUESTED);
        Decide(Prompt(app, window, a, 4), true);
        Decide(Prompt(app, window, b, 4), true);
        Require(Wait([&] { team_info current; return get_team_info(team, &current) != B_OK; }),
            "final approved window close drains native UI and exits the exact browser team");
        std::printf("CLOSE_RESULT PASS checks=%d\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL integration scenario: %s\n", error.what());
        if (verifiedTeam) Cleanup(app, team);
        std::fprintf(stderr, "CLOSE_RESULT FAIL checks=%d reason=%s\n", checks, error.what());
        return 1;
    }
}

/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "NativeDataListSuggestionsHaiku.h"
#include <Application.h>
#include <Roster.h>
#include <WindowScreen.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

using Popup = WebKit::NativeDataListSuggestionsHaiku;
namespace {
constexpr uint32 completed = 'dldt';
constexpr std::array names { "keyboard", "up-wrap", "mouse", "filter-update", "escape", "enter-no-selection",
    "owner-destroyed", "cancel-before-start", "invalid-owner", "empty-list", "invalid-anchor", "keys-before-start" };
}
class Tests final : public BApplication {
public:
    Tests() : BApplication("application/x-vnd.Kunanyi-Summit-DataListTests") { }
    int result() const { return m_failed || !m_done; }
    void ReadyToRun() override
    {
        SetPulseRate(100000);
        std::printf("DATALIST mouse_mode=%d focus_follows_mouse=%d\n", mouse_mode(), focus_follows_mouse());
        be_roster->ActivateApp(Team());
        next();
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what != completed) { BApplication::MessageReceived(message); return; }
        check(message->GetInt32("test", -1) == static_cast<int32>(m_test), "completion belongs to this test");
        check(++m_completions == 1, "completion occurs exactly once");
        const char* value = nullptr;
        bool selected = message->FindString("value", &value) == B_OK;
        if (m_test <= 3 || m_test == 11) {
            check(selected, "selection produces a value");
            const char* expected = m_test == 1 ? "Elderberry" : m_test == 2 ? "Cherry" : m_test == 3 ? "Filtered" : "Banana";
            check(selected && std::string(value) == expected, "selected value matches the visible row");
            check(message->GetUInt64("generation", 0) == (m_test == 3 ? 2u : 1u), "selection retains its result generation");
        } else check(!selected, "dismissal does not choose a value");
        m_finished = system_time();
        m_popup.reset();
    }
    void Pulse() override
    {
        if (m_done) return;
        if (m_finished) {
            if (Popup::activeCount() || system_time() - m_finished < 150000) return;
            if (m_owner.IsValid()) { m_owner.SendMessage(B_QUIT_REQUESTED); return; }
            check(m_completions == 1, "no delayed duplicate completion");
            std::printf("DATALIST %s complete\n", names[m_test]); std::fflush(stdout);
            if (++m_test == names.size() || m_timedOut) { finish(); return; }
            next();
            return;
        }
        if (system_time() - m_started > 6000000) {
            if (m_owner.LockTargetWithTimeout(0) == B_OK) {
                BLooper* looper = nullptr;
                auto* owner = dynamic_cast<BWindow*>(m_owner.Target(&looper));
                if (owner) std::printf("owner active=%d hidden=%d minimized=%d workspaces=%lu current=%ld\n",
                    owner->IsActive(), owner->IsHidden(), owner->IsMinimized(), static_cast<unsigned long>(owner->Workspaces()), static_cast<long>(current_workspace()));
                if (looper) looper->Unlock();
            }
            check(false, "native test finishes before its deadline");
            m_timedOut = true;
            if (m_popup) m_popup->cancel();
            else { m_finished = system_time(); }
            return;
        }
        if (!m_popup) {
            if (system_time() - m_started < 250000 || !active(m_owner)) return;
            startPopup();
            return;
        }
        if (m_acted || m_test >= 7) return;
        auto target = m_popup->target();
        if (target.LockTargetWithTimeout(0) != B_OK) return;
        BLooper* looper = nullptr;
        auto* window = dynamic_cast<BWindow*>(target.Target(&looper));
        auto* list = window ? dynamic_cast<BListView*>(window->FindView("datalist")) : nullptr;
        if (!window || window->IsHidden() || !list) { if (looper) looper->Unlock(); return; }
        check(!window->IsActive(), "suggestions do not take keyboard focus");
        check(active(m_owner), "text field owner keeps keyboard focus");
        if (m_test == 3 && !m_updated) {
            check(list->CountItems() == 5, "initial list contains all five options");
            looper->Unlock();
            m_popup->update({ { "Filtered", "Matching label" } }, BRect(230, 185, 530, 212), 2);
            m_updated = true;
            return;
        }
        if (m_test == 3 && list->CountItems() != 1) { looper->Unlock(); return; }
        check(list->CountItems() == (m_test == 3 ? 1 : 5), "native list reflects the current filter");
        if (m_test == 2) list->MouseDown(list->ItemFrame(2).LeftTop() + BPoint(8, 8));
        looper->Unlock();
        m_acted = true;
        switch (m_test) {
        case 0: m_popup->key(Popup::Key::Down); m_popup->key(Popup::Key::Down); m_popup->key(Popup::Key::Enter); break;
        case 1: m_popup->key(Popup::Key::Up); m_popup->key(Popup::Key::Enter); break;
        case 3: m_popup->key(Popup::Key::Down); m_popup->key(Popup::Key::Enter); break;
        case 4: m_popup->key(Popup::Key::Escape); break;
        case 5: m_popup->key(Popup::Key::Enter); break;
        case 6: m_owner.SendMessage(B_QUIT_REQUESTED); break;
        default: break;
        }
    }
private:
    static bool active(BMessenger target)
    {
        if (target.LockTargetWithTimeout(0) != B_OK) return false;
        BLooper* looper = nullptr;
        auto* window = dynamic_cast<BWindow*>(target.Target(&looper));
        bool result = window && window->IsActive();
        if (looper) looper->Unlock();
        return result;
    }
    void next()
    {
        m_started = system_time(); m_finished = 0; m_completions = 0; m_acted = m_updated = false;
        auto* window = new BWindow(BRect(200, 120, 800, 500), "Datalist native tests", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
        auto* view = new BView(window->Bounds(), "field", B_FOLLOW_ALL, B_WILL_DRAW | B_NAVIGABLE);
        view->SetViewColor(245, 245, 245);
        window->AddChild(view);
        if (!m_test) {
            uint32 buttons;
            view->GetMouse(&m_originalPointer, &buttons, false);
            view->ConvertToScreen(&m_originalPointer);
        }
        view->MakeFocus();
        m_owner = BMessenger(window); m_view = BMessenger(view);
        window->Show();
        // Show starts the native looper and releases its construction lock.
        if (window->Lock()) { window->Activate(); window->Unlock(); }
        set_mouse_position(250, 400);
        std::printf("DATALIST starting %s\n", names[m_test]); std::fflush(stdout);
    }
    void startPopup()
    {
        auto app = BMessenger(this);
        m_popup = std::make_unique<Popup>(m_test == 8 ? BMessenger() : m_view,
            [app, test = m_test](std::optional<std::string> value, uint64_t generation) {
                BMessage message(completed); message.AddInt32("test", test); message.AddUInt64("generation", generation);
                if (value) message.AddString("value", value->c_str());
                app.SendMessage(&message);
            });
        std::vector<Popup::Item> items { { "Apple", "Red fruit" }, { "Banana", "Yellow fruit" }, { "Cherry", "Stone fruit" }, { "Date", "Sweet fruit" }, { "Elderberry", "Purple fruit" } };
        if (m_test == 9) items.clear();
        BRect anchor(230, 185, 530, 212);
        if (m_test == 10) anchor.right = std::numeric_limits<float>::infinity();
        m_popup->update(std::move(items), anchor, 1);
        if (m_test == 7) m_popup->cancel();
        if (m_test == 11) {
            m_popup->key(Popup::Key::Down); m_popup->key(Popup::Key::Down); m_popup->key(Popup::Key::Enter);
        }
        check(m_popup->start() == B_OK, "native worker starts");
    }
    void check(bool okay, const char* text)
    {
        std::printf("%s %s: %s\n", okay ? "PASS" : "FAIL", names[m_test], text);
        if (!okay) ++m_failed;
        std::fflush(stdout);
    }
    void finish()
    {
        m_done = true;
        set_mouse_position(m_originalPointer.x, m_originalPointer.y);
        std::printf("DATALIST RESULT: %u failures\n", m_failed); std::fflush(stdout);
        PostMessage(B_QUIT_REQUESTED);
    }
    BMessenger m_owner, m_view;
    BPoint m_originalPointer;
    std::unique_ptr<Popup> m_popup;
    size_t m_test { 0 };
    unsigned m_failed { 0 }, m_completions { 0 };
    bigtime_t m_started { 0 }, m_finished { 0 };
    bool m_acted { false }, m_updated { false }, m_done { false }, m_timedOut { false };
};
int main()
{
    int result;
    { Tests tests; tests.Run(); result = tests.result(); }
    std::_Exit(result);
}

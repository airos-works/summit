#if SUMMIT_MODERN_WEBKIT
// Fraudulent and malicious website warnings (#150): a page on one of the block
// lists (SafeBrowsingService.h) is replaced in its tab by a warning, as a page
// that could not be loaded is by its error page, with Go Back and Visit This
// Site Anyway (which allows the site until Summit quits).
#include "BrowserWindow.h"
#include "SafeBrowsingService.h"
#include "core/LoadError.h"
#include <Button.h>
#include <CardLayout.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <StringView.h>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace summit {
namespace {
constexpr uint32 kSafetyGoBack = 'sbgb', kSafetyVisit = 'sbvs';
// A warning red, with white text, whatever the system's colours: it must
// not look like an ordinary page.
constexpr rgb_color kWarningBackground = {162, 28, 28, 255};
constexpr rgb_color kWarningText = {255, 255, 255, 255};
constexpr size_t kLineLength = 72;

std::vector<std::string> Wrap(const std::string& text)
{
    std::vector<std::string> lines;
    std::string line;
    size_t start = 0;
    while (start < text.size()) {
        auto end = text.find(' ', start);
        if (end == std::string::npos) end = text.size();
        const std::string word = text.substr(start, end - start);
        if (!line.empty() && line.size() + 1 + word.size() > kLineLength) {
            lines.push_back(line);
            line.clear();
        }
        line += (line.empty() ? "" : " ") + word;
        start = end + 1;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

BStringView* Line(const char* name, const std::string& text, uint32 truncation)
{
    auto* view = new BStringView(name, text.c_str());
    view->SetHighColor(kWarningText);
    view->SetTruncation(truncation);
    view->SetExplicitMinSize(BSize(100, B_SIZE_UNSET));
    view->SetExplicitAlignment(BAlignment(B_ALIGN_HORIZONTAL_CENTER, B_ALIGN_MIDDLE));
    return view;
}

struct WarningText {
    std::string title, explanation;
};

WarningText Describe(const std::string& reason, const std::string& url)
{
    const std::string host = LoadFailureHost(url);
    if (reason == "malware")
        return {"Dangerous Website Warning", host + " is on a list of sites that spread harmful software. Visiting it "
            "may install programs that steal your information or damage your computer."};
    return {"Deceptive Website Warning", host + " is on a list of sites that pretend to be someone else, to trick you "
        "into giving away your passwords, credit card numbers or other personal information."};
}

// What the page area of a tab shows instead of a listed page.
class SafetyWarningView : public BView {
public:
    SafetyWarningView(int64 tab, const WarningText& text, const std::string& address)
        : BView("safety-warning", B_WILL_DRAW)
    {
        SetViewColor(kWarningBackground);
        SetLowColor(kWarningBackground);
        auto* heading = Line("heading", text.title, B_TRUNCATE_END);
        BFont font(be_bold_font);
        font.SetSize(font.Size() * 1.8f);
        heading->SetFont(&font);
        auto* lines = new BGroupView(B_VERTICAL, 2);
        lines->SetViewColor(kWarningBackground);
        for (const auto& line : Wrap(text.explanation)) lines->AddChild(Line("explanation", line, B_TRUNCATE_END));
        lines->AddChild(Line("advice", "Summit recommends that you go back.", B_TRUNCATE_END));
        auto message = [tab](uint32 what) {
            auto* made = new BMessage(what);
            made->AddInt64("tab", tab);
            return made;
        };
        fBack = new BButton("go-back", "Go Back", message(kSafetyGoBack));
        fVisit = new BButton("visit-anyway", "Visit This Site Anyway", message(kSafetyVisit));
        auto* note = Line("source", "Checked against block lists kept on this computer (Preferences › Security).",
            B_TRUNCATE_END);
        BLayoutBuilder::Group<>(this, B_VERTICAL, 12)
            .AddGlue(1)
            .Add(heading)
            .Add(lines)
            .Add(Line("address", address, B_TRUNCATE_MIDDLE))
            .AddGroup(B_HORIZONTAL, 12)
                .AddGlue()
                .Add(fBack)
                .Add(fVisit)
                .AddGlue()
            .End()
            .AddStrut(8)
            .Add(note)
            .AddGlue(2)
            .SetInsets(16);
        SetExplicitMinSize(BSize(160, 100));
        SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    }
    void AttachedToWindow() override
    {
        BView::AttachedToWindow();
        fBack->SetTarget(Window());
        fVisit->SetTarget(Window());
        // Whole pixels for each line, as on the error page: the layout
        // rounds a string view's width down and cuts its last letters. The
        // large bold heading's last letter reaches further than its width
        // says (the "g" of "Warning" lost its right side): room to spare.
        std::function<void(BView*)> measure = [&](BView* parent) {
            for (int32 i = 0; i < parent->CountChildren(); ++i) {
                BView* child = parent->ChildAt(i);
                if (auto* line = dynamic_cast<BStringView*>(child)) {
                    BFont font;
                    line->GetFont(&font);
                    const float width = std::ceil(line->StringWidth(line->Text()) + font.Size() * 0.25f) + 2;
                    line->SetExplicitPreferredSize(BSize(width, B_SIZE_UNSET));
                    line->SetExplicitMaxSize(BSize(width, B_SIZE_UNSET));
                }
                measure(child);
            }
        };
        measure(this);
    }
    void FocusButton() { fBack->MakeFocus(); }
    bool HasFocus(BView* focus) const
    {
        for (; focus; focus = focus->Parent()) if (focus == this) return true;
        return false;
    }
private:
    BButton* fBack;
    BButton* fVisit;
};
}

bool BrowserWindow::SafeBrowsingMessage(BMessage& message)
{
    switch (message.what) {
#ifdef B_WEBKIT_HAS_NAVIGATION_CHECK
    case B_WEBKIT_NAVIGATION_BLOCKED: {
        BMessenger sender;
        if (message.FindMessenger("view", &sender) != B_OK) return true;
        if (auto* tab = FindTab(sender))
            ShowSafetyWarning(*tab, message.GetString("url", ""), message.GetString("reason", "phishing"));
        return true;
    }
#endif
    case kSafetyGoBack:
    case kSafetyVisit: {
        auto* tab = FindTabByID(message.GetInt64("tab", 0));
        if (!tab || !tab->safetyView) return true;
        if (message.what == kSafetyGoBack) {
            LeaveSafetyWarning(*tab);
            return true;
        }
        // The user's choice: the site is not checked again until Summit quits.
        const std::string url = tab->safetyURL;
        std::fprintf(stderr, "Summit: visiting %s despite the %s warning\n", url.c_str(), tab->safetyReason.c_str());
        SafeBrowsingService::Shared().AllowHost(url);
        RemoveSafetyWarning(*tab);
        if (PrepareTabNavigation(*tab)) tab->view->LoadURL(url.c_str());
        return true;
    }
    default:
        return false;
    }
}

void BrowserWindow::UpdateSafetyWarning(Tab& tab)
{
    // Engines without the navigation check: a listed page is stopped as soon
    // as the tab says it loads it, before it can show anything.
    if (!SafeBrowsingService::EngineChecks() && tab.loading && tab.url != tab.safetyURL) {
        const auto threat = SafeBrowsingService::Shared().Check(tab.url);
        if (threat != safebrowsing::Threat::None) {
            tab.view->Stop();
            ShowSafetyWarning(tab, tab.url, safebrowsing::ThreatName(threat));
            return;
        }
    }
    if (!tab.safetyView) return;
    // Another page took the tab's place: the warning goes once it shows. A
    // load that ends without a page (the blocked one) leaves the warning.
    const bool shows = tab.loadOutcome == "succeeded" || (tab.loading && tab.progress > 0.11f);
    if (tab.loadGeneration != tab.safetyGeneration && shows && tab.url != tab.safetyURL) {
        RemoveSafetyWarning(tab, false);
        return;
    }
    if (!tab.loading) tab.safetyGeneration = tab.loadGeneration;
}

void BrowserWindow::ShowSafetyWarning(Tab& tab, const std::string& url, const std::string& reason)
{
    std::fprintf(stderr, "Summit: %s warning for %s\n", reason.c_str(), url.c_str());
    RemoveLoadErrorView(tab, false);
    const auto text = Describe(reason, url);
    auto* old = static_cast<SafetyWarningView*>(tab.safetyView);
    const bool hadFocus = old && old->HasFocus(CurrentFocus());
    auto* view = new SafetyWarningView(tab.id, text, DisplayURL(url));
    const int32 index = old ? fCards->IndexOfView(old) : -1;
    if (old) {
        old->RemoveSelf();
        delete old;
    }
    if (index >= 0) fCards->AddView(index, view);
    else fCards->AddView(view);
    tab.safetyView = view;
    tab.safetyURL = url;
    tab.safetyReason = reason;
    tab.safetyTitle = text.title;
    tab.safetyGeneration = tab.loadGeneration;
    // Whether the tab had a page before, to go back to.
    tab.safetyBack = tab.loadSuccessSequence > 0;
    // The status line says it too, instead of "Ready", while the warning is up.
    tab.notice = text.title;
    tab.noticeUntil = B_INFINITE_TIMEOUT;
    if (tab.id == fSelected) {
        fCards->SetVisibleItem(fCards->IndexOfView(view));
        // Go Back has the focus (Return), unless the user is elsewhere; the
        // address entered stays in the field, to correct.
        if (hadFocus || !CurrentFocus() || CurrentFocus() == tab.view || CurrentFocus() == fAddress->TextView())
            view->FocusButton();
        if (!fAddress->TextView()->IsFocus()) fAddress->SetText(DisplayURL(url).c_str());
        ShowTabStatus(tab);
    }
    RefreshChrome();
}

void BrowserWindow::RemoveSafetyWarning(Tab& tab, bool focus)
{
    if (!tab.safetyView) return;
    auto* view = static_cast<SafetyWarningView*>(tab.safetyView);
    if (tab.id == fSelected) {
        fCards->SetVisibleItem(fCards->IndexOfView(tab.crashView ? tab.crashView : tab.errorView ? tab.errorView : tab.view));
        if (focus || view->HasFocus(CurrentFocus())) tab.view->MakeFocus();
    }
    tab.safetyView = nullptr;
    tab.safetyURL.clear();
    tab.safetyReason.clear();
    if (tab.notice == tab.safetyTitle) {
        tab.notice.clear();
        tab.noticeUntil = 0;
    }
    view->RemoveSelf();
    delete view;
    if (tab.id == fSelected && !fAddress->TextView()->IsFocus()) fAddress->SetText(DisplayURL(tab.ShownURL()).c_str());
    RefreshChrome();
}

void BrowserWindow::ShowSafetyWarningCard(Tab& tab)
{
    if (tab.safetyView && tab.id == fSelected) fCards->SetVisibleItem(fCards->IndexOfView(tab.safetyView));
}

void BrowserWindow::LeaveSafetyWarning(Tab& tab)
{
    // The listed page never loaded: the page before it is still there.
    const bool back = tab.safetyBack;
    RemoveSafetyWarning(tab);
    if (!back && PrepareTabNavigation(tab)) tab.view->LoadURL(LoadableURL(HomeAddress()).c_str());
    if (tab.id == fSelected) ShowTabStatus(tab);
}
}
#endif

#if SUMMIT_MODERN_WEBKIT
// Error pages (#89): a page that could not be loaded is replaced in its tab
// by a view that says why, with Try Again, as a crashed page is (#71). A page
// that failed for want of a network loads again by itself when it is back.
// Also the media document that takes a load over (#95).
#include "BrowserWindow.h"
#include "FeatureMessages.h"
#include "SharedProfile.h"
#include "core/LoadError.h"
#include <Button.h>
#include <CardLayout.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <NetworkAddress.h>
#include <NetworkInterface.h>
#include <NetworkRoster.h>
#include <StringView.h>
#include <GroupView.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>
#include <net/if.h>
#include <netinet/in.h>

namespace summit {
namespace {
// Whether the computer is on a network: an interface other than loopback
// that is up, has a link and an address that is not link-local.
bool NetworkIsUp()
{
    BNetworkRoster& roster = BNetworkRoster::Default();
    BNetworkInterface interface;
    uint32 cookie = 0;
    while (roster.GetNextInterface(&cookie, interface) == B_OK) {
        const uint32 flags = interface.Flags();
        if ((flags & IFF_LOOPBACK) || !(flags & IFF_UP) || !(flags & IFF_LINK)) continue;
        for (int32 i = 0; i < interface.CountAddresses(); ++i) {
            BNetworkInterfaceAddress address;
            if (interface.GetAddressAt(i, address) != B_OK) continue;
            const BNetworkAddress& where = address.Address();
            if (where.IsEmpty() || where.IsWildcard() || where.IsLinkLocal()) continue;
            if (where.Family() == AF_INET) {
                // 169.254/16: no DHCP server answered.
                const uint32 value = ntohl(reinterpret_cast<const sockaddr_in&>(where.SockAddr()).sin_addr.s_addr);
                if ((value >> 16) == 0xa9fe) continue;
            }
            return true;
        }
    }
    return false;
}

bool IsLocalHost(const std::string& host)
{
    return host == "localhost" || host.starts_with("127.") || host == "::1" || host.ends_with(".localhost");
}

// The explanation in lines of about this many characters: line views, not a
// text view, whose height for its width the layout gets wrong.
constexpr size_t kLineLength = 64;
std::vector<std::string> WrapText(const std::string& text)
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

BStringView* TextLine(const char* name, const std::string& text, uint32 truncation)
{
    auto* view = new BStringView(name, text.c_str());
    view->SetHighUIColor(B_DOCUMENT_TEXT_COLOR);
    view->SetTruncation(truncation);
    view->SetExplicitMinSize(BSize(100, B_SIZE_UNSET));
    view->SetExplicitAlignment(BAlignment(B_ALIGN_HORIZONTAL_CENTER, B_ALIGN_MIDDLE));
    return view;
}

// What the page area of a tab shows instead of a page that failed to load.
class LoadErrorView : public BView {
public:
    LoadErrorView(int64 tab, const LoadFailureText& text, const std::string& address)
        : BView("load-error", B_WILL_DRAW), fTitle(text.title), fExplanation(text.explanation), fAddress(address)
    {
        SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
        auto* heading = TextLine("heading", text.title, B_TRUNCATE_END);
        BFont font(be_bold_font);
        font.SetSize(font.Size() * 1.6f);
        heading->SetFont(&font);
        auto* lines = new BGroupView(B_VERTICAL, 2);
        lines->SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
        for (const auto& line : WrapText(text.explanation)) lines->AddChild(TextLine("explanation", line, B_TRUNCATE_END));
        auto* retry = new BMessage(kRetryFailedLoad);
        retry->AddInt64("tab", tab);
        // Not the window's default button: Return in the address field or a
        // page of another tab must not try this one again. Return does when
        // the button has the focus, which it gets with the page.
        fRetry = new BButton("retry", "Try Again", retry);
        fRetry->SetExplicitAlignment(BAlignment(B_ALIGN_HORIZONTAL_CENTER, B_ALIGN_MIDDLE));
        BLayoutBuilder::Group<>(this, B_VERTICAL, 12)
            .AddGlue(1)
            .Add(heading)
            .Add(lines)
            .Add(TextLine("address", address, B_TRUNCATE_MIDDLE))
            .Add(fRetry)
            .AddGlue(2)
            .SetInsets(16);
        SetExplicitMinSize(BSize(160, 100));
        SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    }
    void AttachedToWindow() override
    {
        BView::AttachedToWindow();
        fRetry->SetTarget(Window());
        // A string view asks for the fractional width of its text, and the
        // layout rounds it down: each line lost its last letters to an
        // ellipsis. Whole pixels, with room to spare.
        std::function<void(BView*)> measure = [&](BView* parent) {
            for (int32 i = 0; i < parent->CountChildren(); ++i) {
                BView* child = parent->ChildAt(i);
                if (auto* line = dynamic_cast<BStringView*>(child)) {
                    const float width = std::ceil(line->StringWidth(line->Text())) + 2;
                    line->SetExplicitPreferredSize(BSize(width, B_SIZE_UNSET));
                    line->SetExplicitMaxSize(BSize(width, B_SIZE_UNSET));
                }
                measure(child);
            }
        };
        measure(this);
    }
    bool Shows(const LoadFailureText& text, const std::string& address) const
    {
        return text.title == fTitle && text.explanation == fExplanation && address == fAddress;
    }
    void SetLoading(bool loading)
    {
        const char* label = loading ? "Trying Again…" : "Try Again";
        if (std::strcmp(fRetry->Label(), label)) fRetry->SetLabel(label);
        fRetry->SetEnabled(!loading);
    }
    void FocusButton() { fRetry->MakeFocus(); }
    bool HasFocus(BView* focus) const
    {
        for (; focus; focus = focus->Parent()) if (focus == this) return true;
        return false;
    }
private:
    const std::string fTitle, fExplanation, fAddress;
    BButton* fRetry;
};

LoadFailure FailureOf(const std::string& domain, int32 code, bool offline)
{
    return offline ? LoadFailure::Offline : ClassifyLoadFailure(domain, code);
}
}

void BrowserWindow::UpdateLoadErrorView(Tab& tab)
{
    const auto failure = tab.loadError.empty() || !tab.loadErrorProvisional ? LoadFailure::None
        : ClassifyLoadFailure(tab.loadErrorDomain, tab.loadErrorCode);
    // A certificate Summit can trust has its own question (AskAboutCertificate).
    const bool question = tab.certificate && tab.certificate->generation == tab.loadGeneration
        && !tab.certificate->host.empty() && !tab.certificate->sha256.empty();
    const bool crashed = tab.processExited || tab.crashView;
    if (failure != LoadFailure::None && !question && !crashed && !tab.loadErrorURL.starts_with("webkit-extension:")) {
        if (tab.errorGeneration == tab.loadGeneration) {
            if (tab.errorView) static_cast<LoadErrorView*>(tab.errorView)->SetLoading(tab.loading);
            else {
                // Left with Back: the page shown has no error.
                tab.loadError.clear();
                tab.loadErrorDescription.clear();
            }
            return;
        }
        tab.errorGeneration = tab.loadGeneration;
        if (!tab.errorView) {
            // The page the tab shows behind the error page; none when the
            // failed load was the tab's first.
            tab.errorShownURL = tab.url;
            tab.errorBack = tab.loadSuccessSequence > 0;
        }
        tab.errorURL = tab.loadErrorURL.empty() ? tab.url : tab.loadErrorURL;
        tab.errorOffline = tab.loadErrorDomain == "CurlErrorDomain" && failure != LoadFailure::Secure
            && !IsLocalHost(LoadFailureHost(tab.errorURL)) && !NetworkIsUp();
        ShowLoadErrorView(tab);
        return;
    }
    if (!tab.errorView) return;
    if (tab.loadGeneration == tab.errorGeneration && !crashed) {
        static_cast<LoadErrorView*>(tab.errorView)->SetLoading(tab.loading);
        return;
    }
    // A newer load keeps the error page until it shows a page (the address
    // changes, or the load finishes), as a stopped one does.
    if (crashed || tab.url != tab.errorShownURL || tab.loadOutcome == "succeeded") RemoveLoadErrorView(tab, false);
    else static_cast<LoadErrorView*>(tab.errorView)->SetLoading(tab.loading);
}

void BrowserWindow::ShowLoadErrorView(Tab& tab)
{
    const auto failure = FailureOf(tab.loadErrorDomain, tab.loadErrorCode, tab.errorOffline);
    const auto text = DescribeLoadFailure(failure == LoadFailure::None ? LoadFailure::Other : failure,
        tab.errorURL, tab.loadErrorDescription);
    tab.errorTitle = text.title;
    const std::string address = DisplayURL(tab.errorURL);
    auto* view = static_cast<LoadErrorView*>(tab.errorView);
    if (!view || !view->Shows(text, address)) {
        // Made afresh for another failure, where it was.
        auto* made = new LoadErrorView(tab.id, text, address);
        const bool hadFocus = view && view->HasFocus(CurrentFocus());
        const int32 index = view ? fCards->IndexOfView(view) : -1;
        if (view) {
            view->RemoveSelf();
            delete view;
        }
        if (index >= 0) fCards->AddView(index, made);
        else fCards->AddView(made);
        tab.errorView = view = made;
        if (hadFocus) view->FocusButton();
    }
    view->SetLoading(tab.loading);
    if (tab.id == fSelected) {
        fCards->SetVisibleItem(fCards->IndexOfView(tab.errorView));
        // The address stays in the field, to correct or enter again.
        if (!fAddress->TextView()->IsFocus()) fAddress->SetText(DisplayURL(tab.errorURL).c_str());
        // Return now means Try Again, where it meant the page.
        if (!CurrentFocus() || CurrentFocus() == tab.view) view->FocusButton();
    }
    if (tab.errorOffline && !fNetworkCheck) {
        fNetworkWasUp = false;
        BMessage check(kCheckNetwork);
        fNetworkCheck = std::make_unique<BMessageRunner>(BMessenger(this), &check, 2000000);
    }
}

void BrowserWindow::RemoveLoadErrorView(Tab& tab, bool focus)
{
    if (!tab.errorView) return;
    auto* view = static_cast<LoadErrorView*>(tab.errorView);
    if (tab.id == fSelected) {
        fCards->SetVisibleItem(fCards->IndexOfView(tab.crashView ? tab.crashView : tab.view));
        if (focus || view->HasFocus(CurrentFocus())) tab.view->MakeFocus();
        if (!fAddress->TextView()->IsFocus()) fAddress->SetText(DisplayURL(tab.url).c_str());
    }
    tab.errorView = nullptr;
    tab.errorOffline = false;
    view->RemoveSelf();
    delete view;
}

bool BrowserWindow::LeaveLoadErrorView(Tab& tab)
{
    if (!tab.errorView || !tab.errorBack || tab.loading) return false;
    // The page before is still there; what failed belongs to the error page.
    tab.loadError.clear();
    tab.loadErrorDescription.clear();
    RemoveLoadErrorView(tab);
    if (tab.id == fSelected) ShowTabStatus(tab);
    RefreshChrome();
    return true;
}

void BrowserWindow::RetryFailedLoad(Tab& tab)
{
    if (!tab.errorView || tab.loading || !PrepareTabNavigation(tab)) return;
    static_cast<LoadErrorView*>(tab.errorView)->SetLoading(true);
    tab.view->LoadURL(LoadableURL(tab.errorURL).c_str());
}

void BrowserWindow::CheckNetworkForErrorPages()
{
    const bool waiting = std::any_of(fTabs.begin(), fTabs.end(), [](const Tab& tab) { return tab.errorView && tab.errorOffline; });
    if (!waiting) {
        fNetworkCheck.reset();
        return;
    }
    const bool up = NetworkIsUp();
    // Up for two looks in a row: an interface gets its address a moment
    // before its name server answers.
    if (up && fNetworkWasUp) {
        fNetworkCheck.reset();
        for (auto& tab : fTabs) {
            if (!tab.errorView || !tab.errorOffline) continue;
            tab.errorOffline = false;
            RetryFailedLoad(tab);
        }
    }
    fNetworkWasUp = up;
}

bool BrowserWindow::MediaTookOverLoad(Tab& tab, const BMessage& message, uint64 generation)
{
    if (!IsMediaTakeover(message.GetString("loadErrorDomain", ""), message.GetInt32("loadErrorCode", 0))) return false;
    // A video or audio file opened in the tab: its media document plays it,
    // and the page counts as loaded (as in other ports), with its lock.
    tab.loadOutcome = "succeeded";
    if (tab.mediaGeneration == generation) return true;
    tab.mediaGeneration = generation;
    const std::string url = StoredURL(message.GetString("loadErrorURL", tab.url.c_str()));
    const char* title = message.GetString("title", "");
    if (!fPrivate) {
        const PageRecord visit { url, *title ? title : url };
        fShared->Change([&](Profile& profile) -> uint32 {
            return profile.Visit(visit) ? uint32(SharedProfile::kHistoryChanged) : 0;
        }, BMessenger(this), false);
    }
    return true;
}
}
#endif

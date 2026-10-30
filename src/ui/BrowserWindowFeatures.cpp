#if SUMMIT_MODERN_WEBKIT
// The browser window's October 2026 features: tabs that are pinned, muted and
// dragged between windows (#68, #70), crashed and hung pages (#71), blocked
// pop-ups (#73), app links from redirects (#64), Picture in Picture (#74),
// spelling (#66), saved passwords (#67) and printing (#65).
#include "BrowserWindow.h"
#include "Chrome.h"
#include "DownloadList.h"
#include "FeatureMessages.h"
#include "Messages.h"
#include "NoticeBar.h"
#include "PageScripts.h"
#include "PasswordStore.h"
#include "PictureInPictureWindow.h"
#include "PrintJob.h"
#include "ProtocolHandlers.h"
#include "SharedProfile.h"
#include "WebApps.h"
#include "core/WebAppManifest.h"
#include "core/Address.h"
#include "core/Bookmarks.h"
#include "core/HistoryVisits.h"
#include "core/Protocol.h"
#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <CardLayout.h>
#include <Clipboard.h>
#include <File.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <NodeInfo.h>
#include <Path.h>
#include <Roster.h>
#include <Screen.h>
#include <StringView.h>
#include <TextControl.h>
#include <WebKit/WebKitEmbedding.h>
#include <WebKit/WebKitInfo.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <thread>

namespace summit {
namespace {
using nlohmann::json;

std::string OriginOf(const std::string& url)
{
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    const auto end = url.find_first_of("/?#", scheme + 3);
    std::string origin = url.substr(0, end);
    for (auto& character : origin) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return origin;
}

std::string HostOf(const std::string& url)
{
    const auto origin = OriginOf(url);
    const auto scheme = origin.find("://");
    return scheme == std::string::npos ? origin : origin.substr(scheme + 3);
}

// What a crashed tab says, by the engine's reason.
const char* CrashTitle(const std::string& reason)
{
    if (reason == "memory") return "This page used too much memory";
    if (reason == "stopped") return "This page was stopped";
    if (reason == "unresponsive") return "This page stopped responding";
    if (reason == "crash") return "This page crashed";
    return "This page closed unexpectedly";
}

// The page area of a tab whose web process ended.
class CrashView : public BView {
public:
    CrashView(int64 tab, const std::string& title, const std::string& url, const std::string& reason)
        : BView("crashed-page", B_WILL_DRAW)
    {
        SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
        auto* heading = new BStringView("heading", title.c_str());
        BFont font(be_bold_font);
        font.SetSize(font.Size() * 1.6f);
        heading->SetFont(&font);
        heading->SetHighUIColor(B_DOCUMENT_TEXT_COLOR);
        const std::string detail = reason == "stopped"
            ? "Summit ended the page because it was not responding."
            : reason == "memory" ? "Its process went over its memory limit and was ended. Other tabs were not affected."
            : "Its process ended. Other tabs were not affected.";
        auto* text = new BStringView("detail", detail.c_str());
        text->SetHighUIColor(B_DOCUMENT_TEXT_COLOR);
        auto* address = new BStringView("address", url.c_str());
        address->SetHighUIColor(B_DOCUMENT_TEXT_COLOR);
        address->SetTruncation(B_TRUNCATE_MIDDLE);
        address->SetExplicitMaxSize(BSize(600, B_SIZE_UNSET));
        BMessage* reload = new BMessage(kReloadTab);
        reload->AddInt64("id", tab);
        auto* reloadButton = new BButton("reload", "Reload", reload);
        reloadButton->MakeDefault(true);
        BMessage* report = new BMessage(kOpenLinkInNewTab);
        report->AddString("url", ReportURL(title, url, reason).c_str());
        auto* reportButton = new BButton("report", "Report a Problem…", report);
        BLayoutBuilder::Group<>(this, B_VERTICAL, 12)
            .AddGlue(1)
            .AddGroup(B_HORIZONTAL).AddGlue().Add(heading).AddGlue().End()
            .AddGroup(B_HORIZONTAL).AddGlue().Add(text).AddGlue().End()
            .AddGroup(B_HORIZONTAL).AddGlue().Add(address).AddGlue().End()
            .AddGroup(B_HORIZONTAL, 8).AddGlue().Add(reportButton).Add(reloadButton).AddGlue().End()
            .AddGlue(2)
            .SetInsets(20);
        SetExplicitMinSize(BSize(320, 200));
        SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    }
    void AttachedToWindow() override
    {
        BView::AttachedToWindow();
        // The buttons talk to the browser window, not to this view.
        if (auto* button = dynamic_cast<BButton*>(FindView("reload"))) button->SetTarget(Window());
        if (auto* button = dynamic_cast<BButton*>(FindView("report"))) button->SetTarget(Window());
    }
private:
    // A new GitHub issue with the versions filled in; the site only as its
    // origin, so a private address does not end up in a public report.
    static std::string ReportURL(const std::string& title, const std::string& url, const std::string& reason)
    {
        auto encode = [](const std::string& text) {
            static constexpr char digits[] = "0123456789ABCDEF";
            std::string encoded;
            for (unsigned char character : text) {
                if (std::isalnum(character) || character == '-' || character == '_' || character == '.' || character == '~') encoded += character;
                else { encoded += '%'; encoded += digits[character >> 4]; encoded += digits[character & 15]; }
            }
            return encoded;
        };
        const std::string body = "**What happened:** " + title + " (engine reason: " + (reason.empty() ? "unknown" : reason) + ").\n\n"
            "**Site:** " + OriginOf(url) + "\n\n**Summit:** WebKit " + BWebKitVersion() + ", Haiku port " + BWebKitPortVersion()
            + "\n\n**What I was doing:** \n";
        return "https://github.com/airos-works/summit/issues/new?title=" + encode(title + " on " + HostOf(url)) + "&body=" + encode(body);
    }
};
}

void BrowserWindow::AddPageScripts(BWebKitView* view)
{
    view->AddScriptMessageHandler("summitPopups", BMessenger(this));
    view->AddUserScript(kPopupScript, true, true);
    view->AddScriptMessageHandler("summitPip", BMessenger(this));
    view->AddUserScript(kPictureInPictureScript, true, true);
    // Private windows neither save nor fill passwords.
    if (!fPrivate && fShared->Read([](const Profile& profile) { return profile.savePasswords; })) {
        view->AddScriptMessageHandler("summitPasswords", BMessenger(this));
        view->AddUserScript(kPasswordScript, true, true);
    }
    // Passkey autofill (#111), in private windows too: passkeys are used
    // there, only never made without the user's say. In frames as well: a
    // sign-in frame of another site may offer its passkeys (when the page
    // allows it publickey-credentials-get).
    view->AddScriptMessageHandler("summitPasskeys", BMessenger(this));
    view->AddUserScript(kPasskeyFieldScript, true, true);
}

void BrowserWindow::PageScriptMessage(BMessage& message)
{
    const uint64 token = message.GetUInt64("reply_token", 0);
    std::string reply;
    BMessenger messenger;
    auto* tab = message.FindMessenger("view", &messenger) == B_OK ? FindTab(messenger) : nullptr;
    const std::string name = message.GetString("name", "");
    const std::string origin = message.GetString("origin", "");
    const bool mainFrame = message.GetBool("main_frame", false);
    json body = json::object();
    try {
        body = json::parse(message.GetString("body", "{}"));
    } catch (const std::exception&) { }
    if (!body.is_object()) body = json::object();
    auto text = [&](const char* key) { return body.contains(key) && body[key].is_string() ? body[key].get<std::string>() : std::string(); };
    if (tab && name == "summitPasskeys") {
        // From any frame: the list offers the passkeys of the request that
        // frame's origin made.
        auto number = [&](const char* key) { return body.contains(key) && body[key].is_number() ? body[key].get<double>() : 0.0; };
        if (text("type") == "field")
            PasskeyFieldEntered(*tab, BRect(number("left"), number("top"), number("right"), number("bottom")), number("width"), origin);
        else if (text("type") == "leave" && tab->id == fSelected)
            ClosePasskeyAutofill();
    } else if (tab && name == "summitPopups") {
        const auto url = text("url");
        if (!url.empty() && url.size() < 8192) {
            tab->blockedPopup = url;
            ++tab->blockedPopups;
            RefreshNoticeBar();
        }
    } else if (tab && name == "summitPip") {
        const auto type = text("type");
        if (type == "context") tab->pipToken = text("token");
        else if (type == "enter" && tab->id == fSelected) StartPictureInPicture(text("token"));
        else if (type == "exit" && fPictureInPictureTab == tab->id && fPictureInPicture.IsValid())
            fPictureInPicture.SendMessage(B_QUIT_REQUESTED);
    } else if (tab && name == "summitPasswords" && !fPrivate) {
        const auto type = text("type");
        const bool web = origin.starts_with("https://") || origin.starts_with("http://");
        // The key store may wait for the user (to allow Summit, or unlock
        // the keyring): it is asked on a thread of its own, never the window's.
        if (type == "query" && mainFrame && web && OriginOf(tab->url) == origin && token) {
            // Only the page's own frame, on the site the account was saved for.
            std::thread([token, origin] {
                json accounts = json::array();
                for (const auto& account : PasswordStore::Accounts(origin))
                    accounts.push_back({{"username", account.username}, {"password", account.password}});
                // The page gets the reply as a string, which it parses.
                BWebKitView::ReplyToScriptMessage(token, accounts.dump().c_str());
            }).detach();
            return;
        }
        if (type == "submit" && web) {
            const auto password = text("password");
            const auto username = text("username");
            const bool never = fShared->Read([&](const Profile& profile) { return profile.passwordNever.contains(origin); });
            if (!password.empty() && password.size() < 4096 && username.size() < 1024 && !never) {
                std::thread([window = BMessenger(this), id = tab->id, origin, username, password] {
                    bool known = false, same = false;
                    for (const auto& account : PasswordStore::Accounts(origin)) {
                        if (account.username != username) continue;
                        known = true;
                        same = account.password == password;
                    }
                    BMessage checked(kPasswordChecked);
                    checked.AddInt64("tab", id);
                    checked.AddString("origin", origin.c_str());
                    checked.AddString("username", username.c_str());
                    checked.AddString("password", password.c_str());
                    checked.AddBool("update", known);
                    checked.AddBool("same", same);
                    window.SendMessage(&checked);
                }).detach();
            }
        }
    }
    if (token) BWebKitView::ReplyToScriptMessage(token, reply.c_str());
}

void BrowserWindow::MoveTabTo(int64 id, size_t index)
{
    auto moving = std::find_if(fTabs.begin(), fTabs.end(), [&](const Tab& candidate) { return candidate.id == id; });
    if (moving == fTabs.end()) return;
    Tab moved = std::move(*moving);
    fTabs.erase(moving);
    // Pinned tabs stay before the others.
    size_t pinned = 0;
    while (pinned < fTabs.size() && fTabs[pinned].pinned) ++pinned;
    index = moved.pinned ? std::min(index, pinned) : std::clamp(index, pinned, fTabs.size());
    fTabs.insert(fTabs.begin() + index, std::move(moved));
    SyncBrowserWindow();
    RefreshChrome();
    SaveSession();
}

void BrowserWindow::SetTabPinned(int64 id, bool pinned)
{
    auto* tab = FindTabByID(id);
    if (!tab || tab->pinned == pinned) return;
    tab->pinned = pinned;
#if SUMMIT_MODERN_WEBKIT
    // Extensions see it as Tab.pinned.
    fWebKitContext->SetBrowserTabPinned(tab->view, pinned);
#endif
    size_t count = 0;
    for (const auto& candidate : fTabs) if (candidate.pinned && candidate.id != id) ++count;
    // A newly pinned tab goes after the pinned ones; an unpinned one first
    // among the others.
    MoveTabTo(id, count);
}

void BrowserWindow::ToggleTabMute(int64 id)
{
    auto* tab = FindTabByID(id);
    if (!tab) return;
    tab->audioMuted = !tab->audioMuted;
    tab->view->SetAudioMuted(tab->audioMuted);
    RefreshChrome();
}

void BrowserWindow::TabDraggedOut(int64 id, BPoint where)
{
    auto* tab = FindTabByID(id);
    if (!tab || fClosingWindow || fFullScreenTab == id) return;
    // Dropped on the bookmarks bar: the page becomes a bookmark there.
    if (fBookmarksBar && !fBookmarksBar->IsHidden(fBookmarksBar) && fBookmarksBar->Bounds().Contains(fBookmarksBar->ConvertFromScreen(where))) {
        if (IsBookmarkableURL(tab->url)) {
            BMessage drop(kDropBookmark);
            drop.AddString("url", tab->url.c_str());
            drop.AddString("title", tab->title.c_str());
            drop.AddInt64("folder", kBookmarksBarFolder);
            PostMessage(&drop);
        }
        return;
    }
    // Dropped back on this window: nothing to do (along the strip it was
    // already moved).
    if (Frame().Contains(where)) return;
    BMessage transfer(kTransferTab);
    transfer.AddMessenger("window", BMessenger(this));
    transfer.AddInt64("id", id);
    transfer.AddPoint("where", where);
    transfer.AddBool("private", fPrivate);
    transfer.AddBool("last", fTabs.size() == 1);
    be_app->PostMessage(&transfer);
}

void BrowserWindow::DetachTab(int64 id, const BMessenger& target, int32 index, uint64 command)
{
    auto found = std::find_if(fTabs.begin(), fTabs.end(), [&](const Tab& tab) { return tab.id == id; });
    if (found == fTabs.end() || fClosingWindow || found->closeQueued || found->closeRequested || found->closeApproved) {
        if (command) RespondToCommand(command, B_BUSY, { }, "The tab is closing.");
        return;
    }
    if (fFullScreenTab == id) ExitPageFullScreen();
    if (fPictureInPictureTab == id && fPictureInPicture.IsValid()) fPictureInPicture.SendMessage(B_QUIT_REQUESTED);
    DropTabAuthentications(id);
    CancelTranslation(*found, false);
    found->translation.probed = 0;
    CloseDeveloperTools(*found, true);
    RemoveCrashView(*found);
    RemoveLoadErrorView(*found, false);
    const bool wasSelected = found->id == fSelected;
    int64 next = 0;
    if (wasSelected) {
        const auto index = found - fTabs.begin();
        if (index + 1 < static_cast<long>(fTabs.size())) next = fTabs[index + 1].id;
        else if (index > 0) next = fTabs[index - 1].id;
    }
    // The page view moves with its state; its notifications follow it.
    auto* moved = new Tab(std::move(*found));
    fTabs.erase(found);
    moved->view->RemoveSelf();
    moved->messenger = BMessenger();
    BMessage adopt(kAdoptTab);
    adopt.AddPointer("tab", moved);
    adopt.AddInt32("index", index);
    if (command) adopt.AddUInt64("command", command);
    if (target.SendMessage(&adopt) != B_OK) {
        // The other window went away: keep the tab here.
        fCards->AddView(moved->view);
        moved->messenger = BMessenger(moved->view);
        fTabs.push_back(std::move(*moved));
        delete moved;
        RefreshChrome();
        if (command) RespondToCommand(command, B_BAD_VALUE, { }, "The window is gone.");
        return;
    }
    if (fTabs.empty()) {
        // Enter the normal close handshake even with no documents left.
        // Setting fClosingWindow before that handshake makes BeginWindowClose
        // return early, leaving a hidden window that prevents quitting.
        Hide();
        BeginWindowClose();
        return;
    }
    if (wasSelected && next) SelectTab(next);
    SyncBrowserWindow();
    RefreshChrome();
    SaveSession();
}

void BrowserWindow::AdoptTab(BMessage& message)
{
    Tab* adopted = nullptr;
    if (message.FindPointer("tab", reinterpret_cast<void**>(&adopted)) != B_OK || !adopted) return;
    std::unique_ptr<Tab> tab(adopted);
    const uint64 command = message.GetUInt64("command", 0);
    if (fClosingWindow) {
        // Too late for this window: the page closes with it.
        delete tab->view;
        if (command) RespondToCommand(command, B_BUSY, { }, "The window is closing.");
        return;
    }
    tab->id = fNextID++;
    fCards->AddView(tab->view);
    tab->messenger = BMessenger(tab->view);
    tab->view->SetNotificationTarget(BMessenger(this));
    tab->devTools = BMessenger();
    size_t pinned = 0;
    while (pinned < fTabs.size() && fTabs[pinned].pinned) ++pinned;
    const int64 id = tab->id;
    // An extension's move puts it where asked, pinned tabs staying first.
    const int32 index = message.GetInt32("index", -1);
    size_t position = tab->pinned ? pinned : fTabs.size();
    if (index >= 0) position = tab->pinned ? std::min<size_t>(index, pinned) : std::clamp<size_t>(index, pinned, fTabs.size());
    fTabs.insert(fTabs.begin() + position, std::move(*tab));
    if (command) {
        // The extension moved it: this window is not brought forward.
        if (IsHidden()) Show();
        SyncBrowserWindow();
        RefreshChrome();
        SaveSession();
        RespondToCommand(command, B_OK);
        return;
    }
    SelectTab(id);
    if (IsHidden()) Show();
    Activate();
    SaveSession();
}

void BrowserWindow::ShowCrashView(Tab& tab)
{
    if (tab.crashView) return;
    tab.crashView = new CrashView(tab.id, CrashTitle(tab.exitReason), DisplayURL(tab.url), tab.exitReason);
    fCards->AddView(tab.crashView);
    if (tab.id == fSelected) fCards->SetVisibleItem(fCards->IndexOfView(tab.crashView));
}

void BrowserWindow::RemoveCrashView(Tab& tab)
{
    if (!tab.crashView) return;
    if (tab.id == fSelected) fCards->SetVisibleItem(fCards->IndexOfView(tab.view));
    tab.crashView->RemoveSelf();
    delete std::exchange(tab.crashView, nullptr);
}

void BrowserWindow::ReloadCrashedTab(Tab& tab)
{
    RemoveCrashView(tab);
    tab.exitReason.clear();
    tab.exitedInBackground = false;
    tab.unresponsive = tab.waitedForPage = false;
    if (!PrepareTabNavigation(tab)) return;
    tab.view->Reload();
    RefreshNoticeBar();
}

void BrowserWindow::ProcessExitReason(Tab& tab, const std::string& reason)
{
    tab.exitReason = reason;
    tab.unresponsive = false;
    // A tab in the background reloads when it is selected again; the one in
    // front says what happened.
    if (tab.id != fSelected) tab.exitedInBackground = true;
    else ShowCrashView(tab);
    RefreshNoticeBar();
}

void BrowserWindow::ResponsivenessChanged(const BMessage& message)
{
    BMessenger view;
    if (message.FindMessenger("view", &view) != B_OK) return;
    auto* tab = FindTab(view);
    if (!tab) return;
    tab->unresponsive = !message.GetBool("responsive", true);
    if (!tab->unresponsive) tab->waitedForPage = false;
    RefreshNoticeBar();
}

void BrowserWindow::RefreshNoticeBar()
{
    if (!fNoticeBar) return;
    auto* tab = ActiveTab();
    auto with = [](uint32 what, int64 id) {
        auto* message = new BMessage(what);
        message->AddInt64("tab", id);
        return message;
    };
    auto showSoftwareRenderingNotice = [&] {
        fNoticeBar->SetNotice("Summit is rendering pages in software because graphics acceleration is unavailable.",
            {{"OK", new BMessage(kDismissSoftwareRenderingNotice)}});
    };
    if (!tab) {
        fNoticeBar->Clear();
        return;
    }
    if (!fWebApp.empty() && !tab->unresponsive) {
        // A web app's page that left the app (#75).
        const bool web = tab->url.rfind("https://", 0) == 0 || tab->url.rfind("http://", 0) == 0;
        if (web && !InWebAppScope(fWebAppScope, tab->url))
            fNoticeBar->SetNotice(HostOf(tab->url) + " is not part of " + fWebAppName + ".",
                {{"Open in Summit", new BMessage(kOpenInBrowser)}});
        else if (TranslationNotice(*tab)) return;
        else if (fSoftwareRenderingNoticePending)
            showSoftwareRenderingNotice();
        else fNoticeBar->Clear();
        return;
    }
    if (tab->unresponsive && !tab->waitedForPage && !tab->processExited) {
        fNoticeBar->SetNotice("This page isn't responding. You can wait, or stop it (its process ends).",
            {{"Wait", with(kWaitForPage, tab->id)}, {"Stop Page", with(kStopPage, tab->id)}});
        return;
    }
    if (tab->pendingPassword) {
        const auto& pending = *tab->pendingPassword;
        const std::string who = pending.username.empty() ? std::string() : " for " + pending.username;
        const std::string text = (pending.update ? "Update the saved password" + who + " on " : "Save the password" + who + " on ")
            + HostOf(pending.origin) + "? Summit keeps it in the Haiku key store.";
        auto answer = [&](int32 which) {
            auto* message = with(kPasswordBarAnswer, tab->id);
            message->AddInt32("which", which);
            return message;
        };
        fNoticeBar->SetNotice(text, {{pending.update ? "Update" : "Save", answer(0)}, {"Never for This Site", answer(1)}, {"Not Now", answer(2)}});
        return;
    }
    if (!tab->blockedPopup.empty()) {
        const std::string text = "Summit blocked " + std::string(tab->blockedPopups > 1
            ? std::to_string(tab->blockedPopups) + " pop-ups" : "a pop-up") + " from " + HostOf(tab->url) + ".";
        auto* dismiss = with(kOpenBlockedPopup, tab->id);
        dismiss->AddBool("dismiss", true);
        fNoticeBar->SetNotice(text, {{"Open Pop-up", with(kOpenBlockedPopup, tab->id)},
            {"Always Allow", with(kAllowSitePopups, tab->id)}, {"Close", dismiss}});
        return;
    }
    // A notice for the whole window (#93), when its tab has none.
    if (TranslationNotice(*tab)) return;
    if (!fWindowNotice.empty()) {
        fNoticeBar->SetNotice(fWindowNotice, {{"OK", new BMessage(kDismissWindowNotice)}});
        return;
    }
    if (fSoftwareRenderingNoticePending) {
        showSoftwareRenderingNotice();
        return;
    }
    fNoticeBar->Clear();
}

void BrowserWindow::SchemeOpenRequested(const BMessage& message, Tab& tab)
{
    const std::string url = message.GetString("url", "");
    const bool trusted = message.GetBool("trusted", message.GetBool("user_gesture", true));
    const bool mainFrame = message.GetBool("main_frame", true);
    if (StoreOfferChosen(tab, url)) return;
    const std::string source = message.GetString("source_url", "");
    const std::string committed = message.GetString("committed_url", "");
    // The page (or its redirect) must be the tab's: a page left behind
    // cannot launch anything.
    const std::string checked = source == tab.url ? source : committed == tab.url ? committed : std::string();
    if (checked.empty() && !(trusted && message.GetBool("redirect", false))) return;
    const std::string scheme = URLScheme(url);
    if (!trusted && !mainFrame) {
        tab.notice = "A frame of this page tried to open a " + scheme + ": link without a click; Summit did not open it.";
        tab.noticeUntil = system_time() + 15000000;
        if (tab.id == fSelected) ShowTabStatus(tab);
        return;
    }
    // A script's own navigation asks every time, even for a remembered app.
    fProtocolHandlers->Open(url, tab.id, checked.empty() ? tab.url : checked, !trusted);
}

void BrowserWindow::StartPictureInPicture(const std::string& token)
{
    auto* tab = ActiveTab();
    if (!tab || token.empty() || token.size() > 20) return;
    // One video at a time: the window of another one closes.
    if (fPictureInPicture.IsValid()) fPictureInPicture.SendMessage(B_QUIT_REQUESTED);
    auto* window = new PictureInPictureWindow(BMessenger(this), tab->id, ++fPictureInPictureSession, tab->title, Frame());
    const BSize size = window->PictureSize();
    fPictureInPicture = BMessenger(window);
    fPictureInPictureTab = tab->id;
    window->Show();
    tab->view->EnterPictureInPicture(token.c_str(), fPictureInPicture, size);
}

void BrowserWindow::PictureInPictureCommand(const BMessage& message)
{
    const int64 id = message.GetInt64("tab", 0);
    const std::string command = message.GetString("command", "");
    auto* tab = FindTabByID(id);
    // A window that was replaced has nothing more to say.
    if (message.GetUInt64("session", 0) != fPictureInPictureSession) return;
    if (command == "closed") {
        if (tab && !message.GetBool("ended", false)) tab->view->ExitPictureInPicture();
        fPictureInPicture = BMessenger();
        fPictureInPictureTab = 0;
        return;
    }
    if (!tab) return;
    // The controller's commands go to the engine, which checks their numbers.
    const bool media = command == "toggle" || command == "play" || command == "pause" || command == "mute" || command == "unmute"
        || (command.size() < 40 && (command.starts_with("seek:") || command.starts_with("skip:") || command.starts_with("volume:")));
    if (media) tab->view->PictureInPictureCommand(command.c_str());
    else if (command == "size") {
        BSize size;
        if (message.FindSize("size", &size) == B_OK) tab->view->SetPictureInPictureSize(size);
    } else if (command == "back") {
        SelectTab(id);
        Activate();
        if (fPictureInPicture.IsValid()) fPictureInPicture.SendMessage(B_QUIT_REQUESTED);
    }
}

void BrowserWindow::SpellingCommand(const BMessage& message)
{
    auto* tab = ActiveTab();
    if (!tab) return;
    tab->view->PerformSpellingCommand(message.GetUInt32("command", B_WEBKIT_SPELLING_REPLACE), message.GetString("word", nullptr));
}

void BrowserWindow::PasswordBarAnswer(const BMessage& message)
{
    auto* tab = FindTabByID(message.GetInt64("tab", 0));
    if (!tab || !tab->pendingPassword) return;
    const auto pending = *std::exchange(tab->pendingPassword, std::nullopt);
    switch (message.GetInt32("which", 2)) {
    case 0:
        NotePasswordSignIn(pending.origin, pending.username);
        std::thread([window = BMessenger(this), pending] {
            BMessage saved(kPasswordSaved);
            saved.AddString("error", PasswordStore::Save(pending.origin, pending.username, pending.password).c_str());
            window.SendMessage(&saved);
        }).detach();
        break;
    case 1:
        fShared->Change([&](Profile& profile) -> uint32 {
            return profile.passwordNever.insert(pending.origin).second ? SharedProfile::kSettingsChanged : 0;
        });
        break;
    default:
        break;
    }
    RefreshNoticeBar();
}

void BrowserWindow::PrintPage(uint64 frameIdentifier, uint64 expectedGeneration)
{
    auto* tab = ActiveTab();
    if (!tab || tab->processExited || tab->loading || (expectedGeneration && expectedGeneration != tab->loadGeneration)) return;
    // The print job runs on its own thread: the printer's dialogs wait for
    // the user, the pages come from the engine one at a time.
    PrintJob::Start(tab->id, tab->title, DisplayURL(tab->url), BMessenger(this), frameIdentifier, tab->loadGeneration);
}

void BrowserWindow::SaveAsPDF()
{
    auto* tab = ActiveTab();
    if (!tab || tab->processExited) return;
    BMessenger target(this);
    BMessage chosen(kSaveAsPDFChosen);
    chosen.AddInt64("tab", tab->id);
    fPDFPanel = std::make_unique<BFilePanel>(B_SAVE_PANEL, &target, nullptr, 0, false, &chosen);
    std::string name = tab->title.empty() ? "page" : tab->title;
    for (auto& character : name) if (character == '/' || character == ':') character = '-';
    if (name.size() > 120) name.resize(120);
    fPDFPanel->SetSaveText((name + ".pdf").c_str());
    fPDFPanel->Window()->SetTitle("Save as PDF — Summit");
    fPDFPanel->Show();
}

void BrowserWindow::SaveAsPDFChosen(const BMessage& message)
{
    auto* tab = FindTabByID(message.GetInt64("tab", 0));
    entry_ref directory;
    const char* name = nullptr;
    if (!tab || message.FindRef("directory", &directory) != B_OK || message.FindString("name", &name) != B_OK || !name) return;
    BPath path(&directory);
    if (path.InitCheck() != B_OK || path.Append(name) != B_OK) return;
    const uint64 request = ++fPrintRequest | (uint64(1) << 62);
    fPDFTargets[request] = path.Path();
    const auto paper = PrintJob::DefaultPaper();
    tab->view->DrawPagesForPrinting(BMessenger(this), request, paper.Width(), paper.Height(), BRect(36, 36, 36, 36),
        1, 0, 100000, 0, false);
    fStatus->SetText("Saving as PDF…");
}

void BrowserWindow::PrintPagesReceived(BMessage& message)
{
    const uint64 request = message.GetUInt64("identifier", 0);
    auto target = fPDFTargets.find(request);
    if (target == fPDFTargets.end()) return;
    const std::string path = target->second;
    fPDFTargets.erase(target);
    const void* data = nullptr;
    ssize_t size = 0;
    if (message.FindData("pdf", B_RAW_TYPE, &data, &size) != B_OK || size <= 0) {
        ShowError(std::string("Could not save as PDF: ") + message.GetString("error", "the page has nothing to print."));
        return;
    }
    BFile file(path.c_str(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
    if (file.InitCheck() != B_OK || file.Write(data, size) != size) {
        ShowError("Could not write " + path + ".");
        return;
    }
    BNodeInfo(&file).SetType("application/pdf");
    const int32 pages = message.GetInt32("pageCount", 0);
    fStatus->SetText(("Saved " + std::to_string(pages) + (pages == 1 ? " page" : " pages") + " as " + path).c_str());
}

void BrowserWindow::InstallWebAppResult(const BMessage& message)
{
    const char* result = nullptr;
    const auto plan = message.FindString("result", &result) == B_OK && result ? PlanWebApp(result) : WebAppPlan();
    if (plan.name.empty()) {
        ShowError(std::string("This page cannot be installed as an app") + (message.HasString("error")
            ? std::string(": ") + message.GetString("error", "") : std::string(".")));
        return;
    }
    const std::string text = "Install “" + plan.name + "” as an app?\n\nIt opens in a window of its own, "
        "with its own icon in the Deskbar's Applications menu, and keeps your sign-ins.\n\n" + plan.startURL;
    // The question and the download that follows run on a thread of their
    // own (an alert belongs to the thread that makes it).
    std::thread([text, plan, window = BMessenger(this), directory = fShared->Path().parent_path()] {
        auto* alert = new BAlert("Install as App — Summit", text.c_str(), "Cancel", "Install", nullptr,
            B_WIDTH_AS_USUAL, B_INFO_ALERT);
        alert->SetShortcut(0, B_ESCAPE);
        if (alert->Go() != 1) return;
        WebApp app;
        const auto error = InstallWebApp(plan, directory, app);
        BMessage installed(kWebAppInstalled);
        if (!error.empty()) installed.AddString("error", error.c_str());
        installed.AddString("id", app.id.c_str());
        installed.AddString("name", app.name.c_str());
        installed.AddString("start_url", app.startURL.c_str());
        installed.AddString("scope", app.scope.c_str());
        installed.AddString("launcher", app.launcher.c_str());
        window.SendMessage(&installed);
    }).detach();
}

void BrowserWindow::WebAppInstalled(const BMessage& message)
{
    if (message.HasString("error")) {
        ShowError(message.GetString("error", ""));
        return;
    }
    WebApp app { message.GetString("id", ""), message.GetString("name", ""), message.GetString("start_url", ""),
        message.GetString("scope", ""), message.GetString("launcher", "") };
    if (app.id.empty()) return;
    const std::string name = app.name;
    fShared->Change([&](Profile& profile) -> uint32 {
        profile.webApps.push_back(app);
        return SharedProfile::kSettingsChanged;
    });
    fShared->SaveSoon();
    fStatus->SetText(("Installed " + name + ": it is in the Deskbar's Applications menu.").c_str());
    BMessage open(kOpenWebApp);
    open.AddString("id", app.id.c_str());
    be_app->PostMessage(&open);
}

// chrome.history (#106). Times are milliseconds since the epoch, as in Chrome.
static nlohmann::json HistoryItemJSON(const PageRecord& page)
{
    const auto times = HistoryVisitTimes(page);
    const double last = times.empty() ? page.visited * 1000.0 : times.front() / 1000.0;
    return {
        {"id", std::to_string(std::hash<std::string>()(page.url))},
        {"url", page.url},
        {"title", page.title},
        {"lastVisitTime", last},
        {"visitCount", HistoryVisitCount(page)},
        {"typedCount", 0},
    };
}

void BrowserWindow::RecordVisit(const PageRecord& visit, uint32 transition)
{
    std::optional<PageRecord> recorded;
    fShared->Change([&](Profile& profile) -> uint32 {
        if (!(transition ? profile.VisitWithTransition(visit, transition) : profile.Visit(visit))) return 0;
        for (const auto& page : profile.history) {
            if (page.url == visit.url) { recorded = page; break; }
        }
        return SharedProfile::kHistoryChanged;
    }, BMessenger(this), false);
#if SUMMIT_MODERN_WEBKIT
    if (recorded && !fPrivate) fWebKitContext->NotifyHistoryChanged("visited", HistoryItemJSON(*recorded).dump().c_str());
#endif
}

void BrowserWindow::HistoryCommand(const BMessage& message, uint64 identifier)
{
#if SUMMIT_MODERN_WEBKIT
    const std::string method = message.GetString("method", "");
    const auto arguments = nlohmann::json::parse(message.GetString("arguments", "{}"), nullptr, false);
    if (arguments.is_discarded() || !arguments.is_object()) { RespondToCommand(identifier, B_BAD_VALUE); return; }
    auto respond = [&](const nlohmann::json& data) {
        fWebKitContext->RespondToBrowserCommandWithData(identifier, B_OK, data.dump().c_str());
    };
    auto removed = [&](bool all, const std::vector<std::string>& urls) {
        if (all || !urls.empty())
            fWebKitContext->NotifyHistoryChanged("removed", nlohmann::json({{"allHistory", all}, {"urls", urls}}).dump().c_str());
    };
    const double now = HistoryNowMicroseconds() / 1000.0;
    if (method == "search") {
        std::string text = arguments.value("text", std::string());
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
        // Chrome's defaults: the last 24 hours, at most 100 items.
        const double start = arguments.value("startTime", now - 24 * 3600 * 1000.0);
        const double end = arguments.value("endTime", now);
        const size_t limit = static_cast<size_t>(std::clamp(arguments.value("maxResults", 100.0), 0.0, 100000.0));
        auto items = nlohmann::json::array();
        fShared->Read([&](const Profile& profile) {
            for (const auto& page : profile.history) {
                if (items.size() >= limit) break;
                auto item = HistoryItemJSON(page);
                const double last = item["lastVisitTime"];
                if (last < start || last > end) continue;
                if (!text.empty()) {
                    std::string haystack = page.url + "\n" + page.title;
                    std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char c) { return std::tolower(c); });
                    if (haystack.find(text) == std::string::npos) continue;
                }
                items.push_back(std::move(item));
            }
        });
        respond(items);
    } else if (method == "getVisits") {
        const std::string url = arguments.value("url", std::string());
        auto visits = nlohmann::json::array();
        fShared->Read([&](const Profile& profile) {
            for (const auto& page : profile.history) {
                if (page.url != url) continue;
                const auto id = std::to_string(std::hash<std::string>()(page.url));
                // Chrome's TransitionType for the visit's Firefox type.
                auto transition = [&](int64_t time) {
                    for (const auto& [visitTime, type] : page.visitTransitions) {
                        if (visitTime != time) continue;
                        switch (type) {
                            case 2: return "typed";
                            case 3: return "auto_bookmark";
                            case 4: return "auto_subframe";
                            case 8: return "manual_subframe";
                            case 9: return "reload";
                            default: return "link";
                        }
                    }
                    return "link";
                };
                for (auto time : HistoryVisitTimes(page))
                    visits.push_back({{"id", id}, {"visitId", std::to_string(time)}, {"visitTime", time / 1000.0}, {"referringVisitId", "0"}, {"transition", transition(time)}, {"isLocal", true}});
                if (visits.empty() && page.visited)
                    visits.push_back({{"id", id}, {"visitId", std::to_string(page.visited)}, {"visitTime", page.visited * 1000.0}, {"referringVisitId", "0"}, {"transition", "link"}, {"isLocal", true}});
                break;
            }
        });
        respond(visits);
    } else if (method == "addUrl") {
        PageRecord page { arguments.value("url", std::string()), arguments.value("title", std::string()) };
        if (page.url.empty()) { RespondToCommand(identifier, B_BAD_VALUE); return; }
        std::optional<PageRecord> recorded;
        const double time = arguments.value("visitTime", now);
        fShared->Change([&](Profile& profile) -> uint32 {
            if (!profile.Visit(page, static_cast<int64_t>(time / 1000))) return 0;
            for (const auto& entry : profile.history) {
                if (entry.url == page.url) { recorded = entry; break; }
            }
            return SharedProfile::kHistoryChanged;
        }, BMessenger(this), false);
        if (recorded) fWebKitContext->NotifyHistoryChanged("visited", HistoryItemJSON(*recorded).dump().c_str());
        respond(nullptr);
    } else if (method == "deleteUrl") {
        const std::string url = arguments.value("url", std::string());
        bool erased = false;
        fShared->Change([&](Profile& profile) -> uint32 {
            erased = std::erase_if(profile.history, [&](const PageRecord& page) { return page.url == url; }) > 0;
            return erased ? SharedProfile::kHistoryChanged : 0;
        }, BMessenger(this), false);
        if (erased) removed(false, { url });
        respond(nullptr);
    } else if (method == "deleteRange") {
        // Pages last visited in the range; their earlier visits go with them.
        const double start = arguments.value("startTime", 0.0), end = arguments.value("endTime", 0.0);
        std::vector<std::string> urls;
        fShared->Change([&](Profile& profile) -> uint32 {
            std::erase_if(profile.history, [&](const PageRecord& page) {
                const double last = HistoryItemJSON(page)["lastVisitTime"];
                if (last < start || last > end) return false;
                urls.push_back(page.url);
                return true;
            });
            return urls.empty() ? 0 : SharedProfile::kHistoryChanged;
        }, BMessenger(this), false);
        removed(false, urls);
        respond(nullptr);
    } else if (method == "deleteAll") {
        fShared->ClearHistory(BMessenger(this));
        removed(true, { });
        respond(nullptr);
    } else
        RespondToCommand(identifier, B_NOT_SUPPORTED);
#else
    (void)message;
    RespondToCommand(identifier, B_NOT_SUPPORTED);
#endif
}

// chrome.bookmarks (#106). The root "0" holds the bookmarks bar ("1") and
// Other Bookmarks ("2"), as in Chrome; separators are reported with
// Firefox's type "separator" so indexes match Summit's folders.
static nlohmann::json BookmarkJSON(const BookmarkNode& node, int64_t parent, size_t index, int depth)
{
    nlohmann::json item = {
        {"id", std::to_string(node.id)},
        {"parentId", std::to_string(parent)},
        {"index", index},
        {"title", node.separator ? std::string() : node.title},
        {"dateAdded", node.added * 1000.0},
        {"type", node.separator ? "separator" : node.folder ? "folder" : "bookmark"},
    };
    if (node.folder) {
        if (node.id == kBookmarksBarFolder) item["folderType"] = "bookmarks-bar";
        else if (node.id == kOtherBookmarksFolder) item["folderType"] = "other";
        if (node.modified) item["dateGroupModified"] = node.modified / 1000.0;
        if (depth) {
            auto children = nlohmann::json::array();
            for (size_t i = 0; i < node.children.size(); ++i)
                children.push_back(BookmarkJSON(node.children[i], node.id, i, depth - 1));
            item["children"] = std::move(children);
        }
    } else if (!node.separator)
        item["url"] = node.url;
    return item;
}

static nlohmann::json BookmarkRootJSON(const Bookmarks& bookmarks, int depth)
{
    nlohmann::json root = {{"id", "0"}, {"title", ""}, {"dateAdded", 0}, {"type", "folder"}};
    if (depth)
        root["children"] = {BookmarkJSON(bookmarks.Bar(), 0, 0, depth - 1), BookmarkJSON(bookmarks.Other(), 0, 1, depth - 1)};
    return root;
}

// A node and where it is; the root and the two fixed folders have parent 0.
static std::optional<nlohmann::json> FindBookmarkJSON(const Bookmarks& bookmarks, int64_t id, int depth)
{
    if (!id) return BookmarkRootJSON(bookmarks, depth);
    const auto* node = bookmarks.Find(id);
    if (!node) return std::nullopt;
    size_t index = 0;
    const auto* parent = bookmarks.Parent(id, &index);
    if (!parent) index = id == kBookmarksBarFolder ? 0 : 1;
    return BookmarkJSON(*node, parent ? parent->id : 0, index, depth);
}

static std::optional<int64_t> BookmarkID(const std::string& text)
{
    if (text.empty() || text.size() > 18 || !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); }))
        return std::nullopt;
    return std::stoll(text);
}

static std::string LowerText(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

static void EachBookmark(const BookmarkNode& folder, const std::function<void(const BookmarkNode&, int64_t, size_t)>& visit)
{
    for (size_t i = 0; i < folder.children.size(); ++i) {
        visit(folder.children[i], folder.id, i);
        if (folder.children[i].folder) EachBookmark(folder.children[i], visit);
    }
}

void BrowserWindow::BookmarksCommand(const BMessage& message, uint64 identifier)
{
    const std::string method = message.GetString("method", "");
    const auto arguments = nlohmann::json::parse(message.GetString("arguments", "{}"), nullptr, false);
    if (arguments.is_discarded() || !arguments.is_object()) { RespondToCommand(identifier, B_BAD_VALUE); return; }
    auto respond = [&](const nlohmann::json& data) {
        fWebKitContext->RespondToBrowserCommandWithData(identifier, B_OK, data.dump().c_str());
    };
    auto fail = [&](const char* error) {
        fWebKitContext->RespondToBrowserCommandWithData(identifier, B_BAD_VALUE, "null", error);
    };
    auto argument = [&](const char* key) { return BookmarkID(arguments.value(key, std::string())); };
    const auto id = argument("id");
    if (method == "getTree") {
        respond(nlohmann::json::array({fShared->Read([](const Profile& profile) { return BookmarkRootJSON(profile.bookmarks, 64); })}));
    } else if (method == "get" || method == "getChildren" || method == "getSubTree") {
        std::vector<int64_t> ids;
        if (method == "get") {
            for (const auto& value : arguments.value("ids", nlohmann::json::array())) {
                const auto parsed = value.is_string() ? BookmarkID(value.get<std::string>()) : std::nullopt;
                if (!parsed) { fail("Can't find bookmark for id."); return; }
                ids.push_back(*parsed);
            }
        } else if (id)
            ids.push_back(*id);
        auto items = nlohmann::json::array();
        bool found = !ids.empty();
        fShared->Read([&](const Profile& profile) {
            for (auto each : ids) {
                auto item = FindBookmarkJSON(profile.bookmarks, each, method == "get" ? 0 : method == "getChildren" ? 1 : 64);
                if (!item) { found = false; return; }
                if (method == "getChildren") items = item->value("children", nlohmann::json::array());
                else items.push_back(std::move(*item));
            }
        });
        if (!found) fail("Can't find bookmark for id.");
        else respond(items);
    } else if (method == "getRecent") {
        const size_t count = static_cast<size_t>(std::clamp(arguments.value("numberOfItems", 1.0), 1.0, 100000.0));
        std::vector<std::pair<int64_t, nlohmann::json>> found;
        fShared->Read([&](const Profile& profile) {
            for (const auto* folder : {&profile.bookmarks.Bar(), &profile.bookmarks.Other()}) {
                EachBookmark(*folder, [&](const BookmarkNode& node, int64_t parent, size_t index) {
                    if (!node.folder && !node.separator) found.emplace_back(node.added, BookmarkJSON(node, parent, index, 0));
                });
            }
        });
        std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        auto items = nlohmann::json::array();
        for (size_t i = 0; i < found.size() && i < count; ++i) items.push_back(std::move(found[i].second));
        respond(items);
    } else if (method == "search") {
        // Every word of query in the title or address; url and title match exactly.
        std::vector<std::string> words;
        {
            std::istringstream stream(LowerText(arguments.value("query", std::string())));
            for (std::string word; stream >> word;) words.push_back(word);
        }
        const bool byURL = arguments.contains("url"), byTitle = arguments.contains("title");
        const std::string url = arguments.value("url", std::string()), title = arguments.value("title", std::string());
        auto items = nlohmann::json::array();
        fShared->Read([&](const Profile& profile) {
            for (const auto* folder : {&profile.bookmarks.Bar(), &profile.bookmarks.Other()}) {
                EachBookmark(*folder, [&](const BookmarkNode& node, int64_t parent, size_t index) {
                    if (node.separator || (byURL && (node.folder || node.url != url)) || (byTitle && node.title != title)) return;
                    if (!words.empty()) {
                        const std::string haystack = LowerText(node.title + "\n" + node.url);
                        for (const auto& word : words)
                            if (haystack.find(word) == std::string::npos) return;
                    }
                    if (words.empty() && !byURL && !byTitle) return;
                    items.push_back(BookmarkJSON(node, parent, index, 0));
                });
            }
        });
        respond(items);
    } else if (method == "create" || method == "move" || method == "update" || method == "remove" || method == "removeTree") {
        if (method != "create" && !id) { fail("Can't find bookmark for id."); return; }
        const char* error = nullptr;
        int64_t target = id.value_or(0);
        const size_t index = arguments.contains("index") ? static_cast<size_t>(arguments.value("index", 0.0)) : kBookmarkEnd;
        fShared->Change([&](Profile& profile) -> uint32 {
            auto& bookmarks = profile.bookmarks;
            const auto* node = target ? bookmarks.Find(target) : nullptr;
            if (method != "create" && !node) { error = "Can't find bookmark for id."; return 0; }
            const bool fixed = target == kBookmarksBarFolder || target == kOtherBookmarksFolder;
            if (method == "create") {
                const auto parent = arguments.contains("parentId") ? argument("parentId") : std::optional<int64_t>(kOtherBookmarksFolder);
                const auto* folder = parent ? bookmarks.Find(*parent) : nullptr;
                if (!folder || !folder->folder) { error = "Can't find parent bookmark for id."; return 0; }
                const std::string title = arguments.value("title", std::string());
                if (arguments.contains("url")) {
                    const std::string url = arguments.value("url", std::string());
                    if (!IsBookmarkableURL(url)) { error = "Invalid URL."; return 0; }
                    target = bookmarks.Add({url, title}, *parent, index, std::time(nullptr));
                } else
                    target = bookmarks.AddFolder(*parent, title, index, std::time(nullptr));
                if (!target) { error = "Can't create the bookmark: the folder is full."; return 0; }
                return SharedProfile::kBookmarksChanged;
            }
            if (fixed) { error = "Can't modify the root bookmark folders."; return 0; }
            if (method == "move") {
                const auto parent = arguments.contains("parentId") ? argument("parentId") : std::nullopt;
                const auto* current = bookmarks.Parent(target);
                const int64_t folder = parent.value_or(current ? current->id : 0);
                const auto* destination = bookmarks.Find(folder);
                if (!destination || !destination->folder) { error = "Can't find parent bookmark for id."; return 0; }
                for (int64_t inside = folder; inside; ) {
                    if (inside == target) { error = "Can't move a folder into itself."; return 0; }
                    const auto* up = bookmarks.Parent(inside);
                    inside = up ? up->id : 0;
                }
                return bookmarks.Move(target, folder, index) ? SharedProfile::kBookmarksChanged : 0;
            }
            if (method == "update") {
                std::string url = arguments.value("url", node->url);
                if (node->folder && arguments.contains("url")) { error = "Can't set URL of a bookmark folder."; return 0; }
                if (!node->folder && !IsBookmarkableURL(url)) { error = "Invalid URL."; return 0; }
                return bookmarks.Edit(target, arguments.value("title", node->title), url) ? SharedProfile::kBookmarksChanged : 0;
            }
            if (method == "remove" && node->folder && !node->children.empty()) { error = "Can't remove non-empty folder (use recursive to force)."; return 0; }
            return bookmarks.Remove(target) ? SharedProfile::kBookmarksChanged : 0;
        }, BMessenger(this));
        if (error) { fail(error); return; }
        // Extensions hear of the change before the answer arrives, as in Chrome.
        ReportBookmarkChanges();
        if (method == "remove" || method == "removeTree") { respond(nullptr); return; }
        auto item = fShared->Read([&](const Profile& profile) { return FindBookmarkJSON(profile.bookmarks, target, 0); });
        respond(item ? *item : nlohmann::json());
    } else
        RespondToCommand(identifier, B_NOT_SUPPORTED);
}

void BrowserWindow::SearchCommand(const BMessage& message, uint64 identifier, Tab* tab)
{
    const std::string method = message.GetString("method", "");
    const auto arguments = nlohmann::json::parse(message.GetString("arguments", "{}"), nullptr, false);
    if (arguments.is_discarded() || !arguments.is_object()) { RespondToCommand(identifier, B_BAD_VALUE); return; }
    const auto& current = CurrentSearchEngine();
    if (method == "get") {
        auto engines = nlohmann::json::array();
        for (const auto& engine : SearchEngines()) {
            const std::string prefix = engine.prefix;
            engines.push_back({{"name", engine.name}, {"isDefault", engine.id == current.id},
                {"favIconUrl", prefix.substr(0, prefix.find('/', 8)) + "/favicon.ico"}});
        }
        fWebKitContext->RespondToBrowserCommandWithData(identifier, B_OK, engines.dump().c_str());
        return;
    }
    if (method != "query" && method != "search") { RespondToCommand(identifier, B_NOT_SUPPORTED); return; }
    // Firefox names the engine as search.get() does; Summit's ids work too.
    const SearchEngine* engine = &current;
    if (arguments.contains("engine")) {
        const std::string name = LowerText(arguments.value("engine", std::string()));
        engine = nullptr;
        for (const auto& each : SearchEngines())
            if (name == LowerText(each.name) || name == each.id) engine = &each;
        if (!engine) {
            fWebKitContext->RespondToBrowserCommandWithData(identifier, B_BAD_VALUE, "null", "No search engine with that name.");
            return;
        }
    }
    const std::string text = Trim(arguments.value("text", std::string()));
    if (text.empty()) {
        fWebKitContext->RespondToBrowserCommandWithData(identifier, B_BAD_VALUE, "null", "The search text is empty.");
        return;
    }
    const std::string url = engine->prefix + PercentEncode(text);
    const std::string disposition = arguments.value("disposition", std::string("CURRENT_TAB"));
    if (disposition == "NEW_WINDOW")
        RequestNewWindow({url}, false);
    else if (disposition == "NEW_TAB" || !(tab = tab ? tab : ActiveTab()))
        CreateTab(url, true);
    else if (summit::IsExtensionPageURL(tab->url))
        ReplaceTabView(*tab, url);
    else {
        if (!PrepareTabNavigation(*tab)) { RespondToCommand(identifier, B_BUSY, { }, "The tab is closing."); return; }
        if (tab->id == fSelected) fAddress->SetText(DisplayURL(url).c_str());
        tab->view->LoadURL(url.c_str());
        tab->pageRevision = fPagesRevision;
    }
    fWebKitContext->RespondToBrowserCommandWithData(identifier, B_OK, "null");
}

// chrome.browsingData (#106), removed as Preferences › History and Data does.
// Site filters work on whole sites (registrable domains), as Summit's data list
// names them.
void BrowserWindow::BrowsingDataCommand(const BMessage& message, uint64 identifier)
{
    const std::string method = message.GetString("method", "");
    const auto arguments = nlohmann::json::parse(message.GetString("arguments", "{}"), nullptr, false);
    if (arguments.is_discarded() || !arguments.is_object()) { RespondToCommand(identifier, B_BAD_VALUE); return; }
    auto fail = [&](const char* error) {
        fWebKitContext->RespondToBrowserCommandWithData(identifier, B_BAD_VALUE, "null", error);
    };
    static const std::map<std::string, uint32> kSiteData {
        {"cookies", B_WEBKIT_DATA_COOKIES}, {"cache", B_WEBKIT_DATA_CACHE}, {"appcache", B_WEBKIT_DATA_CACHE},
        {"localStorage", B_WEBKIT_DATA_LOCAL_STORAGE}, {"indexedDB", B_WEBKIT_DATA_INDEXED_DB},
        {"serviceWorkers", B_WEBKIT_DATA_SERVICE_WORKERS}, {"cacheStorage", B_WEBKIT_DATA_SERVICE_WORKERS},
    };
    if (method == "settings") {
        // Summit keeps no form data; passwords are removed in Passwords only.
        nlohmann::json removable = nlohmann::json::object(), none = nlohmann::json::object();
        for (const char* type : {"appcache", "cache", "cacheStorage", "cookies", "downloads", "fileSystems", "formData", "history",
                 "indexedDB", "localStorage", "passwords", "pluginData", "serviceWorkers", "webSQL"}) {
            removable[type] = std::strcmp(type, "passwords") != 0;
            none[type] = false;
        }
        fWebKitContext->RespondToBrowserCommandWithData(identifier, B_OK, nlohmann::json({{"options", {{"since", 0}}},
            {"dataToRemove", none}, {"dataRemovalPermitted", removable}}).dump().c_str());
        return;
    }
    if (method != "remove") { RespondToCommand(identifier, B_NOT_SUPPORTED); return; }
    BrowsingDataRequest request { identifier };
    bool history = false, downloads = false;
    for (const auto& type : arguments.value("types", nlohmann::json::array())) {
        const std::string name = type.is_string() ? type.get<std::string>() : std::string();
        if (name == "passwords") { fail("Removing passwords is not supported; use Summit's Passwords."); return; }
        if (auto found = kSiteData.find(name); found != kSiteData.end()) request.types |= found->second;
        history |= name == "history";
        downloads |= name == "downloads";
    }
    for (const auto& host : arguments.value("hosts", nlohmann::json::array()))
        if (host.is_string()) request.hosts.push_back(host);
    for (const auto& host : arguments.value("excludeHosts", nlohmann::json::array()))
        if (host.is_string()) request.excludeHosts.push_back(host);
    const bool filtered = !request.hosts.empty() || !request.excludeHosts.empty();
    if (filtered && (history || downloads)) { fail("Origin filters only apply to cookies, caches and site storage."); return; }
    const int64 since = static_cast<int64>(arguments.value("since", 0.0) / 1000);
    // Storage keeps no times: what changed since then is that of the sites
    // visited since, so read them before the history goes.
    std::set<std::string> visited;
    if (since > 0 && request.types && !filtered) visited = fShared->HostsVisitedSince(since);
    if (history) {
        if (since > 0) fShared->ForgetHistorySince(since, BMessenger());
        else {
            fShared->ClearHistory(BMessenger());
            fWebKitContext->NotifyHistoryChanged("removed", nlohmann::json({{"allHistory", true}, {"urls", nlohmann::json::array()}}).dump().c_str());
        }
    }
    if (downloads) DownloadList::Default().ClearFinished(false);
    if (!request.types) { fWebKitContext->RespondToBrowserCommandWithData(identifier, B_OK, "null"); return; }
    const uint64 id = ++fNextBrowsingDataRequest;
    const uint32 types = request.types;
    fBrowsingDataRequests[id] = std::move(request);
    // Without a cookieStoreId (the engine handles one) Firefox removes from
    // every store: the profile's and each container's.
    const std::vector<std::string> hosts(visited.begin(), visited.end());
    fStoreRequests.Issue(id, fWebKitContext, [&](BWebKitContext& context, uint64 identifier) {
        if (filtered)
            context.FetchWebsiteDataSizes(types, BMessenger(this), identifier);
        else if (since > 0)
            context.RemoveWebsiteDataSince(types, since, hosts, BMessenger(this), identifier);
        else
            context.RemoveWebsiteData(types, BMessenger(this), identifier);
    });
}

void BrowserWindow::BrowsingDataReply(const BMessage& message)
{
    // Every store's answer is in (summit::StoreRequests).
    auto answered = fStoreRequests.Reply(message);
    if (!answered) return;
    auto found = fBrowsingDataRequests.find(answered->key);
    if (found == fBrowsingDataRequests.end()) return;
    const uint64 id = found->first;
    auto request = found->second;
    fBrowsingDataRequests.erase(found);
    if (!answered->error.empty()) {
        fWebKitContext->RespondToBrowserCommandWithData(request.command, B_ERROR, "null", answered->error.c_str());
        return;
    }
    if (message.what == B_WEBKIT_WEBSITE_DATA_REMOVED) {
        fWebKitContext->RespondToBrowserCommandWithData(request.command, B_OK, "null");
        return;
    }
    // The sizes: the sites the filter names (a host is in a site when it is
    // the site or under it).
    auto inSite = [](const std::string& host, const std::string& site) {
        return host == site || (host.size() > site.size() && host.compare(host.size() - site.size(), site.size(), site) == 0
            && host[host.size() - site.size() - 1] == '.');
    };
    std::vector<std::string> sites;
    for (const auto& [site, size] : answered->sizes) {
        auto named = [&](const std::vector<std::string>& hosts) {
            return std::any_of(hosts.begin(), hosts.end(), [&](const std::string& host) { return inSite(host, site); });
        };
        if (request.hosts.empty() ? !named(request.excludeHosts) : named(request.hosts)) sites.push_back(site);
    }
    if (sites.empty()) {
        fWebKitContext->RespondToBrowserCommandWithData(request.command, B_OK, "null");
        return;
    }
    const uint32 types = request.types;
    fBrowsingDataRequests[id] = std::move(request);
    fStoreRequests.Issue(id, fWebKitContext, [&](BWebKitContext& context, uint64 identifier) {
        context.RemoveWebsiteDataOfSites(types, sites, BMessenger(this), identifier);
    });
}

// chrome.sessions (#106): the tabs and windows closed while Summit runs,
// newest last, shared by all windows.
struct ClosedSession {
    int64 id = 0;
    int64 time = 0;
    std::vector<PageRecord> tabs;
    bool window = false;
    size_t index = 0;
};
static std::mutex sClosedSessionsLock;
static std::vector<ClosedSession> sClosedSessions;
static int64 sNextClosedSession = 0;
constexpr size_t kMaxClosedSessions = 25;

static nlohmann::json SessionJSON(const ClosedSession& session)
{
    auto tab = [&](const PageRecord& page, size_t index, int64 id) {
        return nlohmann::json({{"sessionId", std::to_string(id)}, {"url", page.url}, {"title", page.title},
            {"index", index}, {"windowId", -1}, {"active", false}, {"highlighted", false}, {"selected", false},
            {"pinned", false}, {"incognito", false}, {"discarded", false}, {"autoDiscardable", true}, {"groupId", -1}});
    };
    nlohmann::json result = {{"lastModified", session.time}};
    if (!session.window) {
        if (!session.tabs.empty()) result["tab"] = tab(session.tabs.front(), session.index, session.id);
        return result;
    }
    auto tabs = nlohmann::json::array();
    for (size_t i = 0; i < session.tabs.size(); ++i) {
        // A window's tabs are restored with it; their ids are the window's.
        auto item = tab(session.tabs[i], i, session.id);
        item.erase("sessionId");
        tabs.push_back(std::move(item));
    }
    result["window"] = {{"sessionId", std::to_string(session.id)}, {"tabs", std::move(tabs)}, {"type", "normal"},
        {"state", "normal"}, {"focused", false}, {"incognito", false}, {"alwaysOnTop", false}};
    return result;
}

void BrowserWindow::RecordClosedSession(const std::vector<PageRecord>& tabs, bool window, size_t index)
{
    if (fPrivate || fPopup) return;
    std::vector<PageRecord> kept;
    for (const auto& page : tabs)
        if (!page.url.empty() && page.url != "about:blank") kept.push_back(page);
    if (kept.empty()) return;
    window = window && kept.size() > 1;
    {
        std::lock_guard lock(sClosedSessionsLock);
        sClosedSessions.push_back({++sNextClosedSession, static_cast<int64>(std::time(nullptr)), std::move(kept), window, index});
        if (sClosedSessions.size() > kMaxClosedSessions) sClosedSessions.erase(sClosedSessions.begin());
    }
    fWebKitContext->NotifySessionsChanged();
}

void BrowserWindow::ForgetClosedSessionTab(const std::string& url)
{
    {
        std::lock_guard lock(sClosedSessionsLock);
        auto found = std::find_if(sClosedSessions.rbegin(), sClosedSessions.rend(), [&](const ClosedSession& session) {
            return !session.window && session.tabs.front().url == url;
        });
        if (found == sClosedSessions.rend()) return;
        sClosedSessions.erase(std::next(found).base());
    }
    fWebKitContext->NotifySessionsChanged();
}

void BrowserWindow::SessionsCommand(const BMessage& message, uint64 identifier)
{
    const std::string method = message.GetString("method", "");
    const auto arguments = nlohmann::json::parse(message.GetString("arguments", "{}"), nullptr, false);
    if (arguments.is_discarded() || !arguments.is_object()) { RespondToCommand(identifier, B_BAD_VALUE); return; }
    auto respond = [&](const nlohmann::json& data) {
        fWebKitContext->RespondToBrowserCommandWithData(identifier, B_OK, data.dump().c_str());
    };
    const std::string wanted = arguments.value("sessionId", std::string());
    auto matches = [&](const ClosedSession& session) { return wanted.empty() || std::to_string(session.id) == wanted; };
    if (method == "getRecentlyClosed") {
        const size_t limit = static_cast<size_t>(std::clamp(arguments.value("maxResults", 25.0), 0.0, 25.0));
        auto sessions = nlohmann::json::array();
        std::lock_guard lock(sClosedSessionsLock);
        for (auto it = sClosedSessions.rbegin(); it != sClosedSessions.rend() && sessions.size() < limit; ++it)
            sessions.push_back(SessionJSON(*it));
        respond(sessions);
        return;
    }
    if (method != "restore" && method != "forget") { RespondToCommand(identifier, B_NOT_SUPPORTED); return; }
    std::optional<ClosedSession> taken;
    {
        std::lock_guard lock(sClosedSessionsLock);
        auto found = std::find_if(sClosedSessions.rbegin(), sClosedSessions.rend(), matches);
        if (found != sClosedSessions.rend()) {
            taken = *found;
            sClosedSessions.erase(std::next(found).base());
        }
    }
    if (!taken) {
        fWebKitContext->RespondToBrowserCommandWithData(identifier, B_BAD_VALUE, "null",
            wanted.empty() ? "There are no recently closed sessions." : "Could not find a session with that id.");
        return;
    }
    fWebKitContext->NotifySessionsChanged();
    if (method == "forget") { respond(nullptr); return; }
    std::vector<std::string> urls;
    for (const auto& page : taken->tabs) urls.push_back(page.url);
    if (taken->window) RequestNewWindow(urls, false);
    else {
        // Reopen Closed Tab no longer offers it.
        auto reopen = std::find_if(fClosedTabs.rbegin(), fClosedTabs.rend(), [&](const PageRecord& page) { return page.url == urls.front(); });
        if (reopen != fClosedTabs.rend()) fClosedTabs.erase(std::next(reopen).base());
        CreateTab(urls.front(), true);
    }
    taken->time = static_cast<int64>(std::time(nullptr));
    respond(SessionJSON(*taken));
}

// What extensions see of each node, for comparing two states of the bookmarks.
struct BookmarkEntry {
    int64_t parent = 0;
    size_t index = 0;
    std::string title, url;
    // Without children; a removed folder's are put together from the others.
    nlohmann::json node;
};
using BookmarkEntries = std::map<int64_t, BookmarkEntry>;
static std::mutex sBookmarkReportLock;
static std::optional<BookmarkEntries> sReportedBookmarks;
static const SharedProfile* sReportedProfile = nullptr;

static BookmarkEntries BookmarkSnapshot(const Bookmarks& bookmarks)
{
    BookmarkEntries entries;
    for (const auto* folder : {&bookmarks.Bar(), &bookmarks.Other()}) {
        EachBookmark(*folder, [&](const BookmarkNode& node, int64_t parent, size_t index) {
            entries[node.id] = {parent, index, node.title, node.url, BookmarkJSON(node, parent, index, 0)};
        });
    }
    return entries;
}

// A node of entries with its children, as onRemoved reports a folder.
static nlohmann::json BookmarkTreeJSON(const BookmarkEntries& entries, const std::multimap<int64_t, int64_t>& children, int64_t id)
{
    auto node = entries.at(id).node;
    if (node.value("type", std::string()) != "folder") return node;
    std::vector<int64_t> inside;
    for (auto [it, end] = children.equal_range(id); it != end; ++it) inside.push_back(it->second);
    std::sort(inside.begin(), inside.end(), [&](int64_t a, int64_t b) { return entries.at(a).index < entries.at(b).index; });
    auto list = nlohmann::json::array();
    for (auto child : inside) list.push_back(BookmarkTreeJSON(entries, children, child));
    node["children"] = std::move(list);
    return node;
}

void BrowserWindow::ReportBookmarkChanges()
{
    // Private windows can use a context of their own; the main one reports.
    if (fPrivate) return;
    std::vector<std::tuple<const char*, int64_t, nlohmann::json>> events;
    {
        std::lock_guard lock(sBookmarkReportLock);
        auto now = fShared->Read([](const Profile& profile) { return BookmarkSnapshot(profile.bookmarks); });
        if (!sReportedBookmarks || sReportedProfile != fShared.get()) {
            sReportedBookmarks = std::move(now);
            sReportedProfile = fShared.get();
            return;
        }
        const auto& before = *sReportedBookmarks;
        // Removed: only the top of a removed folder, as in Chrome.
        std::optional<std::multimap<int64_t, int64_t>> children;
        for (const auto& [id, entry] : before) {
            if (now.contains(id) || (before.contains(entry.parent) && !now.contains(entry.parent))) continue;
            if (!children) {
                children.emplace();
                for (const auto& [child, item] : before) children->emplace(item.parent, child);
            }
            events.emplace_back("removed", id, nlohmann::json({{"parentId", std::to_string(entry.parent)}, {"index", entry.index},
                {"node", BookmarkTreeJSON(before, *children, id)}}));
        }
        // Moved: a new folder, or out of order with the siblings that stayed
        // (those off the longest run kept in their old order).
        std::map<int64_t, std::vector<int64_t>> stayed;
        for (const auto& [id, entry] : now) {
            auto old = before.find(id);
            if (old == before.end()) continue;
            if (old->second.parent != entry.parent)
                events.emplace_back("moved", id, nlohmann::json({{"parentId", std::to_string(entry.parent)}, {"index", entry.index},
                    {"oldParentId", std::to_string(old->second.parent)}, {"oldIndex", old->second.index}}));
            else
                stayed[entry.parent].push_back(id);
        }
        for (auto& [parent, ids] : stayed) {
            std::sort(ids.begin(), ids.end(), [&](int64_t a, int64_t b) { return now.at(a).index < now.at(b).index; });
            // Longest increasing run of old indexes, by patience sorting.
            std::vector<size_t> tails, previous(ids.size(), SIZE_MAX), tailAt;
            for (size_t i = 0; i < ids.size(); ++i) {
                const size_t old = before.at(ids[i]).index;
                auto at = std::lower_bound(tails.begin(), tails.end(), old) - tails.begin();
                if (at == static_cast<ptrdiff_t>(tails.size())) { tails.push_back(old); tailAt.push_back(i); }
                else { tails[at] = old; tailAt[at] = i; }
                if (at) previous[i] = tailAt[at - 1];
            }
            std::set<size_t> kept;
            for (size_t i = tailAt.empty() ? SIZE_MAX : tailAt.back(); i != SIZE_MAX; i = previous[i]) kept.insert(i);
            for (size_t i = 0; i < ids.size(); ++i) {
                if (kept.contains(i)) continue;
                const auto& entry = now.at(ids[i]);
                events.emplace_back("moved", ids[i], nlohmann::json({{"parentId", std::to_string(parent)}, {"index", entry.index},
                    {"oldParentId", std::to_string(parent)}, {"oldIndex", before.at(ids[i]).index}}));
            }
        }
        for (const auto& [id, entry] : now) {
            auto old = before.find(id);
            if (old == before.end()) {
                events.emplace_back("created", id, entry.node);
            } else if (old->second.title != entry.title || old->second.url != entry.url) {
                nlohmann::json info = {{"title", entry.title}};
                if (!entry.url.empty()) info["url"] = entry.url;
                events.emplace_back("changed", id, std::move(info));
            }
        }
        sReportedBookmarks = std::move(now);
    }
    for (const auto& [event, id, info] : events)
        fWebKitContext->NotifyBookmarksChanged(event, std::to_string(id).c_str(), info.dump().c_str());
}

}
#endif

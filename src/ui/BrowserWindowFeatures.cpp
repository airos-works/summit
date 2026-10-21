#if SUMMIT_MODERN_WEBKIT
// The browser window's October 2026 features: tabs that are pinned, muted and
// dragged between windows (#68, #70), crashed and hung pages (#71), blocked
// pop-ups (#73), app links from redirects (#64), Picture in Picture (#74),
// spelling (#66), saved passwords (#67) and printing (#65).
#include "BrowserWindow.h"
#include "Chrome.h"
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
#include <WebKit/WebKitInfo.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
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
        return "https://github.com/jmgasper/summit/issues/new?title=" + encode(title + " on " + HostOf(url)) + "&body=" + encode(body);
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
    if (tab && name == "summitPopups") {
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

void BrowserWindow::DetachTab(int64 id, const BMessenger& target)
{
    auto found = std::find_if(fTabs.begin(), fTabs.end(), [&](const Tab& tab) { return tab.id == id; });
    if (found == fTabs.end() || fClosingWindow || found->closeQueued || found->closeRequested || found->closeApproved) return;
    if (fFullScreenTab == id) ExitPageFullScreen();
    if (fPictureInPictureTab == id && fPictureInPicture.IsValid()) fPictureInPicture.SendMessage(B_QUIT_REQUESTED);
    DropTabAuthentications(id);
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
    adopt.AddInt32("index", -1);
    if (target.SendMessage(&adopt) != B_OK) {
        // The other window went away: keep the tab here.
        fCards->AddView(moved->view);
        moved->messenger = BMessenger(moved->view);
        fTabs.push_back(std::move(*moved));
        delete moved;
        RefreshChrome();
        return;
    }
    if (fTabs.empty()) {
        // The last tab left: the window goes too.
        fClosingWindow = true;
        SaveSession();
        PostMessage(B_QUIT_REQUESTED);
        Hide();
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
    if (fClosingWindow) {
        // Too late for this window: the page closes with it.
        delete tab->view;
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
    fTabs.insert(tab->pinned ? fTabs.begin() + pinned : fTabs.end(), std::move(*tab));
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

void BrowserWindow::PrintPage()
{
    auto* tab = ActiveTab();
    if (!tab || tab->processExited) return;
    // The print job runs on its own thread: the printer's dialogs wait for
    // the user, the pages come from the engine one at a time.
    PrintJob::Start(tab->id, tab->title, DisplayURL(tab->url), BMessenger(this));
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
}
#endif

// The browser window's touch layout for air/OS Mobile (Mobile Kit): Safari's
// on the early iPhone and iPad, with Chrome's tab grid. See docs/mobile.md.
//
// A phone-sized window (compact) has the address field in a metal bar above
// the page and Back, Forward, Actions, Bookmarks and Tabs in a bar below it;
// a wider one has everything in the bar above. The page area can give way to
// three panels: the tab switcher (a grid of pictures of the open pages, and
// the private ones), the Bookmarks, Reading List and History lists, and the
// bookmark editor. The desktop's tabs, views and address field are the same
// objects in both presentations.
#include "BrowserWindow.h"
#include "compat/MobileKit.h"

#include "AddressSuggestions.h"
#include "BookmarkEditor.h"
#include "Chrome.h"
#include "FaviconCache.h"
#include "FeatureMessages.h"
#include "Messages.h"
#include "SharedProfile.h"
#include "core/Bookmarks.h"
#include "core/ReadingList.h"
#include "core/Address.h"
#include "core/UserAgent.h"

#include <Alert.h>
#include <Application.h>
#include <Bitmap.h>
#include <CardLayout.h>
#include <CardView.h>
#include <Clipboard.h>
#include <LayoutBuilder.h>
#include <MenuBar.h>
#include <Screen.h>
#include <SpaceLayoutItem.h>
#include <StringView.h>
#include <TextControl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

#if SUMMIT_MODERN_WEBKIT
#include <WebKit/WebKitView.h>
#include <nlohmann/json.hpp>
#endif

namespace summit {
namespace {
using namespace BMobileKit;

// The panels and their controls, to the window.
constexpr uint32 kMobileTabs = 'mTab';          // the tab switcher
constexpr uint32 kMobileMore = 'mMor';          // the actions sheet
constexpr uint32 kMobileLists = 'mLst';         // the lists ("section")
constexpr uint32 kMobileDone = 'mDon';          // back to the page
constexpr uint32 kMobileOpenTab = 'mOpT';       // a card ("id")
constexpr uint32 kMobileCloseTab = 'mClT';      // a card's ✕ ("id")
constexpr uint32 kMobileAddTab = 'mNwT';
constexpr uint32 kMobileCloseAll = 'mCAl';
constexpr uint32 kMobileCloseAllReply = 'mCAr';
constexpr uint32 kMobileTabMode = 'mTMd';       // Tabs or Private ("index")
// From another window: show this window's tab switcher.
constexpr uint32 kMobileShowTabs = 'mShT';
constexpr uint32 kMobileCancelAddress = 'mCan';
constexpr uint32 kMobileReloadStop = 'mRld';
constexpr uint32 kMobileSection = 'mSec';       // the lists' segments ("index")
constexpr uint32 kMobileRow = 'mRow';           // a row ("kind", "id", "url")
constexpr uint32 kMobileRowDelete = 'mRDl';
constexpr uint32 kMobileListBack = 'mNBk';
constexpr uint32 kMobileEdit = 'mEdt';
constexpr uint32 kMobileNewFolder = 'mNFd';
constexpr uint32 kMobileEditorSave = 'mSvB';
constexpr uint32 kMobileEditorCancel = 'mCnB';
constexpr uint32 kMobileEditorFolder = 'mFdC';  // a folder row ("id")
constexpr uint32 kMobileBookmarkPage = 'mAdB';
constexpr uint32 kMobileDesktopSite = 'mDsk';
constexpr uint32 kMobileCopyLink = 'mCpy';
constexpr uint32 kMobileClearHistory = 'mClH';
constexpr uint32 kMobileNewPrivateTab = 'mNPv';
constexpr uint32 kMobileSettings = 'mSet';      // the settings panel
constexpr uint32 kMobileSetting = 'mStg';       // a settings row ("key", "value")
constexpr uint32 kMobileSettingsBack = 'mSBk';
constexpr uint32 kMobileClearDataReply = 'mCDr';

enum Panel : int32 { kPagePanel, kTabsPanel, kListPanel, kEditorPanel, kSettingsPanel };
enum Section : int32 { kBookmarksSection, kReadingListSection, kHistorySection };
enum RowKind : int32 { kFolderRow, kBookmarkRow, kReadingRow, kHistoryRow };

// Chrome on Android: sites send it their phone layouts and media that plays
// through Media Source (an iPhone's agent gets HLS, which the engine lacks).
constexpr const char* kMobileUserAgent = "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/140.0.0.0 Mobile Safari/537.36";

// Pictures in the tab switcher are at most this wide (in device pixels).
constexpr float kPictureWidth = 480;

BMessage* Message(uint32 what)
{
    return new BMessage(what);
}

MetalButton* GlyphButton(const char* name, Glyph glyph, uint32 what, const char* tip)
{
    auto* button = new MetalButton(name, nullptr, Message(what), B_PLAIN_BUTTON, glyph);
    button->SetToolTip(tip);
    return button;
}

// A copy of a picture scaled to fit width (keeping its aspect).
BBitmap* ScaledCopy(const BBitmap& source, float width)
{
    const BRect from = source.Bounds();
    if (!from.IsValid()) return nullptr;
    const float scale = std::min(1.0f, width / (from.Width() + 1));
    const BRect to(0, 0, std::max(1.0f, floorf((from.Width() + 1) * scale)) - 1,
        std::max(1.0f, floorf((from.Height() + 1) * scale)) - 1);
    auto* scaled = new BBitmap(to, B_BITMAP_ACCEPTS_VIEWS, B_RGB32);
    if (scaled->InitCheck() != B_OK) { delete scaled; return nullptr; }
    auto* view = new BView(to, "scale", B_FOLLOW_NONE, 0);
    scaled->AddChild(view);
    if (scaled->Lock()) {
        view->DrawBitmap(&source, from, to, B_FILTER_BITMAP_BILINEAR);
        view->Sync();
        scaled->Unlock();
    }
    scaled->RemoveChild(view);
    delete view;
    return scaled;
}

// A picture that is all one dark colour (a frame not drawn yet).
bool IsBlank(const BBitmap& bitmap)
{
    if (bitmap.ColorSpace() != B_RGB32 && bitmap.ColorSpace() != B_RGBA32) return false;
    const auto* bits = static_cast<const uint8*>(bitmap.Bits());
    const int32 rows = bitmap.Bounds().IntegerHeight() + 1, columns = bitmap.Bounds().IntegerWidth() + 1;
    const int32 stride = bitmap.BytesPerRow();
    for (int32 y = 0; y < rows; y += std::max(1, rows / 24))
        for (int32 x = 0; x < columns; x += std::max(1, columns / 24)) {
            const uint8* pixel = bits + y * stride + x * 4;
            if (pixel[0] > 24 || pixel[1] > 24 || pixel[2] > 24) return false;
        }
    return true;
}

std::string HostOf(const std::string& url)
{
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return url;
    const auto start = scheme + 3;
    const auto end = url.find_first_of("/?#", start);
    std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (host.starts_with("www.")) host.erase(0, 4);
    return host;
}

std::string DayHeading(int64_t seconds)
{
    if (!seconds) return "Earlier";
    const std::time_t now = std::time(nullptr);
    std::tm today {}, day {};
    localtime_r(&now, &today);
    const std::time_t time = static_cast<std::time_t>(seconds);
    localtime_r(&time, &day);
    std::tm midnight = today;
    midnight.tm_hour = midnight.tm_min = midnight.tm_sec = 0;
    const std::time_t start = std::mktime(&midnight);
    if (time >= start) return "Today";
    if (time >= start - 86400) return "Yesterday";
    char text[64];
    std::strftime(text, sizeof text, "%A %e %B", &day);
    return text;
}

// Empties a group layout; its views keep existing, without a parent.
void ClearLayout(BGroupLayout* layout)
{
    while (layout->CountItems() > 0) {
        BLayoutItem* item = layout->ItemAt(0);
        if (BView* view = item->View()) layout->RemoveView(view);
        else delete layout->RemoveItem(int32(0));
    }
}

void ShowView(BView* view, bool shown)
{
    if (!view) return;
    if (shown && view->IsHidden(view)) view->Show();
    else if (!shown && !view->IsHidden(view)) view->Hide();
}

BView* Glue() { return nullptr; }
}

const char* BrowserWindow::NormalUserAgent(const Tab& tab) const
{
    if (fMobile && !tab.desktopSite) return kMobileUserAgent;
    return fShared->Read([](const Profile& profile) { return FindUserAgentPreset(profile.userAgent).agent; });
}

void BrowserWindow::BuildMobileChrome()
{
    auto& ui = fMobileUI;
    ui.top = new MetalBar("mobile-top", B_MOBILE_TOP_BAR);
    ui.bottom = new MetalBar("mobile-bottom", B_MOBILE_BOTTOM_BAR);
    ui.back = GlyphButton("mobile-back", B_GLYPH_ARROW_BACK, kBack, "Back");
    ui.forward = GlyphButton("mobile-forward", B_GLYPH_ARROW_FORWARD, kForward, "Forward");
    ui.action = GlyphButton("mobile-actions", B_GLYPH_ACTION, kMobileMore, "Share and more");
    ui.bookmarks = GlyphButton("mobile-bookmarks", B_GLYPH_BOOK, kMobileLists, "Bookmarks, Reading List and History");
    ui.tabs = GlyphButton("mobile-tabs", B_GLYPH_PAGES, kMobileTabs, "Tabs");
    ui.reload = GlyphButton("mobile-reload", B_GLYPH_RELOAD, kMobileReloadStop, "Reload");
    ui.newTab = GlyphButton("mobile-new-tab", B_GLYPH_ADD, kNewTab, "New tab");
    ui.cancel = new MetalButton("mobile-cancel", "Cancel", Message(kMobileCancelAddress));
    ui.tabBar = new TabBar("mobile-tab-bar", Message(kSelectTab), Message(kCloseTab));
    // The address field and the buttons go into the bars when the window
    // becomes mobile (LayoutMobileChrome).

    // The tab switcher: Tabs or Private over a grid of the pages, and Close
    // All, New Tab and Done under it.
    auto* tabsTop = new MetalBar("mobile-tabs-top", B_MOBILE_TOP_BAR);
    ui.tabMode = new SegmentedControl("mobile-tab-mode", Message(kMobileTabMode));
    ui.tabMode->AddSegment("Tabs");
    ui.tabMode->AddSegment("Private");
    ui.tabMode->SetExplicitMinSize(BSize(TargetSize() * 5, B_SIZE_UNSET));
    BLayoutBuilder::Group<>(tabsTop->GroupLayout()).AddGlue().Add(ui.tabMode).AddGlue();
    ui.grid = new CardGrid("mobile-tab-grid", Message(kMobileOpenTab), Message(kMobileCloseTab));
    auto* tabsBottom = new MetalBar("mobile-tabs-bottom", B_MOBILE_BOTTOM_BAR);
    ui.closeAll = new MetalButton("mobile-close-all", "Close All", Message(kMobileCloseAll));
    ui.add = new MetalButton("mobile-add-tab", nullptr, Message(kMobileAddTab), B_PILL_BUTTON, B_GLYPH_ADD);
    ui.add->SetExplicitMinSize(BSize(TargetSize() * 2, B_SIZE_UNSET));
    ui.add->SetToolTip("New tab");
    ui.tabsDone = new MetalButton("mobile-tabs-done", "Done", Message(kMobileDone));
    BLayoutBuilder::Group<>(tabsBottom->GroupLayout())
        .Add(ui.closeAll).AddGlue().Add(ui.add).AddGlue().Add(ui.tabsDone);
    ui.tabsPanel = new BGroupView("mobile-tab-switcher", B_VERTICAL, 0);
    BLayoutBuilder::Group<>(static_cast<BGroupView*>(ui.tabsPanel))
        .Add(tabsTop).Add(ui.grid, 1).Add(tabsBottom);

    // Bookmarks, Reading List and History: a navigation bar, the rows, and
    // Edit with the three sections under them.
    auto* listTop = new MetalBar("mobile-list-top", B_MOBILE_TOP_BAR);
    ui.listBack = new MetalButton("mobile-list-back", "Bookmarks", Message(kMobileListBack), B_BACK_BUTTON);
    ui.listTitle = new TitleView("mobile-list-title", "Bookmarks");
    ui.listDone = new MetalButton("mobile-list-done", "Done", Message(kMobileDone));
    BLayoutBuilder::Group<>(listTop->GroupLayout()).Add(ui.listBack).Add(ui.listTitle, 1).Add(ui.listDone);
    ui.table = new TableView("mobile-list");
    auto* listBottom = new MetalBar("mobile-list-bottom", B_MOBILE_BOTTOM_BAR);
    ui.edit = new MetalButton("mobile-list-edit", "Edit", Message(kMobileEdit));
    ui.section = new SegmentedControl("mobile-list-section", Message(kMobileSection));
    ui.section->AddSegment(nullptr, B_GLYPH_BOOK);
    ui.section->AddSegment(nullptr, B_GLYPH_GLASSES);
    ui.section->AddSegment(nullptr, B_GLYPH_CLOCK);
    ui.section->SetExplicitMinSize(BSize(TargetSize() * 4.5f, B_SIZE_UNSET));
    ui.newFolder = new MetalButton("mobile-new-folder", "New Folder", Message(kMobileNewFolder));
    BLayoutBuilder::Group<>(listBottom->GroupLayout())
        .Add(ui.edit).AddGlue().Add(ui.section).AddGlue().Add(ui.newFolder);
    ui.listPanel = new BGroupView("mobile-lists", B_VERTICAL, 0);
    BLayoutBuilder::Group<>(static_cast<BGroupView*>(ui.listPanel))
        .Add(listTop).Add(Scroll("mobile-list-scroll", ui.table), 1).Add(listBottom);

    // The bookmark editor: Cancel, the title and Save; the name, the address
    // and the folder to keep it in.
    auto* editTop = new MetalBar("mobile-editor-top", B_MOBILE_TOP_BAR);
    ui.editCancel = new MetalButton("mobile-editor-cancel", "Cancel", Message(kMobileEditorCancel));
    ui.editTitle = new TitleView("mobile-editor-title", "Add Bookmark");
    ui.editSave = new MetalButton("mobile-editor-save", "Save", Message(kMobileEditorSave));
    BLayoutBuilder::Group<>(editTop->GroupLayout()).Add(ui.editCancel).Add(ui.editTitle, 1).Add(ui.editSave);
    ui.editName = new BTextControl("mobile-editor-name", nullptr, "", nullptr);
    ui.editURL = new BTextControl("mobile-editor-address", nullptr, "", nullptr);
    auto* nameLabel = new BStringView("mobile-editor-name-label", "Name");
    auto* urlLabel = new BStringView("mobile-editor-address-label", "Address");
    for (auto* label : {nameLabel, urlLabel}) label->SetFont(be_bold_font);
    ui.editURLLabel = urlLabel;
    ui.editFolders = new TableView("mobile-editor-folders");
    auto* form = new BGroupView("mobile-editor-form", B_VERTICAL, 0);
    BLayoutBuilder::Group<>(form)
        .AddGrid(B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
            .SetInsets(B_USE_WINDOW_SPACING)
            .Add(nameLabel, 0, 0).Add(ui.editName, 1, 0)
            .Add(urlLabel, 0, 1).Add(ui.editURL, 1, 1)
        .End();
    ui.editPanel = new BGroupView("mobile-editor", B_VERTICAL, 0);
    BLayoutBuilder::Group<>(static_cast<BGroupView*>(ui.editPanel))
        .Add(editTop).Add(form).Add(Scroll("mobile-editor-scroll", ui.editFolders), 1);

    // Settings: Back (from a choice), the title and Done over a table.
    auto* settingsTop = new MetalBar("mobile-settings-top", B_MOBILE_TOP_BAR);
    ui.settingsBack = new MetalButton("mobile-settings-back", "Settings", Message(kMobileSettingsBack), B_BACK_BUTTON);
    ui.settingsTitle = new TitleView("mobile-settings-title", "Settings");
    ui.settingsDone = new MetalButton("mobile-settings-done", "Done", Message(kMobileDone));
    BLayoutBuilder::Group<>(settingsTop->GroupLayout()).Add(ui.settingsBack).Add(ui.settingsTitle, 1).Add(ui.settingsDone);
    ui.settings = new TableView("mobile-settings");
    ui.settingsPanel = new BGroupView("mobile-settings-panel", B_VERTICAL, 0);
    BLayoutBuilder::Group<>(static_cast<BGroupView*>(ui.settingsPanel))
        .Add(settingsTop).Add(Scroll("mobile-settings-scroll", ui.settings), 1);

    ui.panels = new BCardView("mobile-panels");
    ui.panels->CardLayout()->AddView(ui.tabsPanel);
    ui.panels->CardLayout()->AddView(ui.listPanel);
    ui.panels->CardLayout()->AddView(ui.editPanel);
    ui.panels->CardLayout()->AddView(ui.settingsPanel);

#if SUMMIT_MODERN_WEBKIT
    BView* content = fContentSplit;
#else
    BView* content = fPages;
#endif
    // The bars never hold the window wider than the shell's area: their
    // controls are laid out again for a narrower window (LayoutMobileChrome).
    for (BView* bar : {static_cast<BView*>(ui.top), static_cast<BView*>(ui.bottom), static_cast<BView*>(ui.panels)})
        bar->SetExplicitMinSize(BSize(TargetSize() * 6, B_SIZE_UNSET));
    fLayout->AddView(0, ui.top);
    fLayout->AddView(1, ui.tabBar);
    ui.tabBar->Hide();
    fLayout->AddView(fLayout->IndexOfView(content) + 1, ui.panels, 1);
    fLayout->AddView(ui.bottom);
    ui.top->Hide();
    ui.bottom->Hide();
    ui.panels->Hide();
}

// Puts the controls in the bars for the window's width: a phone's bars or
// a tablet's one bar.
void BrowserWindow::LayoutMobileChrome(bool forceCompact)
{
    auto& ui = fMobileUI;
    const bool compact = forceCompact || Bounds().Width() < TargetSize() * 15;
    const bool attached = fAddress->Parent() == ui.top;
    if (attached && compact == ui.compact) return;
    ui.compact = compact;
    BGroupLayout* top = ui.top->GroupLayout();
    BGroupLayout* bottom = ui.bottom->GroupLayout();
    ClearLayout(top);
    ClearLayout(bottom);
    auto add = [](BGroupLayout* layout, BView* view, float weight = 0) {
        if (view) layout->AddView(view, weight);
        else layout->AddItem(BSpaceLayoutItem::CreateGlue());
    };
    if (compact) {
        add(top, fAddress, 1);
        add(top, ui.reload);
        add(top, ui.cancel);
        for (BView* view : {Glue(), static_cast<BView*>(ui.back), Glue(), static_cast<BView*>(ui.forward), Glue(),
                static_cast<BView*>(ui.action), Glue(), static_cast<BView*>(ui.bookmarks), Glue(),
                static_cast<BView*>(ui.tabs), Glue()})
            add(bottom, view);
        ShowView(ui.newTab, false);
        add(bottom, ui.newTab);
    } else {
        for (BView* view : {static_cast<BView*>(ui.back), static_cast<BView*>(ui.forward), static_cast<BView*>(ui.reload)})
            add(top, view);
        add(top, fAddress, 1);
        for (BView* view : {static_cast<BView*>(ui.cancel), static_cast<BView*>(ui.action), static_cast<BView*>(ui.bookmarks),
                static_cast<BView*>(ui.tabs), static_cast<BView*>(ui.newTab)})
            add(top, view);
        ShowView(ui.newTab, true);
    }
    ShowView(ui.bottom, compact && ui.panel == kPagePanel);
    ShowView(ui.tabBar, !compact && ui.panel == kPagePanel);
    RefreshMobileChrome();
}

void BrowserWindow::SetMobileView(bool mobile)
{
    if (mobile == fMobile) return;
    if (fMobileSheet.IsValid()) fMobileSheet.SendMessage(B_QUIT_REQUESTED);
#if SUMMIT_MODERN_WEBKIT
    ExitPageFullScreen();
#endif
    auto& ui = fMobileUI;
    fMobile = mobile;
    fSuggestions->Hide();
    // A private window's bars are graphite, as a reminder.
    SetWindowSkin(this, mobile && fPrivate ? B_GRAPHITE_SKIN : B_PANEL_SKIN);
    // Move the omnibox itself so its URL, selection and submit target survive.
    if (mobile) {
        fDesktopFrame = Frame();
        fAddressIndex = fToolbar->GroupLayout()->IndexOfView(fAddress);
        fToolbar->GroupLayout()->RemoveView(fAddress);
        SetAddressMobile(true);
        fAddress->SetExplicitMinSize(BSize(TargetSize() * 3, TargetSize()));
        fAddress->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, TargetSize()));
        // Phone bars first: the shell sizes the window after this.
        ui.compact = false;
        ClearLayout(ui.top->GroupLayout());
        ui.top->GroupLayout()->AddView(fAddress);
        LayoutMobileChrome(true);
        // These hides have matching shows, including for desktop chrome
        // already hidden by a popup or the user's bookmarks preference.
        for (BView* view : std::initializer_list<BView*>{fDesktopMenu, fToolbar, fBookmarksBar, fTabStrip, fStatusLine})
            view->Hide();
        ui.top->Show();
        ui.panel = kPagePanel;
        ShowMobilePanel(kPagePanel);
        SetSizeLimits(280, 10000, 150, 10000);
    } else {
        ShowMobilePanel(kPagePanel);
        ui.top->Hide();
        if (!ui.tabBar->IsHidden(ui.tabBar)) ui.tabBar->Hide();
        if (!ui.bottom->IsHidden(ui.bottom)) ui.bottom->Hide();
        fAddress->RemoveSelf();
        ClearLayout(ui.top->GroupLayout());
        ClearLayout(ui.bottom->GroupLayout());
        SetAddressMobile(false);
        fToolbar->GroupLayout()->AddView(fAddressIndex, fAddress, 3);
        fAddress->SetExplicitMinSize(BSize(fPopup ? 120 : 240, 30));
        fAddress->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
        for (BView* view : std::initializer_list<BView*>{fDesktopMenu, fToolbar, fBookmarksBar, fTabStrip, fStatusLine})
            view->Show();
        bool showBookmarks = fBookmarksBarVisible && fWebApp.empty() && !fPopup;
        if (showBookmarks && fBookmarksBar->IsHidden(fBookmarksBar)) fBookmarksBar->Show();
        if (!showBookmarks && !fBookmarksBar->IsHidden(fBookmarksBar)) fBookmarksBar->Hide();
        ApplyInterfaceStyle();
        SetSizeLimits(fPopup ? 200 : fWebApp.empty() ? 760 : 320, 10000,
            fPopup ? 150 : fWebApp.empty() ? 450 : 240, 10000);
    }
    // New pages ask for the phone's or the desktop's layout.
#if SUMMIT_MODERN_WEBKIT
    for (auto& tab : fTabs)
        if (!ResponsiveUserAgent(tab.responsive)) tab.view->SetCustomUserAgent(NormalUserAgent(tab));
#endif
    RefreshChrome();
}

// Which panel replaces the page area, and the bars that go with it.
void BrowserWindow::ShowMobilePanel(int32 panel)
{
    auto& ui = fMobileUI;
    if (fMobileSheet.IsValid()) fMobileSheet.SendMessage(B_QUIT_REQUESTED);
#if SUMMIT_MODERN_WEBKIT
    BView* content = fContentSplit;
#else
    BView* content = fPages;
#endif
    if (panel != kPagePanel && ui.panel == kPagePanel)
        if (auto* tab = ActiveTab()) CaptureTabPicture(*tab);
    ui.panel = panel;
    const bool page = panel == kPagePanel || !fMobile;
    if (!page) {
        HideSuggestions();
        BView* card = panel == kTabsPanel ? ui.tabsPanel : panel == kListPanel ? ui.listPanel
            : panel == kSettingsPanel ? ui.settingsPanel : ui.editPanel;
        ui.panels->CardLayout()->SetVisibleItem(ui.panels->CardLayout()->IndexOfView(card));
    }
    ShowView(ui.panels, !page);
    ShowView(content, page);
    ShowView(ui.top, fMobile && page);
    ShowView(ui.bottom, fMobile && page && ui.compact);
    ShowView(ui.tabBar, fMobile && page && !ui.compact);
    if (!page && !fFindBar->IsHidden(fFindBar)) PostMessage(kCloseFind);
    switch (panel) {
        case kTabsPanel:
            // The grid's size decides where the page on show is.
            Layout(true);
            RefreshMobileTabs();
            ui.grid->ScrollTo(0, 0);
            ui.grid->ScrollToSelected();
            ui.grid->MakeFocus();
            break;
        case kListPanel:
            RefreshMobileList();
            ui.table->MakeFocus();
            break;
        case kEditorPanel:
            ui.editName->MakeFocus();
            break;
        case kSettingsPanel:
            RefreshMobileSettings();
            ui.settings->MakeFocus();
            break;
        default:
            if (auto* tab = ActiveTab(); tab && fMobile) tab->view->MakeFocus();
            break;
    }
}

void BrowserWindow::CaptureTabPicture(Tab& tab)
{
    if (!fMobile || !tab.view || tab.view->IsHidden(tab.view) || !tab.view->Window() || IsMinimized()) return;
#if SUMMIT_MODERN_WEBKIT
    // A page that has only just come on show may not have been drawn yet.
    if (tab.lastSelected && system_time() - tab.lastSelected < 700000) return;
    if (tab.processExited || tab.crashView || tab.errorView) return;
#endif
    std::unique_ptr<BBitmap> frame;
    // What is on the screen, while the window is in front; frames put
    // straight into the screen are not kept by the view.
    if (IsActive() || IsFront()) {
        BScreen screen(this);
        BBitmap* shot = nullptr;
        BRect area = tab.view->ConvertToScreen(tab.view->Bounds()) & screen.Frame();
        if (area.IsValid() && screen.GetBitmap(&shot, false, &area) == B_OK) frame.reset(shot);
        // The screen's picture keeps its place on the screen as its bounds.
        if (frame && frame->Bounds().LeftTop() != B_ORIGIN) {
            auto* origin = new BBitmap(frame->Bounds().OffsetToCopy(B_ORIGIN), frame->ColorSpace());
            if (origin->InitCheck() == B_OK && origin->BitsLength() == frame->BitsLength())
                memcpy(origin->Bits(), frame->Bits(), frame->BitsLength());
            frame.reset(origin);
        }
    }
#if SUMMIT_MODERN_WEBKIT
    if (!frame || IsBlank(*frame)) frame.reset(tab.view->CopyPresentedFrame());
#endif
    if (frame && IsBlank(*frame)) frame.reset();
    if (std::getenv("SUMMIT_MOBILE_TRACE") && frame) {
        const auto* p = static_cast<const uint8*>(frame->Bits());
        const int32 mid = (frame->Bounds().IntegerHeight() / 2) * frame->BytesPerRow() + (frame->Bounds().IntegerWidth() / 2) * 4;
        fprintf(stderr, "mobile picture: space %#x %gx%g bpr %d first %d,%d,%d,%d mid %d,%d,%d,%d active %d front %d\n",
            frame->ColorSpace(), frame->Bounds().Width() + 1, frame->Bounds().Height() + 1, (int)frame->BytesPerRow(),
            p[0], p[1], p[2], p[3], p[mid], p[mid + 1], p[mid + 2], p[mid + 3], IsActive(), IsFront());
    }
    if (!frame) return;
    if (BBitmap* scaled = ScaledCopy(*frame, kPictureWidth)) tab.picture.reset(scaled);
}

void BrowserWindow::MobileToast(const char* text)
{
    if (fMobile) ShowToast(this, text);
}

// The bars follow the page on show: Back, Forward, progress, Reload or Stop,
// the count of tabs; an open panel follows the tabs and the profile.
void BrowserWindow::RefreshMobileChrome()
{
    auto& ui = fMobileUI;
    if (!fMobile) return;
    if (auto* tab = ActiveTab()) {
        ui.back->SetEnabled(tab->back || (tab->errorView && tab->errorBack) || tab->safetyView);
        ui.forward->SetEnabled(tab->forward);
        ui.reload->SetGlyph(tab->loading ? B_GLYPH_STOP : B_GLYPH_RELOAD);
        ui.reload->SetToolTip(tab->loading ? "Stop" : "Reload");
        ui.top->SetProgress(tab->loading ? std::max(0.05f, tab->progress) : -1);
        ui.action->SetEnabled(!tab->url.empty());
    }
    const std::string count = fTabs.size() > 99 ? ":)" : std::to_string(fTabs.size());
    ui.tabs->SetGlyphText(count.c_str());
    ui.tabs->SetHighlighted(fPrivate);
    if (ui.panel == kTabsPanel) RefreshMobileTabs();
    if (!ui.compact) {
        ui.tabBar->RemoveTabs();
        for (const auto& tab : fTabs) {
            std::string title = tab.ShownTitle();
            if (title.empty()) title = tab.url.empty() || tab.url == NewTabAddress() ? "New Tab" : HostOf(tab.url);
            ui.tabBar->AddTab(tab.id, title.c_str(), tab.errorView ? nullptr : fFavicons->Icon(tab.url));
        }
        ui.tabBar->SetSelected(fSelected);
    }
    RefreshMobileEditing();
}

// While the address is being typed the field widens over Reload, and Cancel
// puts it back.
void BrowserWindow::RefreshMobileEditing()
{
    auto& ui = fMobileUI;
    if (!fMobile) return;
    const bool editing = fAddress->TextView()->IsFocus() && ui.panel == kPagePanel;
    ShowView(ui.cancel, editing);
    ShowView(ui.reload, !editing || !ui.compact);
    if (ui.compact) {
        ShowView(ui.bottom, !editing && ui.panel == kPagePanel);
    }
    if (editing != ui.editingAddress) {
        ui.editingAddress = editing;
        fAddress->Invalidate();
    }
}

void BrowserWindow::RefreshMobileTabs()
{
    auto& ui = fMobileUI;
    const bool showPrivate = fPrivate || ui.privateShown;
    ui.tabMode->Select(showPrivate ? 1 : 0);
    ui.grid->RemoveCards();
    ui.grid->SetEmptyText(nullptr);
    if (showPrivate && !fPrivate) {
        // No private window yet: what one is, and New Tab opens it.
        ui.grid->SetEmptyText("Private Browsing\nPages you open in private tabs leave no history, cookies or site data "
            "behind when you close them.\nBookmarks you make and files you download are kept.", B_GLYPH_PRIVATE);
        ui.closeAll->SetEnabled(false);
        return;
    }
    const BRect pages = ui.panels->Bounds();
    if (pages.IsValid() && pages.Width() > 0) ui.grid->SetAspect(pages.Height() / pages.Width());
    for (const auto& tab : fTabs) {
        std::string title = tab.ShownTitle();
        if (title.empty()) title = tab.url.empty() || tab.url == NewTabAddress() ? "New Tab" : HostOf(tab.url);
        ui.grid->AddCard(tab.id, title.c_str(), tab.picture.get(), tab.errorView ? nullptr : fFavicons->Icon(tab.url));
    }
    ui.grid->SetSelected(fSelected);
    ui.closeAll->SetEnabled(!fTabs.empty());
}

// The rows of the list on show: a bookmarks folder, the Reading List or
// History, newest first, under day headings.
void BrowserWindow::RefreshMobileList()
{
    auto& ui = fMobileUI;
    TableView* table = ui.table;
    const float top = table->Bounds().top;
    table->RemoveRows();
    ui.section->Select(ui.listSection);
    const bool editing = table->IsEditing();
    ui.edit->SetLabel(editing ? "Done" : ui.listSection == kHistorySection ? "Clear" : "Edit");
    ShowView(ui.newFolder, ui.listSection == kBookmarksSection && editing);
    auto row = [&](RowKind kind, const std::string& title, const std::string& detail, int64 id, const std::string& url) {
        auto* message = new BMessage(kMobileRow);
        message->AddInt32("kind", kind);
        message->AddInt64("id", id);
        message->AddString("url", url.c_str());
        auto* item = new TableRow(title.c_str(), detail.empty() ? nullptr : detail.c_str(), message);
        auto* remove = new BMessage(*message);
        remove->what = kMobileRowDelete;
        item->SetDeleteMessage(remove);
        table->AddItem(item);
        return item;
    };
    auto header = [&](const std::string& title) {
        auto* item = new TableRow(title.c_str());
        item->SetHeader(true);
        table->AddItem(item);
    };
    std::string title = "Bookmarks";
    std::string backLabel;
    switch (ui.listSection) {
        case kBookmarksSection: {
            if (fPrivate) table->SetEmptyText("Bookmarks are shown in normal windows.");
            else table->SetEmptyText("No bookmarks yet");
            if (fPrivate) break;
            struct Entry { RowKind kind; std::string title, url; int64 id; size_t count; };
            std::vector<Entry> entries;
            fShared->Read([&](const Profile& profile) {
                const auto& bookmarks = profile.bookmarks;
                // A folder that went away (deleted elsewhere) closes.
                while (!ui.folders.empty() && !bookmarks.Find(ui.folders.back())) ui.folders.pop_back();
                auto list = [&](const BookmarkNode& folder) {
                    for (const auto& child : folder.children) {
                        if (child.separator) continue;
                        entries.push_back({child.folder ? kFolderRow : kBookmarkRow, child.title, child.url, child.id,
                            child.folder ? CountBookmarks(child) : 0});
                    }
                };
                if (ui.folders.empty()) {
                    // The top: Favorites (the bookmarks bar), then the rest.
                    entries.push_back({kFolderRow, "Favorites", "", kBookmarksBarFolder, CountBookmarks(bookmarks.Bar())});
                    list(bookmarks.Other());
                } else if (const auto* folder = bookmarks.Find(ui.folders.back())) {
                    title = folder->id == kBookmarksBarFolder ? "Favorites" : folder->title;
                    const auto* parent = ui.folders.size() > 1 ? bookmarks.Find(ui.folders[ui.folders.size() - 2]) : nullptr;
                    backLabel = parent ? (parent->id == kBookmarksBarFolder ? "Favorites" : parent->title) : "Bookmarks";
                    list(*folder);
                }
            });
            for (const auto& entry : entries) {
                if (entry.kind == kFolderRow) {
                    auto* item = row(kFolderRow, entry.title.empty() ? "Untitled Folder" : entry.title,
                        entry.count == 1 ? "1 bookmark" : std::to_string(entry.count) + " bookmarks", entry.id, "");
                    item->SetGlyph(B_GLYPH_FOLDER, ui_color(B_LIST_ITEM_TEXT_COLOR).red > 128
                        ? make_color(120, 160, 220) : make_color(70, 120, 200));
                    item->SetAccessory(B_ROW_CHEVRON);
                    // The bookmarks bar's folder is always there.
                    if (entry.id == kBookmarksBarFolder) item->SetDeleteMessage(nullptr);
                } else {
                    auto* item = row(kBookmarkRow, entry.title.empty() ? entry.url : entry.title, HostOf(entry.url), entry.id, entry.url);
                    if (const BBitmap* icon = fFavicons->Icon(entry.url)) item->SetIcon(icon);
                    else item->SetGlyph(B_GLYPH_GLOBE);
                }
            }
            break;
        }
        case kReadingListSection: {
            title = "Reading List";
            table->SetEmptyText(fPrivate ? "The Reading List is kept in normal windows." : "Nothing to read later");
            if (fPrivate) break;
            const auto items = fShared->Read([](const Profile& profile) { return profile.readingList; });
            bool unread = false, read = false;
            for (const auto& item : items) (item.read ? read : unread) = true;
            for (int pass = 0; pass < 2; ++pass) {
                if ((pass == 0 && !unread) || (pass == 1 && !read)) continue;
                if (unread && read) header(pass == 0 ? "Unread" : "Read");
                for (const auto& item : items) {
                    if ((item.read != 0) != (pass == 1)) continue;
                    auto* entry = row(kReadingRow, item.title.empty() ? item.url : item.title, HostOf(item.url), 0, item.url);
                    entry->Message()->AddString("item", item.id.c_str());
                    entry->DeleteMessage()->AddString("item", item.id.c_str());
                    if (const BBitmap* icon = fFavicons->Icon(item.url)) entry->SetIcon(icon);
                    else entry->SetGlyph(B_GLYPH_GLASSES);
                }
            }
            break;
        }
        case kHistorySection: {
            title = "History";
            table->SetEmptyText(fPrivate ? "Private windows keep no history." : "No history");
            if (fPrivate) break;
            std::vector<PageRecord> pages = fShared->Read([](const Profile& profile) { return profile.history; });
            std::stable_sort(pages.begin(), pages.end(), [](const PageRecord& a, const PageRecord& b) { return a.visited > b.visited; });
            if (pages.size() > 400) pages.resize(400);
            std::string day;
            for (const auto& page : pages) {
                if (const std::string heading = DayHeading(page.visited); heading != day) {
                    header(heading);
                    day = heading;
                }
                auto* item = row(kHistoryRow, page.title.empty() ? page.url : page.title, HostOf(page.url), 0, page.url);
                if (const BBitmap* icon = fFavicons->Icon(page.url)) item->SetIcon(icon);
                else item->SetGlyph(B_GLYPH_CLOCK);
            }
            break;
        }
    }
    ui.listTitle->SetTitle(title.c_str());
    const bool nested = ui.listSection == kBookmarksSection && !ui.folders.empty();
    if (nested) ui.listBack->SetLabel(backLabel.c_str());
    ShowView(ui.listBack, nested);
    ui.edit->SetEnabled(!fPrivate && (table->CountItems() > 0 || editing));
    // Keep the place in a list that changed under the finger.
    table->ScrollTo(0, std::min(top, std::max(0.0f, table->CountItems() > 0
        ? table->ItemFrame(table->CountItems() - 1).bottom - table->Bounds().Height() : 0.0f)));
}

// The preferences a phone needs, as Safari's Settings: the search engine,
// the home page, privacy switches and clearing; the rest in All Settings.
void BrowserWindow::RefreshMobileSettings()
{
    auto& ui = fMobileUI;
    TableView* table = ui.settings;
    table->RemoveRows();
    struct Values {
        std::string engine, home, autoplay;
        bool restore, tracking, httpsFirst, passwords, spelling, reduceMotion;
    };
    const Values values = fShared->Read([](const Profile& profile) {
        return Values { profile.searchEngine, profile.homeURL, profile.autoplay, profile.restoreSession,
            profile.preventCrossSiteTracking, profile.httpsFirst, profile.savePasswords, profile.spellChecking,
            profile.reduceMotion };
    });
    auto header = [&](const char* title) {
        auto* item = new TableRow(title);
        item->SetHeader(true);
        table->AddItem(item);
    };
    auto row = [&](const char* key, const std::string& title, const std::string& detail = { }) {
        auto* message = new BMessage(kMobileSetting);
        message->AddString("key", key);
        auto* item = new TableRow(title.c_str(), detail.empty() ? nullptr : detail.c_str(), message);
        table->AddItem(item);
        return item;
    };
    auto toggle = [&](const char* key, const char* title, bool on) {
        auto* item = row(key, title);
        item->SetAccessory(B_ROW_SWITCH);
        item->SetSwitch(on);
        item->Message()->AddBool("value", !on);
    };
    auto choice = [&](const char* key, const char* id, const char* title, bool chosen) {
        auto* item = row(key, title);
        item->Message()->AddString("value", id);
        item->SetAccessory(chosen ? B_ROW_CHECK : B_ROW_NO_ACCESSORY);
    };
    static const std::pair<const char*, const char*> kAutoplay[] = {
        {"allow", "Allow All"}, {"block-audio", "Stop Media with Sound"}, {"block", "Never Autoplay"} };
    ShowView(ui.settingsBack, ui.settingsPage != 0);
    if (ui.settingsPage == 1) {
        ui.settingsTitle->SetTitle("Search Engine");
        for (const auto& engine : SearchEngines()) choice("search_engine", engine.id, engine.name, values.engine == engine.id);
        return;
    }
    if (ui.settingsPage == 2) {
        ui.settingsTitle->SetTitle("Autoplay");
        for (const auto& [id, name] : kAutoplay) choice("autoplay", id, name, values.autoplay == id);
        return;
    }
    ui.settingsTitle->SetTitle("Settings");
    std::string engineName = values.engine;
    for (const auto& engine : SearchEngines()) if (values.engine == engine.id) engineName = engine.name;
    header("General");
    row("search_engine_page", "Search Engine", engineName)->SetAccessory(B_ROW_CHEVRON);
    row("home_url", "Home Page", values.home.empty() ? "Summit start page" : values.home);
    if (auto* tab = ActiveTab(); tab && (tab->url.starts_with("https://") || tab->url.starts_with("http://")))
        row("home_current", "Use This Page as Home Page", HostOf(tab->url));
    toggle("restore_session", "Reopen Tabs at Start", values.restore);
    header("Privacy & Security");
    toggle("prevent_cross_site_tracking", "Prevent Cross-Site Tracking", values.tracking);
    toggle("https_first", "Try Secure Connections First", values.httpsFirst);
    toggle("save_passwords", "Offer to Save Passwords", values.passwords);
    std::string autoplayName = values.autoplay;
    for (const auto& [id, name] : kAutoplay) if (values.autoplay == id) autoplayName = name;
    row("autoplay_page", "Autoplay", autoplayName)->SetAccessory(B_ROW_CHEVRON);
    header("Reading");
    toggle("spell_checking", "Check Spelling", values.spelling);
    toggle("reduce_motion", "Reduce Motion", values.reduceMotion);
    header("Clear");
    if (!fPrivate) row("clear_history", "Clear History")->SetDestructive(true);
    row("clear_data", "Clear Cookies and Data")->SetDestructive(true);
    header("More");
    row("all_settings", "All Settings…", "Extensions, passwords, site permissions, developer tools")
        ->SetAccessory(B_ROW_CHEVRON);
}

void BrowserWindow::ShowMobileActions()
{
    auto* tab = ActiveTab();
    if (!tab) return;
    if (fMobileSheet.IsValid()) fMobileSheet.SendMessage(B_QUIT_REQUESTED);
    const std::string title = tab->ShownTitle().empty() ? HostOf(tab->url) : tab->ShownTitle();
    auto* sheet = new ActionSheet(this, title.c_str());
    const bool web = tab->url.starts_with("https://") || tab->url.starts_with("http://");
    const bool bookmarked = fShared->Read([&](const Profile& profile) { return profile.bookmarks.Contains(tab->url); });
    if (IsBookmarkableURL(tab->url) && !fPrivate)
        sheet->AddActivity(bookmarked ? "Edit Bookmark" : "Bookmark", B_GLYPH_STAR, BMessage(kMobileBookmarkPage));
    if (web && !fPrivate) sheet->AddActivity("Reading List", B_GLYPH_GLASSES, BMessage(kAddToReadingList));
    if (web) sheet->AddActivity("Copy Link", B_GLYPH_PAGES, BMessage(kMobileCopyLink));
    sheet->AddActivity("Find on Page", B_GLYPH_SEARCH, BMessage(kFind));
#if SUMMIT_MODERN_WEBKIT
    if (tab->readerAvailable || tab->readerActive)
        sheet->AddActivity(tab->readerActive ? "Leave Reader" : "Reader", B_GLYPH_BOOK, BMessage(kToggleReader));
#endif
    if (web) sheet->AddActivity(tab->desktopSite ? "Mobile Site" : "Desktop Site", B_GLYPH_GLOBE, BMessage(kMobileDesktopSite));
    BMessage history(kMobileLists);
    history.AddInt32("section", kHistorySection);
    sheet->AddActivity("History", B_GLYPH_CLOCK, history);
    sheet->AddActivity("Downloads", B_GLYPH_DOWNLOAD, BMessage(kToggleDownloadsPanel));
    sheet->AddAction("New Tab", BMessage(kNewTab));
    sheet->AddAction("New Private Tab", BMessage(kMobileNewPrivateTab));
    sheet->AddAction("Settings", BMessage(kMobileSettings));
    sheet->AddAction("Close Tab", BMessage(kCloseTab), B_ACTION_DESTRUCTIVE);
    fMobileSheet = BMessenger(sheet);
    sheet->Open();
}

void BrowserWindow::OpenMobileEditor(int32 mode, int64 id, int32 returnTo)
{
    auto& ui = fMobileUI;
    std::string name, url;
    int64 folder = kOtherBookmarksFolder;
    std::vector<BookmarkFolderChoice> folders;
    const bool found = fShared->Read([&](const Profile& profile) {
        const auto& bookmarks = profile.bookmarks;
        if (mode == BookmarkEditor::kEditBookmarkMode || mode == BookmarkEditor::kEditFolderMode) {
            const auto* node = bookmarks.Find(id);
            const auto* parent = bookmarks.Parent(id);
            if (!node || !parent) return false;
            name = node->title;
            url = node->url;
            folder = parent->id;
        } else if (mode == BookmarkEditor::kNewFolderMode) {
            name = "New Folder";
            folder = id ? id : kOtherBookmarksFolder;
        } else if (auto* tab = ActiveTab()) {
            // The page's bookmark when it has one: Add becomes Edit.
            if (const auto* existing = bookmarks.FindURL(tab->url)) {
                mode = BookmarkEditor::kEditBookmarkMode;
                id = existing->id;
                name = existing->title;
                url = existing->url;
                if (const auto* parent = bookmarks.Parent(id)) folder = parent->id;
            } else {
                name = tab->title;
                url = tab->url;
            }
        }
        folders = bookmarks.Folders(mode == BookmarkEditor::kEditFolderMode ? id : 0);
        return true;
    });
    if (!found) return;
    ui.editMode = mode;
    ui.editID = id;
    ui.editFolder = folder;
    ui.editReturn = returnTo;
    const bool isFolder = mode == BookmarkEditor::kEditFolderMode || mode == BookmarkEditor::kNewFolderMode;
    ui.editTitle->SetTitle(mode == BookmarkEditor::kNewBookmarkMode ? "Add Bookmark" : mode == BookmarkEditor::kNewFolderMode
        ? "New Folder" : isFolder ? "Edit Folder" : "Edit Bookmark");
    ui.editName->SetText(name.c_str());
    ui.editURL->SetText(url.c_str());
    ShowView(ui.editURL, !isFolder);
    ShowView(ui.editURLLabel, !isFolder);
    ui.editFolders->RemoveRows();
    auto* heading = new TableRow("Folder");
    heading->SetHeader(true);
    ui.editFolders->AddItem(heading);
    for (const auto& choice : folders) {
        std::string label(choice.depth * 3, ' ');
        label += choice.id == kBookmarksBarFolder ? "Favorites" : choice.title;
        auto* message = new BMessage(kMobileEditorFolder);
        message->AddInt64("id", choice.id);
        auto* item = new TableRow(label.c_str(), nullptr, message);
        item->SetGlyph(B_GLYPH_FOLDER);
        item->SetAccessory(choice.id == folder ? B_ROW_CHECK : B_ROW_NO_ACCESSORY);
        ui.editFolders->AddItem(item);
    }
    ShowMobilePanel(kEditorPanel);
    ui.editName->TextView()->SelectAll();
}

void BrowserWindow::SaveMobileEditor()
{
    auto& ui = fMobileUI;
    BMessage edited(kBookmarkEdited);
    edited.AddInt32("mode", ui.editMode);
    edited.AddInt64("id", ui.editID);
    edited.AddInt64("folder", ui.editFolder);
    edited.AddString("title", ui.editName->Text());
    edited.AddString("url", ui.editURL->Text());
    BookmarkEdited(edited);
    MobileToast(fStatus->Text());
    ShowMobilePanel(ui.editReturn);
}

bool BrowserWindow::MobileMessage(BMessage& message)
{
    auto& ui = fMobileUI;
    switch (message.what) {
        case B_MOBILE_VIEW_CHANGED:
            SetMobileView(message.GetBool("mobile", false));
            return true;
        case kMobileTabs:
            ui.privateShown = false;
            ShowMobilePanel(ui.panel == kTabsPanel ? kPagePanel : kTabsPanel);
            return true;
        case kMobileShowTabs:
            ui.privateShown = false;
            if (fMobile) ShowMobilePanel(kTabsPanel);
            return true;
        case kMobileMore:
            ShowMobileActions();
            return true;
        case kMobileLists:
            ui.listSection = message.GetInt32("section", ui.listSection);
            ui.table->SetEditing(false);
            ShowMobilePanel(kListPanel);
            return true;
        case kMobileDone:
            ui.table->SetEditing(false);
            ShowMobilePanel(kPagePanel);
            return true;
        case kMobileOpenTab:
            ShowMobilePanel(kPagePanel);
            SelectTab(message.GetInt64("id", fSelected));
            return true;
        case kMobileCloseTab:
            CloseTab(message.GetInt64("id", 0));
            return true;
        case kMobileAddTab:
            if (ui.privateShown && !fPrivate) {
                // The first private tab opens a private window.
                ui.privateShown = false;
                ShowMobilePanel(kPagePanel);
                RequestNewWindow({ }, true);
                return true;
            }
            ShowMobilePanel(kPagePanel);
            PostMessage(kNewTab);
            return true;
        case kMobileNewPrivateTab:
            if (fPrivate) PostMessage(kNewTab);
            else if (BMessenger other = OtherWindow(true); other.IsValid()) {
                other.SendMessage(kNewTab);
                other.SendMessage(kActivateWindow);
            } else RequestNewWindow({ }, true);
            return true;
        case kMobileCloseAll: {
            const std::string question = fTabs.size() == 1 ? "Close the tab?" : "Close all " + std::to_string(fTabs.size()) + " tabs?";
            auto* alert = new BAlert("Close All", question.c_str(), "Cancel", "Close All", nullptr,
                B_WIDTH_AS_USUAL, B_WARNING_ALERT);
            alert->SetShortcut(0, B_ESCAPE);
            alert->Go(new BInvoker(new BMessage(kMobileCloseAllReply), this));
            return true;
        }
        case kMobileCloseAllReply:
            if (message.GetInt32("which", 0) != 1) return true;
            if (fPrivate) {
                // Closing every private tab ends private browsing.
                if (BMessenger other = OtherWindow(false); other.IsValid()) {
                    other.SendMessage(kMobileShowTabs);
                    other.SendMessage(kActivateWindow);
                }
                PostMessage(B_QUIT_REQUESTED);
                return true;
            }
            {
                // A new tab first, so the window stays open.
                std::vector<int64> old;
                for (const auto& tab : fTabs) old.push_back(tab.id);
                CreateTab(HomeAddress());
                for (int64 id : old) CloseTab(id);
                ShowMobilePanel(kPagePanel);
            }
            return true;
        case kMobileTabMode: {
            const bool wantPrivate = message.GetInt32("index", 0) == 1;
            if (wantPrivate == fPrivate) {
                ui.privateShown = false;
                RefreshMobileTabs();
                return true;
            }
            // The other kind of tabs live in another window: show its switcher.
            if (BMessenger other = OtherWindow(wantPrivate); other.IsValid()) {
                ui.privateShown = false;
                other.SendMessage(kMobileShowTabs);
                other.SendMessage(kActivateWindow);
                ShowMobilePanel(kPagePanel);
                return true;
            }
            if (fPrivate) {
                // No normal window is left: open one.
                RequestNewWindow({ }, false);
                return true;
            }
            ui.privateShown = wantPrivate;
            RefreshMobileTabs();
            return true;
        }
        case kMobileCancelAddress:
            HideSuggestions();
            if (auto* tab = ActiveTab()) {
                fAddress->SetText(DisplayURL(tab->ShownURL()).c_str());
                tab->view->MakeFocus();
            }
            RefreshMobileEditing();
            return true;
        case kMobileReloadStop:
            PostMessage(kReload);
            return true;
        case kMobileSection:
            ui.listSection = message.GetInt32("index", kBookmarksSection);
            ui.table->SetEditing(false);
            RefreshMobileList();
            return true;
        case kMobileListBack:
            if (!ui.folders.empty()) ui.folders.pop_back();
            RefreshMobileList();
            return true;
        case kMobileEdit:
            if (ui.listSection == kHistorySection && !ui.table->IsEditing()) {
                auto* alert = new BAlert("Clear History", "Remove every page from your history?\n\nBookmarks and open tabs are kept.",
                    "Cancel", "Clear History", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
                alert->SetShortcut(0, B_ESCAPE);
                alert->Go(new BInvoker(new BMessage(kMobileClearHistory), this));
                return true;
            }
            ui.table->SetEditing(!ui.table->IsEditing());
            RefreshMobileList();
            return true;
        case kMobileClearHistory:
            if (message.GetInt32("which", 0) == 1) {
                // As History › Clear History does (form history goes too).
                BMessage reply(kClearHistoryReply);
                reply.AddInt32("which", 1);
                PostMessage(&reply);
                MobileToast("History cleared");
            }
            return true;
        case kMobileNewFolder:
            OpenMobileEditor(BookmarkEditor::kNewFolderMode, ui.folders.empty() ? kOtherBookmarksFolder : ui.folders.back(), kListPanel);
            return true;
        case kMobileRow: {
            const int32 kind = message.GetInt32("kind", kBookmarkRow);
            const int64 id = message.GetInt64("id", 0);
            const std::string url = message.GetString("url", "");
            if (kind == kFolderRow && !ui.table->IsEditing()) {
                ui.folders.push_back(id);
                RefreshMobileList();
                ui.table->ScrollTo(0, 0);
            } else if (ui.table->IsEditing() && (kind == kFolderRow || kind == kBookmarkRow)) {
                OpenMobileEditor(kind == kFolderRow ? BookmarkEditor::kEditFolderMode : BookmarkEditor::kEditBookmarkMode, id, kListPanel);
            } else if (kind == kReadingRow) {
                ShowMobilePanel(kPagePanel);
                BMessage open(kOpenReadingListItem);
                open.AddString("id", message.GetString("item", ""));
                ReadingListCommand(open);
            } else if (!url.empty()) {
                ShowMobilePanel(kPagePanel);
                if (auto* tab = ActiveTab()) tab->nextTransition = kind == kBookmarkRow ? 3 : 0;
                Navigate(url);
            }
            return true;
        }
        case kMobileRowDelete: {
            const int32 kind = message.GetInt32("kind", kBookmarkRow);
            if (kind == kFolderRow || kind == kBookmarkRow) DeleteBookmark(message.GetInt64("id", 0), true);
            else if (kind == kReadingRow) {
                BMessage remove(kRemoveReadingListItem);
                remove.AddString("id", message.GetString("item", ""));
                ReadingListCommand(remove);
            } else if (kind == kHistoryRow) {
                const std::string url = message.GetString("url", "");
                bool erased = false;
                fShared->Change([&](Profile& profile) -> uint32 {
                    erased = std::erase_if(profile.history, [&](const PageRecord& page) { return page.url == url; }) > 0;
                    return erased ? SharedProfile::kHistoryChanged : 0;
                }, BMessenger(this), false);
#if SUMMIT_MODERN_WEBKIT
                if (erased)
                    fWebKitContext->NotifyHistoryChanged("removed", nlohmann::json({{"allHistory", false}, {"urls", {url}}}).dump().c_str());
#endif
            }
            RefreshMobileList();
            return true;
        }
        case kMobileSettings:
            ui.settingsPage = 0;
            ShowMobilePanel(kSettingsPanel);
            return true;
        case kMobileSettingsBack:
            ui.settingsPage = 0;
            RefreshMobileSettings();
            return true;
        case kMobileSetting: {
            const std::string key = message.GetString("key", "");
            BMessage changed(kPreferencesChanged);
            if (key == "search_engine_page") ui.settingsPage = 1;
            else if (key == "autoplay_page") ui.settingsPage = 2;
            else if (key == "search_engine" || key == "autoplay") {
                changed.AddString(key.c_str(), message.GetString("value", ""));
                ui.settingsPage = 0;
            } else if (key == "home_current") {
                if (auto* tab = ActiveTab()) changed.AddString("home_url", tab->url.c_str());
                MobileToast("Home page set");
            } else if (key == "home_url") {
                // Typing an address on a phone belongs in All Settings.
                be_app->PostMessage(kShowPreferences);
            } else if (key == "clear_history") {
                auto* alert = new BAlert("Clear History", "Remove every page from your history, and what you typed in forms?\n\n"
                    "Bookmarks and open tabs are kept.", "Cancel", "Clear History", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
                alert->SetShortcut(0, B_ESCAPE);
                alert->Go(new BInvoker(new BMessage(kMobileClearHistory), this));
            } else if (key == "clear_data") {
                auto* alert = new BAlert("Clear Cookies and Data", "Remove cookies, caches and the data websites keep?\n\n"
                    "Sites you are signed in to will sign you out.", "Cancel", "Clear", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
                alert->SetShortcut(0, B_ESCAPE);
                alert->Go(new BInvoker(new BMessage(kMobileClearDataReply), this));
            } else if (key == "all_settings") be_app->PostMessage(kShowPreferences);
            else if (message.HasBool("value")) changed.AddBool(key.c_str(), message.GetBool("value", false));
            if (!changed.IsEmpty()) be_app->PostMessage(&changed);
            // The profile changes on the application's thread: show the
            // switches as they will be at once.
            if (message.HasBool("value")) {
                const int32 index = message.GetInt32("index", -1);
                if (auto* item = ui.settings->RowAt(index)) {
                    item->SetSwitch(message.GetBool("value", false));
                    item->Message()->ReplaceBool("value", !message.GetBool("value", false));
                    ui.settings->InvalidateItem(index);
                }
            } else RefreshMobileSettings();
            return true;
        }
        case kMobileClearDataReply:
            if (message.GetInt32("which", 0) == 1) {
                be_app->PostMessage(kClearCacheRequest);
                be_app->PostMessage(kClearSiteDataRequest);
                MobileToast("Cookies and data cleared");
            }
            return true;
        case kMobileBookmarkPage:
            OpenMobileEditor(BookmarkEditor::kNewBookmarkMode, 0, kPagePanel);
            return true;
        case kMobileEditorFolder: {
            ui.editFolder = message.GetInt64("id", ui.editFolder);
            for (int32 i = 0; i < ui.editFolders->CountItems(); ++i) {
                auto* row = ui.editFolders->RowAt(i);
                if (!row || row->IsHeader() || !row->Message()) continue;
                row->SetAccessory(row->Message()->GetInt64("id", 0) == ui.editFolder ? B_ROW_CHECK : B_ROW_NO_ACCESSORY);
            }
            ui.editFolders->Invalidate();
            return true;
        }
        case kMobileEditorSave:
            SaveMobileEditor();
            return true;
        case kMobileEditorCancel:
            ShowMobilePanel(ui.editReturn);
            return true;
        case kMobileDesktopSite:
            if (auto* tab = ActiveTab()) {
                tab->desktopSite = !tab->desktopSite;
#if SUMMIT_MODERN_WEBKIT
                if (!ResponsiveUserAgent(tab->responsive)) tab->view->SetCustomUserAgent(NormalUserAgent(*tab));
#endif
                tab->view->Reload();
            }
            return true;
        case kMobileCopyLink:
            if (auto* tab = ActiveTab(); tab && be_clipboard->Lock()) {
                be_clipboard->Clear();
                if (BMessage* data = be_clipboard->Data()) {
                    data->AddData("text/plain", B_MIME_TYPE, tab->url.data(), tab->url.size());
                    be_clipboard->Commit();
                }
                be_clipboard->Unlock();
                MobileToast("Link copied");
            }
            return true;
        case kAddToReadingList:
            if (!fMobile) return false;
            ReadingListCommand(message);
            MobileToast(fStatus->Text());
            return true;
        case kBookmarkEdited: case kDeleteBookmarkReply:
            // The lists follow the profile; the reply's own handling goes on.
            return false;
    }
    return false;
}
}

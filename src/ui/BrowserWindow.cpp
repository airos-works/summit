#include "BrowserWindow.h"
#if SUMMIT_MODERN_WEBKIT
#include "ExtensionShortcuts.h"
#include "core/Protocol.h"
#endif
#include "ProtocolHandlers.h"
#include "ExtensionTiming.h"
#include "StallTrace.h"
#include "NavigationTrace.h"
#include "Chrome.h"
#include "FaviconCache.h"
#include "AddressSuggestions.h"
#include "DirectWindowRescue.h"
#include "Messages.h"
#include "SharedProfile.h"
#include "BookmarkEditor.h"
#include "BookmarkImport.h"
#if SUMMIT_MODERN_WEBKIT
#include "DevToolsWindow.h"
#include "PageSourceWindow.h"
#include "DownloadList.h"
#include "DownloadsPanel.h"
#include "ExtensionStoreDownload.h"
#include "FrameHistory.h"
#include "McpService.h"
#include "UpdateChecker.h"
#include "core/StoreOffer.h"
#include <WebKit/WebKitInspector.h>
#endif
#include "core/Address.h"
#include "core/UserAgent.h"
#include "core/BuildTarget.h"
#include "core/BookmarkIO.h"
#include "core/InternalPages.h"
#include "core/Suggest.h"
#include "core/Zoom.h"
#include "core/PageLanguage.h"
#include <Alert.h>
#include <Bitmap.h>
#include <Clipboard.h>
#include <ControlLook.h>
#include <Screen.h>
#include <Application.h>
#include <atomic>
#include <CardLayout.h>
#include <Entry.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <GroupView.h>
#include <Invoker.h>
#include <LayoutBuilder.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <MessageFilter.h>
#include <NodeInfo.h>
#include <OS.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <PropertyInfo.h>
#include <Roster.h>
#include <SplitView.h>
#include <StatusBar.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <WindowScreen.h>
#if SUMMIT_MODERN_WEBKIT
#include "ExtensionInstaller.h"
#include "AuthenticationPrompt.h"
#include "PasskeyPrompt.h"
#include "FeatureMessages.h"
#include "NoticeBar.h"
#include "PrintJob.h"
#include "WebApps.h"
#include "core/Bookmarks.h"
#include "core/BookmarkQueries.h"
#include "CertificateInfoWindow.h"
#include "ReaderMode.h"
#include "PageTranslation.h"
#include <WebKit/WebKitInfo.h>
#include <WebKit/WebKitEmbedding.h>
#include <nlohmann/json.hpp>
#else
#include <WebDownload.h>
#include <WebKitInfo.h>
#include <WebPage.h>
#include <WebView.h>
#endif
#include <app/AppMisc.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <ctime>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#if SUMMIT_MODERN_WEBKIT
// air/OS extends the native zoom policy to individual displays. Keep the
// whole-screen fallback for Haiku versions that do not export this API.
namespace BPrivate {
status_t get_display_frame(BRect, bool forZoom, BRect&) __attribute__((weak));
}
#endif

namespace summit {
#if SUMMIT_MODERN_WEBKIT
// A Firefox Add-ons item's add-on ID came from Mozilla ("slug"; "identifier"
// and "name" unless that failed).
static constexpr uint32 kStoreItemResolved = 'stir';
// The identifier of window.__summitPipLargest()'s answer (reader requests
// count up from 1).
static constexpr uint64 kPictureInPictureQuery = uint64(1) << 61;
// The page script of Install as App… (#75).
static constexpr uint64 kWebAppQuery = (uint64(1) << 61) + 1;
static constexpr uint64 kBookmarkletQuery = (uint64(1) << 61) + 2;
// Every 20 seconds while the current tab asks for the screen to stay on.
static constexpr uint32 kKeepScreenAwake = 'kawk';
static constexpr bigtime_t kKeepScreenAwakeInterval = 20000000;
// The engine sends one 'wvfr' message when a coordinated frame reaches its
// native view. Count those deliveries only in explicit performance runs.
class FrameStatsWebKitView final : public BWebKitView {
public:
    using BWebKitView::BWebKitView;

    void ResetFrameStats()
    {
        fCaptureStart = system_time();
        fCaptureLastFrame = 0;
        fCaptureLongestGap = 0;
        fCaptureFirstFrameDelay = 0;
        fCaptureLongestInterframeGap = 0;
        fCaptureFrames = 0;
        fCaptureLongGaps = 0;
        fCaptureLongestQueueDelay = 0;
        fCaptureLongQueueDelays = 0;
        fWindowStart = 0;
        fLastFrame = 0;
        fLongestGap = 0;
        fLongGaps = 0;
        fFrames = 0;
        fLongestQueueDelay = 0;
        fLongQueueDelays = 0;
    }

    bool AppendFrameStats(BMessage& reply) const
    {
        if (!fCaptureStart)
            return false;
        const bigtime_t now = system_time();
        const bigtime_t elapsed = std::max<bigtime_t>(0, now - fCaptureStart);
        const bigtime_t pendingGap = std::max<bigtime_t>(0, now - (fCaptureLastFrame ? fCaptureLastFrame : fCaptureStart));
        reply.AddInt64("elapsed_us", elapsed);
        reply.AddInt32("frames", fCaptureFrames);
        reply.AddInt64("longest_gap_us", fCaptureLongestGap);
        reply.AddInt64("first_frame_delay_us", fCaptureFirstFrameDelay);
        reply.AddInt64("longest_interframe_gap_us", fCaptureLongestInterframeGap);
        reply.AddInt32("long_gaps", fCaptureLongGaps);
        reply.AddInt64("pending_gap_us", pendingGap);
        reply.AddInt64("queue_max_us", fCaptureLongestQueueDelay);
        reply.AddInt32("queue_over_33", fCaptureLongQueueDelays);
        return true;
    }

    // When the view was last shown (its tab selected) and when its first frame
    // after that arrived: a tab switch's latency, for benchmarks.
    void Show() override
    {
        BWebKitView::Show();
        fShownAt = system_time();
        fFirstFrameAfterShow = 0;
    }
    bigtime_t ShownAt() const { return fShownAt; }
    bigtime_t FirstFrameAfterShow() const { return fFirstFrameAfterShow; }

    void MessageReceived(BMessage* message) override
    {
        if (message->what == 'wvfr') {
            const bigtime_t now = system_time();
            if (fShownAt && !fFirstFrameAfterShow)
                fFirstFrameAfterShow = now;
            if (fCaptureStart) {
                const bigtime_t gap = now - (fCaptureLastFrame ? fCaptureLastFrame : fCaptureStart);
                fCaptureLongestGap = std::max(fCaptureLongestGap, gap);
                if (fCaptureLastFrame)
                    fCaptureLongestInterframeGap = std::max(fCaptureLongestInterframeGap, gap);
                else
                    fCaptureFirstFrameDelay = gap;
                if (gap > 33000)
                    ++fCaptureLongGaps;
                // SUMMIT_UI_FRAME_STATS=2 says when each late frame of a
                // burst arrived, on the clock of the compositor's trace.
                static const bool traceGaps = [] {
                    const char* value = std::getenv("SUMMIT_UI_FRAME_STATS");
                    return value && !std::strcmp(value, "2");
                }();
                if (traceGaps && gap > 25000 && fCaptureLastFrame)
                    std::fprintf(stderr, "Summit UI frame gap: at=%lld gap=%.1f ms frame=%u\n", static_cast<long long>(now), gap / 1000.0, fCaptureFrames);
                fCaptureLastFrame = now;
                ++fCaptureFrames;
            }
            if (!fWindowStart)
                fWindowStart = now;
            if (fLastFrame) {
                const bigtime_t gap = now - fLastFrame;
                fLongestGap = std::max(fLongestGap, gap);
                if (gap > 33000)
                    ++fLongGaps;
            }
            fLastFrame = now;
            ++fFrames;
            int64 queuedMicros = 0;
            if (message->FindInt64("summit:queuedMicros", &queuedMicros) == B_OK && queuedMicros > 0 && queuedMicros <= now) {
                const bigtime_t delay = now - queuedMicros;
                fLongestQueueDelay = std::max(fLongestQueueDelay, delay);
                if (delay > 33000)
                    ++fLongQueueDelays;
                if (fCaptureStart) {
                    fCaptureLongestQueueDelay = std::max(fCaptureLongestQueueDelay, delay);
                    if (delay > 33000)
                        ++fCaptureLongQueueDelays;
                }
            }
            const bigtime_t elapsed = now - fWindowStart;
            if (elapsed >= 1000000) {
                std::fprintf(stderr, "Summit UI frames: %.1f/s frames=%u longest=%.1f ms over33=%u queueMax=%.1f ms queueOver33=%u\n",
                    1000000.0 * fFrames / elapsed, fFrames, fLongestGap / 1000.0, fLongGaps,
                    fLongestQueueDelay / 1000.0, fLongQueueDelays);
                fWindowStart = now;
                fFrames = 0;
                fLongestGap = 0;
                fLongGaps = 0;
                fLongestQueueDelay = 0;
                fLongQueueDelays = 0;
            }
        }
        BWebKitView::MessageReceived(message);
    }

private:
    bigtime_t fShownAt { 0 };
    bigtime_t fFirstFrameAfterShow { 0 };
    bigtime_t fCaptureStart { 0 };
    bigtime_t fCaptureLastFrame { 0 };
    bigtime_t fCaptureLongestGap { 0 };
    bigtime_t fCaptureFirstFrameDelay { 0 };
    bigtime_t fCaptureLongestInterframeGap { 0 };
    unsigned fCaptureFrames { 0 };
    unsigned fCaptureLongGaps { 0 };
    bigtime_t fCaptureLongestQueueDelay { 0 };
    unsigned fCaptureLongQueueDelays { 0 };
    bigtime_t fWindowStart { 0 };
    bigtime_t fLastFrame { 0 };
    bigtime_t fLongestGap { 0 };
    unsigned fFrames { 0 };
    unsigned fLongGaps { 0 };
    bigtime_t fLongestQueueDelay { 0 };
    unsigned fLongQueueDelays { 0 };
};

class ExtensionActionMenuItem final : public BMenuItem {
public:
    ExtensionActionMenuItem(const char* label, BMessage* message) : BMenuItem(label, message) { }
    bool Selected() const { return IsSelected(); }
    status_t Activate() { return Invoke(); }
};
class ExtensionActionsMenu final : public BPopUpMenu {
public:
    explicit ExtensionActionsMenu(std::shared_ptr<std::atomic<bool>> cancelled)
        : BPopUpMenu("Extension actions", false, false), fCancelled(std::move(cancelled))
    {
        SetAsyncAutoDestruct(true);
        SetTrackingHook([](BMenu*, void* state) {
            return static_cast<std::atomic<bool>*>(state)->load();
        }, fCancelled.get());
    }
    void AttachedToWindow() override
    {
        BPopUpMenu::AttachedToWindow();
        BMessage tick('exmt');
        fTimer = std::make_unique<BMessageRunner>(BMessenger(this), &tick, 50000);
        if (fTimer->InitCheck() != B_OK) *fCancelled = true;
    }
    void DetachedFromWindow() override
    {
        fTimer.reset();
        BPopUpMenu::DetachedFromWindow();
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == 'exmt') {
            // Haiku's tracking hook is outside its mouse-idle wait loop.
            // Deliver Escape on the menu looper to wake that loop as well.
            if (fCancelled->load()) { const char escape = B_ESCAPE; KeyDown(&escape, 1); }
            return;
        }
        BPopUpMenu::MessageReceived(message);
    }
    void KeyDown(const char* bytes, int32 count) override
    {
        if (count && (bytes[0] == B_ENTER || bytes[0] == B_SPACE)) {
            for (int32 i = 0; i < CountItems(); ++i) {
                auto* item = dynamic_cast<ExtensionActionMenuItem*>(ItemAt(i));
                if (!item || !item->Selected() || !item->IsEnabled()) continue;
                // BPopUpMenu::Go can repeat tracking during its opening-click
                // interval and discard a quick keyboard choice. Deliver once
                // here, then cancel tracking without leaving a chosen item.
                item->Activate();
                *fCancelled = true;
                const char escape = B_ESCAPE;
                BPopUpMenu::KeyDown(&escape, 1);
                return;
            }
        }
        BPopUpMenu::KeyDown(bytes, count);
        if (!Window()) {
            // Escape and native mnemonic activation can also finish during
            // the opening-click interval. Do not restart a dismissed menu.
            *fCancelled = true;
            return;
        }
        if (count && bytes[0] == B_DOWN_ARROW) {
            // Haiku's first Down traversal skips the final item when no item
            // was selected. If all actions are disabled, reach Manage as well.
            for (int32 i = 0; i < CountItems(); ++i)
                if (auto* item = dynamic_cast<ExtensionActionMenuItem*>(ItemAt(i)); item && item->Selected()) return;
            const char up = B_UP_ARROW;
            BPopUpMenu::KeyDown(&up, 1);
        }
    }
private:
    // The native tracking thread can outlive its browser window's derived
    // members. Keep the cancellation flag alive until the menu is destroyed.
    std::shared_ptr<std::atomic<bool>> fCancelled;
    std::unique_ptr<BMessageRunner> fTimer;
};
#endif
class AddressEnterFilter final : public BMessageFilter {
public:
    explicit AddressEnterFilter(BTextControl& control)
        : BMessageFilter(B_KEY_DOWN), fControl(control) { }

    // Keys the suggestion list under the field takes first (the arrow keys,
    // Escape, Tab); it returns true for the keys it used.
    std::function<bool(char)> fKeyHandler;

    filter_result Filter(BMessage* message, BHandler**) override
    {
        const char* bytes = nullptr;
        if (message->FindString("bytes", &bytes) != B_OK || !bytes || bytes[1] != '\0') return B_DISPATCH_MESSAGE;
        if (bytes[0] != B_ENTER) {
            const bool listKey = bytes[0] == B_UP_ARROW || bytes[0] == B_DOWN_ARROW || bytes[0] == B_ESCAPE
                || bytes[0] == B_TAB;
            const int32 modifiers = message->GetInt32("modifiers", 0);
            if (listKey && fKeyHandler && fControl.TextView()->IsFocus() && (modifiers & (B_COMMAND_KEY | B_CONTROL_KEY)) == 0
                && fKeyHandler(bytes[0]))
                return B_SKIP_MESSAGE;
            return B_DISPATCH_MESSAGE;
        }
        if (fControl.IsEnabled() && fControl.TextView()->IsFocus()) {
            NavigationTraceBegin(message->GetInt64("when", 0), "Enter in the address field");
            // BTextControl normally suppresses Enter when the text matches its
            // saved value. An address must remain submit-able for retry/reload.
            fControl.Invoke();
            fControl.TextView()->SelectAll();
        }
        return B_SKIP_MESSAGE;
    }

private:
    BTextControl& fControl;
};

class AddressControl final : public BTextControl {
public:
    AddressControl() : BTextControl("address", nullptr, "", new BMessage(kNavigate))
    {
        fFilter = new AddressEnterFilter(*this);
        TextView()->AddFilter(fFilter);
        SetModificationMessage(new BMessage(kAddressModified));
#if SUMMIT_MODERN_WEBKIT
        fCertificate = new ToolButton("connection-certificate", "View connection certificate", Icon::Lock, kShowCertificate);
        AddChild(fCertificate);
        fCertificate->Hide();
        fReader = new ToolButton("reader", "Enter Reader mode", Icon::Reader, kToggleReader);
        AddChild(fReader);
        fReader->Hide();
#endif
    }

    void SetKeyHandler(std::function<bool(char)> handler) { fFilter->fKeyHandler = std::move(handler); }

#if SUMMIT_MODERN_WEBKIT
    ToolButton* CertificateButton() const { return fCertificate; }
    void SetCertificate(bool visible, bool verified)
    {
        fCertificate->SetIcon(verified ? Icon::Lock : Icon::LockWarning);
        fCertificate->SetToolTip(verified ? "Encrypted connection — view certificate" : "Connection warning — view certificate");
        SetButtonVisible(fCertificate, visible);
    }
    void SetReader(bool visible, bool active, bool enabled)
    {
        fReader->SetIcon(active ? Icon::ReaderActive : Icon::Reader);
        fReader->SetToolTip(active ? "Exit Reader mode" : "Enter Reader mode");
        fReader->SetEnabled(enabled);
        SetButtonVisible(fReader, visible);
    }
    void SetButtonVisible(ToolButton* button, bool visible)
    {
        if (visible == !button->IsHidden(button)) return;
        if (visible) button->Show(); else button->Hide();
        InvalidateLayout();
        Layout(true);
        Invalidate();
    }
    void Draw(BRect update) override
    {
        if (!HasButton(fCertificate) && !HasButton(fReader)) { BTextControl::Draw(update); return; }
        // BTextControl normally outlines only its text view. Keep the outline
        // around the text view and both embedded buttons.
        BRect frame = fFullTextFrame.InsetByCopy(-2, -2);
        uint32 flags = IsEnabled() ? 0 : BControlLook::B_DISABLED;
        if (TextView()->IsFocus() && Window()->IsActive()) flags |= BControlLook::B_FOCUSED;
        SetHighColor(TextView()->ViewColor());
        FillRect(frame.InsetByCopy(2, 2));
        be_control_look->DrawTextControlBorder(this, frame, update, ViewColor(), flags);
    }
    void DoLayout() override
    {
        BTextControl::DoLayout();
        fFullTextFrame = TextView()->Frame();
        constexpr float width = 28;
        BRect text = fFullTextFrame;
        if (HasButton(fCertificate)) {
            fCertificate->MoveTo(fFullTextFrame.LeftTop());
            fCertificate->ResizeTo(width, fFullTextFrame.Height());
            text.left += width + 4;
        }
        if (HasButton(fReader)) {
            fReader->MoveTo(fFullTextFrame.right - width, fFullTextFrame.top);
            fReader->ResizeTo(width, fFullTextFrame.Height());
            text.right -= width + 4;
        }
        TextView()->MoveTo(text.LeftTop());
        TextView()->ResizeTo(std::max(1.0f, text.Width()), text.Height());
        TextView()->SetTextRect(TextView()->Bounds());
    }
#endif

    status_t Invoke(BMessage* message = nullptr) override
    {
        // Native BTextControl also invokes changed text when focus leaves.
        // A tab switch or beforeunload prompt must not submit an address draft.
        if (!TextView()->IsFocus()) return B_OK;
        return BTextControl::Invoke(message);
    }

private:
    AddressEnterFilter* fFilter;
#if SUMMIT_MODERN_WEBKIT
    static bool HasButton(ToolButton* button) { return button && !button->IsHidden(button); }
    ToolButton* fCertificate = nullptr;
    ToolButton* fReader = nullptr;
    BRect fFullTextFrame;
#endif
};

// A click anywhere in the window may take the focus from the address field,
// whose suggestion list then closes.
class AddressFocusFilter final : public BMessageFilter {
public:
    AddressFocusFilter() : BMessageFilter(B_ANY_DELIVERY, B_ANY_SOURCE, B_MOUSE_DOWN) { }

    filter_result Filter(BMessage*, BHandler** target) override
    {
        if (target && *target && (*target)->Looper()) (*target)->Looper()->PostMessage(kAddressFocusCheck);
        return B_DISPATCH_MESSAGE;
    }
};

// Alt (Command) and the wheel over a page zoom it, as Ctrl and the wheel do
// in other browsers. Each notch is one zoom step.
class ZoomWheelFilter final : public BMessageFilter {
public:
    ZoomWheelFilter() : BMessageFilter(B_ANY_DELIVERY, B_ANY_SOURCE) { }

    filter_result Filter(BMessage* message, BHandler** target) override
    {
        switch (message->what) {
            case B_KEY_DOWN: case B_KEY_UP: case B_UNMAPPED_KEY_DOWN: case B_UNMAPPED_KEY_UP:
            case B_MODIFIERS_CHANGED: case B_MOUSE_DOWN: case B_MOUSE_UP: case B_MOUSE_MOVED:
                // Input messages carry the modifier keys; wheel messages do not.
                fModifiers = message->GetInt32("modifiers", fModifiers);
                return B_DISPATCH_MESSAGE;
            case B_MOUSE_WHEEL_CHANGED: break;
            default: return B_DISPATCH_MESSAGE;
        }
        if (!target || !dynamic_cast<BrowserWebView*>(*target)) return B_DISPATCH_MESSAGE;
        // An active window hears of every modifier change. Asking input_server
        // instead costs a round trip on every notch of a scroll.
        auto* window = dynamic_cast<BWindow*>(Looper());
        const uint32 keys = window && window->IsActive() ? fModifiers : modifiers();
        if (!(keys & B_COMMAND_KEY)) return B_DISPATCH_MESSAGE;
        float delta = 0;
        if (message->FindFloat("be:wheel_delta_y", &delta) != B_OK || delta == 0)
            message->FindFloat("be:wheel_delta_x", &delta);
        // Smooth wheels send fractions of a notch.
        fPending += delta;
        while (std::fabs(fPending) >= 1) {
            const bool in = fPending < 0;
            fPending += in ? 1 : -1;
            Looper()->PostMessage(in ? kZoomIn : kZoomOut);
        }
        return B_SKIP_MESSAGE;
    }

private:
    float fPending = 0;
    int32 fModifiers = 0;
};

static bool Bookmarkable(const std::string& url)
{
    return IsBookmarkableURL(url);
}
// Menu labels are cut at a character boundary; BMenu does not truncate.
static std::string MenuLabel(const PageRecord& page)
{
    std::string label = page.title.empty() ? page.url : page.title;
    if (label.size() <= 70) return label;
    size_t cut = 70;
    while (cut && (static_cast<unsigned char>(label[cut]) & 0xC0) == 0x80) --cut;
    return label.substr(0, cut) + "…";
}
// What the Bookmarks page's links must carry: random, made once per run and
// shared by every window (they all write the same page).
static const std::string& BookmarkPageSecret()
{
    static std::once_flag once;
    static std::string secret;
    std::call_once(once, [] {
        uint8 bytes[16];
        std::ifstream random("/dev/urandom", std::ios::binary);
        if (!random.read(reinterpret_cast<char*>(bytes), sizeof bytes))
            for (auto& byte : bytes) byte = uint8(std::rand() ^ system_time());
        for (uint8 byte : bytes) {
            secret += "0123456789abcdef"[byte >> 4];
            secret += "0123456789abcdef"[byte & 15];
        }
    });
    return secret;
}
// An address typed without its scheme is a web address.
static std::string BookmarkAddress(const std::string& typed)
{
    return NormalizeBookmarkAddress(typed);
}
static bool FindDownloads(BPath& path, std::string& error)
{
    status_t status = find_directory(B_USER_DIRECTORY, &path);
    if (status == B_OK) status = path.Append("Downloads");
    if (status != B_OK) {
        error = "Could not locate the Downloads folder: " + std::string(std::strerror(status));
        return false;
    }
    std::error_code filesystemError;
    std::filesystem::create_directories(path.Path(), filesystemError);
    if (filesystemError) {
        error = "Could not open the Downloads folder: " + filesystemError.message();
        return false;
    }
    return true;
}

static void AddItem(BMenu* menu, const char* label, uint32 what, char key = 0, uint32 mods = 0)
{
    menu->AddItem(new BMenuItem(label, new BMessage(what), key, mods));
}
// Every browser window, for the Window menu. Each window keeps its own entry
// up to date; any window thread may read the list.
namespace {
#if SUMMIT_MODERN_WEBKIT
// The slot under the pages that docked Developer Tools are placed over: it
// says when it moves or changes size (the split's divider, the window).
constexpr uint32 kDockSlotChanged = 'dkch';
class DockSlot final : public BView {
public:
    // Under the pages (a height) or beside them (a width).
    explicit DockSlot(bool beside = false) : BView(beside ? "devtools-dock-side" : "devtools-dock", B_FRAME_EVENTS)
    {
        SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
        SetExplicitMinSize(beside ? BSize(320, B_SIZE_UNSET) : BSize(B_SIZE_UNSET, 120));
    }
    void FrameMoved(BPoint) override { Changed(); }
    void FrameResized(float, float) override { Changed(); }

private:
    void Changed()
    {
        if (Window()) Window()->PostMessage(kDockSlotChanged);
    }
};
#endif
struct WindowEntry { BMessenger window; uint64 key; std::string title; };
std::mutex sWindowListLock;
std::vector<WindowEntry> sWindowList;
std::atomic<int32> sOpenWindows { 0 };
}
int32 BrowserWindow::CountOpenWindows() { return sOpenWindows.load(); }

#if SUMMIT_MODERN_WEBKIT
// View › Caret Browsing, Reduce Motion and Increase Contrast, and the
// minimum font size from Preferences: the profile's, for every tab (#118).
static BrowserWindow::AccessibilityAids ReadAccessibilityAids(SharedProfile& shared)
{
    return shared.Read([](const Profile& profile) {
        return BrowserWindow::AccessibilityAids { profile.caretBrowsing, profile.reduceMotion, profile.increaseContrast, profile.minimumFontSize,
            profile.overrideColors };
    });
}

// View › Override Page Colours: white text on black, links yellow (visited
// lilac), no background pictures; images and video are left as they are.
static constexpr const char* kOverrideColorsStyle =
    "*, *::before, *::after { background-color: #000 !important; color: #fff !important; border-color: #ccc !important;"
    " background-image: none !important; text-shadow: none !important; box-shadow: none !important;"
    " outline-color: #fff !important; caret-color: #fff !important; }"
    " a:link, a:link * { color: #ffe066 !important; } a:visited, a:visited * { color: #d4b8ff !important; }"
    " :focus-visible { outline: 2px solid #ffe066 !important; }"
    " ::selection { background-color: #fff !important; color: #000 !important; }"
    " img, video, canvas, svg, picture, iframe { background-color: transparent !important; }";

static void ApplyAccessibilityAids(BWebKitView* view, const BrowserWindow::AccessibilityAids& aids)
{
    if (!view) return;
    BMessage settings;
    settings.AddBool("caretBrowsing", aids.caretBrowsing);
    settings.AddBool("reducedMotion", aids.reduceMotion);
    settings.AddBool("increasedContrast", aids.increaseContrast);
    settings.AddInt32("minimumFontSize", std::clamp(aids.minimumFontSize, 0, 72));
    view->SetPageSettings(settings);
    view->SetUserStyleSheet(aids.overrideColors ? kOverrideColorsStyle : nullptr);
}
#endif

#if SUMMIT_MODERN_WEBKIT
// View › Sidebar: the loaded extensions' sidebars, read when it opens, the
// one shown marked; and Close Sidebar.
class SidebarMenu final : public BMenu {
public:
    SidebarMenu(std::shared_ptr<SharedProfile> shared, std::function<std::string()> shown)
        : BMenu("Sidebar"), fShared(std::move(shared)), fShown(std::move(shown)) { }
    void AttachedToWindow() override
    {
        RemoveItems(0, CountItems(), true);
        const std::string shown = fShown();
        for (const auto& sidebar : fShared->ExtensionSidebars()) {
            auto* message = new BMessage(kToggleSidebar);
            message->AddString("identifier", sidebar.identifier.c_str());
            message->AddString("url", sidebar.url.c_str());
            message->AddString("title", sidebar.title.c_str());
            auto* item = new BMenuItem(sidebar.title.c_str(), message);
            item->SetMarked(sidebar.identifier == shown);
            AddItem(item);
        }
        if (!CountItems()) {
            auto* none = new BMenuItem("No extension has a sidebar", nullptr);
            none->SetEnabled(false);
            AddItem(none);
        }
        AddSeparatorItem();
        auto* close = new BMenuItem("Close Sidebar", new BMessage(kCloseSidebar));
        close->SetEnabled(!shown.empty());
        AddItem(close);
        SetTargetForItems(Window());
        BMenu::AttachedToWindow();
    }
private:
    std::shared_ptr<SharedProfile> fShared;
    std::function<std::string()> fShown;
};
#endif

static BRect DefaultWindowFrame(const BrowserWindowOptions& options)
{
    BRect frame = options.frame.IsValid() ? options.frame
        : options.session.HasFrame() ? BRect(options.session.frame[0], options.session.frame[1], options.session.frame[2], options.session.frame[3])
        : BRect(75, 65, 1195, 745);
    // Keep the title tab reachable on the current screen.
    const BRect screen = BScreen().Frame();
    if (frame.Width() > screen.Width() - 10) frame.right = frame.left + screen.Width() - 10;
    if (frame.Height() > screen.Height() - 30) frame.bottom = frame.top + screen.Height() - 30;
    if (frame.left < 0 || frame.left > screen.right - 100) frame.OffsetTo(5, frame.top);
    if (frame.top < 25 || frame.top > screen.bottom - 100) frame.OffsetTo(frame.left, 25);
    return frame;
}

#ifndef B_DIRECT_DEVICE_PIXELS
#define B_DIRECT_DEVICE_PIXELS 0x00000400
#endif
#if SUMMIT_MODERN_WEBKIT && SUMMIT_AIROS
// Pages go straight into the screen where app_server lets a direct window
// draw in frame buffer pixels (air/OS). SUMMIT_DIRECT_PRESENT=0 keeps every
// frame going through app_server.
static uint32 DirectPresentFlags()
{
    const char* value = std::getenv("SUMMIT_DIRECT_PRESENT");
    return value && !std::strcmp(value, "0") ? 0 : B_DIRECT_DEVICE_PIXELS;
}
#else
// A stock app_server drops the flag from a new window but refuses every
// later SetFlags() that still has it.
static uint32 DirectPresentFlags() { return 0; }
#endif

// SetFlags() as a stock app_server takes it: it refuses a set with a flag it
// does not know, B_DIRECT_DEVICE_PIXELS of an air/OS build included.
static void SetWindowFlags(BWindow* window, uint32 flags)
{
    if (window->SetFlags(flags) != B_OK && (flags & B_DIRECT_DEVICE_PIXELS))
        window->SetFlags(flags & ~B_DIRECT_DEVICE_PIXELS);
}

BrowserWindow::BrowserWindow(std::shared_ptr<SharedProfile> profile, std::string startURL,
    const BrowserWindowOptions& options
#if SUMMIT_MODERN_WEBKIT
    , std::shared_ptr<BWebKitContext> context, bool extensionsEnabled
#endif
    )
    : BrowserWindowBase(DefaultWindowFrame(options), "Summit", B_TITLED_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
          B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS | DirectPresentFlags()),
#if SUMMIT_MODERN_WEBKIT
      fWebKitContext(std::move(context)),
#endif
      fShared(std::move(profile)), fKey(options.key), fPrivate(options.privateBrowsing), fStartURL(std::move(startURL))
{
#if SUMMIT_MODERN_WEBKIT
    // In a private window only extensions allowed there show (the engine
    // leaves the others out).
    fExtensionsEnabled = extensionsEnabled;
    fPopup = options.popup;
#endif
    SetPrivateWindow(this, fPrivate);
    ++sOpenWindows;
    {
        std::lock_guard lock(sWindowListLock);
        sWindowList.push_back({BMessenger(this), fKey, "Summit"});
    }
#if SUMMIT_MODERN_WEBKIT
    McpRegisterWindow({BMessenger(this), fKey, fPrivate});
#endif
    fShared->AddListener(BMessenger(this));
    fProtocolHandlers = std::make_unique<ProtocolHandlers>(fShared, fPrivate,
        [this](int64 id, const std::string& source) {
            if (IsClosing()) return false;
            const auto* tab = ActiveTab();
            return !id ? source.empty() : tab && tab->id == id && tab->url == source;
        }, [this](const std::string& url) { if (ActiveTab()) Navigate(url); else CreateTab(url); },
        [this](const std::string& error) { ShowError(error); });
    AddHandler(fProtocolHandlers.get());
#if SUMMIT_MODERN_WEBKIT
    // A web app's window shows only its pages (#75).
    if (!options.webApp.empty()) {
        fShared->Read([&](const Profile& profile) {
            if (const auto* app = profile.FindWebApp(options.webApp)) {
                fWebApp = app->id;
                fWebAppScope = app->scope;
                fWebAppName = app->name;
            }
            return 0;
        });
    }
#endif
#if SUMMIT_MODERN_WEBKIT
    // The bookmarks to compare the first change with.
    ReportBookmarkChanges();
    {
        BMessage check(kCheckMemory);
        fMemoryCheck = std::make_unique<BMessageRunner>(BMessenger(this), &check, 30000000);
    }
#endif
    std::tie(fBookmarksBarVisible, fInterfaceStyle) = fShared->Read([](const Profile& profile) {
        return std::make_pair(profile.showBookmarksBar, profile.interfaceStyle);
    });
    // A private window shows icons it already knows but writes none.
    fFavicons = std::make_unique<FaviconCache>(fShared->Path().parent_path() / "Favicons", !fPrivate);
    auto* menu = new BMenuBar("menu");
    auto* file = new BMenu("File");
    AddItem(file, "New Window", kNewWindow, 'N');
    AddItem(file, "New Private Window", kNewPrivateWindow, 'N', B_SHIFT_KEY);
    AddItem(file, "New Tab", kNewTab, 'T');
    AddItem(file, "Open File…", kOpenFile, 'O');
#if SUMMIT_MODERN_WEBKIT
    file->AddSeparatorItem();
    if (fWebApp.empty()) AddItem(file, "Install as App…", kInstallWebApp);
    else AddItem(file, "Open in Summit", kOpenInBrowser);
    AddItem(file, "Save as PDF…", kSaveAsPDF, 'S', B_SHIFT_KEY);
    AddItem(file, "Page Setup…", kPageSetup);
    AddItem(file, "Print…", kPrint, 'P');
#endif
    file->AddSeparatorItem();
    AddItem(file, "Import Bookmarks and History…", kImportBookmarks);
    AddItem(file, "Export Bookmarks…", kExportBookmarks);
    file->AddSeparatorItem();
    AddItem(file, "Close Window", kCloseWindow, 'W', B_SHIFT_KEY);
    AddItem(file, "Close Tab", kCloseTab, 'W');
    AddItem(file, "Reopen Closed Tab", kReopenTab, 'T', B_SHIFT_KEY);
    file->AddSeparatorItem();
    AddItem(file, "About Summit", B_ABOUT_REQUESTED);
#if SUMMIT_MODERN_WEBKIT
    AddItem(file, "Check for Updates…", kCheckForUpdates);
#endif
    AddItem(file, "Quit", B_QUIT_REQUESTED, 'Q');
    // Quit ends the application, with every window.
    file->FindItem(B_QUIT_REQUESTED)->SetTarget(be_app);
    menu->AddItem(file);
    auto* edit = new BMenu("Edit");
    AddItem(edit, "Undo", B_UNDO, 'Z');
    AddItem(edit, "Redo", B_REDO, 'Z', B_SHIFT_KEY);
    edit->AddSeparatorItem();
    AddItem(edit, "Cut", B_CUT, 'X');
    AddItem(edit, "Copy", B_COPY, 'C');
    AddItem(edit, "Paste", B_PASTE, 'V');
    AddItem(edit, "Select All", B_SELECT_ALL, 'A');
    edit->AddSeparatorItem();
    AddItem(edit, "Find in Page…", kFind, 'F');
    AddItem(edit, "Find Next", kFindNext, 'G');
#if SUMMIT_MODERN_WEBKIT
    edit->AddSeparatorItem();
    fSpellingItem = new BMenuItem("Check Spelling While Typing", new BMessage(kToggleSpellChecking));
    edit->AddItem(fSpellingItem);
#endif
    edit->AddSeparatorItem();
    AddItem(edit, "Preferences…", kShowPreferences, ',');
    menu->AddItem(edit);
    auto* view = new BMenu("View");
    fBookmarksBarItem = new BMenuItem("Hide Bookmarks Bar", new BMessage(kToggleBookmarksBar), 'B', B_SHIFT_KEY);
    view->AddItem(fBookmarksBarItem);
    view->AddSeparatorItem();
    AddItem(view, "Focus Address", kFocusAddress, 'L');
    AddItem(view, "Reload", kReload, 'R');
    AddItem(view, "Home", kHome, 'H', B_SHIFT_KEY);
    view->AddSeparatorItem();
    AddItem(view, "Zoom In", kZoomIn, '+');
    AddItem(view, "Zoom Out", kZoomOut, '-');
    AddItem(view, "Actual Size", kZoomReset, '0');
    fZoomTextOnlyItem = new BMenuItem("Zoom Text Only", new BMessage(kToggleZoomTextOnly));
    view->AddItem(fZoomTextOnlyItem);
    // '+' is Shift-= on most keyboards, and shortcuts match their modifiers exactly.
    AddShortcut('+', B_SHIFT_KEY, new BMessage(kZoomIn));
    AddShortcut('=', 0, new BMessage(kZoomIn));
#if SUMMIT_MODERN_WEBKIT
    // F7, as in Firefox and Chrome; a menu shortcut cannot be a function key.
    fCaretBrowsingItem = new BMenuItem("Caret Browsing (F7)", new BMessage(kToggleCaretBrowsing));
    fAids = ReadAccessibilityAids(*fShared);
    fCaretBrowsingItem->SetMarked(fAids.caretBrowsing);
    view->AddItem(fCaretBrowsingItem);
    // Pages' prefers-reduced-motion and prefers-contrast media queries.
    fReducedMotionItem = new BMenuItem("Reduce Motion", new BMessage(kToggleReducedMotion));
    fReducedMotionItem->SetMarked(fAids.reduceMotion);
    view->AddItem(fReducedMotionItem);
    fIncreasedContrastItem = new BMenuItem("Increase Contrast", new BMessage(kToggleIncreasedContrast));
    fIncreasedContrastItem->SetMarked(fAids.increaseContrast);
    view->AddItem(fIncreasedContrastItem);
    fOverrideColorsItem = new BMenuItem("Override Page Colours", new BMessage(kToggleOverrideColors));
    fOverrideColorsItem->SetMarked(fAids.overrideColors);
    view->AddItem(fOverrideColorsItem);
    view->AddSeparatorItem();
    // Extensions' sidebars (Firefox's sidebar_action), listed when opened.
    if (!fPrivate && !fPopup) view->AddItem(new SidebarMenu(fShared, [this] { return fSidebarExtension; }));
    AddItem(view, "View Page Source", kViewPageSource, 'U');
    AddItem(view, "Picture in Picture", kPictureInPicture, 'P', B_SHIFT_KEY);
    auto* translation = new BMessage(kTranslatePage);
    translation->AddBool("offer", true);
    view->AddItem(new BMenuItem("Translate Page…", translation));
    view->AddSeparatorItem();
    AddItem(view, "Developer Tools", kShowDeveloperTools, 'I', B_SHIFT_KEY);
    auto* pick = new BMessage(kShowDeveloperTools);
    pick->AddString("panel", "elements");
    pick->AddBool("pick", true);
    view->AddItem(new BMenuItem("Select Element in Page", pick, 'C', B_SHIFT_KEY));
#endif
    menu->AddItem(view);
    fHistoryMenu = new BMenu("History");
    AddItem(fHistoryMenu, "Back", kBack, '[');
    AddItem(fHistoryMenu, "Forward", kForward, ']');
    AddItem(fHistoryMenu, "Home", kHome);
    fHistoryMenu->AddSeparatorItem();
    AddItem(fHistoryMenu, "Show History", kShowHistory, 'Y');
    AddItem(fHistoryMenu, "Clear History…", kClearHistory);
    fHistoryMenuFixed = fHistoryMenu->CountItems();
    menu->AddItem(fHistoryMenu);
    fBookmarksMenu = new BMenu("Bookmarks");
    AddItem(fBookmarksMenu, "Bookmark This Page", kBookmark, 'D');
    AddItem(fBookmarksMenu, "Add to Bookmarks Bar", kAddToBookmarksBar, 'D', B_SHIFT_KEY);
    fEditBookmarkItem = new BMenuItem("Edit Bookmark…", new BMessage(kEditBookmark));
    fBookmarksMenu->AddItem(fEditBookmarkItem);
    fRemoveBookmarkItem = new BMenuItem("Remove Bookmark", new BMessage(kRemoveBookmark));
    fBookmarksMenu->AddItem(fRemoveBookmarkItem);
    fBookmarksMenu->AddSeparatorItem();
    AddItem(fBookmarksMenu, "New Folder…", kNewBookmarkFolder);
    AddItem(fBookmarksMenu, "Show All Bookmarks", kShowBookmarks, 'B');
    fBookmarksMenuFixed = fBookmarksMenu->CountItems();
    menu->AddItem(fBookmarksMenu);
    fWindowMenu = new BMenu("Window");
    AddItem(fWindowMenu, "Next Tab", kNextTab);
    AddItem(fWindowMenu, "Previous Tab", kPreviousTab);
    AddItem(fWindowMenu, "Move Tab to New Window", kMoveTabToNewWindow);
    fWindowMenu->AddSeparatorItem();
#if SUMMIT_MODERN_WEBKIT
    AddItem(fWindowMenu, "Downloads", kToggleDownloadsPanel, 'J');
    AddItem(fWindowMenu, "Show Downloads Folder", kShowDownloads);
#else
    AddItem(fWindowMenu, "Downloads", kShowDownloads, 'J');
#endif
#if SUMMIT_MODERN_WEBKIT
    auto* extensions = new BMenuItem("Extensions…", new BMessage(kShowExtensions));
    extensions->SetEnabled(extensionsEnabled);
    fWindowMenu->AddItem(extensions);
#endif
    fWindowMenuFixed = fWindowMenu->CountItems();
    menu->AddItem(fWindowMenu);

    fToolbar = new BGroupView(B_HORIZONTAL, 4);
    auto* homeButton = new ToolButton("home", "Home", Icon::Home, kHome);
    fBack = new ToolButton("back", "Back", Icon::Back, kBack);
    fForward = new ToolButton("forward", "Forward", Icon::Forward, kForward);
    fReload = new ToolButton("reload", "Reload / stop", Icon::Reload, kReload);
    fAddress = new AddressControl;
    fAddress->SetExplicitMinSize(BSize(fPopup ? 120 : 240, 30));
    static_cast<AddressControl*>(fAddress)->SetKeyHandler([this](char key) { return AddressKey(key); });
    fSuggestions = std::make_unique<AddressSuggestions>(this);
    fZoomButton = new ZoomButton;
#if SUMMIT_MODERN_WEBKIT
    auto* downloadsButton = fDownloadsButton = new DownloadsButton;
#else
    auto* downloadsButton = new ToolButton("downloads", "Open Downloads", Icon::Downloads, kShowDownloads);
#endif
    fBookmarkButton = new ToolButton("bookmark", "Bookmark this page", Icon::Bookmark, kBookmarkButton);
#if SUMMIT_MODERN_WEBKIT
    fExtensionActions = new BGroupView("extension-actions", B_HORIZONTAL, 2);
    fCertificateButton = static_cast<AddressControl*>(fAddress)->CertificateButton();
#endif
    fTabStrip = new TabStrip();
    fProgress = new ProgressLine();
    fBookmarksBar = new BookmarksBar();
    fBookmarksBar->SetMenusCancelled(&fMenusCancelled);
    fPages = new BView("pages", 0);
    fCards = new BCardLayout();
    fPages->SetLayout(fCards);
    fPages->SetExplicitMinSize(fPopup ? BSize(160, 100) : BSize(320, 200));
    fFindBar = new BGroupView(B_HORIZONTAL, 6);
    fFindText = new BTextControl("find", "Find:", "", new BMessage(kFindNext));
    BLayoutBuilder::Group<>(fFindBar).SetInsets(8, 5, 8, 5)
        .Add(fFindText).Add(new BButton("previous", "Previous", new BMessage(kFindPrevious)))
        .Add(new BButton("next", "Next", new BMessage(kFindNext)))
        .Add(new BButton("done", "Done", new BMessage(kCloseFind)));
    fFindBar->Hide();
    fStatus = new BStringView("status", "Ready");
    fStatus->SetExplicitMinSize(BSize(150, 21));
    fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 21));
    fStatus->SetTruncation(B_TRUNCATE_MIDDLE);
    fStatusProgress = new BStatusBar("page-progress");
    fStatusProgress->SetExplicitMinSize(BSize(120, B_SIZE_UNSET));
    fStatusProgress->SetExplicitMaxSize(BSize(160, 21));
    fStatusProgress->SetBarHeight(10);
    fStatusProgress->SetMaxValue(1.0f);
    auto* statusLine = new BGroupView(B_HORIZONTAL, 8);
    BLayoutBuilder::Group<>(statusLine).SetInsets(4, 0, 4, 0).Add(fStatus, 1).Add(fStatusProgress);
    fStatusProgress->Hide();
#if SUMMIT_MODERN_WEBKIT
    fNoticeBar = new NoticeBar();
    // The sidebar: its title and a close button over the extension's page.
    fSidebar = new BGroupView("sidebar", B_VERTICAL, 0);
    fSidebarTitle = new BStringView("sidebar-title", "");
    fSidebarTitle->SetFont(be_bold_font);
    fSidebarTitle->SetTruncation(B_TRUNCATE_END);
    fSidebarTitle->SetExplicitMinSize(BSize(60, B_SIZE_UNSET));
    fSidebarTitle->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    auto* closeSidebar = new BButton("sidebar-close", "×", new BMessage(kCloseSidebar));
    closeSidebar->SetToolTip("Close the sidebar");
    closeSidebar->SetFlat(true);
    BLayoutBuilder::Group<>(fSidebar)
        .AddGroup(B_HORIZONTAL, 4).SetInsets(8, 2, 2, 2).Add(fSidebarTitle, 1).Add(closeSidebar).End();
    fSidebar->SetExplicitMinSize(BSize(180, B_SIZE_UNSET));
    fSidebar->Hide();
#endif
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .Add(menu).Add(fToolbar).Add(fBookmarksBar).Add(fTabStrip).Add(fProgress)
#if SUMMIT_MODERN_WEBKIT
        .Add(fNoticeBar)
#endif
#if SUMMIT_MODERN_WEBKIT
        .AddSplit(B_HORIZONTAL, 0, 1.0f).GetSplitView(&fContentSplit)
            .Add(fSidebar, 0.25f)
            .AddSplit(B_HORIZONTAL, 0, 0.75f).GetSplitView(&fSideSplit)
                .AddSplit(B_VERTICAL, 0, 0.75f).GetSplitView(&fPageSplit)
                    .Add(fPages, 0.65f)
                    .Add(fDock = new DockSlot(), 0.35f)
                .End()
                .Add(fSideDock = new DockSlot(true), 0.4f)
            .End()
        .End()
#else
        .Add(fPages, 1)
#endif
        .Add(fFindBar).Add(statusLine);
    fLayout = static_cast<BGroupLayout*>(GetLayout());
#if SUMMIT_MODERN_WEBKIT
    fNoticeBar->Hide();
    fDock->Hide();
    fSideDock->Hide();
    fSplitterSize = fContentSplit->SplitterSize();
    UpdateSplitters();
#endif
#if SUMMIT_MODERN_WEBKIT
    fFullScreenNotice = new BStringView("fullscreen-notice", "");
    fFullScreenNotice->SetAlignment(B_ALIGN_CENTER);
    fFullScreenNotice->SetTruncation(B_TRUNCATE_MIDDLE);
    fFullScreenNotice->SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
    // BStringView otherwise caps the layout at its text's preferred width.
    // Showing the notice must not shrink a fullscreen window to that width.
    fFullScreenNotice->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    fFullScreenNotice->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
    fFullScreenNotice->SetHighUIColor(B_PANEL_TEXT_COLOR);
    fLayout->AddView(fFullScreenNotice);
    fFullScreenNotice->Hide();
#endif
    // Glue that only the Safari-like look shows (ApplyInterfaceStyle), to
    // centre the address field.
    auto glue = [] {
        auto* view = new BView("toolbar-glue", 0);
        view->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
        view->SetExplicitMinSize(BSize(0, 0));
        view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
        return view;
    };
    BLayoutBuilder::Group<>(fToolbar)
        .SetInsets(8, 7, 8, 7)
        .Add(fBack).Add(fForward).Add(homeButton)
        .Add(glue())
        .Add(fAddress, 3).Add(fZoomButton).Add(fReload).Add(glue())
        .Add(fBookmarkButton)
        .Add(downloadsButton)
#if SUMMIT_MODERN_WEBKIT
        .Add(fExtensionActions)
#endif
        ;
#if SUMMIT_MODERN_WEBKIT
    fExtensionActions->Hide();
#endif
    fZoomButton->Hide();
    if (fPrivate) {
        BLayoutBuilder::Group<>(fToolbar).Add(new PrivateBadge);
        // Views that Summit does not draw itself take the private colours too.
        const auto colors = ChromeColorsFor(true);
        const rgb_color panel = colors.panel;
        for (BView* view : std::initializer_list<BView*>{fToolbar, fAddress, statusLine, fStatus, fStatusProgress}) {
            view->SetViewColor(panel);
            view->SetLowColor(panel);
        }
        for (int32 i = 0; i < fToolbar->CountChildren(); ++i)
            if (auto* child = fToolbar->ChildAt(i); !std::strcmp(child->Name(), "toolbar-glue")) child->SetViewColor(panel);
        fStatus->SetHighColor(colors.text);
    }
    ShowSearchEngine();
    AddCommonFilter(new ZoomWheelFilter);
    AddCommonFilter(new AddressFocusFilter);
    ApplyInterfaceStyle();
    if (!fBookmarksBarVisible || !fWebApp.empty() || fPopup) {
        fBookmarksBar->Hide();
        fBookmarksBarItem->SetLabel("Show Bookmarks Bar");
    }
#if SUMMIT_MODERN_WEBKIT
    for (char number = '1'; number <= '9'; ++number) {
        auto* select = new BMessage(kSelectTabNumber);
        select->AddInt32("number", number - '0');
        AddShortcut(number, B_COMMAND_KEY, select);
    }
#endif
    AddShortcut(B_TAB, B_CONTROL_KEY, new BMessage(kNextTab));
    AddShortcut(B_TAB, B_CONTROL_KEY | B_SHIFT_KEY, new BMessage(kPreviousTab));
#if SUMMIT_MODERN_WEBKIT
    // Summit's own shortcuts win over extensions' (the Keyboard Shortcuts
    // window says so); a full browser window has them all.
    if (fWebApp.empty() && !fPopup) {
        std::vector<std::pair<ExtensionShortcutKeys, std::string>> shortcuts;
        CollectMenuShortcuts(menu, shortcuts);
        for (unsigned number = '1'; number <= '9'; ++number)
            shortcuts.emplace_back(ExtensionShortcutKeys { number, ExtensionShortcutKeys::Command }, "Select Tab " + std::string(1, char(number)));
        ExtensionShortcuts::Default().SetBrowserShortcuts(std::move(shortcuts));
    }
#endif
    // Pop-ups as small as pages make them (sign-in and share dialogs), web
    // apps as their pages need (#95); the rest room for the whole toolbar.
    if (fPopup) SetSizeLimits(200, 10000, 150, 10000);
    else if (!fWebApp.empty()) SetSizeLimits(320, 10000, 240, 10000);
    else SetSizeLimits(760, 10000, 450, 10000);
#if SUMMIT_MODERN_WEBKIT
    DownloadList::Default().AddListener(BMessenger(this));
    RefreshDownloadsButton();
    {
        uint8 secret[16];
        std::ifstream random("/dev/urandom", std::ios::binary);
        if (!random.read(reinterpret_cast<char*>(secret), sizeof secret))
            for (auto& byte : secret) byte = uint8(std::rand() ^ system_time());
        for (uint8 byte : secret) {
            fStoreOfferToken += "0123456789abcdef"[byte >> 4];
            fStoreOfferToken += "0123456789abcdef"[byte & 15];
        }
    }
    BPath downloadsPath;
    std::string downloadsError;
    if (!FindDownloads(downloadsPath, downloadsError)) ShowError(downloadsError);
    else if (auto status = fWebKitContext->SetDownloadDirectory(downloadsPath.Path()); status != B_OK)
        ShowError("Could not configure downloads: " + std::string(std::strerror(status)));
#else
    BWebPage::SetDownloadListener(BMessenger(this));
#endif

    if (!options.empty) {
#if SUMMIT_MODERN_WEBKIT
        if (options.newPage) CreateTab(options.newPageURL, true, nullptr, -1, 0, 0, options.newPage);
        else
#endif
        if (!options.urls.empty()) for (const auto& url : options.urls) CreateTab(url);
        else if (!options.session.tabs.empty()) {
#if SUMMIT_MODERN_WEBKIT
            fRestoringSession = true;
            for (const auto& page : options.session.tabs) { fRestoringTitle = page.title; CreateTab(page.url, false); }
            fRestoringSession = false;
            fRestoringTitle.clear();
            for (size_t i = 0; i < std::min(options.session.pinned, fTabs.size()); ++i) {
                fTabs[i].pinned = true;
                fWebKitContext->SetBrowserTabPinned(fTabs[i].view, true);
            }
#else
            for (const auto& page : options.session.tabs) CreateTab(page.url, false);
#endif
            if (!fTabs.empty()) SelectTab(fTabs[std::min(options.session.selected, fTabs.size() - 1)].id);
        } else CreateTab(fPrivate ? NewTabAddress() : HomeAddress());
        // Invalid command-line or saved URLs (or home page) may all have been rejected.
        if (fTabs.empty()) CreateTab("summit:home");
    }
    RefreshBookmarks();
    BMessage save(kSaveSession);
    fSaveTimer = std::make_unique<BMessageRunner>(BMessenger(this), &save, 5000000);
    SaveSession();
}
BrowserWindow::~BrowserWindow()
{
    RemoveHandler(fProtocolHandlers.get());
    fProtocolHandlers.reset();
#if SUMMIT_MODERN_WEBKIT
    fTranslationTimer.reset();
    fTranslator.reset();
    if (fAuthenticationPrompt.IsValid()) fAuthenticationPrompt.SendMessage(B_QUIT_REQUESTED);
    if (fPasskeyPrompt.IsValid()) fPasskeyPrompt.SendMessage(B_QUIT_REQUESTED);
    BWebKitView::WindowDirectConnected(this, nullptr);
    // Without its daemon thread ~BDirectWindow would wait for ever.
    if (DirectDaemonLost()) {
        std::fprintf(stderr, "Summit: the window's direct daemon is gone; closing without it\n");
        summit::ReleaseDeadDirectConnection(*this);
    }
#endif
    SetPrivateWindow(this, false);
    fShared->RemoveListener(BMessenger(this));
    {
        std::lock_guard lock(sWindowListLock);
        std::erase_if(sWindowList, [this](const WindowEntry& entry) { return entry.key == fKey; });
#if SUMMIT_MODERN_WEBKIT
        McpUnregisterWindow(fKey);
#endif
    }
#if SUMMIT_MODERN_WEBKIT
    if (fExtensionMenuCancelled) *fExtensionMenuCancelled = true;
    if (fCertificateWindow.IsValid()) fCertificateWindow.SendMessage(B_QUIT_REQUESTED);
    DownloadList::Default().RemoveListener(BMessenger(this));
    CloseDownloadsPanel(true);
    fWebKitContext->SetBrowserWindowTabs(BMessenger(this), { }, nullptr, false);
    SaveSession();
    fSaveTimer.reset();
    for (auto& tab : fTabs) {
        CloseDeveloperTools(tab, true);
        tab.view->RemoveSelf();
        delete tab.view;
    }
    fTabs.clear();
#endif
}
#if SUMMIT_MODERN_WEBKIT
BrowserWindow::Tab* BrowserWindow::FindTab(const BMessenger& view)
{
    if (!view.IsValid()) return nullptr;
    for (auto& tab : fTabs) if (tab.messenger == view) return &tab;
    return nullptr;
}
#else
BrowserWindow::Tab* BrowserWindow::FindTab(BWebView* view)
{
    for (auto& tab : fTabs) if (tab.view == view) return &tab;
    return nullptr;
}
#endif
BrowserWindow::Tab* BrowserWindow::ActiveTab()
{
    for (auto& tab : fTabs) if (tab.id == fSelected) return &tab;
    return nullptr;
}
// Posts the burst from its own thread: the window thread has to stay free to
// draw the frames whose pacing is being measured.
status_t BrowserWindow::RunScrollBurst(void* data)
{
    std::unique_ptr<ScrollBurst> burst(static_cast<ScrollBurst*>(data));
    const bigtime_t started = system_time();
    int32 sent = 0;
    status_t result = B_OK;
    for (int32 index = 0; index < burst->count; ++index) {
        // A full view port throttles the burst instead of dropping events.
        result = burst->view.SendMessage(&burst->event, static_cast<BHandler*>(nullptr), 2000000);
        if (result != B_OK)
            break;
        ++sent;
        if (burst->interval) snooze(burst->interval);
    }
    BMessage completed(kScrollBurstCompleted);
    completed.AddInt32("sent", sent);
    completed.AddInt32("status", result);
    completed.AddInt64("duration_us", system_time() - started);
    burst->window.SendMessage(&completed);
    return result;
}
void BrowserWindow::SimulateScroll(const BMessage& message, BMessage& reply)
{
    // The MCP server (BrowserWindowMcp.cpp) may: whoever turned it on allowed it.
    const char* enabled = std::getenv("SUMMIT_ENABLE_INPUT_SYNTHESIS");
    if (!message.GetBool("mcp", false) && (!enabled || std::strcmp(enabled, "1") != 0)) {
        reply.AddString("error", "input synthesis is disabled");
        return;
    }
    auto* tab = ActiveTab();
    if (!tab || !tab->view) {
        reply.AddString("error", "no active tab");
        return;
    }
    if (fScrollBurstActive) {
        reply.AddString("error", "a scroll burst is already active");
        return;
    }
    const int32 count = message.GetInt32("count", 60);
    const int32 interval = message.GetInt32("interval_ms", 16);
    const float delta = message.GetFloat("delta", 3.0f);
    const float deltaX = message.GetFloat("delta_x", 0.0f);
    if (count < 1 || count > 100000 || interval < 0 || interval > 10000
        || !std::isfinite(delta) || std::fabs(delta) > 1000
        || !std::isfinite(deltaX) || std::fabs(deltaX) > 1000) {
        reply.AddString("error", "unusable burst parameters");
        return;
    }
    auto burst = std::make_unique<ScrollBurst>();
    status_t status = B_OK;
    burst->view = BMessenger(tab->view, this, &status);
    if (status != B_OK || !burst->view.IsValid()) {
        reply.AddString("error", "the page view cannot be addressed");
        return;
    }
    burst->window = BMessenger(this);
    burst->count = count;
    burst->interval = bigtime_t(interval) * 1000;
    burst->event.what = B_MOUSE_WHEEL_CHANGED;
    burst->event.AddFloat("be:wheel_delta_x", deltaX);
    burst->event.AddFloat("be:wheel_delta_y", delta);
    // A wheel notch applies where the pointer is. Name the middle of the page
    // instead of moving the pointer, so a burst does not take the mouse away
    // from whoever is using the machine.
    BRect bounds = tab->view->Bounds();
    BPoint centre(bounds.left + bounds.Width() / 2, bounds.top + bounds.Height() / 2);
    // FindPoint() clears the point it does not find, which sent every burst
    // without a named point to the page's top left corner.
    BPoint at = centre, named;
    if (message.FindPoint("at", &named) == B_OK) {
        if (!std::isfinite(named.x) || !std::isfinite(named.y) || !bounds.Contains(named)) {
            reply.AddString("error", "wheel point is outside the page view");
            return;
        }
        at = named;
    }
    burst->event.AddPoint("summit:view_where", at);
    // BWindow routes a wheel message to the view named by "_view_token", and
    // only falls back to whichever view the pointer last moved over. Without
    // the token a synthesized notch is delivered to that other view, or
    // dropped, and the page never scrolls.
    burst->event.AddInt32("_view_token", _get_object_token_(tab->view));
    thread_id thread = spawn_thread(&BrowserWindow::RunScrollBurst, "summit scroll burst",
        B_DISPLAY_PRIORITY, burst.get());
    if (thread < 0) {
        reply.AddString("error", "no thread for the burst");
        return;
    }
#if SUMMIT_MODERN_WEBKIT
    if (auto* statsView = dynamic_cast<FrameStatsWebKitView*>(tab->view))
        statsView->ResetFrameStats();
#endif
    burst.release();
    fScrollBurstActive = true;
    fScrollBurstRequested = count;
    fScrollBurstSent = 0;
    fScrollBurstDuration = 0;
    fScrollBurstStatus = B_OK;
#if SUMMIT_MODERN_WEBKIT
    fScrollBurstCompletionFrameStatsAvailable = false;
#endif
    resume_thread(thread);
    reply.AddInt32("count", count);
    reply.AddInt32("interval_ms", interval);
    reply.AddFloat("delta", delta);
    reply.AddPoint("at", at);
}
std::string BrowserWindow::StoredURL(const BString& url) const
{
    // Haiku's URL notification can use file:/path before WebKit commits
    // the canonical file:///path form. Both refer to our built-in pages.
    const std::string value = url.String();
#if SUMMIT_MODERN_WEBKIT
    for (const auto& tab : fTabs)
        if (auto found = tab.readerOriginals.find(value); found != tab.readerOriginals.end()) return found->second;
#endif
    auto matches = [&](const std::string& file) {
        return value == file || (file.rfind("file:///", 0) == 0 && value == "file:" + file.substr(7));
    };
    if (matches(fStartURL)) return "summit:home";
    if (fPrivate && matches(FileURL((fShared->Path().parent_path() / "Pages" / "private.html").string()))) return "summit:home";
    for (const char* page : {kHistoryPage, kBookmarksPage})
        if (matches(FileURL(InternalPagePath(page).string()))) return page;
    if (BookmarkQueryForURL(url)) return kBookmarksPage;
    return value;
}
#if SUMMIT_MODERN_WEBKIT
void BrowserWindow::CreateTab(const std::string& input, bool select, const char* extensionIdentifier, int32 index, uint64 command, int64 replaces, uint64 newPage)
#else
void BrowserWindow::CreateTab(const std::string& input, bool select, BWebView* adopted)
#endif
{
#if SUMMIT_MODERN_WEBKIT
    // An extension learns about a failure through its own promise; only the
    // user's own requests interrupt with an alert.
    auto fail = [&](const std::string& error) {
        if (newPage) BWebKitView::DeclineNewPage(newPage);
        if (command) TabOpenedForCommand(command, nullptr, error);
        else if (!newPage) ShowError(error);
    };
    if (fClosingWindow) { fail("The window is closing."); return; }
#else
    auto fail = [&](const std::string& error) { ShowError(error); };
#endif
#if !SUMMIT_MODERN_WEBKIT
    // BWebPage is owned by the application looper. Its constructor accesses
    // WebCore and takes the application lock; never call it from a window thread.
    if (!adopted && find_thread(nullptr) != be_app->Thread()) {
        BMessage create(kCreateTabOnApp);
        create.AddString("url", input.c_str());
        create.AddBool("select", select);
        create.AddMessenger("window", BMessenger(this));
        be_app->PostMessage(&create);
        return;
    }
#endif
    if (fTabs.size() >= 512) { fail("The session has reached its 512-tab limit."); return; }
#if SUMMIT_MODERN_WEBKIT
    // The engine loads a page another page opened; its address is only shown.
    const auto address = newPage ? Address { input, "", false } : ResolveAddress(input);
#else
    const auto address = ResolveAddress(input);
#endif
    if (!address.error.empty()) { fail(address.error); return; }
    if (IsExternalScheme(URLScheme(address.url))) {
#if SUMMIT_MODERN_WEBKIT
        if (newPage || command) { fail("Open external links from the page or address field."); return; }
        // Restoring a session must never launch another application.
        if (fRestoringSession) return;
#endif
        auto* tab = ActiveTab();
        fProtocolHandlers->Open(address.url, tab ? tab->id : 0, tab ? tab->url : "");
        return;
    }
#if SUMMIT_MODERN_WEBKIT
    const bool extensionPage = !newPage && summit::IsExtensionPageURL(address.url);
    if (extensionPage && !extensionIdentifier) {
        // The application owns the live extension catalog. Resolve its current
        // origin and create the privileged view on WebKit's application thread.
        BMessage create(kCreateTabOnApp);
        create.AddString("url", address.url.c_str());
        create.AddBool("select", select);
        create.AddMessenger("window", BMessenger(this));
        // A restored tab keeps its place: the tabs after it are added while
        // its extension may still be loading.
        create.AddInt32("index", index < 0 && fRestoringSession ? static_cast<int32>(fTabs.size()) : index);
        create.AddUInt64("command", command);
        create.AddInt64("replaces", replaces);
        if (be_app->PostMessage(&create) != B_OK) fail("Could not open the extension page.");
        return;
    }
    BWebKitView* webView = nullptr;
    if (extensionPage) {
        if (!*extensionIdentifier) { fail("This extension is not available."); return; }
        status_t status;
        webView = fWebKitContext->CreateExtensionView(BRect(0, 0, 319, 199), "web-page", extensionIdentifier, BMessenger(this), &status);
        if (!webView) { fail("Could not open the extension page: " + std::string(std::strerror(status))); return; }
    } else {
        const char* frameStats = std::getenv("SUMMIT_UI_FRAME_STATS");
        if (newPage)
            webView = new BWebKitView(BRect(0, 0, 319, 199), "web-page", BMessenger(this), newPage, B_FOLLOW_ALL, fWebKitContext);
        else if (frameStats && (std::strcmp(frameStats, "1") == 0 || std::strcmp(frameStats, "2") == 0))
            webView = new FrameStatsWebKitView(BRect(0, 0, 319, 199), "web-page", BMessenger(this), B_FOLLOW_ALL, fWebKitContext);
        else
            webView = new BWebKitView(BRect(0, 0, 319, 199), "web-page", BMessenger(this), B_FOLLOW_ALL, fWebKitContext);
    }
    if (webView->InitCheck() != B_OK) {
        const status_t status = webView->InitCheck();
        delete webView;
        newPage = 0; // The view declined it.
        fail("Could not create the web page: " + std::string(std::strerror(status)));
        return;
    }
    if (fAids != AccessibilityAids { }) ApplyAccessibilityAids(webView, fAids);
#else
    auto* webView = adopted ? adopted : new BWebView("web-page");
#endif
    webView->SetExplicitMinSize(fPopup ? BSize(160, 100) : BSize(320, 200));
    webView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
#if SUMMIT_MODERN_WEBKIT
    // The card layout and the tab list share one order; keep them in step.
    const size_t position = index < 0 ? fTabs.size() : std::min(static_cast<size_t>(index), fTabs.size());
    fCards->AddView(static_cast<int32>(position), webView);
    Tab created { };
    created.id = fNextID++;
    created.view = webView;
    created.url = address.url;
    created.title = address.url == "summit:home" ? "Start Page" : address.url == kHistoryPage ? "History"
        : address.url == kBookmarksPage ? "Bookmarks" : "Loading…";
    created.pageRevision = fPagesRevision;
    created.lastSelected = system_time();
    AttachPageView(created, webView, extensionPage);
    // As in other browsers, a restored session's tabs wait to be selected
    // before they load: a window of fifteen tabs would otherwise start fifteen
    // page loads and web processes at once, with only one of them on screen.
    const bool deferred = fRestoringSession && !newPage && !address.url.starts_with("summit:");
    if (deferred) {
        created.deferredURL = LoadableURL(address.url);
        if (!fRestoringTitle.empty()) created.title = fRestoringTitle;
        fWebKitContext->SetBrowserTabDiscarded(webView, true, address.url.c_str(), created.title.c_str());
    }
    // The site's remembered zoom is set before its page loads.
    created.zoomKey = ZoomKey(address.url);
    created.pageZoom = fShared->SiteZoom(created.zoomKey, fPrivate);
    if (!IsDefaultZoom(created.pageZoom)) webView->SetZoomFactor(created.pageZoom);
    const int64 createdID = created.id;
    fTabs.insert(fTabs.begin() + position, std::move(created));
    if (select || fSelected == 0) SelectTab(createdID);
    else if (auto* active = ActiveTab()) {
        // Inserting before the visible card shifts its index.
        for (size_t i = 0; i < fTabs.size(); ++i)
            if (fTabs[i].id == active->id) fCards->SetVisibleItem(fCards->IndexOfView(fTabs[i].view));
    }
#else
    fCards->AddView(webView);
    fTabs.push_back({fNextID++, webView, address.url, address.url == "summit:home" ? "Start Page" : "Loading…"});
    if (select || fSelected == 0) SelectTab(fTabs.back().id);
#endif
#if SUMMIT_MODERN_WEBKIT
    if (!newPage && !deferred) webView->LoadURL(LoadableURL(address.url).c_str());
#else
    if (!adopted) webView->LoadURL(LoadableURL(address.url).c_str(), select);
#endif
    if (select && address.url == "summit:home") fAddress->MakeFocus();
#if SUMMIT_MODERN_WEBKIT
    SyncBrowserWindow();
#endif
    RefreshChrome();
#if SUMMIT_MODERN_WEBKIT
    if (replaces && replaces != createdID) CloseTab(replaces);
    // Answer after the registry submission above, so the engine can already
    // resolve the new view as a tab of this window.
    if (command) {
        auto created = std::find_if(fTabs.begin(), fTabs.end(), [&](const Tab& tab) { return tab.id == createdID; });
        TabOpenedForCommand(command, created == fTabs.end() ? nullptr : &*created, "The tab closed before it could be reported.");
    }
#endif
}
#if SUMMIT_MODERN_WEBKIT
void BrowserWindow::AttachPageView(Tab& tab, BWebKitView* webView, bool extensionPage)
{
    tab.view = webView;
    tab.messenger = BMessenger(webView);
    tab.frames = new FrameHistory();
    webView->AddFilter(tab.frames);
    webView->SetURLSchemeHandler("summit-reader", BMessenger(this));
    if (!extensionPage) AddPageScripts(webView);
    // Sites that ask for a name and password get a sign-in sheet (#87).
    if (!extensionPage) webView->SetAuthenticationRequests(true);
    // Summit's own passkeys (#111): a page's WebAuthn request that no
    // extension takes asks the user (PasskeyPrompt).
    if (!extensionPage) webView->SetPasskeyRequests(true);
    webView->SetCustomUserAgent(fShared->Read([](const Profile& profile) {
        return FindUserAgentPreset(profile.userAgent).agent;
    }));
}

std::string BrowserWindow::DiscardTab(int64 id)
{
    auto* tab = FindTabByID(id);
    if (!tab) return "The tab is not in this window.";
    if (!tab->deferredURL.empty()) return { };
    // What a page would lose, or the user would notice, stays loaded.
    if (id == fSelected) return "The tab is shown.";
    if (tab->playingAudio || tab->capturingCamera || tab->capturingMicrophone || tab->capturingScreen || tab->keepScreenAwake)
        return "The tab plays sound, captures or keeps the screen on.";
    if (tab->url.empty() || tab->url.starts_with("summit:") || summit::IsExtensionPageURL(tab->url) || !Bookmarkable(tab->url))
        return "The tab does not show a web page.";
    if (tab->closeQueued || tab->closeRequested || tab->devTools.IsValid() || tab->crashView || tab->errorView
        || tab->readerActive || fPictureInPictureTab == id || fFullScreenTab == id)
        return "The tab is busy.";
    auto* webView = new BWebKitView(BRect(0, 0, 319, 199), "web-page", BMessenger(this), B_FOLLOW_ALL, fWebKitContext);
    if (webView->InitCheck() != B_OK) {
        delete webView;
        return "Could not create the unloaded page.";
    }
    if (fAids != AccessibilityAids { }) ApplyAccessibilityAids(webView, fAids);
    webView->SetExplicitMinSize(fPopup ? BSize(160, 100) : BSize(320, 200));
    webView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    if (!IsDefaultZoom(tab->pageZoom)) webView->SetZoomFactor(tab->pageZoom);
    // The new view takes the old one's card; the old page and its process go.
    auto* old = tab->view;
    fCards->AddView(fCards->IndexOfView(old), webView);
    CancelTranslation(*tab);
    DropTabAuthentications(id);
    old->RemoveSelf();
    delete old;
    AttachPageView(*tab, webView, false);
    // It keeps its address, title and icon; its back and forward lists go.
    tab->deferredURL = LoadableURL(tab->url);
    tab->loading = tab->back = tab->forward = false;
    tab->progress = 0;
    tab->processID = 0;
    tab->pageIdentifier = 0;
    tab->processExited = tab->unresponsive = tab->waitedForPage = false;
    tab->readerAvailable = false;
    tab->translation = { };
    tab->certificate.reset();
    tab->connectionCertificate.MakeEmpty();
    tab->softwareRendering = false;
    fWebKitContext->SetBrowserTabDiscarded(webView, true, tab->url.c_str(), tab->title.c_str());
    if (auto* active = ActiveTab()) fCards->SetVisibleItem(fCards->IndexOfView(active->view));
    SyncBrowserWindow();
    RefreshChrome();
    return { };
}

BrowserWindow::Tab* BrowserWindow::LeastRecentlyUsedTab()
{
    Tab* chosen = nullptr;
    for (auto& tab : fTabs) {
        if (tab.id == fSelected || !tab.deferredURL.empty() || tab.playingAudio || tab.url.starts_with("summit:")) continue;
        if (!chosen || tab.lastSelected < chosen->lastSelected) chosen = &tab;
    }
    return chosen;
}

void BrowserWindow::UnloadTabsIfMemoryIsLow()
{
    // Preferences › Browsing can turn this off.
    if (!fShared->Read([](const Profile& profile) { return profile.unloadWhenMemoryIsLow; })) return;
    // Low: less than a tenth of the memory, and at least 512 MiB, is left.
    // SUMMIT_UNLOAD_BELOW_MIB and SUMMIT_UNLOAD_AFTER_S change the limit and
    // how long a tab must have been behind (default five minutes); 0 for
    // either turns this off.
    auto setting = [](const char* name, int64 fallback) {
        const char* value = std::getenv(name);
        return value && *value ? std::strtoll(value, nullptr, 10) : fallback;
    };
    system_info info;
    if (get_system_info(&info) != B_OK) return;
    const int64 total = static_cast<int64>(info.max_pages) * B_PAGE_SIZE;
    const int64 below = setting("SUMMIT_UNLOAD_BELOW_MIB", std::max<int64>(512, total / 10 / (1024 * 1024))) * 1024 * 1024;
    const int64 after = setting("SUMMIT_UNLOAD_AFTER_S", 300) * 1000000;
    if (below <= 0 || after <= 0 || static_cast<int64>(info.free_memory) >= below) return;
    // One tab a check, the longest unseen first, as long as memory stays low.
    std::vector<Tab*> candidates;
    for (auto& tab : fTabs)
        if (tab.id != fSelected && tab.deferredURL.empty() && system_time() - tab.lastSelected >= after) candidates.push_back(&tab);
    std::sort(candidates.begin(), candidates.end(), [](const Tab* a, const Tab* b) { return a->lastSelected < b->lastSelected; });
    for (auto* tab : candidates) {
        const std::string title = tab->title;
        if (DiscardTab(tab->id).empty()) {
            fStatus->SetText(("Unloaded “" + title + "” to free memory; it loads again when you select it.").c_str());
            return;
        }
    }
}

void BrowserWindow::ReplaceTabView(const Tab& tab, const std::string& url)
{
    int32 index = -1;
    for (size_t i = 0; i < fTabs.size(); ++i) if (fTabs[i].id == tab.id) index = static_cast<int32>(i);
    // Views are created on the application thread; the old tab closes once the new one exists.
    BMessage create(kCreateTabOnApp);
    create.AddString("url", url.c_str());
    create.AddBool("select", tab.id == fSelected);
    create.AddMessenger("window", BMessenger(this));
    create.AddInt32("index", index);
    create.AddInt64("replaces", tab.id);
    be_app->PostMessage(&create);
}
#endif
void BrowserWindow::SelectTab(int64 id, bool forClose)
{
#if SUMMIT_MODERN_WEBKIT
    if (fFullScreenTab && fFullScreenTab != id) ExitPageFullScreen();
#endif
#if !SUMMIT_MODERN_WEBKIT
    (void)forClose;
#endif
    for (size_t i = 0; i < fTabs.size(); ++i) {
        if (fTabs[i].id != id) continue;
#if SUMMIT_MODERN_WEBKIT
        if (!forClose) ++fSelectionGeneration;
#endif
        if (fSelected != id) {
            fBackgroundInsert = -1;
#if SUMMIT_MODERN_WEBKIT
            fHoveredLink.clear();
#endif
        }
        fSelected = id;
        // Tabs can be reordered (tabs.move) without moving their views, so
        // find the tab's own card rather than assuming the same position.
        fCards->SetVisibleItem(fCards->IndexOfView(fTabs[i].view));
#if SUMMIT_MODERN_WEBKIT
        // A page whose process ended while the tab was behind loads again;
        // the one that ended in front says what happened.
        if (fTabs[i].processExited && fTabs[i].exitedInBackground && !fRestoringSession) ReloadCrashedTab(fTabs[i]);
        else if (fTabs[i].crashView) fCards->SetVisibleItem(fCards->IndexOfView(fTabs[i].crashView));
#endif
#if !SUMMIT_MODERN_WEBKIT
        SetCurrentWebView(fTabs[i].view);
        fTabs[i].view->WebPage()->ResendNotifications();
#else
        ShowTabStatus(fTabs[i]);
        if (!fRestoringSession && !fTabs[i].deferredURL.empty()) {
            fTabs[i].view->LoadURL(std::exchange(fTabs[i].deferredURL, {}).c_str());
            fWebKitContext->SetBrowserTabDiscarded(fTabs[i].view, false);
        }
        if (!forClose) fTabs[i].lastSelected = system_time();
        if (!forClose && !fRestoringSession && fTabs[i].certificate && !fTabs[i].certificate->asked)
            AskAboutCertificate(fTabs[i]);
        ShowAuthenticationPrompt();
        ShowPasskeyPrompt();
        ProbeReader(fTabs[i]);
        ProbeTranslation(fTabs[i]);
#endif
        fTabs[i].view->MakeFocus();
        fAddress->SetText(DisplayURL(fTabs[i].ShownURL()).c_str());
#if SUMMIT_MODERN_WEBKIT
        if (fTabs[i].errorView) ShowLoadErrorView(fTabs[i]);
#endif
        RefreshInternalPage(fTabs[i]);
#if SUMMIT_MODERN_WEBKIT
        // The dock shows this tab's tools, if they are docked.
        PlaceDockedDeveloperTools();
        SyncBrowserWindow();
        RefreshNoticeBar();
        UpdateKeepScreenAwake();
#endif
        RefreshChrome();
        AnnouncePointer();
        return;
    }
}

// Haiku sends a wheel notch to the view the pointer last moved over. When a
// window opens, or a tab's page view takes the place of another, under a
// pointer that has not moved since, there is no such view (or it is the hidden
// one), and every notch is dropped until the mouse happens to move. Putting
// the pointer back where it already is makes input_server send a mouse-moved
// event, which gives the notches a target without moving anything on screen.
void BrowserWindow::AnnouncePointer()
{
    BPoint where;
    uint32 buttons = 0;
    if (get_mouse(&where, &buttons) != B_OK || buttons || !Frame().Contains(where))
        return;
    set_mouse_position(static_cast<int32>(where.x), static_cast<int32>(where.y));
}
#if SUMMIT_MODERN_WEBKIT
static bool keepScreenAwakeTraced()
{
    static const bool traced = [] {
        const char* value = std::getenv("SUMMIT_KEEP_AWAKE_TRACE");
        return value && !std::strcmp(value, "1");
    }();
    return traced;
}

// A video with sound playing in the current tab, or a page's screen wake
// lock, keeps the screen on (#88): every 20 seconds the pointer is put where
// it already is, which input_server counts as use of the computer, as
// airTime and MediaPlayer do. The engine stops asking when the video pauses
// or ends, the lock is released, or the page is hidden.
void BrowserWindow::UpdateKeepScreenAwake()
{
    const auto* tab = ActiveTab();
    const bool wanted = tab && tab->keepScreenAwake;
    if (wanted == !!fKeepAwakeTimer) return;
    if (wanted) {
        BMessage kick(kKeepScreenAwake);
        fKeepAwakeTimer = std::make_unique<BMessageRunner>(BMessenger(this), &kick, kKeepScreenAwakeInterval);
    } else
        fKeepAwakeTimer.reset();
    if (keepScreenAwakeTraced())
        std::fprintf(stderr, "Summit keep awake: %s (window %llu, tab %lld)\n", wanted ? "on" : "off",
            static_cast<unsigned long long>(fKey), static_cast<long long>(tab ? tab->id : 0));
}

// Only where the user can see the page, and not while a button is held (a
// drag would jump).
void BrowserWindow::KeepScreenAwake()
{
    const auto* tab = ActiveTab();
    if (!tab || !tab->keepScreenAwake || IsHidden() || IsMinimized() || !(Workspaces() & (1u << current_workspace())))
        return;
    BPoint where;
    uint32 buttons = 0;
    if (get_mouse(&where, &buttons) != B_OK || buttons)
        return;
    const bigtime_t idle = idle_time();
    set_mouse_position(static_cast<int32>(where.x), static_cast<int32>(where.y));
    if (keepScreenAwakeTraced())
        std::fprintf(stderr, "Summit keep awake: pointer kept at %.0f,%.0f at %lld (idle for %.1f s)\n", where.x, where.y,
            static_cast<long long>(system_time()), idle / 1000000.0);
}
#endif

void BrowserWindow::CloseTab(int64 id)
{
#if SUMMIT_MODERN_WEBKIT
    if (fClosingWindow) return;
    for (auto& tab : fTabs) {
        if (tab.id != id || tab.closeRequested || tab.closeQueued || tab.closeApproved) continue;
        tab.closeQueued = true;
        ContinueTabCloses();
        return;
    }
}

void BrowserWindow::StartCloseRequest(Tab& tab)
{
    SUMMIT_QUIT_TRACE("asking tab %lld to close", static_cast<long long>(tab.id));
    tab.closeQueued = false;
    tab.closeRequested = true;
    tab.view->RequestClose();
}

void BrowserWindow::ContinueTabCloses()
{
    if (fCloseCommitPending) return;
    if (fClosingWindow) { ContinueWindowClose(); return; }
    // One outstanding decision avoids overlapping beforeunload prompts and
    // lets a cancelled window close reset completed approvals safely.
    if (std::any_of(fTabs.begin(), fTabs.end(), [](const Tab& tab) { return tab.closeRequested; })) return;
    // A page can request window.close while another approval is committing.
    // Keep that completed approval queued instead of requesting it again.
    for (const auto& tab : fTabs) {
        if (!tab.closeApproved) continue;
        CommitTabCloses({ tab.id }, false);
        return;
    }
    for (auto& tab : fTabs) {
        if (!tab.closeQueued) continue;
        StartCloseRequest(tab);
        return;
    }
}

BrowserWindow::CloseFocusState BrowserWindow::CaptureCloseFocus() const
{
    CloseFocusState state;
    state.selected = fSelected;
    if (auto* focus = CurrentFocus()) state.focus = BMessenger(focus);
    state.address = fAddress->Text();
    fAddress->TextView()->GetSelection(&state.selectionStart, &state.selectionEnd);
    state.generation = fSelectionGeneration;
    return state;
}

void BrowserWindow::RestoreCloseFocus(const CloseFocusState& state)
{
    if (std::none_of(fTabs.begin(), fTabs.end(), [&](const Tab& tab) { return tab.id == state.selected; })) return;
    SelectTab(state.selected, true);
    fAddress->SetText(state.address.c_str());
    BLooper* looper = nullptr;
    if (auto* focus = dynamic_cast<BView*>(state.focus.Target(&looper)); focus && looper == this)
        focus->MakeFocus();
    // BTextControl selects all text when its editor regains focus.
    fAddress->TextView()->Select(state.selectionStart, state.selectionEnd);
}

void BrowserWindow::WebKitClosePrompt(const BMessage& message)
{
    BMessenger sender;
    if (message.FindMessenger("view", &sender) != B_OK) return;
    auto* tab = FindTab(sender);
    if (!tab || !tab->closeRequested) return;
    // The bridge sends this before the actual dialog message to this same
    // window looper, so making the card visible precedes native presentation.
    if (tab->id == fSelected) return;
    if (fClosingWindow) {
        if (!fClosePromptTab || !fWindowCloseFocus || fWindowCloseFocus->generation != fSelectionGeneration)
            fWindowCloseFocus = CaptureCloseFocus();
        fClosePromptTab = tab->id;
    } else if (!tab->closeFocus || tab->closeFocus->generation != fSelectionGeneration)
        tab->closeFocus = CaptureCloseFocus();
    SelectTab(tab->id, true);
}

void BrowserWindow::BeginWindowClose()
{
    if (fExtensionMenuCancelled) *fExtensionMenuCancelled = true;
    *fMenusCancelled = true;
    if (fClosingWindow) return;
    if (fCloseCommitPending) { fWindowCloseQueued = true; return; }
    fWindowCloseQueued = false;
    fWindowCloseInvalidated = false;
    SUMMIT_QUIT_TRACE("window close begins (%zu tabs)", fTabs.size());
    // Approval is provisional until every tab agrees. Keep live documents,
    // undo state and the full saved session intact when any tab chooses Stay.
    SaveSession();
    SUMMIT_QUIT_TRACE("session saved");
    fWindowCloseFocus = CaptureCloseFocus();
    fClosePromptTab = 0;
    for (const auto& tab : fTabs) {
        if (tab.closeRequested && tab.closeFocus && tab.closeFocus->generation == fSelectionGeneration && tab.id == fSelected) {
            fWindowCloseFocus = tab.closeFocus;
            fClosePromptTab = tab.id;
            break;
        }
    }
    fClosingWindow = true;
    ContinueWindowClose();
}

void BrowserWindow::ContinueWindowClose()
{
    if (!fClosingWindow || fCloseCommitPending) return;
    if (std::any_of(fTabs.begin(), fTabs.end(), [](const Tab& tab) { return tab.closeRequested; })) return;
    if (fWindowCloseInvalidated) {
        CancelWindowClose();
        fStatus->SetText("Close cancelled");
        return;
    }
    if (auto* selected = ActiveTab(); selected && !selected->closeApproved) {
        StartCloseRequest(*selected);
        return;
    }
    for (auto& tab : fTabs) {
        if (tab.closeApproved) continue;
        StartCloseRequest(tab);
        return;
    }
    // The application asks about unfinished downloads before Summit quits.
    std::vector<int64> identifiers;
    for (const auto& tab : fTabs) identifiers.push_back(tab.id);
    CommitTabCloses(identifiers, true);
}

void BrowserWindow::InvalidateWindowClose()
{
    if (!fClosingWindow) return;
    fWindowCloseInvalidated = true;
    // Keep the pending request owned by the window-close sequence until its
    // definitive reply. Resetting now could turn a late approval into an
    // ordinary tab-close event after cancellation.
    ContinueWindowClose();
}

bool BrowserWindow::PrepareNavigation()
{
    if (fCloseCommitPending && (fCommitWholeWindow
        || std::find(fCommitTabs.begin(), fCommitTabs.end(), fSelected) != fCommitTabs.end()))
        return false;
    ++fSelectionGeneration;
    InvalidateWindowClose();
    return true;
}

void BrowserWindow::CommitTabCloses(const std::vector<int64>& identifiers, bool wholeWindow)
{
    if (fCloseCommitPending) return;
    std::vector<BWebKitView*> views;
    for (auto id : identifiers) {
        auto found = std::find_if(fTabs.begin(), fTabs.end(), [id](const Tab& tab) { return tab.id == id; });
        if (found == fTabs.end()) return;
        views.push_back(found->view);
    }
    fCloseCommitPending = true;
    fCommitWholeWindow = wholeWindow;
    fCommitTabs = identifiers;
    BWebKitView::CommitClose(views, BMessenger(this), ++fCloseCommitIdentifier);
}

void BrowserWindow::WebKitCloseCommitted(const BMessage& message)
{
    uint64 identifier;
    bool closed;
    if (!fCloseCommitPending || message.FindUInt64("identifier", &identifier) != B_OK
        || identifier != fCloseCommitIdentifier || message.FindBool("closed", &closed) != B_OK) return;
    fCloseCommitPending = false;
    auto identifiers = std::exchange(fCommitTabs, { });
    SUMMIT_QUIT_TRACE("close committed: %s", closed ? "closed" : "kept");
    if (fCommitWholeWindow) {
        if (!closed) {
            CancelWindowClose();
            fStatus->SetText("Close cancelled");
            return;
        }
#if SUMMIT_MODERN_WEBKIT
        {
            std::vector<PageRecord> closed;
            for (const auto& tab : fTabs) closed.push_back({tab.url, tab.title});
            RecordClosedSession(closed, closed.size() > 1, 0);
        }
#endif
        for (auto& tab : fTabs) {
            tab.view->RemoveSelf();
            delete tab.view;
        }
        fTabs.clear();
        fSelected = 0;
        --sOpenWindows;
        SUMMIT_QUIT_TRACE("window's views deleted");
        if (fCloseWindowCommand) RespondToCommand(std::exchange(fCloseWindowCommand, 0), B_OK);
        BMessage ready(kWindowReadyToClose);
        ready.AddMessenger("window", BMessenger(this));
        be_app->PostMessage(&ready);
        return;
    }
    for (auto id : identifiers) {
        auto found = std::find_if(fTabs.begin(), fTabs.end(), [id](const Tab& tab) { return tab.id == id; });
        if (found == fTabs.end()) continue;
        auto focus = found->closeFocus;
        const bool restoreFocus = focus && id == fSelected && focus->generation == fSelectionGeneration;
        if (closed)
            FinishCloseTab(id);
        else {
            found->closeApproved = found->closeRequested = found->closeQueued = false;
            found->closeFocus.reset();
            found->view->ResetCloseRequest();
            TabCloseSettled(id, false);
        }
        if (restoreFocus) RestoreCloseFocus(*focus);
    }
    if (!closed) { SaveSession(); fStatus->SetText("Close cancelled"); }
    if (fWindowCloseQueued) BeginWindowClose();
    else ContinueTabCloses();
}

void BrowserWindow::CancelWindowClose()
{
    for (auto& tab : fTabs) {
        tab.closeQueued = tab.closeRequested = tab.closeApproved = false;
        tab.closeFocus.reset();
        tab.view->ResetCloseRequest();
    }
    // Tab closes an extension asked for were absorbed by this window close.
    while (!fCloseCommands.empty()) TabCloseSettled(*fCloseCommands.front().tabs.begin(), false);
    fClosingWindow = false;
    fWindowCloseInvalidated = false;
    fWindowCloseQueued = false;
    fMenusCancelled = std::make_shared<std::atomic<bool>>(false);
    if (fWindowCloseFocus && fClosePromptTab == fSelected && fWindowCloseFocus->generation == fSelectionGeneration)
        RestoreCloseFocus(*fWindowCloseFocus);
    fWindowCloseFocus.reset();
    fClosePromptTab = 0;
    SaveSession();
    if (fCloseWindowCommand) RespondToCommand(std::exchange(fCloseWindowCommand, 0), B_CANCELED, { }, "The window was kept open.");
    BMessage cancelled(kWindowCloseCancelled);
    cancelled.AddMessenger("window", BMessenger(this));
    be_app->PostMessage(&cancelled);
}

void BrowserWindow::WebKitCloseResult(const BMessage& message)
{
    BMessenger sender;
    if (message.FindMessenger("view", &sender) != B_OK) return;
    auto* tab = FindTab(sender);
    if (!tab) return;
    SUMMIT_QUIT_TRACE("tab %lld %s", static_cast<long long>(tab->id), message.what == B_WEBKIT_CLOSE_CANCELLED ? "stays" : "may close");
    if (message.what == B_WEBKIT_CLOSE_CANCELLED) {
        if (!tab->closeRequested) return;
        tab->closeRequested = false;
        if (!fClosingWindow) TabCloseSettled(tab->id, false);
        if (fClosingWindow)
            CancelWindowClose();
        else {
            auto focus = std::exchange(tab->closeFocus, std::nullopt);
            if (focus && tab->id == fSelected && focus->generation == fSelectionGeneration)
                RestoreCloseFocus(*focus);
            SaveSession();
            ContinueTabCloses();
        }
        fStatus->SetText("Close cancelled");
        return;
    }
    tab->closeRequested = tab->closeQueued = false;
    if (fClosingWindow) {
        tab->closeApproved = true;
        ContinueWindowClose();
        return;
    }
    tab->closeApproved = true;
    ContinueTabCloses();
}

void BrowserWindow::FinishCloseTab(int64 id)
{
    if (fFullScreenTab == id) ExitPageFullScreen();
#endif
    for (size_t i = 0; i < fTabs.size(); ++i) {
        if (fTabs[i].id != id) continue;
        const bool selected = id == fSelected;
        auto* webView = fTabs[i].view;
        fClosedTabs.push_back({fTabs[i].url, fTabs[i].title});
        if (fClosedTabs.size() > 25) fClosedTabs.erase(fClosedTabs.begin());
#if SUMMIT_MODERN_WEBKIT
        RecordClosedSession({fClosedTabs.back()}, false, i);
#endif
#if !SUMMIT_MODERN_WEBKIT
        if (selected) SetCurrentWebView(nullptr);
#endif
        webView->RemoveSelf();
#if SUMMIT_MODERN_WEBKIT
        CancelTranslation(fTabs[i]);
        DropTabAuthentications(id);
        CloseDeveloperTools(fTabs[i]);
        RemoveCrashView(fTabs[i]);
        RemoveLoadErrorView(fTabs[i], false);
        if (fPictureInPictureTab == id && fPictureInPicture.IsValid()) fPictureInPicture.SendMessage(B_QUIT_REQUESTED);
        delete webView;
#else
        webView->Shutdown();
#endif
        fTabs.erase(fTabs.begin() + i);
        if (fTabs.empty()) {
            fSelected = 0;
#if SUMMIT_MODERN_WEBKIT
            // Closing the last tab closes the window when others remain open.
            if (!fClosingWindow && CountOpenWindows() > 1) PostMessage(B_QUIT_REQUESTED);
            else if (!fClosingWindow)
#endif
                CreateTab(fPrivate ? NewTabAddress() : HomeAddress());
        }
        else if (selected) SelectTab(fTabs[std::min(i, fTabs.size() - 1)].id);
#if SUMMIT_MODERN_WEBKIT
        SyncBrowserWindow();
#endif
        RefreshChrome();
#if SUMMIT_MODERN_WEBKIT
        TabCloseSettled(id, true);
#endif
        return;
    }
}
void BrowserWindow::RunBookmarklet(const std::string& url)
{
    auto* tab = ActiveTab();
    if (!IsBookmarkletURL(url)) return;
    // Internal pages have a summit: stored URL even though the engine loads
    // generated files. Never give a saved script their browser privileges.
    if (!tab || !Bookmarkable(tab->url) || IsBookmarkletURL(tab->url) || tab->loading) {
        fStatus->SetText("Open a webpage before running this bookmarklet.");
        return;
    }
#if SUMMIT_MODERN_WEBKIT
    if (tab->processExited || tab->errorView || tab->readerActive || !tab->loadError.empty() || !tab->loadGeneration) {
        fStatus->SetText("This page cannot run bookmarklets.");
        return;
    }
    HideSuggestions();
    fAddressTyped.clear();
    fAddressShown = DisplayURL(tab->ShownURL());
    fAddress->SetText(fAddressShown.c_str());
    tab->view->MakeFocus(true);
    tab->view->RunBookmarklet(url.c_str(), BMessenger(this), kBookmarkletQuery, tab->loadGeneration);
    fStatus->SetText("Running bookmarklet…");
#else
    fStatus->SetText("Bookmarklets require Summit's modern engine.");
#endif
}
void BrowserWindow::Navigate(const std::string& text)
{
    const auto keyword = fShared->Read([&](const Profile& profile) { return profile.bookmarks.ResolveKeyword(text); });
    const auto query = keyword ? keyword->url : Trim(text);
    if (IsBookmarkQueryURL(query) && (!keyword || keyword->error.empty())) {
        const auto id = fShared->Read([&](const Profile& profile) {
            const auto* node = keyword ? profile.bookmarks.Find(keyword->bookmarkID) : profile.bookmarks.FindURL(query);
            return node && node->url == query ? node->id : int64_t(0);
        });
        if (id) ShowBookmarkQuery(id);
        else ShowError("Save this smart folder in Bookmarks before opening it.");
        return;
    }
    auto address = keyword ? Address{keyword->url, keyword->error, false} : ResolveAddress(text);
    if (!address.error.empty()) { ShowError(address.error); return; }
    if (keyword && IsBookmarkletURL(address.url)) { RunBookmarklet(address.url); return; }
    if (IsExternalScheme(URLScheme(address.url))) {
        if (auto* tab = ActiveTab()) fProtocolHandlers->Open(address.url, tab->id, tab->url);
        return;
    }
#if SUMMIT_MODERN_WEBKIT
    if (!PrepareNavigation()) return;
#endif
    if (auto* tab = ActiveTab()) {
        fAddress->SetText(DisplayURL(address.url).c_str());
        if (NavigationTraceEnabled()) {
            // Navigations that did not come from the key (a suggestion, the
            // MCP's navigate) are traced from here.
            if (!NavigationTraceStart() || system_time() - NavigationTraceStart() > 2000000)
                NavigationTraceBegin(0, "navigation");
            NavigationTrace("LoadURL %s", LoadableURL(address.url).c_str());
        }
        if (keyword && keyword->post) {
#if SUMMIT_MODERN_WEBKIT
            if (tab->view->LoadForm(address.url.c_str(), keyword->postData.c_str()) != B_OK) {
                ShowError("The bookmark's search request could not be opened.");
                return;
            }
#else
            ShowError("POST bookmark searches require Summit's modern engine.");
            return;
#endif
        } else tab->view->LoadURL(LoadableURL(address.url).c_str());
        NavigationTrace("LoadURL returned");
        tab->pageRevision = fPagesRevision;
    }
}
void BrowserWindow::FrameMoved(BPoint where)
{
    BrowserWindowBase::FrameMoved(where);
    HideSuggestions();
#if SUMMIT_MODERN_WEBKIT
    PlaceDockedDeveloperTools();
    fWebKitContext->SetBrowserWindowFrame(BMessenger(this), Frame());
    ReportExtensionAnchors();
#endif
}

void BrowserWindow::FrameResized(float width, float height)
{
    BrowserWindowBase::FrameResized(width, height);
    HideSuggestions();
#if SUMMIT_MODERN_WEBKIT
    PlaceDockedDeveloperTools();
    fWebKitContext->SetBrowserWindowFrame(BMessenger(this), Frame());
    ReportExtensionAnchors();
#endif
}

void BrowserWindow::AddressModified()
{
    auto* textView = fAddress->TextView();
    if (!textView->IsFocus()) {
        // The page or a tab set the field.
        HideSuggestions();
        fAddressTyped.clear();
        fAddressShown.clear();
        fAutofill = { };
        return;
    }
    const std::string text = fAddress->Text();
    // Changes made here (a completion, a suggestion chosen with the arrow
    // keys) come back as modifications too.
    if (text == fAddressShown) return;
    if (const int32 selected = fSuggestions->Selected(); selected >= 0 && text == fSuggestions->Rows()[selected].fill) return;
#if SUMMIT_MODERN_WEBKIT
    // An address being typed is a navigation soon, most often to another
    // site, which runs in a web process of its own: have one started while
    // the user types rather than after Enter (0.15 s on the laptop).
    if (auto* tab = ActiveTab(); tab && tab->view && system_time() - fProcessPrewarmedAt > 1000000) {
        fProcessPrewarmedAt = system_time();
        tab->view->PrewarmProcess();
    }
#endif
    int32 start = 0, end = 0;
    textView->GetSelection(&start, &end);
    // Only typing forward at the end completes; deleting (Backspace, which
    // leaves a prefix of what was there) keeps what was typed, as in Firefox.
    // Typing over a selected address is typing forward too.
    const bool deleted = text.size() <= fAddressTyped.size() && fAddressTyped.compare(0, text.size(), text) == 0;
    const bool forward = start == end && end == static_cast<int32>(text.size()) && !deleted;
    fAddressTyped = text;
    fAddressShown = text;
    fAutofill = { };
    const auto keyword = fShared->Read([&](const Profile& profile) { return profile.bookmarks.ResolveKeyword(text); });
    if (forward && !keyword) {
        const int64 now = std::time(nullptr);
        fAutofill = fShared->Read([&](const Profile& profile) {
            return AutofillAddress(profile.history, profile.bookmarks.Pages(), text, now);
        });
        if (!fAutofill.Empty()) {
            fAddressShown = fAutofill.text;
            textView->Insert(text.size(), fAutofill.text.c_str() + text.size(), fAutofill.text.size() - text.size());
            textView->Select(text.size(), fAutofill.text.size());
        }
    }
    ShowSuggestions(text);
}

void BrowserWindow::ShowSuggestions(const std::string& typed)
{
    if (Trim(typed).empty() || !fAddress->TextView()->IsFocus()) {
        HideSuggestions();
        return;
    }
    const int64 now = std::time(nullptr);
    const auto pages = fShared->Read([&](const Profile& profile) {
        return SuggestPages(profile.history, profile.bookmarks.Pages(), typed, now, 8);
    });
    std::vector<SuggestionRow> rows;
    // The first row is what Enter does with the text as it stands.
    const auto address = ResolveAddress(typed);
    const auto keyword = fShared->Read([&](const Profile& profile) { return profile.bookmarks.ResolveKeyword(typed); });
    if (keyword && !keyword->error.empty()) { HideSuggestions(); return; }
    SuggestionRow search;
    search.kind = SuggestionRow::Kind::Search;
    search.title = Trim(typed);
    search.detail = std::string("Search with ") + CurrentSearchEngine().name;
    search.url = SearchURL(typed);
    search.fill = typed;
    if (keyword) {
        SuggestionRow row;
        row.kind = SuggestionRow::Kind::Bookmark;
        row.title = keyword->title.empty() ? ShortAddress(keyword->url) : keyword->title;
        row.detail = "Bookmark shortcut · " + ShortAddress(keyword->url);
        // Keep the shortcut text so selection/click also submits its POST data.
        row.url = typed;
        row.fill = typed;
        rows.push_back(std::move(row));
    } else if (!fAutofill.Empty()) {
        SuggestionRow visit;
        visit.kind = SuggestionRow::Kind::Visit;
        visit.title = ShortAddress(fAutofill.url);
        visit.detail = "Visit";
        visit.url = fAutofill.url;
        visit.fill = fAutofill.text;
        rows.push_back(std::move(visit));
    } else if (address.error.empty() && !address.search) {
        SuggestionRow visit;
        visit.kind = SuggestionRow::Kind::Visit;
        visit.title = ShortAddress(address.url);
        visit.detail = "Visit";
        visit.url = address.url;
        visit.fill = typed;
        rows.push_back(std::move(visit));
    } else if (address.search) {
        rows.push_back(search);
    }
    const std::string first = rows.empty() ? std::string() : rows.front().url;
    for (const auto& page : pages) {
        if (page.url == first || rows.size() >= 8) continue;
        SuggestionRow row;
        row.kind = page.kind == Suggestion::Kind::Bookmark ? SuggestionRow::Kind::Bookmark : SuggestionRow::Kind::History;
        row.title = page.title.empty() ? ShortAddress(page.url) : page.title;
        row.detail = ShortAddress(page.url);
        row.url = page.url;
        row.fill = DisplayURL(page.url);
        if (const BBitmap* icon = fFavicons->Icon(page.url)) row.icon = std::make_shared<BBitmap>(icon);
        rows.push_back(std::move(row));
    }
    // A search stays one key away when the text reads as an address.
    if (!address.search && search.url != first && !Trim(typed).empty()) rows.push_back(search);
    // Enter goes where the first row does: a page completed from history or
    // a search. Have the connection ready by then.
    if (!rows.empty() && (!fAutofill.Empty() || address.search)) PreconnectTo(rows.front().url);
    // Only a guess at what Enter does, and nothing else: not worth a list.
    if (rows.size() == 1 && rows.front().kind == SuggestionRow::Kind::Visit && fAutofill.Empty()) {
        HideSuggestions();
        return;
    }
    fSuggestions->Show(fAddress->ConvertToScreen(fAddress->Bounds()), std::move(rows));
}

void BrowserWindow::HideSuggestions()
{
    if (fSuggestions) fSuggestions->Hide();
}

void BrowserWindow::PreconnectTo(const std::string& url)
{
#if SUMMIT_MODERN_WEBKIT
    // Private windows tell no server what is being typed.
    if (fPrivate || url.rfind("https://", 0) != 0) return;
    auto* tab = ActiveTab();
    if (!tab || !tab->view) return;
    const std::string origin = url.substr(0, url.find_first_of("/?#", 8));
    // An idle connection stays open for about two minutes; asking again
    // while typing on costs a message for nothing.
    const bigtime_t now = system_time();
    if (fPreconnected.size() > 64) fPreconnected.clear();
    bigtime_t& last = fPreconnected[origin];
    if (last && now - last < 10000000) return;
    last = now;
    tab->view->Preconnect((origin + "/").c_str());
#else
    (void)url;
#endif
}

std::string BrowserWindow::AddressTarget() const
{
    const std::string text = fAddress->Text();
    if (const int32 selected = fSuggestions->Selected(); fSuggestions->IsShowing() && selected >= 0
        && text == fSuggestions->Rows()[selected].fill)
        return fSuggestions->Rows()[selected].url;
    if (!fAutofill.Empty() && text == fAutofill.text) return fAutofill.url;
    return text;
}

bool BrowserWindow::AddressKey(char key)
{
    auto* textView = fAddress->TextView();
    // Shows text in the field without it counting as typed.
    auto show = [&](const std::string& text, int32 selectFrom) {
        textView->SetText(text.c_str());
        textView->Select(std::min<int32>(selectFrom, text.size()), text.size());
        textView->ScrollToSelection();
    };
    switch (key) {
    case B_DOWN_ARROW:
    case B_UP_ARROW: {
        if (!fSuggestions->IsShowing()) {
            if (key != B_DOWN_ARROW || fAddressTyped.empty()) return false;
            ShowSuggestions(fAddressTyped);
            return fSuggestions->IsShowing();
        }
        // The typed text and the rows form a cycle.
        const int32 count = fSuggestions->Rows().size();
        int32 position = fSuggestions->Selected() + 1;
        position = key == B_DOWN_ARROW ? (position + 1) % (count + 1) : (position + count) % (count + 1);
        fSuggestions->Select(position - 1);
        if (position == 0) show(fAddressShown, fAddressTyped.size());
        else {
            show(fSuggestions->Rows()[position - 1].fill, INT32_MAX);
            PreconnectTo(fSuggestions->Rows()[position - 1].url);
        }
        return true;
    }
    case B_ESCAPE:
        if (fSuggestions->IsShowing()) {
            const bool rowSelected = fSuggestions->Selected() >= 0;
            HideSuggestions();
            if (rowSelected) show(fAddressShown, fAddressTyped.size());
            return true;
        }
        // A second Escape puts the page's address back.
        if (auto* tab = ActiveTab()) {
            fAutofill = { };
            fAddressTyped = fAddressShown = DisplayURL(tab->url);
            show(fAddressShown, 0);
            return true;
        }
        return false;
    case B_TAB:
        HideSuggestions();
        return false;
    }
    return false;
}
#if SUMMIT_MODERN_WEBKIT
void BrowserWindow::SyncBrowserWindow()
{
    std::vector<BWebKitView*> views;
    views.reserve(fTabs.size());
    for (auto& tab : fTabs)
        views.push_back(tab.view);
    auto* active = ActiveTab();
    fWebKitContext->SetBrowserWindowTabs(BMessenger(this), views, active ? active->view : nullptr, IsActive());
    // Extensions' Window.left/top/width/height.
    if (!views.empty()) fWebKitContext->SetBrowserWindowFrame(BMessenger(this), Frame());
    // Registration is dispatched to the application looper. A queued window
    // invalidation also works during construction on the application thread.
    if (fExtensionsEnabled) PostMessage(B_WEBKIT_EXTENSION_ACTIONS_CHANGED);
}

void BrowserWindow::RespondToCommand(uint64 identifier, status_t status, const std::vector<BWebKitView*>& views, const char* error)
{
    fWebKitContext->RespondToBrowserCommand(identifier, status, views, error);
}

bool BrowserWindow::PrepareTabNavigation(const Tab& tab)
{
    // A tab whose close is being committed no longer accepts navigation.
    if (fCloseCommitPending && (fCommitWholeWindow
        || std::find(fCommitTabs.begin(), fCommitTabs.end(), tab.id) != fCommitTabs.end()))
        return false;
    if (tab.id == fSelected) ++fSelectionGeneration;
    InvalidateWindowClose();
    return true;
}

void BrowserWindow::TabOpenedForCommand(uint64 command, Tab* tab, const std::string& error)
{
    auto found = std::find_if(fOpenCommands.begin(), fOpenCommands.end(),
        [command](const OpenCommand& pending) { return pending.identifier == command; });
    if (found == fOpenCommands.end()) return; // The engine stopped waiting.
    if (tab) {
        found->views.push_back(tab->view);
        tab->openInReader = found->reader;
        if (found->window) fExtensionWindowTabs[found->extension].push_back(tab->id);
    } else if (found->error.empty()) found->error = error;
    if (found->pending && --found->pending) return;
    auto finished = std::move(*found);
    fOpenCommands.erase(found);
    if (finished.views.empty()) RespondToCommand(finished.identifier, B_ERROR, { }, finished.error.c_str());
    else RespondToCommand(finished.identifier, B_OK, finished.views);
}

void BrowserWindow::TabCloseSettled(int64 id, bool closed)
{
    for (auto& entry : fExtensionWindowTabs)
        if (closed) std::erase(entry.second, id);
    for (auto command = fCloseCommands.begin(); command != fCloseCommands.end();) {
        if (!command->tabs.contains(id)) { ++command; continue; }
        if (!closed) {
            // One kept tab fails the whole request, like a rejected tabs.remove().
            const auto identifier = command->identifier;
            command = fCloseCommands.erase(command);
            RespondToCommand(identifier, B_CANCELED, { }, "The tab was kept open.");
            continue;
        }
        command->tabs.erase(id);
        if (!command->tabs.empty()) { ++command; continue; }
        const auto identifier = command->identifier;
        command = fCloseCommands.erase(command);
        RespondToCommand(identifier, B_OK);
    }
}

void BrowserWindow::CloseTabsForCommand(uint64 identifier, const std::vector<int64>& tabs)
{
    if (fClosingWindow) { RespondToCommand(identifier, B_BUSY, { }, "The window is closing."); return; }
    CloseCommand command;
    command.identifier = identifier;
    for (auto id : tabs)
        if (std::any_of(fTabs.begin(), fTabs.end(), [id](const Tab& tab) { return tab.id == id; })) command.tabs.insert(id);
    // Tabs that are already gone count as closed.
    if (command.tabs.empty()) { RespondToCommand(identifier, B_OK); return; }
    const auto pending = command.tabs;
    fCloseCommands.push_back(std::move(command));
    // The usual asynchronous handshake runs beforeunload one tab at a time and
    // never blocks this looper; its outcome arrives through TabCloseSettled.
    for (auto id : pending) CloseTab(id);
}

void BrowserWindow::ApplyCommandFrame(const BMessage& message)
{
    // left, top, width, height in screen pixels; NaN or absent keeps a member.
    BRect frame = Frame();
    const double left = message.GetDouble("left", NAN), top = message.GetDouble("top", NAN);
    const double width = message.GetDouble("width", NAN), height = message.GetDouble("height", NAN);
    if (!std::isnan(left) || !std::isnan(top))
        MoveTo(std::isnan(left) ? frame.left : static_cast<float>(left), std::isnan(top) ? frame.top : static_cast<float>(top));
    if (!std::isnan(width) || !std::isnan(height)) {
        float minWidth, maxWidth, minHeight, maxHeight;
        GetSizeLimits(&minWidth, &maxWidth, &minHeight, &maxHeight);
        const float w = std::isnan(width) ? frame.Width() : std::clamp(static_cast<float>(width) - 1, minWidth, maxWidth);
        const float h = std::isnan(height) ? frame.Height() : std::clamp(static_cast<float>(height) - 1, minHeight, maxHeight);
        ResizeTo(w, h);
    }
    fWebKitContext->SetBrowserWindowFrame(BMessenger(this), Frame());
}

void BrowserWindow::OpenTabsForCommand(const BMessage& message, uint64 identifier, bool window)
{
    OpenCommand command;
    command.identifier = identifier;
    command.window = window;
    command.extension = message.GetString("extension_identifier", "");
    command.reader = !window && message.GetBool("reader", false);
    std::vector<std::string> urls;
    const char* url = nullptr;
    for (int32 i = 0; message.FindString("url", i, &url) == B_OK; ++i) urls.push_back(url);
    bool select = message.GetBool(window ? "focused" : "active", true);
    if (window) {
        // windows.create({left, top, width, height}).
        ApplyCommandFrame(message);
        // The application made this window for windows.create() (empty): its
        // pages open as tabs here, and the existing tabs it names are ours.
        BMessenger view;
        for (int32 i = 0; message.FindMessenger("view", i, &view) == B_OK; ++i) {
            auto* tab = FindTab(view);
            if (!tab) continue;
            command.views.push_back(tab->view);
            fExtensionWindowTabs[command.extension].push_back(tab->id);
            if (select) { SelectTab(tab->id); select = false; }
        }
        if (urls.empty() && command.views.empty()) urls.push_back(NewTabAddress());
        if (urls.empty()) { RespondToCommand(identifier, B_OK, command.views); return; }
    } else if (urls.empty()) urls.push_back(NewTabAddress());
    const int32 index = window ? -1 : message.GetInt32("index", -1);
    command.pending = urls.size();
    fOpenCommands.push_back(std::move(command));
    for (const auto& address : urls) {
        // Only the first page of a new "window" takes the selection.
        CreateTab(address, select, nullptr, index, identifier);
        select = false;
    }
}

void BrowserWindow::BrowserCommand(const BMessage& message)
{
    uint64 identifier = 0;
    uint32 command = 0;
    if (message.FindUInt64("identifier", &identifier) != B_OK || !identifier) return;
    if (message.what == B_WEBKIT_BROWSER_COMMAND_CANCELLED) {
        std::erase_if(fOpenCommands, [identifier](const OpenCommand& pending) { return pending.identifier == identifier; });
        std::erase_if(fCloseCommands, [identifier](const CloseCommand& pending) { return pending.identifier == identifier; });
        return;
    }
    if (message.FindUInt32("command", &command) != B_OK) { RespondToCommand(identifier, B_BAD_VALUE); return; }
    BMessenger view;
    auto* tab = message.FindMessenger("view", &view) == B_OK ? FindTab(view) : nullptr;
    switch (command) {
        case B_WEBKIT_BROWSER_OPEN_TAB: OpenTabsForCommand(message, identifier, false); return;
        case B_WEBKIT_BROWSER_OPEN_WINDOW: OpenTabsForCommand(message, identifier, true); return;
        case B_WEBKIT_BROWSER_CLOSE_TABS: {
            std::vector<int64> tabs;
            for (int32 i = 0; message.FindMessenger("view", i, &view) == B_OK; ++i)
                if (auto* closing = FindTab(view)) tabs.push_back(closing->id);
            CloseTabsForCommand(identifier, tabs);
            return;
        }
        case B_WEBKIT_BROWSER_CLOSE_WINDOW: {
            if (CountOpenWindows() > 1) {
                // Answered once the window has closed, or kept open by a page.
                if (fClosingWindow || fCloseWindowCommand) { RespondToCommand(identifier, B_BUSY, { }, "The window is closing."); return; }
                fCloseWindowCommand = identifier;
                PostMessage(B_QUIT_REQUESTED);
                return;
            }
            // Never let an extension close the last browser window. It may
            // close the tabs this window opened for its windows.create() calls.
            const auto tabs = fExtensionWindowTabs[message.GetString("extension_identifier", "")];
            if (tabs.empty()) RespondToCommand(identifier, B_NOT_ALLOWED, { }, "Extensions cannot close the last browser window.");
            else CloseTabsForCommand(identifier, tabs);
            return;
        }
        case B_WEBKIT_BROWSER_FOCUS_WINDOW:
            if (IsMinimized()) Minimize(false);
            Activate(true);
            RespondToCommand(identifier, B_OK);
            return;
        case B_WEBKIT_BROWSER_SET_WINDOW_FRAME:
            // windows.update({left, top, width, height}).
            if (IsMinimized()) Minimize(false);
            ApplyCommandFrame(message);
            RespondToCommand(identifier, B_OK);
            return;
        case B_WEBKIT_BROWSER_HISTORY: HistoryCommand(message, identifier); return;
        case B_WEBKIT_BROWSER_BOOKMARKS: BookmarksCommand(message, identifier); return;
        case B_WEBKIT_BROWSER_SEARCH: SearchCommand(message, identifier, tab); return;
        case B_WEBKIT_BROWSER_BROWSING_DATA: BrowsingDataCommand(message, identifier); return;
        case B_WEBKIT_BROWSER_SESSIONS: SessionsCommand(message, identifier); return;
        case B_WEBKIT_BROWSER_SIDEBAR: SidebarCommand(message, identifier); return;
        case B_WEBKIT_BROWSER_DETECT_LANGUAGE: {
            // i18n.detectLanguage, with the detector page translation uses.
            const auto detected = DetectTextLanguages(message.GetString("text", ""));
            auto languages = nlohmann::json::array();
            for (const auto& [language, percentage] : detected.languages)
                languages.push_back({{"language", language}, {"percentage", percentage}});
            const nlohmann::json answer = {{"isReliable", detected.reliable}, {"languages", languages}};
            fWebKitContext->RespondToBrowserCommandWithData(identifier, B_OK, answer.dump().c_str());
            return;
        }
        case B_WEBKIT_BROWSER_SET_READER: {
            // tabs.toggleReaderMode: Reader view starts from the shown tab.
            const bool active = message.GetBool("active", true);
            if (!tab || tab->id != fSelected) { RespondToCommand(identifier, B_NOT_ALLOWED, { }, "Reader view can only be switched for the shown tab."); return; }
            if (active != tab->readerActive) {
                if (active && !tab->readerAvailable) { RespondToCommand(identifier, B_NOT_ALLOWED, { }, "The page has no article for Reader view."); return; }
                ToggleReader();
            }
            RespondToCommand(identifier, B_OK);
            return;
        }
        case B_WEBKIT_BROWSER_DISCARD_TAB: {
            // tabs.discard: the named tab, else the least recently used one.
            Tab* target = message.HasMessenger("view") ? tab : LeastRecentlyUsedTab();
            if (!target) { RespondToCommand(identifier, B_NOT_ALLOWED, { }, "There is no tab to discard."); return; }
            const int64 id = target->id;
            if (const auto error = DiscardTab(id); !error.empty()) { RespondToCommand(identifier, B_NOT_ALLOWED, { }, error.c_str()); return; }
            target = FindTabByID(id);
            RespondToCommand(identifier, B_OK, {target->view});
            return;
        }
        case B_WEBKIT_BROWSER_SET_WINDOW_STATE: {
            const std::string state = message.GetString("state", "");
            if (state == "minimized") Minimize(true);
            else if (state == "normal") { if (IsMinimized()) Minimize(false); }
            else { RespondToCommand(identifier, B_NOT_SUPPORTED); return; }
            RespondToCommand(identifier, B_OK);
            return;
        }
        default: break;
    }
    if (!tab) { RespondToCommand(identifier, B_ENTRY_NOT_FOUND, { }, "The tab is not open in this window."); return; }
    switch (command) {
        case B_WEBKIT_BROWSER_SET_PINNED:
            // tabs.update({pinned}): as the tab menu's Pin Tab.
            SetTabPinned(tab->id, message.GetBool("pinned", false));
            RespondToCommand(identifier, B_OK);
            return;
        case B_WEBKIT_BROWSER_SET_ZOOM: {
            // tabs.setZoom(): 0 is the default zoom.
            const double zoom = message.GetDouble("zoom", -1);
            if (!(zoom == 0 || (zoom >= 0.25 && zoom <= 5))) { RespondToCommand(identifier, B_BAD_VALUE, { }, "The zoom is out of range."); return; }
            SetTabZoom(*tab, zoom ? zoom : 1.0);
            RespondToCommand(identifier, B_OK);
            return;
        }
        case B_WEBKIT_BROWSER_ACTIVATE_TAB:
            if (fClosingWindow) { RespondToCommand(identifier, B_BUSY, { }, "The window is closing."); return; }
            SelectTab(tab->id);
            RespondToCommand(identifier, B_OK);
            return;
        case B_WEBKIT_BROWSER_MOVE_TAB: {
            if (fClosingWindow) { RespondToCommand(identifier, B_BUSY, { }, "The window is closing."); return; }
            const int32 index = message.GetInt32("index", -1);
            // Into another window: its page view goes there (as dragging does).
            if (BMessenger target; message.FindMessenger("target_window", &target) == B_OK && target != BMessenger(this)) {
                DetachTab(tab->id, target, index, identifier);
                return;
            }
            // Only the tab list changes order; each page view stays in its card.
            auto moving = std::find_if(fTabs.begin(), fTabs.end(), [&](const Tab& candidate) { return candidate.id == tab->id; });
            Tab moved = std::move(*moving);
            fTabs.erase(moving);
            const size_t position = index < 0 ? fTabs.size() : std::min(static_cast<size_t>(index), fTabs.size());
            fTabs.insert(fTabs.begin() + position, std::move(moved));
            SyncBrowserWindow();
            RefreshChrome();
            SaveSession();
            RespondToCommand(identifier, B_OK);
            return;
        }
        case B_WEBKIT_BROWSER_NAVIGATE_TAB: {
            const auto address = ResolveAddress(message.GetString("url", ""));
            if (!address.error.empty()) { RespondToCommand(identifier, B_BAD_VALUE, { }, address.error.c_str()); return; }
            // Extension pages need the privileged view that CreateTab makes;
            // an ordinary web view cannot become one.
            const bool extensionPage = summit::IsExtensionPageURL(address.url);
            const bool extensionTab = summit::IsExtensionPageURL(tab->url);
            if (extensionPage != extensionTab || (extensionPage
                && address.url.substr(0, address.url.find('/', 19)) != tab->url.substr(0, tab->url.find('/', 19)))) {
                // The page needs the other kind of view: replace the tab's view in place.
                ReplaceTabView(*tab, address.url);
                RespondToCommand(identifier, B_OK);
                return;
            }
            if (!PrepareTabNavigation(*tab)) { RespondToCommand(identifier, B_BUSY, { }, "The tab is closing."); return; }
            if (tab->id == fSelected) fAddress->SetText(DisplayURL(address.url).c_str());
            tab->view->LoadURL(LoadableURL(address.url).c_str());
            tab->pageRevision = fPagesRevision;
            RespondToCommand(identifier, B_OK);
            return;
        }
        case B_WEBKIT_BROWSER_RELOAD_TAB:
            if (!PrepareTabNavigation(*tab)) { RespondToCommand(identifier, B_BUSY, { }, "The tab is closing."); return; }
            if (message.GetBool("bypass_cache", false)) tab->view->ReloadFromOrigin();
            else tab->view->Reload();
            RespondToCommand(identifier, B_OK);
            return;
        case B_WEBKIT_BROWSER_GO_BACK: case B_WEBKIT_BROWSER_GO_FORWARD: {
            const bool back = command == B_WEBKIT_BROWSER_GO_BACK;
            if (!(back ? tab->back : tab->forward)) {
                RespondToCommand(identifier, B_ERROR, { }, back ? "There is no page to go back to." : "There is no page to go forward to.");
                return;
            }
            if (!PrepareTabNavigation(*tab)) { RespondToCommand(identifier, B_BUSY, { }, "The tab is closing."); return; }
            if (back) tab->view->GoBack(); else tab->view->GoForward();
            RespondToCommand(identifier, B_OK);
            return;
        }
        default:
            RespondToCommand(identifier, B_NOT_SUPPORTED);
    }
}

void BrowserWindow::RefreshExtensionActions()
{
    if (!fExtensionsEnabled) return;
    fExtensionActionSnapshot = 0;
    for (auto* button : fExtensionActionButtons) button->SetEnabled(false);
    if (fExtensionActionsOverflow) fExtensionActionsOverflow->SetEnabled(false);
    const auto status = fWebKitContext->GetExtensionActions(BMessenger(this), BMessenger(this), ++fExtensionActionRequest);
    if (status != B_OK) {
        BMessage reply(B_WEBKIT_EXTENSION_ACTIONS);
        reply.AddUInt64("identifier", fExtensionActionRequest);
        reply.AddInt32("error", status);
        ExtensionActionsReceived(reply);
    }
}

void BrowserWindow::ExtensionActionsReceived(const BMessage& message)
{
    uint64 request = 0;
    if (!fExtensionsEnabled || message.FindUInt64("identifier", &request) != B_OK || request != fExtensionActionRequest) return;
    std::vector<BMessage> actions;
    if (message.GetInt32("error", B_ERROR) == B_OK) {
        BMessage action;
        for (int32 index = 0; message.FindMessage("action", index, &action) == B_OK; ++index) {
            const char* identity = nullptr;
            if (action.FindString("extension_identifier", &identity) != B_OK || !*identity
                || !action.GetUInt64("load_identifier", 0) || !action.GetUInt64("page_identifier", 0)) continue;
            actions.push_back(action);
        }
    }
    // Pinned actions come first and get buttons; unpinned ones and those
    // beyond four are in the menu of the toolbar's extensions button.
    const auto unpinned = fShared->Read([](const Profile& profile) { return profile.unpinnedExtensions; });
    const auto firstUnpinned = std::stable_partition(actions.begin(), actions.end(), [&](const BMessage& action) {
        return !unpinned.contains(action.GetString("extension_identifier", ""));
    });
    const auto visible = std::min(size_t(4), size_t(firstUnpinned - actions.begin()));
    bool sameState = actions.size() == fExtensionActionState.size();
    for (size_t i = 0; sameState && i < actions.size(); ++i)
        sameState = actions[i].HasSameData(fExtensionActionState[i]);
    if (!sameState || !fExtensionActionRevision) {
        ++fExtensionActionRevision;
        if (fExtensionMenuCancelled) *fExtensionMenuCancelled = true;
    }
    bool sameButtons = visible == fExtensionActionButtons.size() && bool(fExtensionActionsOverflow) == (actions.size() > visible);
    for (size_t i = 0; sameButtons && i < visible; ++i) {
        const char* identity = "";
        actions[i].FindString("extension_identifier", &identity);
        sameButtons = std::string(fExtensionActionButtons[i]->Name()) == std::string("extension-action-") + identity;
    }
    if (!sameButtons) {
        while (auto* child = fExtensionActions->ChildAt(0)) { child->RemoveSelf(); delete child; }
        fExtensionActionButtons.clear();
        fExtensionActionsOverflow = nullptr;
        for (size_t i = 0; i < visible; ++i) {
            const char* identity = "";
            actions[i].FindString("extension_identifier", &identity);
            auto* button = new ExtensionActionButton(identity);
            BLayoutBuilder::Group<>(fExtensionActions).Add(button);
            button->SetTarget(this);
            fExtensionActionButtons.push_back(button);
        }
        if (actions.size() > visible) {
            fExtensionActionsOverflow = new ToolButton("extension-actions-more", "More extension actions", Icon::More, kShowExtensionActions);
            BLayoutBuilder::Group<>(fExtensionActions).Add(fExtensionActionsOverflow);
            fExtensionActionsOverflow->SetTarget(this);
        }
    }
    fExtensionActionState = std::move(actions);
    fExtensionActionSnapshot = fExtensionActionRevision;
    for (size_t i = 0; i < visible; ++i) fExtensionActionButtons[i]->SetAction(fExtensionActionState[i], fExtensionActionSnapshot);
    if (fExtensionActionsOverflow) fExtensionActionsOverflow->SetEnabled(true);
    if (fExtensionActionState.empty()) {
        if (!fExtensionActions->IsHidden(fExtensionActions)) fExtensionActions->Hide();
    } else if (fExtensionActions->IsHidden(fExtensionActions)) fExtensionActions->Show();
    // A popup the extension opens itself (openPopup) opens under its button too.
    ReportExtensionAnchors();
}

void BrowserWindow::ActivateExtensionAction(const BMessage& message)
{
    if (!fExtensionsEnabled || !fExtensionActionSnapshot || message.GetUInt64("snapshot", 0) != fExtensionActionSnapshot
        || fClosingWindow || fCloseCommitPending || !IsActive()) return;
    const char* identity = nullptr;
    if (message.FindString("extension_identifier", &identity) != B_OK) return;
    const auto load = message.GetUInt64("load_identifier", 0);
    const auto page = message.GetUInt64("page_identifier", 0);
    SUMMIT_EXTENSION_TIMING("action button of %s clicked", identity);
    ReportExtensionAnchors();
    const auto status = fWebKitContext->ActivateExtensionAction(identity, BMessenger(this), load, page,
        BMessenger(this), ++fExtensionActionInvocation);
    if (status != B_OK) {
        fStatus->SetText(("Could not activate extension: " + std::string(std::strerror(status))).c_str());
        RefreshExtensionActions();
    }
}

bool BrowserWindow::RunExtensionShortcut(const BMessage& message)
{
    if (!fExtensionsEnabled || fClosingWindow || fCloseCommitPending) return false;
    const auto binding = ExtensionShortcuts::Default().Find(message);
    if (!binding) return false;
    // A menu shortcut of Summit's own goes first (only Command ones exist).
    const uint32 modifiers = message.GetInt32("modifiers", 0);
    const char* bytes = message.GetString("bytes", "");
    if ((modifiers & B_COMMAND_KEY) && bytes[0] && HasShortcut(static_cast<unsigned char>(bytes[0]), modifiers)) return false;
    // Held keys run it once.
    if (message.GetInt32("be:key_repeat", 0) > 0) return true;
    SUMMIT_EXTENSION_TIMING("shortcut %s of %s pressed", binding->command.c_str(), binding->extension.c_str());
    const auto status = fWebKitContext->PerformExtensionCommand(binding->extension.c_str(), binding->command.c_str(), BMessenger(this));
    if (status != B_OK) fStatus->SetText(("Could not run the extension's shortcut: " + std::string(std::strerror(status))).c_str());
    return true;
}

void BrowserWindow::DispatchMessage(BMessage* message, BHandler* handler)
{
    summit::StallScope scope("window", message ? message->what : 0);
#if SUMMIT_MODERN_WEBKIT
    if (message && message->what == B_KEY_DOWN && RunExtensionShortcut(*message)) return;
    if (message && message->what == B_MOUSE_DOWN) {
        fLastMouseDown = system_time();
        fMouseButtons = message->GetInt32("buttons", 0);
    } else if (message && message->what == B_MOUSE_UP) fMouseButtons = message->GetInt32("buttons", 0);
    else if (message && message->what == B_MOUSE_MOVED && fMouseButtons && !message->GetInt32("buttons", 0)
        && system_time() - fLastMouseDown < 1000000) {
        // A window that a click activates hears of it with a mouse-moved
        // message that says no button is down, before the click's mouse-up:
        // the page took the mouse-up for another button's and fired no click
        // (a link, or a full-screen button, needed a second click, #95). The
        // button is down until its mouse-up.
        message->ReplaceInt32("buttons", fMouseButtons);
    }
    if (message && message->what == B_KEY_DOWN && message->GetString("bytes", "")[0] == B_FUNCTION_KEY
        && message->GetInt32("key", 0) == B_F7_KEY
        && !(message->GetInt32("modifiers", 0) & (B_SHIFT_KEY | B_COMMAND_KEY | B_CONTROL_KEY | B_OPTION_KEY))) {
        PostMessage(kToggleCaretBrowsing);
        return;
    }
    if (fFullScreenTab && message && message->what == B_KEY_DOWN
        && message->GetString("bytes", "")[0] == B_ESCAPE) {
        ExitPageFullScreen();
        return;
    }
#endif
    // SUMMIT_INPUT_LAG_TRACE=1: once a second, how late pointer events reach
    // this window after input_server stamped them, which is the time they
    // spent in input_server and app_server (whose event thread also moves
    // the cursor). Summit cannot make them earlier; it can only report them.
    static const bool traceLag = [] {
        const char* value = std::getenv("SUMMIT_INPUT_LAG_TRACE");
        return value && !std::strcmp(value, "1");
    }();
    if (traceLag && message && message->what == B_MOUSE_MOVED) {
        // Also the gaps between consecutive moves as input_server stamped
        // them. A moving mouse reports every 8-16 ms; a gap of 40-400 ms is
        // either the hand pausing or the device's link stalling. They are
        // told apart by the motion around the gap: a hand that pauses slows
        // down first and starts again slowly, a stalled link loses the
        // reports of a pointer in full motion, so the pointer was moving
        // fast before the gap and jumps across it ("in motion"), and reports
        // the link held back arrive together right after it ("bunched").
        static bigtime_t bucketStart = 0, sum = 0, worst = 0, lastWhen = 0, worstGap = 0, worstMotionGap = 0;
        static int count = 0, over30 = 0, over100 = 0, stalls = 0, motionStalls = 0, bunched = 0;
        static float lastX = 0, lastY = 0, lastStep = 0, stepBefore = 0, worstJump = 0;
        static bool afterGap = false;
        const bigtime_t now = system_time();
        bigtime_t when = 0;
        BPoint where;
        if (message->FindInt64("when", &when) == B_OK && when > 0 && when <= now
            && message->FindPoint("screen_where", &where) == B_OK) {
            const bigtime_t lag = now - when;
            if (!bucketStart) bucketStart = now;
            ++count; sum += lag; worst = std::max(worst, lag);
            if (lag > 30000) ++over30;
            if (lag > 100000) ++over100;
            const float step = lastWhen ? std::hypot(where.x - lastX, where.y - lastY) : 0;
            if (lastWhen) {
                const bigtime_t gap = when - lastWhen;
                if (gap > 40000 && gap <= 400000) {
                    ++stalls; worstGap = std::max(worstGap, gap);
                    // Two reports of at least 4 px each before the gap and
                    // at least 12 px across it: the pointer did not stop.
                    if (lastStep >= 4 && stepBefore >= 4 && step >= 12) {
                        ++motionStalls;
                        worstMotionGap = std::max(worstMotionGap, gap);
                        worstJump = std::max(worstJump, step);
                    }
                    afterGap = true;
                } else if (afterGap && gap < 3000)
                    ++bunched;
                else
                    afterGap = false;
            }
            stepBefore = lastStep; lastStep = step;
            lastX = where.x; lastY = where.y;
            lastWhen = when;
            if (now - bucketStart >= 1000000) {
                char clock[16] = "";
                const time_t wall = time(nullptr);
                struct tm local {};
                if (localtime_r(&wall, &local)) std::strftime(clock, sizeof(clock), "%H:%M:%S", &local);
                std::fprintf(stderr, "Summit input lag: %s %d moves, mean %.1f ms, max %.1f ms, over 30 ms %d, over 100 ms %d; source gaps 40-400 ms: %d, worst %.0f ms; in motion %d, worst %.0f ms, jump %.0f px, bunched after %d\n",
                    clock, count, sum / 1000.0 / count, worst / 1000.0, over30, over100, stalls, worstGap / 1000.0,
                    motionStalls, worstMotionGap / 1000.0, worstJump, bunched);
                bucketStart = now; sum = worst = worstGap = worstMotionGap = 0; worstJump = 0;
                count = over30 = over100 = stalls = motionStalls = bunched = 0;
            }
        }
    }
    BrowserWindowBase::DispatchMessage(message, handler);
}

void BrowserWindow::PreloadExtensionAction(const BMessage& message)
{
    if (!fExtensionsEnabled || !fExtensionActionSnapshot || message.GetUInt64("snapshot", 0) != fExtensionActionSnapshot
        || fClosingWindow || fCloseCommitPending || !IsActive()) return;
    const char* identity = nullptr;
    if (message.FindString("extension_identifier", &identity) != B_OK || !message.GetBool("has_popup", false)) return;
    SUMMIT_EXTENSION_TIMING("action button of %s hovered", identity);
    ReportExtensionAnchors();
    fWebKitContext->PreloadExtensionAction(identity, BMessenger(this), message.GetUInt64("load_identifier", 0),
        message.GetUInt64("page_identifier", 0));
}

void BrowserWindow::ReportExtensionAnchors()
{
    if (!fExtensionsEnabled || !fExtensionActions) return;
    // Where the buttons are now, laid out.
    Layout(false);
    for (size_t i = 0; i < fExtensionActionState.size(); ++i) {
        const char* identity = fExtensionActionState[i].GetString("extension_identifier", "");
        if (!*identity) continue;
        // An action in the overflow menu opens under the overflow button.
        BView* button = i < fExtensionActionButtons.size() ? static_cast<BView*>(fExtensionActionButtons[i]) : fExtensionActionsOverflow;
        const bool shown = button && !button->IsHidden();
        fWebKitContext->SetExtensionActionAnchor(BMessenger(this), identity, shown ? button->ConvertToScreen(button->Bounds()) : BRect());
    }
}

void BrowserWindow::ShowExtensionActionMenu(const BMessage& message)
{
    const char* identity = nullptr;
    BPoint where;
    if (message.FindString("extension_identifier", &identity) != B_OK || !*identity
        || message.FindPoint("where", &where) != B_OK) return;
    if (fExtensionMenuCancelled) *fExtensionMenuCancelled = true;
    fExtensionMenuCancelled = std::make_shared<std::atomic<bool>>(false);
    auto* menu = new ExtensionActionsMenu(fExtensionMenuCancelled);
    auto* unpin = new BMessage(kExtensionSetPinned);
    unpin->AddString("extension_identifier", identity);
    unpin->AddBool("pinned", false);
    auto* item = new ExtensionActionMenuItem("Unpin from Toolbar", unpin);
    item->SetTarget(be_app_messenger);
    menu->AddItem(item);
    menu->AddSeparatorItem();
    auto* manage = new ExtensionActionMenuItem("Manage extensions…", new BMessage(kShowExtensions));
    manage->SetTarget(BMessenger(this));
    menu->AddItem(manage);
    menu->Go(where, true, true, true);
}

void BrowserWindow::ShowExtensionActions()
{
    if (!fExtensionActionSnapshot || !fExtensionActionsOverflow) return;
    if (fExtensionMenuCancelled) *fExtensionMenuCancelled = true;
    fExtensionMenuCancelled = std::make_shared<std::atomic<bool>>(false);
    auto* menu = new ExtensionActionsMenu(fExtensionMenuCancelled);
    for (size_t i = fExtensionActionButtons.size(); i < fExtensionActionState.size(); ++i) {
        const auto& action = fExtensionActionState[i];
        const char* title = "";
        action.FindString("title", &title);
        if (!*title) action.FindString("name", &title);
        auto label = ExtensionDisplayText(title);
        const char* badge = "";
        action.FindString("badge", &badge);
        if (*badge) label += " (" + ExtensionDisplayText(badge) + ")";
        BString menuLabel(label.c_str());
        menu->TruncateString(&menuLabel, B_TRUNCATE_END, 360);
        auto* invocation = new BMessage(action);
        invocation->what = kActivateExtensionAction;
        invocation->AddUInt64("snapshot", fExtensionActionSnapshot);
        auto* item = new ExtensionActionMenuItem(menuLabel.String(), invocation);
        item->SetTarget(BMessenger(this));
        item->SetEnabled(action.GetBool("enabled", false));
        menu->AddItem(item);
    }
    menu->AddSeparatorItem();
    auto* manage = new ExtensionActionMenuItem("Manage extensions…", new BMessage(kShowExtensions));
    manage->SetTarget(BMessenger(this));
    menu->AddItem(manage);
    menu->Go(fExtensionActionsOverflow->ConvertToScreen(fExtensionActionsOverflow->Bounds().LeftBottom()), true, true, true);
}

#if SUMMIT_MODERN_WEBKIT
// The direct daemon thread, without the window lock: app_server waits here
// (half a second at most) while it moves or covers the window.
void BrowserWindow::DirectConnected(direct_buffer_info* info)
{
    // SUMMIT_DIRECT_PRESENT=flag connects the window but leaves the pages
    // presenting through app_server (for finding faults).
    static const bool forward = [] {
        const char* value = std::getenv("SUMMIT_DIRECT_PRESENT");
        return !value || std::strcmp(value, "flag");
    }();
    fDirectDaemon = find_thread(nullptr);
    if (forward)
        BWebKitView::WindowDirectConnected(this, info);
    fDirectConnections.fetch_add(1);
}

bool BrowserWindow::WaitForDirectConnection(bigtime_t timeout) const
{
    const bigtime_t end = system_time() + timeout;
    while (!fDirectConnections.load() && system_time() < end)
        snooze(2000);
    return fDirectConnections.load() > 0;
}

bool BrowserWindow::DirectDaemonLost() const
{
    thread_id daemon = fDirectDaemon;
    thread_info info;
    return daemon >= 0 && get_thread_info(daemon, &info) != B_OK;
}
#endif

void BrowserWindow::WindowActivated(bool active)
{
    BrowserWindowBase::WindowActivated(active);
    SyncBrowserWindow();
    if (!active) HideSuggestions();
    if (active) {
        fActivatedAt = system_time();
        AnnouncePointer();
        // The application sends new tabs and extension requests to the front window.
        BMessage activated(kWindowActivated);
        activated.AddMessenger("window", BMessenger(this));
        be_app->PostMessage(&activated);
    }
}
#endif

void BrowserWindow::RefreshChrome()
{
#if SUMMIT_MODERN_WEBKIT
    // Extensions' Tab.isArticle and isInReaderMode, when they change.
    for (auto& tab : fTabs) {
        // A page in Reader view is still an article.
        const std::pair reader { (tab.readerAvailable || tab.readerActive) && !tab.processExited && tab.loadError.empty(), tab.readerActive };
        if (tab.reportedReader != reader) {
            tab.reportedReader = reader;
            fWebKitContext->SetBrowserTabReader(tab.view, reader.first, reader.second);
        }
    }
    const auto* active = ActiveTab();
    static_cast<AddressControl*>(fAddress)->SetReader(active && !active->processExited
        && active->loadError.empty() && (active->readerActive || active->readerAvailable),
        active && active->readerActive, active && !active->loading && !active->readerExtracting && !active->processExited);
    const bool hasCertificate = active && !active->loading && !active->processExited && active->loadError.empty()
        && active->url == active->connectionCertificate.GetString("url", "")
        && active->connectionCertificate.HasString("sha256");
    static_cast<AddressControl*>(fAddress)->SetCertificate(hasCertificate, hasCertificate
        && active->connectionCertificate.GetBool("verified", false)
        && !active->connectionCertificate.GetBool("mixedContent", false));
    if (fCertificateWindow.IsValid() && (!hasCertificate || fCertificateTab != active->id
        || fCertificateGeneration != active->loadGeneration
        || fCertificateVerified != active->connectionCertificate.GetBool("verified", false)
        || fCertificateMixedContent != active->connectionCertificate.GetBool("mixedContent", false))) {
        fCertificateWindow.SendMessage(B_QUIT_REQUESTED);
        fCertificateWindow = {};
    }
#endif
    std::vector<TabLabel> labels;
    for (const auto& tab : fTabs) {
        labels.push_back({tab.id, tab.ShownTitle(), tab.loading, tab.errorView ? nullptr : fFavicons->Icon(tab.url),
            tab.capturingCamera || tab.capturingMicrophone || tab.capturingScreen});
        labels.back().url = tab.url;
#if SUMMIT_MODERN_WEBKIT
        labels.back().pinned = tab.pinned;
        labels.back().audio = tab.audioMuted ? TabAudio::Muted : tab.playingAudio ? TabAudio::Playing : TabAudio::None;
#endif
    }
    fTabStrip->SetTabs(std::move(labels), fSelected);
    NoteWindowIcon();
    if (auto* tab = ActiveTab()) {
        fBookmarkButton->SetEnabled(Bookmarkable(tab->url));
        const bool bookmarked = fShared->Read([&](const Profile& profile) { return profile.bookmarks.Contains(tab->url); });
        fBookmarkButton->SetIcon(bookmarked ? Icon::BookmarkFilled : Icon::Bookmark);
        const std::string& shown = tab->ShownTitle();
        const std::string title = !fWebApp.empty() ? (shown.empty() || shown == fWebAppName ? fWebAppName
            : shown + " — " + fWebAppName) : shown + (fPrivate ? " — Summit Private Browsing" : " — Summit");
        if (title != Title()) {
            SetTitle(title.c_str());
            std::lock_guard lock(sWindowListLock);
            for (auto& entry : sWindowList) if (entry.key == fKey) entry.title = fPrivate ? tab->title + " (Private)" : tab->title;
        }
        fBack->SetEnabled(tab->back || (tab->errorView && tab->errorBack)); fForward->SetEnabled(tab->forward);
        fReload->SetIcon(tab->loading ? Icon::Stop : Icon::Reload);
        fProgress->SetProgress(tab->loading ? std::max(0.03f, tab->progress) : 0);
        // The Haiku look shows progress in the status bar, like WebPositive.
        const bool showProgress = fInterfaceStyle == "haiku" && tab->loading;
        if (showProgress) {
            fStatusProgress->SetTo(std::max(0.03f, tab->progress));
            if (fStatusProgress->IsHidden(fStatusProgress)) fStatusProgress->Show();
        } else if (!fStatusProgress->IsHidden(fStatusProgress)) fStatusProgress->Hide();
    }
    ShowZoom();
}

BrowserWindow::Tab* BrowserWindow::FindTabByID(int64 id)
{
    for (auto& tab : fTabs) if (tab.id == id) return &tab;
    return nullptr;
}

void BrowserWindow::ProfileChanged(const BMessage& message)
{
    const uint32 changes = message.GetUInt32("changes", 0);
    BMessenger sender;
    message.FindMessenger("sender", &sender);
    if (changes & SharedProfile::kIconChanged) {
        // Another window stored a new icon; this window's copy is out of date.
        const char* url = nullptr;
        if (sender != BMessenger(this) && message.FindString("icon_url", &url) == B_OK) fFavicons->Forget(url);
        fWindowIconStale = true;
        PagesChanged();
        RefreshChrome();
        RefreshBookmarks();
    }
    if (changes & SharedProfile::kIconsCleared) {
        fFavicons = std::make_unique<FaviconCache>(fShared->Path().parent_path() / "Favicons", !fPrivate);
        fWindowIconStale = true;
        PagesChanged();
        RefreshChrome();
        RefreshBookmarks();
    }
    if (changes & (SharedProfile::kHistoryChanged | SharedProfile::kHistoryCleared)) {
        PagesChanged();
        RefreshBookmarks();
        if (auto* tab = ActiveTab(); tab && tab->url == kBookmarksPage && tab->bookmarkQuery) RefreshInternalPage(*tab);
    }
    if (changes & SharedProfile::kHistoryCleared) {
        // Recently closed tabs are history too.
        fClosedTabs.clear();
        if (auto* tab = ActiveTab(); tab && tab->url == kHistoryPage) RefreshInternalPage(*tab);
    }
    if (changes & SharedProfile::kZoomChanged && message.GetBool("private", false) == fPrivate) {
        // The same site in other tabs and windows follows, as in Firefox.
        const std::string key = message.GetString("zoom_key", "");
        const double zoom = fShared->SiteZoom(key, fPrivate);
        for (auto& tab : fTabs) {
            if (key.empty() || tab.zoomKey != key || std::fabs(tab.pageZoom - zoom) < 0.001) continue;
            tab.view->SetZoomFactor(zoom);
            tab.pageZoom = zoom;
        }
        ShowZoom();
    }
#if SUMMIT_MODERN_WEBKIT
    if (changes & SharedProfile::kExtensionsPinnedChanged) RefreshExtensionActions();
    if (changes & SharedProfile::kExtensionsInstalledChanged)
        for (auto& tab : fTabs) UpdateStoreOffer(tab);
#endif
    if (changes & SharedProfile::kBookmarksChanged) {
#if SUMMIT_MODERN_WEBKIT
        ReportBookmarkChanges();
#endif
        PagesChanged();
        RefreshBookmarks();
        RefreshChrome();
        if (auto* tab = ActiveTab(); tab && tab->url == kBookmarksPage) RefreshInternalPage(*tab);
    }
    if (changes & SharedProfile::kSettingsChanged) {
        ShowSearchEngine();
#if SUMMIT_MODERN_WEBKIT
        if (const auto aids = ReadAccessibilityAids(*fShared); aids != fAids) {
            fAids = aids;
            for (auto& tab : fTabs) ApplyAccessibilityAids(tab.view, fAids);
            if (fCaretBrowsingItem) fCaretBrowsingItem->SetMarked(fAids.caretBrowsing);
            if (fReducedMotionItem) fReducedMotionItem->SetMarked(fAids.reduceMotion);
            if (fIncreasedContrastItem) fIncreasedContrastItem->SetMarked(fAids.increaseContrast);
            if (fOverrideColorsItem) fOverrideColorsItem->SetMarked(fAids.overrideColors);
        }
#endif
#if SUMMIT_MODERN_WEBKIT
        const char* agent = fShared->Read([](const Profile& profile) { return FindUserAgentPreset(profile.userAgent).agent; });
        for (auto& tab : fTabs) tab.view->SetCustomUserAgent(agent);
#endif
        const auto [bar, style] = fShared->Read([](const Profile& profile) {
            return std::make_pair(profile.showBookmarksBar, profile.interfaceStyle);
        });
        if (bar != fBookmarksBarVisible) {
            fBookmarksBarVisible = bar;
            if (bar && fBookmarksBar->IsHidden(fBookmarksBar) && fWebApp.empty() && !fPopup) fBookmarksBar->Show();
            else if (!bar && !fBookmarksBar->IsHidden(fBookmarksBar)) fBookmarksBar->Hide();
            fBookmarksBarItem->SetLabel(bar ? "Hide Bookmarks Bar" : "Show Bookmarks Bar");
        }
        if (style != fInterfaceStyle) {
            fInterfaceStyle = style;
            ApplyInterfaceStyle();
            RefreshChrome();
        }
    }
}

void BrowserWindow::RequestNewWindow(const std::vector<std::string>& urls, bool privateWindow)
{
    BMessage request(kNewWindow);
    for (const auto& url : urls) request.AddString("url", url.c_str());
    request.AddBool("private", privateWindow);
    // Cascade from this window.
    request.AddRect("frame", Frame().OffsetByCopy(24, 24));
    be_app->PostMessage(&request);
}

void BrowserWindow::ShowTabMenu(const BMessage& message)
{
    int64 id = 0;
    BPoint where;
    if (message.FindInt64("id", &id) != B_OK || message.FindPoint("where", &where) != B_OK || !FindTabByID(id)) return;
    auto item = [id](const char* label, uint32 what) {
        auto* invocation = new BMessage(what);
        invocation->AddInt64("id", id);
        return new BMenuItem(label, invocation);
    };
    if (IsClosing()) return;
    auto* menu = new CancellableMenu("tab", fMenusCancelled);
    menu->AddItem(new BMenuItem("New Tab", new BMessage(kNewTab)));
    menu->AddSeparatorItem();
    menu->AddItem(item("Reload Tab", kReloadTab));
    menu->AddItem(item("Duplicate Tab", kDuplicateTab));
#if SUMMIT_MODERN_WEBKIT
    const auto* clicked = FindTabByID(id);
    auto* pin = item(clicked && clicked->pinned ? "Unpin Tab" : "Pin Tab", kPinTab);
    pin->Message()->AddBool("pinned", !(clicked && clicked->pinned));
    menu->AddItem(pin);
    menu->AddItem(item(clicked && clicked->audioMuted ? "Unmute Tab" : "Mute Tab", kToggleTabMute));
    // Frees what the page holds; it loads again when selected (#116).
    auto* unload = item("Unload Tab", kDiscardTab);
    unload->SetEnabled(clicked && id != fSelected && clicked->deferredURL.empty());
    menu->AddItem(unload);
#endif
    auto* move = item("Move Tab to New Window", kMoveTabToNewWindow);
    move->SetEnabled(fTabs.size() > 1);
    menu->AddItem(move);
    menu->AddSeparatorItem();
    menu->AddItem(item("Close Tab", kCloseTab));
    auto* others = item("Close Other Tabs", kCloseOtherTabs);
    others->SetEnabled(fTabs.size() > 1);
    menu->AddItem(others);
#if SUMMIT_MODERN_WEBKIT
    auto* all = new BMenuItem("Bookmark All Tabs", new BMessage(kBookmarkAllTabs));
    menu->AddItem(all);
    auto* right = item("Close Tabs to the Right", kCloseTabsToRight);
    right->SetEnabled(!fTabs.empty() && fTabs.back().id != id);
    menu->AddItem(right);
    menu->AddSeparatorItem();
    auto* reopen = new BMenuItem("Reopen Closed Tab", new BMessage(kReopenTab));
    reopen->SetEnabled(!fClosedTabs.empty());
    menu->AddItem(reopen);
#endif
    menu->SetTargetForItems(this);
    menu->SetAsyncAutoDestruct(true);
    menu->Go(where, true, false, true);
}

void BrowserWindow::ApplyInterfaceStyle()
{
#if SUMMIT_MODERN_WEBKIT
    ExitPageFullScreen();
#endif
    const bool haiku = fInterfaceStyle == "haiku";
    SetInterfaceStyle(haiku);
    // Haiku: tabs on top, as in WebPositive, then a toolbar of real buttons with
    // a left-aligned address field and a Go button; progress in the status bar.
    // Safari: toolbar, bookmarks bar, then tabs; a centered address field.
    fTabStrip->RemoveSelf();
    fLayout->AddView(haiku ? 1 : 3, fTabStrip);
    auto* toolbar = fToolbar->GroupLayout();
    // IsHidden(view): the window itself is still hidden while it is built.
    auto show = [](BView* view, bool visible) {
        if (visible && view->IsHidden(view)) view->Show();
        else if (!visible && !view->IsHidden(view)) view->Hide();
    };
    // Order: Back, Forward, Reload (Haiku), Home, glue, Address, Reload (Safari), glue, …
    BView* reload = fReload;
    reload->RemoveSelf();
    toolbar->AddView(haiku ? 2 : 5, reload);
    for (int32 i = 0; i < toolbar->CountItems(); ++i) {
        auto* view = toolbar->ItemAt(i)->View();
        if (view && !std::strcmp(view->Name(), "toolbar-glue")) show(view, !haiku);
    }
    fAddress->SetExplicitMaxSize(BSize(haiku ? B_SIZE_UNLIMITED : 660, B_SIZE_UNSET));
    fAddress->TextView()->SetAlignment(haiku ? B_ALIGN_LEFT : B_ALIGN_CENTER);
    toolbar->SetInsets(haiku ? 5 : 8, haiku ? 4 : 7, haiku ? 5 : 8, haiku ? 4 : 7);
    toolbar->SetSpacing(haiku ? 3 : 4);
    show(fProgress, !haiku);
    for (BView* view : std::initializer_list<BView*>{fToolbar, fTabStrip, fBookmarksBar, fProgress})
        view->Invalidate();
    for (int32 i = 0; i < fToolbar->CountChildren(); ++i) fToolbar->ChildAt(i)->Invalidate();
    fTabStrip->InvalidateLayout();
    if (!fWebApp.empty()) {
        // A web app's window: its pages, a menu and the status line.
        for (BView* view : std::initializer_list<BView*>{fToolbar, fTabStrip, fBookmarksBar, fProgress})
            show(view, false);
    }
}
std::string BrowserWindow::DisplayURL(const std::string& url) const
{
    return url.rfind("summit:", 0) == 0 ? std::string() : url;
}
std::string BrowserWindow::HomeAddress() const
{
    const auto home = fShared->Read([](const Profile& profile) { return profile.homeURL; });
    return home.empty() ? "summit:home" : home;
}
#if SUMMIT_MODERN_WEBKIT
void BrowserWindow::ShowSidebar(const std::string& extension, const std::string& url, const std::string& title)
{
    if (fClosingWindow || extension.empty() || url.empty()) return;
    CloseSidebar();
    status_t status = B_ERROR;
    auto* view = fWebKitContext->CreateExtensionView(BRect(0, 0, 179, 199), "sidebar-page", extension.c_str(), BMessenger(this), &status);
    if (view && view->InitCheck() != B_OK) {
        status = view->InitCheck();
        delete std::exchange(view, nullptr);
    }
    if (!view) {
        fStatus->SetText(("Could not open the sidebar: " + std::string(std::strerror(status))).c_str());
        return;
    }
    view->SetExplicitMinSize(BSize(160, 100));
    view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    if (fAids != AccessibilityAids { }) ApplyAccessibilityAids(view, fAids);
    fSidebar->GetLayout()->AddView(view);
    fSidebarView = view;
    fSidebarExtension = extension;
    fSidebarTitle->SetText(title.c_str());
    if (fSidebar->IsHidden(fSidebar)) fSidebar->Show();
    UpdateSplitters();
    view->LoadURL(url.c_str());
}

std::pair<std::string, std::string> BrowserWindow::SidebarPage(const std::string& extension) const
{
    std::pair<std::string, std::string> page;
    for (const auto& sidebar : fShared->ExtensionSidebars())
        if (sidebar.identifier == extension) page = {sidebar.url, sidebar.title};
    if (auto found = fSidebarOverrides.find(extension); found != fSidebarOverrides.end()) {
        if (!found->second.first.empty()) page.first = found->second.first;
        if (!found->second.second.empty()) page.second = found->second.second;
    }
    return page;
}

void BrowserWindow::SidebarCommand(const BMessage& message, uint64 identifier)
{
    const std::string method = message.GetString("method", ""), extension = message.GetString("extension_identifier", "");
    const auto arguments = nlohmann::json::parse(message.GetString("arguments", "{}"), nullptr, false);
    if (!arguments.is_object() || extension.empty()) { RespondToCommand(identifier, B_BAD_VALUE); return; }
    auto answer = [&](const nlohmann::json& value) { fWebKitContext->RespondToBrowserCommandWithData(identifier, B_OK, value.dump().c_str()); };
    const bool shown = fSidebarExtension == extension;
    if (fPrivate || fPopup) {
        // No sidebar in private windows and pop-ups.
        if (method == "isOpen") answer(false);
        else RespondToCommand(identifier, B_NOT_ALLOWED, { }, "This window has no sidebar.");
        return;
    }
    auto [url, title] = SidebarPage(extension);
    if (method == "open" || (method == "toggle" && !shown)) {
        if (url.empty()) { RespondToCommand(identifier, B_NOT_ALLOWED, { }, "The extension has no sidebar panel."); return; }
        if (!shown) {
            BMessage open(kOpenSidebarOnApp);
            open.AddMessenger("window", BMessenger(this));
            open.AddString("identifier", extension.c_str());
            open.AddString("url", url.c_str());
            open.AddString("title", title.c_str());
            be_app->PostMessage(&open);
        }
        answer(nullptr);
    } else if (method == "close" || method == "toggle") {
        if (shown) CloseSidebar();
        answer(nullptr);
    } else if (method == "isOpen") answer(shown);
    else if (method == "getPanel") answer(url);
    else if (method == "getTitle") answer(title);
    else if (method == "setPanel" || method == "setTitle") {
        const char* key = method == "setPanel" ? "panel" : "title";
        const auto value = arguments.contains(key) && arguments[key].is_string() ? arguments[key].get<std::string>() : std::string();
        auto& overrides = fSidebarOverrides[extension];
        (method == "setPanel" ? overrides.first : overrides.second) = value;
        const auto [newURL, newTitle] = SidebarPage(extension);
        if (shown && method == "setPanel" && fSidebarView) fSidebarView->LoadURL(newURL.c_str());
        if (shown && method == "setTitle") fSidebarTitle->SetText(newTitle.c_str());
        answer(nullptr);
    } else RespondToCommand(identifier, B_NOT_SUPPORTED);
}

void BrowserWindow::CloseSidebar()
{
    if (!fSidebarView) return;
    fSidebarView->RemoveSelf();
    delete std::exchange(fSidebarView, nullptr);
    fSidebarExtension.clear();
    if (!fSidebar->IsHidden(fSidebar)) fSidebar->Hide();
    UpdateSplitters();
}

// BSplitLayout lays out the splitter beside a hidden item too, drawn and
// draggable, so each split holds one pane that can be hidden and has no
// splitter while that pane is hidden.
void BrowserWindow::UpdateSplitters()
{
    if (!fContentSplit || !fSideSplit || !fPageSplit) return;
    fContentSplit->SetSplitterSize(fSidebar->IsHidden(fSidebar) ? 0 : fSplitterSize);
    fSideSplit->SetSplitterSize(fSideDock->IsHidden(fSideDock) ? 0 : fSplitterSize);
    fPageSplit->SetSplitterSize(fDock->IsHidden(fDock) ? 0 : fSplitterSize);
}
#endif

std::string BrowserWindow::NewTabAddress() const
{
    // Private windows open the private browsing page; extensions do not run there.
    if (fPrivate) return "summit:home";
    auto page = fShared->NewTabOverride();
    return page.empty() ? HomeAddress() : page;
}
void BrowserWindow::ShowSearchEngine()
{
    fAddress->SetToolTip((std::string("Search with ") + CurrentSearchEngine().name + " or enter a website address").c_str());
}
void BrowserWindow::ApplySiteZoom(Tab& tab)
{
    const auto key = ZoomKey(tab.url);
    // Pages without a site (about:blank) keep the zoom they have.
    if (key.empty() || key == tab.zoomKey) return;
    tab.zoomKey = key;
    const double zoom = fShared->SiteZoom(key, fPrivate);
    if (std::fabs(zoom - tab.pageZoom) >= 0.001) {
        tab.view->SetZoomFactor(zoom);
        tab.pageZoom = zoom;
    }
    if (tab.id == fSelected) ShowZoom();
}
void BrowserWindow::ChangeZoom(int direction)
{
    auto* tab = ActiveTab();
    if (!tab) return;
#if SUMMIT_MODERN_WEBKIT
    // Text only: the engine's text zoom of the tab, not remembered for the
    // site. Actual Size resets both.
    if (direction && fShared->Read([](const Profile& profile) { return profile.zoomTextOnly; })) {
        if (direction > 0) tab->view->IncreaseZoomFactor(true);
        else tab->view->DecreaseZoomFactor(true);
        return;
    }
    if (!direction) tab->view->ResetZoomFactor();
#endif
    SetTabZoom(*tab, direction ? NextZoomLevel(tab->pageZoom, direction) : 1.0);
}
void BrowserWindow::SetTabZoom(Tab& tab, double zoom)
{
    if (std::fabs(zoom - tab.pageZoom) >= 0.001) {
        tab.view->SetZoomFactor(zoom);
        tab.pageZoom = zoom;
    }
    // Remembered for the site; its other tabs follow (ProfileChanged).
    tab.zoomKey = ZoomKey(tab.url);
    fShared->SetSiteZoom(tab.zoomKey, zoom, fPrivate, BMessenger(this));
    if (&tab == ActiveTab()) ShowZoom();
}
void BrowserWindow::ShowZoom()
{
    auto* tab = ActiveTab();
    const bool show = tab && !IsDefaultZoom(tab->pageZoom);
    if (show) fZoomButton->SetZoom(tab->pageZoom);
    if (show && fZoomButton->IsHidden(fZoomButton)) fZoomButton->Show();
    else if (!show && !fZoomButton->IsHidden(fZoomButton)) fZoomButton->Hide();
}
int64 BrowserWindow::BookmarkQueryForURL(const BString& url, unsigned* section) const
{
    if (section) *section = 0;
    auto prefix = FileURL((fShared->Path().parent_path() / "Pages").string()) + "/";
    const std::string value = url.String();
    if (value.rfind(prefix, 0) != 0 && prefix.rfind("file:///", 0) == 0) prefix = "file:" + prefix.substr(7);
    if (value.rfind(prefix, 0) != 0) return 0;
    const auto location = ParseBookmarkQueryPageName(std::string_view(value).substr(prefix.size()));
    if (!location) return 0;
    if (section) *section = location->section;
    return location->id;
}
void BrowserWindow::UpdateBookmarkQueryLocation(Tab& tab, const BString& url)
{
    unsigned section = 0;
    const auto id = BookmarkQueryForURL(url, &section);
    // The revision belongs to the displayed query, not to every internal page
    // in this tab's back/forward history. Revalidate a newly selected view once
    // it loads, even when another view was refreshed at the current revision.
    if (tab.bookmarkQuery != id || tab.bookmarkQuerySection != section) tab.pageRevision = 0;
    tab.bookmarkQuery = id;
    tab.bookmarkQuerySection = section;
}
std::filesystem::path BrowserWindow::InternalPagePath(const std::string& url, int64 bookmarkQuery, unsigned section) const
{
    return fShared->Path().parent_path() / "Pages" / (url == kHistoryPage ? "history.html"
        : bookmarkQuery ? BookmarkQueryPageName(bookmarkQuery, section) : "bookmarks.html");
}
bool BrowserWindow::WriteInternalPage(const std::string& url, int64 bookmarkQuery, unsigned section)
{
    const auto icons = [this](const std::string& page) { return fFavicons->DataURL(page); };
    std::string html;
    if (url == kHistoryPage) {
        const auto pages = fShared->Read([](const Profile& profile) { return profile.history; });
        html = RenderHistoryPage(pages, std::time(nullptr), icons);
    } else {
        const auto snapshot = fShared->Read([](const Profile& profile) { return std::pair{profile.bookmarks, profile.history}; });
        html = bookmarkQuery ? RenderBookmarkQueryPage(snapshot.first, snapshot.second, bookmarkQuery, icons, BookmarkPageSecret(), section)
            : RenderBookmarksPage(snapshot.first, icons, BookmarkPageSecret());
    }
    const auto path = InternalPagePath(url, bookmarkQuery, section);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << html;
        if (!file.flush()) error = std::make_error_code(std::errc::io_error);
    }
    if (error || std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        fStatus->SetText(url == kHistoryPage ? "Could not write the History page." : "Could not write the Bookmarks page.");
        return false;
    }
    return true;
}
std::string BrowserWindow::LoadableURL(const std::string& url)
{
    if (url == "summit:home" && fPrivate) {
        // Written each time, for the search engine chosen now.
        const auto path = fShared->Path().parent_path() / "Pages" / "private.html";
        const auto& engine = CurrentSearchEngine();
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        const auto temporary = path.string() + ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file << RenderPrivateStartPage(engine.name, engine.prefix);
            if (!file.flush()) error = std::make_error_code(std::errc::io_error);
        }
        if (!error && std::rename(temporary.c_str(), path.c_str()) == 0) return FileURL(path.string());
        std::remove(temporary.c_str());
    }
    if (url == "summit:home") return fStartURL;
    if (url == kHistoryPage || url == kBookmarksPage) {
        WriteInternalPage(url);
        return FileURL(InternalPagePath(url).string());
    }
    return url;
}
void BrowserWindow::ShowInternalPage(const std::string& url)
{
    // One History tab and one Bookmarks tab: show the open one, brought up to date.
    for (auto& tab : fTabs) {
        if (tab.url != url) continue;
        if (tab.id == fSelected) RefreshInternalPage(tab);
        else SelectTab(tab.id);
        return;
    }
    CreateTab(url);
}
void BrowserWindow::ShowBookmarkQuery(int64 id, unsigned section)
{
    ShowInternalPage(kBookmarksPage);
    if (auto* tab = ActiveTab(); tab && tab->url == kBookmarksPage) {
        tab->bookmarkQuery = id;
        tab->bookmarkQuerySection = section;
        RefreshInternalPage(*tab, true, true);
    }
}
void BrowserWindow::RefreshInternalPage(Tab& tab, bool force, bool navigate)
{
    if (tab.url != kHistoryPage && tab.url != kBookmarksPage) return;
    if (!force && tab.pageRevision == fPagesRevision) return;
#if SUMMIT_MODERN_WEBKIT
    if (!PrepareTabNavigation(tab)) return;
#endif
    if (!WriteInternalPage(tab.url, tab.bookmarkQuery, tab.bookmarkQuerySection)) return;
    tab.pageRevision = fPagesRevision;
    if (navigate) tab.view->LoadURL(FileURL(InternalPagePath(tab.url, tab.bookmarkQuery, tab.bookmarkQuerySection).string()).c_str());
    else tab.view->Reload();
}
void BrowserWindow::PagesChanged()
{
    ++fPagesRevision;
}
void BrowserWindow::BookmarksChanged()
{
    PagesChanged();
    RefreshBookmarks();
    RefreshChrome();
    SaveSession();
    if (auto* tab = ActiveTab(); tab && tab->url == kBookmarksPage) RefreshInternalPage(*tab);
}
void BrowserWindow::RefreshBookmarks()
{
    auto bar = std::make_shared<BookmarkButton>(fShared->Read([](const Profile& profile) {
        return BookmarkButtons(profile.bookmarks, profile.history, profile.bookmarks.Bar());
    }));
    // Icons for the bar's own buttons; its folders' menus show none.
    for (auto& button : bar->children)
        if (!button.folder && !button.separator) button.icon = fFavicons->Icon(button.url);
    fBookmarksBar->SetBookmarks(std::move(bar));
}
void BrowserWindow::SetBookmarksBarVisible(bool visible)
{
    // Shown or hidden in every window; ProfileChanged applies it.
    fShared->Change([visible](Profile& profile) -> uint32 {
        if (profile.showBookmarksBar == visible) return 0;
        profile.showBookmarksBar = visible;
        return SharedProfile::kSettingsChanged;
    }, BMessenger(this));
}
void BrowserWindow::MenusBeginning()
{
    BrowserWindowBase::MenusBeginning();
    RebuildDynamicMenus();
#if SUMMIT_MODERN_WEBKIT
    if (fSpellingItem) fSpellingItem->SetMarked(fShared->Read([](const Profile& profile) { return profile.spellChecking; }));
    if (fZoomTextOnlyItem) fZoomTextOnlyItem->SetMarked(fShared->Read([](const Profile& profile) { return profile.zoomTextOnly; }));
#endif
}
void BrowserWindow::RebuildDynamicMenus()
{
    auto* tab = ActiveTab();
    const bool bookmarkable = tab && Bookmarkable(tab->url);
    std::vector<PageRecord> history;
    bool bookmarked = false;
    // Both folders, for submenus that are filled when they open.
    auto roots = std::make_shared<BookmarkButton>();
    fShared->Read([&](const Profile& profile) {
        history.assign(profile.history.begin(), profile.history.begin() + std::min<size_t>(profile.history.size(), 15));
        roots->children = {BookmarkButtons(profile.bookmarks, profile.history, profile.bookmarks.Bar()),
            BookmarkButtons(profile.bookmarks, profile.history, profile.bookmarks.Other())};
        bookmarked = tab && profile.bookmarks.Contains(tab->url);
        return 0;
    });
    if (auto* item = fBookmarksMenu->FindItem(kBookmark)) item->SetEnabled(bookmarkable);
    if (auto* item = fBookmarksMenu->FindItem(kAddToBookmarksBar)) item->SetEnabled(bookmarkable);
    fRemoveBookmarkItem->SetEnabled(bookmarkable && bookmarked);
    fEditBookmarkItem->SetEnabled(bookmarkable && bookmarked);
    auto clear = [](BMenu* menu, int32 fixed) {
        while (menu->CountItems() > fixed) delete menu->RemoveItem(fixed);
    };
    auto add = [this](BMenu* menu, const PageRecord& page) {
        auto* open = new BMessage(kOpenBookmark);
        open->AddString("url", page.url.c_str());
        auto* item = NewPageMenuItem(MenuLabel(page).c_str(), open, fFavicons->Icon(page.url));
        item->SetTarget(this);
        menu->AddItem(item);
    };
    // The bookmarks' icons for the folders' menus (filled when they open,
    // from this copy), as many as the menus show.
    size_t icons = 0;
    std::function<void(BookmarkButton&)> addIcons = [&](BookmarkButton& button) {
        for (auto& child : button.children) {
            if (icons >= 600) return;
            if (child.separator) continue;
            if (child.folder) addIcons(child);
            else if (const BBitmap* icon = fFavicons->Icon(child.url)) {
                child.menuIcon = std::make_shared<const BBitmap>(icon);
                ++icons;
            }
        }
    };
    addIcons(*roots);
    // Recently visited pages, then every bookmark, as in Safari's menus.
    clear(fHistoryMenu, fHistoryMenuFixed);
    if (!history.empty()) fHistoryMenu->AddSeparatorItem();
    for (const auto& page : history) add(fHistoryMenu, page);
    if (auto* item = fHistoryMenu->FindItem(kClearHistory)) item->SetEnabled(!history.empty());
    clear(fBookmarksMenu, fBookmarksMenuFixed);
    // The bar's as a submenu, then Other Bookmarks' own, as in Firefox.
    fBookmarksMenu->AddSeparatorItem();
    const std::shared_ptr<const BookmarkButton> snapshot = roots;
    const BMessenger target(this);
    fBookmarksMenu->AddItem(NewBookmarkFolderItem(snapshot, snapshot->children[0], target));
    size_t shown = 0;
    for (const auto& child : snapshot->children[1].children) {
        if (shown++ == 60) {
            auto* more = new BMenuItem("More Bookmarks…", new BMessage(kShowBookmarks));
            more->SetTarget(this);
            fBookmarksMenu->AddItem(more);
            break;
        }
        if (child.separator) fBookmarksMenu->AddSeparatorItem();
        else if (child.folder) fBookmarksMenu->AddItem(NewBookmarkFolderItem(snapshot, child, target));
        else add(fBookmarksMenu, {child.url, child.title});
    }
    if (auto* item = fWindowMenu->FindItem(kMoveTabToNewWindow)) item->SetEnabled(fTabs.size() > 1);
    // Every browser window, the current one marked, as in other Haiku applications.
    clear(fWindowMenu, fWindowMenuFixed);
    std::vector<WindowEntry> windows;
    {
        std::lock_guard lock(sWindowListLock);
        windows = sWindowList;
    }
    std::sort(windows.begin(), windows.end(), [](const WindowEntry& a, const WindowEntry& b) { return a.key < b.key; });
    fWindowMenu->AddSeparatorItem();
    for (const auto& window : windows) {
        auto* activate = new BMessage(kActivateWindow);
        auto* item = new BMenuItem(MenuLabel({"", window.title}).c_str(), activate);
        item->SetTarget(window.window);
        item->SetMarked(window.key == fKey);
        fWindowMenu->AddItem(item);
    }
}
void BrowserWindow::ShowBookmarkMenu()
{
    auto* tab = ActiveTab();
    if (!tab || !Bookmarkable(tab->url)) return;
    struct Bookmarked { bool found = false, onBar = false; std::string folder; };
    const auto bookmark = fShared->Read([&](const Profile& profile) {
        Bookmarked state;
        if (const auto* node = profile.bookmarks.FindURL(tab->url)) {
            state.found = true;
            state.onBar = profile.bookmarks.OnBar(tab->url);
            state.folder = profile.bookmarks.FolderPath(node->id);
        }
        return state;
    });
    if (IsClosing()) return;
    auto* menu = new CancellableMenu("bookmark-page", fMenusCancelled);
    if (!bookmark.found) {
        menu->AddItem(new BMenuItem("Add to Bookmarks Bar", new BMessage(kAddToBookmarksBar)));
        menu->AddItem(new BMenuItem("Add to Bookmarks", new BMessage(kBookmark)));
        menu->AddItem(new BMenuItem("Add to Folder…", new BMessage(kAddBookmarkToFolder)));
    } else {
        // Where it is, then what can be done with it.
        auto* where = new BMenuItem(MenuLabel({"", "In " + bookmark.folder}).c_str(), nullptr);
        where->SetEnabled(false);
        menu->AddItem(where);
        menu->AddSeparatorItem();
        menu->AddItem(new BMenuItem("Edit Bookmark…", new BMessage(kEditBookmark)));
        menu->AddItem(bookmark.onBar ? new BMenuItem("Remove from Bookmarks Bar", new BMessage(kRemoveFromBookmarksBar))
            : new BMenuItem("Add to Bookmarks Bar", new BMessage(kAddToBookmarksBar)));
        menu->AddItem(new BMenuItem("Remove Bookmark", new BMessage(kRemoveBookmark)));
    }
    menu->AddSeparatorItem();
    menu->AddItem(new BMenuItem("Show All Bookmarks", new BMessage(kShowBookmarks)));
    menu->SetTargetForItems(this);
    menu->SetAsyncAutoDestruct(true);
    const BRect frame = fBookmarkButton->ConvertToScreen(fBookmarkButton->Bounds());
    menu->Go(BPoint(frame.left, frame.bottom + 1), true, false, frame, true);
}
void BrowserWindow::OpenBookmarkEditor(int64 id)
{
    BookmarkEditor::Fields fields;
    const bool found = fShared->Read([&](const Profile& profile) {
        const auto* node = profile.bookmarks.Find(id);
        const auto* parent = profile.bookmarks.Parent(id);
        if (!node || !parent || node->separator) return false;
        fields.mode = node->folder ? BookmarkEditor::kEditFolderMode : BookmarkEditor::kEditBookmarkMode;
        fields.id = id;
        fields.title = node->title;
        fields.url = node->url;
        fields.metadata = node->metadata;
        fields.folder = parent->id;
        // A folder cannot go inside itself.
        fields.folders = profile.bookmarks.Folders(node->folder ? id : 0);
        return true;
    });
    if (!found) {
        fStatus->SetText("This page is not bookmarked.");
        return;
    }
    (new BookmarkEditor(fields, BMessenger(this), Frame()))->Show();
}
void BrowserWindow::OpenNewBookmarkEditor(int64 folder, bool newFolder)
{
    BookmarkEditor::Fields fields;
    fields.mode = newFolder ? BookmarkEditor::kNewFolderMode : BookmarkEditor::kNewBookmarkMode;
    fields.folder = folder;
    if (newFolder) fields.title = "New Folder";
    else {
        auto* tab = ActiveTab();
        if (!tab || !Bookmarkable(tab->url)) return;
        fields.title = tab->title;
        fields.url = tab->url;
    }
    fields.folders = fShared->Read([](const Profile& profile) { return profile.bookmarks.Folders(); });
    (new BookmarkEditor(fields, BMessenger(this), Frame()))->Show();
}
void BrowserWindow::BookmarkEdited(const BMessage& message)
{
    const int32 mode = message.GetInt32("mode", BookmarkEditor::kEditBookmarkMode);
    const int64 id = message.GetInt64("id", 0);
    const int64 folder = message.GetInt64("folder", kOtherBookmarksFolder);
    const std::string title = message.GetString("title", "");
    const std::string url = BookmarkAddress(message.GetString("url", ""));
    BookmarkMetadata metadata;
    metadata.description = message.GetString("description", "");
    const char* tag = nullptr;
    for (int32 i = 0; message.FindString("tag", i, &tag) == B_OK; ++i) metadata.tags.emplace_back(tag);
    const bool hasMetadata = message.HasString("description");
    const bool hasKeywords = message.HasString("keywords");
    if (hasKeywords) metadata.keywords = EditBookmarkKeywords({}, message.GetString("keywords", ""));
    const char* status = nullptr;
    fShared->Change([&](Profile& profile) -> uint32 {
        auto& bookmarks = profile.bookmarks;
        const int64 now = std::time(nullptr);
        if (mode == BookmarkEditor::kNewFolderMode) {
            const auto added = bookmarks.AddFolder(folder, title, kBookmarkEnd, now);
            if (!added) { status = "The folder could not be made there."; return 0; }
            if (hasMetadata) bookmarks.SetMetadata(added, metadata);
            status = "Folder added";
            return SharedProfile::kBookmarksChanged;
        }
        int64 target = id;
        if (mode == BookmarkEditor::kNewBookmarkMode) {
            // A page bookmarked already keeps one bookmark, changed as asked.
            const auto* existing = bookmarks.FindURL(url);
            if (!existing) {
                const auto added = bookmarks.Add({url, title}, folder, kBookmarkEnd, now);
                if (!added) { status = "You have reached the bookmark limit."; return 0; }
                if (hasMetadata) bookmarks.SetMetadata(added, metadata);
                status = "Bookmark saved";
                return SharedProfile::kBookmarksChanged;
            }
            target = existing->id;
        }
        const auto* node = bookmarks.Find(target);
        if (!node) return 0;
        metadata.keywords = hasKeywords ? EditBookmarkKeywords(node->metadata.keywords, message.GetString("keywords", ""))
            : node->metadata.keywords;
        bool changed = bookmarks.Edit(target, title, url);
        if (hasMetadata) changed = bookmarks.SetMetadata(target, metadata) || changed;
        if (const auto* parent = bookmarks.Parent(target); parent && parent->id != folder)
            changed = bookmarks.Move(target, folder, kBookmarkEnd) || changed;
        if (changed) status = "Bookmark saved";
        return changed ? SharedProfile::kBookmarksChanged : 0;
    }, BMessenger(this));
    if (status) fStatus->SetText(status);
}
void BrowserWindow::DeleteBookmark(int64 id, bool confirm)
{
    struct Node { bool found = false, folder = false; size_t bookmarks = 0; std::string title; bool separator = false; };
    const auto node = fShared->Read([&](const Profile& profile) {
        Node node;
        if (const auto* found = profile.bookmarks.Find(id); found && profile.bookmarks.Parent(id))
            node = {true, found->folder, found->folder ? CountBookmarks(*found) : !found->separator, found->title, found->separator};
        return node;
    });
    if (!node.found) return;
    if (confirm && node.folder && node.bookmarks) {
        const std::string question = "Delete the folder “" + node.title + "” and the "
            + (node.bookmarks == 1 ? std::string("bookmark") : std::to_string(node.bookmarks) + " bookmarks") + " in it?";
        auto* alert = new BAlert("Delete Folder", question.c_str(), "Cancel", "Delete", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
        alert->SetShortcut(0, B_ESCAPE);
        auto* reply = new BMessage(kDeleteBookmarkReply);
        reply->AddInt64("id", id);
        alert->Go(new BInvoker(reply, this));
        return;
    }
    fShared->Change([&](Profile& profile) -> uint32 {
        return profile.bookmarks.Remove(id) ? SharedProfile::kBookmarksChanged : 0;
    }, BMessenger(this));
    fStatus->SetText(node.folder ? "Folder deleted" : node.separator ? "Separator deleted" : "Bookmark deleted");
}
bool BrowserWindow::BookmarkPageRequest(Tab& tab, const std::string& source, const std::string& url)
{
    const std::string scheme = std::string(kBookmarksPageScheme) + ":";
    if (url.size() < scheme.size() || strncasecmp(url.c_str(), scheme.c_str(), scheme.size()) != 0) return false;
    // Only the Bookmarks page's own links, which carry this run's secret;
    // anything else asking for summit-bookmarks: is ignored.
    const auto link = ParseBookmarkPageLink(url);
    if (!link || link->token != BookmarkPageSecret() || tab.url != kBookmarksPage
        || StoredURL(BString(source.c_str())) != kBookmarksPage)
        return true;
    const std::string& action = link->action;
    if (action == "import") ShowBookmarkImportWindow(fShared, Frame());
    else if (action == "export") ExportBookmarks();
    else if (action == "edit") OpenBookmarkEditor(link->id);
    else if (action == "query") {
        tab.bookmarkQuery = link->id; tab.bookmarkQuerySection = link->section;
        RefreshInternalPage(tab, true, true);
    }
    else if (action == "delete") DeleteBookmark(link->id, true);
    else if (action == "move") {
        fShared->Change([&](Profile& profile) -> uint32 {
            return profile.bookmarks.Move(link->id, link->folder, link->index) ? SharedProfile::kBookmarksChanged : 0;
        }, BMessenger(this));
    } else if (action == "new-folder") {
        const int64 parent = link->folder ? link->folder : kOtherBookmarksFolder;
        if (!link->hasTitle) OpenNewBookmarkEditor(parent, true);
        else {
            const std::string title = Trim(link->title).empty() ? "New Folder" : Trim(link->title);
            fShared->Change([&](Profile& profile) -> uint32 {
                return profile.bookmarks.AddFolder(parent, title, kBookmarkEnd, std::time(nullptr)) ? SharedProfile::kBookmarksChanged : 0;
            }, BMessenger(this));
        }
    } else if (action == "save") {
        // The page's own editor: name, address and folder.
        const std::string title = Trim(link->title);
        const std::string address = BookmarkAddress(link->url);
        const char* status = nullptr;
        bool changed = false;
        fShared->Change([&](Profile& profile) -> uint32 {
            auto& bookmarks = profile.bookmarks;
            const auto* node = bookmarks.Find(link->id);
            if (!node || !bookmarks.Parent(link->id)) return 0;
            if (!node->folder && !IsBookmarkableURL(address)) {
                status = "Not saved: bookmarks keep web addresses (https://…), files (file://…), bookmarklets (javascript:…) and smart folders (place:…).";
                return 0;
            }
            changed = bookmarks.Edit(link->id, title.empty() ? (node->folder ? "Untitled Folder" : address) : title, address);
            if (link->hasDescription || link->hasTags || link->hasKeywords) {
                auto metadata = node->metadata;
                if (link->hasDescription) metadata.description = link->description;
                if (link->hasTags) metadata.tags = ParseBookmarkTags(link->tags);
                if (link->hasKeywords) metadata.keywords = EditBookmarkKeywords(metadata.keywords, link->keywords);
                changed = bookmarks.SetMetadata(link->id, std::move(metadata)) || changed;
            }
            if (link->folder && bookmarks.Parent(link->id)->id != link->folder)
                changed = bookmarks.Move(link->id, link->folder, kBookmarkEnd) || changed;
            if (changed) status = "Bookmark saved";
            return changed ? SharedProfile::kBookmarksChanged : 0;
        }, BMessenger(this));
        if (status) fStatus->SetText(status);
        // A change brings the page up to date; with nothing to change it is
        // written again all the same, which closes its editor. An address
        // that cannot be kept leaves the editor open, as typed.
        if (!changed && !status) RefreshInternalPage(tab, true);
    }
    return true;
}
void BrowserWindow::ExportBookmarks()
{
    if (!fExportPanel) {
        BMessenger target(this);
        fExportPanel = std::make_unique<BFilePanel>(B_SAVE_PANEL, &target, nullptr, 0, false, new BMessage(kExportBookmarksChosen));
        fExportPanel->Window()->SetTitle("Summit: Export Bookmarks");
        BPath home;
        if (find_directory(B_USER_DIRECTORY, &home) == B_OK) fExportPanel->SetPanelDirectory(home.Path());
    }
    fExportPanel->SetSaveText("bookmarks.html");
    fExportPanel->Show();
}
void BrowserWindow::ExportBookmarksTo(const BMessage& message)
{
    entry_ref directory;
    const char* name = nullptr;
    if (message.FindRef("directory", &directory) != B_OK || message.FindString("name", &name) != B_OK || !name) return;
    BPath path(&directory);
    if (path.InitCheck() != B_OK || path.Append(name) != B_OK) return;
    std::string error;
    const size_t count = ExportBookmarksFile(*fShared, path.Path(), error);
    if (!error.empty()) ShowError("Could not export the bookmarks: " + error);
    else fStatus->SetText(("Exported " + std::to_string(count) + (count == 1 ? " bookmark to " : " bookmarks to ") + name).c_str());
}
void BrowserWindow::IconLoaded(const BMessage& message)
{
    BMessenger sender;
    const char* url = nullptr;
    const void* data = nullptr;
    ssize_t size = 0;
    if (message.FindString("url", &url) != B_OK || !url
        || message.FindData("data", B_RAW_TYPE, &data, &size) != B_OK || size <= 0) return;
    std::string page = url;
#if SUMMIT_MODERN_WEBKIT
    if (page.empty() && message.FindMessenger("view", &sender) == B_OK)
        if (auto* tab = FindTab(sender)) page = tab->url;
#endif
    if (!fFavicons->Store(page, data, static_cast<size_t>(size))) return;
    fWindowIconStale = true;
    if (fPrivate) {
        // Kept in this window only.
        RefreshChrome();
        RefreshBookmarks();
        return;
    }
    // Every window redraws; the others read the new icon from disk.
    fShared->IconChanged(page, BMessenger(this));
}
#if SUMMIT_MODERN_WEBKIT
void BrowserWindow::FullScreenRequested(const BMessage& message)
{
    BMessenger messenger;
    if (message.FindMessenger("view", &messenger) != B_OK) return;
    auto* tab = FindTab(messenger);
    if (!tab) return;
    const bool enter = message.GetBool("fullscreen", false);
    const bool accepted = SetPageFullScreen(tab->id, enter);
    if (auto request = message.GetUInt64("request", 0))
        tab->view->CompleteFullScreenRequest(request, accepted);
}

bool BrowserWindow::SetPageFullScreen(int64 tab, bool enter)
{
    if (enter) {
        if (tab != fSelected || fClosingWindow) return false;
        // A click on an inactive window reaches its page before the window
        // hears that it is active: a request that comes with the click is
        // the user's, and the window comes to the front for it.
        if (!IsActive()) {
            if (system_time() - fLastMouseDown > 1000000) return false;
            Activate();
        }
        if (fFullScreenTab) return fFullScreenTab == tab;
        // The display the window is on, whatever the desktop's zoom setting
        // says about spanning displays: a full-screen page, like airTime's
        // full-screen video, fills one monitor.
        BRect screen;
        if (!BPrivate::get_display_frame
            || BPrivate::get_display_frame(Frame(), false, screen) != B_OK
            || !screen.IsValid())
            screen = BScreen(this).Frame();
        if (!screen.IsValid()) return false;
        fBeforeFullScreen = Frame();
        fBeforeFullScreenLook = Look();
        fBeforeFullScreenFlags = Flags();
        fFullScreenTab = tab;
        fSuggestions->Hide();
        for (int32 i = 0; i < fLayout->CountItems(); ++i) {
            auto* view = fLayout->ItemAt(i)->View();
            if (view && view != fPages && view != fFullScreenNotice && view != fContentSplit && !view->IsHidden(view)) {
                fFullScreenHidden.push_back(view);
                view->Hide();
            }
        }
        // The page alone: the sidebar beside it goes too.
        if (fSidebar && !fSidebar->IsHidden(fSidebar)) {
            fFullScreenHidden.push_back(fSidebar);
            fSidebar->Hide();
            UpdateSplitters();
        }
        SetLook(B_NO_BORDER_WINDOW_LOOK);
        SetWindowFlags(this, Flags() | B_NOT_MOVABLE | B_NOT_RESIZABLE);
        MoveTo(screen.LeftTop());
        ResizeTo(screen.Width(), screen.Height());
        // Show the real URL in native chrome briefly so a page cannot enter
        // fullscreen without identifying itself and explaining how to leave.
        auto* active = ActiveTab();
        std::string notice = (active ? active->url : std::string()) + " — Full screen. Press Esc to exit.";
        fFullScreenNotice->SetText(notice.c_str());
        fFullScreenNotice->Show();
        BMessage hide('fsnh');
        fFullScreenNoticeTimer = std::make_unique<BMessageRunner>(BMessenger(this), &hide, 3000000, 1);
        if (active) active->view->MakeFocus();
    } else if (fFullScreenTab == tab) {
        fFullScreenTab = 0;
        fFullScreenNoticeTimer.reset();
        if (!fFullScreenNotice->IsHidden(fFullScreenNotice)) fFullScreenNotice->Hide();
        SetWindowFlags(this, fBeforeFullScreenFlags);
        SetLook(fBeforeFullScreenLook);
        for (auto* view : fFullScreenHidden) view->Show();
        fFullScreenHidden.clear();
        UpdateSplitters();
        MoveTo(fBeforeFullScreen.LeftTop());
        ResizeTo(fBeforeFullScreen.Width(), fBeforeFullScreen.Height());
    }
    return true;
}

void BrowserWindow::ExitPageFullScreen()
{
    if (!fFullScreenTab) return;
    for (auto& tab : fTabs) {
        if (tab.id != fFullScreenTab) continue;
        tab.view->ExitFullScreen();
        SetPageFullScreen(tab.id, false);
        break;
    }
}
#endif

void BrowserWindow::SaveSession()
{
#if SUMMIT_MODERN_WEBKIT
    if (fClosingWindow) return;
#endif
    // Private windows are not reopened, nor are web apps' (their launchers do).
    if (fPrivate || !fWebApp.empty()) return;
    WindowSession session;
    for (size_t i = 0; i < fTabs.size(); ++i) {
        session.tabs.push_back({fTabs[i].url, fTabs[i].title});
        if (fTabs[i].id == fSelected) session.selected = i;
#if SUMMIT_MODERN_WEBKIT
        if (fTabs[i].pinned && session.pinned == i) ++session.pinned;
#endif
    }
#if SUMMIT_MODERN_WEBKIT
    const BRect frame = fFullScreenTab ? fBeforeFullScreen : Frame();
#else
    const BRect frame = Frame();
#endif
    session.frame[0] = frame.left; session.frame[1] = frame.top;
    session.frame[2] = frame.right; session.frame[3] = frame.bottom;
    fShared->SetWindowSession(fKey, session);
    fShared->SaveSoon();
    if (const auto error = fShared->SaveError(); !error.empty())
        fStatus->SetText(("Could not save session: " + error).c_str());
}
void BrowserWindow::ShowError(const std::string& error)
{
    (new BAlert("Summit", error.c_str(), "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT))->Go(nullptr);
}
bool BrowserWindow::QuitRequested()
{
#if SUMMIT_MODERN_WEBKIT
    // The application decides: closing the last window quits Summit. It then
    // closes this window and drains queued WebKit destruction.
    BMessage request(kCloseWindowRequest);
    request.AddMessenger("window", BMessenger(this));
    be_app->PostMessage(&request);
    return false;
#else
    if (!fDownloads.empty()) {
        auto* prompt = new BAlert("Downloads", "Downloads are still in progress. Quit and cancel them?", "Keep Browsing", "Quit");
        if (prompt->Go() == 0) return false;
        for (auto* download : fDownloads) download->Cancel();
    }
    SaveSession();
    fSaveTimer.reset();
    SetCurrentWebView(nullptr);
    for (auto& tab : fTabs) { tab.view->RemoveSelf(); tab.view->Shutdown(); }
    fTabs.clear();
    be_app->PostMessage(B_QUIT_REQUESTED);
    return true;
#endif
}
// The window icon protocol of air/OS's Deskbar: the Deskbar asks each window
// for its "Icon" property and watches it for kWindowIconChanged notices, which
// carry the new icon the same way ("result", an archived BBitmap; none when
// the window has no icon of its own).
static const char* const kWindowIconProperty = "Icon";
static constexpr uint32 kWindowIconChanged = 'wicn';

BHandler* BrowserWindow::ResolveSpecifier(BMessage* message, int32 index, BMessage* specifier, int32 what,
    const char* property)
{
    if (what == B_DIRECT_SPECIFIER && property && !std::strcmp(property, kWindowIconProperty))
        return this;
    return BrowserWindowBase::ResolveSpecifier(message, index, specifier, what, property);
}

status_t BrowserWindow::GetSupportedSuites(BMessage* data)
{
    static property_info properties[] = {
        { const_cast<char*>(kWindowIconProperty), { B_GET_PROPERTY, 0 }, { B_DIRECT_SPECIFIER, 0 },
            const_cast<char*>("The site icon of the page shown, as an archived BBitmap."), 0, { }, { }, { } },
        { }
    };
    data->AddString("suites", "suite/vnd.Kunanyi-Summit-browser-window");
    BPropertyInfo info(properties);
    data->AddFlat("messages", &info);
    return BrowserWindowBase::GetSupportedSuites(data);
}

void BrowserWindow::AddWindowIcon(BMessage& message)
{
    auto* tab = ActiveTab();
    const BBitmap* icon = tab ? fFavicons->Icon(tab->url) : nullptr;
    BMessage archive;
    if (icon && icon->Archive(&archive) == B_OK)
        message.AddMessage("result", &archive);
}

void BrowserWindow::NoteWindowIcon()
{
    auto* tab = ActiveTab();
    const BBitmap* icon = tab ? fFavicons->Icon(tab->url) : nullptr;
    if (icon == fNotedWindowIcon && !fWindowIconStale)
        return;
    fNotedWindowIcon = icon;
    fWindowIconStale = false;
    BMessage notice;
    AddWindowIcon(notice);
    SendNotices(kWindowIconChanged, &notice);
}

void BrowserWindow::MessageReceived(BMessage* message)
{
    auto* tab = ActiveTab();
    switch (message->what) {
        case B_GET_PROPERTY: {
            int32 index;
            BMessage specifier;
            int32 form;
            const char* property = nullptr;
            if (message->GetCurrentSpecifier(&index, &specifier, &form, &property) == B_OK && property
                && !std::strcmp(property, kWindowIconProperty)) {
                BMessage reply(B_REPLY);
                AddWindowIcon(reply);
                reply.AddInt32("error", B_OK);
                message->SendReply(&reply);
                return;
            }
            BrowserWindowBase::MessageReceived(message);
            return;
        }
#if SUMMIT_MODERN_WEBKIT
        case B_WEBKIT_EXTENSION_ACTIONS_CHANGED: RefreshExtensionActions(); break;
        case B_WEBKIT_BROWSER_COMMAND: case B_WEBKIT_BROWSER_COMMAND_CANCELLED: BrowserCommand(*message); break;
        case B_WEBKIT_WEBSITE_DATA_SIZES: case B_WEBKIT_WEBSITE_DATA_REMOVED: BrowsingDataReply(*message); break;
        case B_WEBKIT_EXTENSION_ACTIONS: ExtensionActionsReceived(*message); break;
        case kActivateExtensionAction: ActivateExtensionAction(*message); break;
        case kPreloadExtensionAction: PreloadExtensionAction(*message); break;
        case kShowExtensionActions: ShowExtensionActions(); break;
        case kExtensionActionMenu: ShowExtensionActionMenu(*message); break;
        case B_WEBKIT_EXTENSION_ACTION_ACTIVATED:
            if (message->GetUInt64("identifier", 0) == fExtensionActionInvocation) {
                fExtensionActionResultIdentifier = fExtensionActionInvocation;
                fExtensionActionResultError = message->GetInt32("error", B_ERROR);
                if (fExtensionActionResultError != B_OK) {
                    fStatus->SetText("The extension action changed before it could open. Please try again.");
                    RefreshExtensionActions();
                }
            }
            break;
        case kRequestWindowClose:
            fQuittingApp = message->GetBool("quitting", false);
            BeginWindowClose();
            break;
        case kActivateWindow:
            if (IsMinimized()) Minimize(false);
            Activate(true);
            break;
        case B_WEBKIT_CLOSE_PROMPT: WebKitClosePrompt(*message); break;
        case B_WEBKIT_CLOSE_COMMITTED: WebKitCloseCommitted(*message); break;
        case B_WEBKIT_CLOSE_REQUESTED: case B_WEBKIT_CLOSE_CANCELLED:
            WebKitCloseResult(*message); break;
        case B_WEBKIT_EXTERNAL_NAVIGATION_REQUESTED: {
            // An extension page sent its tab to a web page (for example a sign-in page).
            BMessenger view;
            const char* url = nullptr;
            if (message->FindMessenger("view", &view) != B_OK || message->FindString("url", &url) != B_OK) break;
            if (auto* tab = FindTab(view); tab && !tab->closeRequested && !tab->closeQueued)
                ReplaceTabView(*tab, url);
            break;
        }
#endif
        case kNavigate: {
            const char* url = nullptr;
            const bool typed = message->FindString("url", &url) != B_OK;
            const std::string target = typed ? AddressTarget() : std::string(url);
            NavigationTrace("window handles the address: %s", target.c_str());
            HideSuggestions();
            if (auto* active = ActiveTab()) active->nextTransition = 2;
            Navigate(target);
            // As in other browsers, the page takes the focus once an address
            // is entered, so the field follows the page again.
            if (typed && fAddress->TextView()->IsFocus())
                if (auto* tab = ActiveTab()) tab->view->MakeFocus();
            break;
        }
        case kAddressModified: AddressModified(); break;
        case kTypeInAddress: {
            BMessage reply(B_REPLY);
            const char* enabled = std::getenv("SUMMIT_ENABLE_INPUT_SYNTHESIS");
            if (!enabled || std::strcmp(enabled, "1") != 0) {
                reply.AddString("error", "input synthesis is disabled");
            } else {
                auto* textView = fAddress->TextView();
                if (!textView->IsFocus()) fAddress->MakeFocus();
                if (message->GetBool("select_all", false)) textView->SelectAll();
                // Real key-down messages to the field, as the keyboard sends
                // them: the field's filters and BTextView handle them.
                auto press = [&](const std::string& bytes) {
                    BMessage key(B_KEY_DOWN);
                    key.AddInt64("when", system_time());
                    key.AddInt32("modifiers", 0);
                    key.AddString("bytes", bytes.c_str());
                    key.AddInt8("byte", bytes[0]);
                    key.AddInt32("raw_char", static_cast<uint8>(bytes[0]));
                    PostMessage(&key, textView);
                };
                const std::string text = message->GetString("text", "");
                for (size_t i = 0; i < text.size();) {
                    size_t length = 1;
                    const auto lead = static_cast<unsigned char>(text[i]);
                    if (lead >= 0xf0) length = 4; else if (lead >= 0xe0) length = 3; else if (lead >= 0xc0) length = 2;
                    press(text.substr(i, length));
                    i += length;
                }
                const std::string key = message->GetString("key", "");
                if (key == "down") press(std::string(1, B_DOWN_ARROW));
                else if (key == "up") press(std::string(1, B_UP_ARROW));
                else if (key == "enter") press(std::string(1, B_ENTER));
                else if (key == "escape") press(std::string(1, B_ESCAPE));
                else if (key == "backspace") press(std::string(1, B_BACKSPACE));
                reply.AddBool("queued", true);
            }
            message->SendReply(&reply);
            break;
        }
        case kAddressFocusCheck:
            if (!fAddress->TextView()->IsFocus()) HideSuggestions();
            break;
        case kSuggestionChosen: {
            const char* url = nullptr;
            if (message->FindString("url", &url) != B_OK || !fSuggestions->IsShowing()) break;
            const std::string target = url;
            HideSuggestions();
            if (auto* active = ActiveTab()) active->nextTransition = 2;
            Navigate(target);
            if (auto* tab = ActiveTab()) tab->view->MakeFocus();
            break;
        }
        case kNewTab: {
            const char* url = nullptr;
#if SUMMIT_MODERN_WEBKIT
            if (!fWebApp.empty()) {
                // A web app's window has no tabs: the page opens in an ordinary window.
                RequestNewWindow({ message->FindString("url", &url) == B_OK ? url : NewTabAddress() }, fPrivate);
                break;
            }
#endif
            // A new tab shows the home page; its address is selected, ready to type over.
            CreateTab(message->FindString("url", &url) == B_OK ? url : NewTabAddress());
            fAddress->MakeFocus();
            fAddress->TextView()->SelectAll();
            break;
        }
        case kNewWindow: RequestNewWindow({ }, false); break;
        case kNewPrivateWindow: RequestNewWindow({ }, true); break;
        case kCloseWindow: PostMessage(B_QUIT_REQUESTED); break;
        case kTabMenu: ShowTabMenu(*message); break;
        case kReloadTab: case kDuplicateTab: case kMoveTabToNewWindow: case kCloseOtherTabs: {
            int64 id;
            if (message->FindInt64("id", &id) != B_OK) id = fSelected;
            auto* target = FindTabByID(id);
            if (!target) break;
            if (message->what == kReloadTab) {
#if SUMMIT_MODERN_WEBKIT
                if (target->processExited || target->crashView) { ReloadCrashedTab(*target); break; }
                if (target->errorView) { RetryFailedLoad(*target); break; }
                if (!PrepareTabNavigation(*target)) break;
                if (!target->deferredURL.empty()) {
                    target->view->LoadURL(std::exchange(target->deferredURL, {}).c_str());
                    fWebKitContext->SetBrowserTabDiscarded(target->view, false);
                    break;
                }
#endif
                target->view->Reload();
            } else if (message->what == kDuplicateTab) {
                int32 index = 0;
                for (size_t i = 0; i < fTabs.size(); ++i) if (fTabs[i].id == id) index = int32(i) + 1;
#if SUMMIT_MODERN_WEBKIT
                CreateTab(target->url, true, nullptr, index);
#else
                CreateTab(target->url, true);
#endif
            } else if (message->what == kMoveTabToNewWindow) {
                // The page opens afresh in the new window; its back list stays behind.
                if (fTabs.size() < 2) break;
                RequestNewWindow({ target->url }, fPrivate);
                CloseTab(id);
            } else {
                std::vector<int64> others;
                for (const auto& page : fTabs) if (page.id != id) others.push_back(page.id);
                SelectTab(id);
                for (auto other : others) CloseTab(other);
            }
            break;
        }
        case kSelectTab: case kCloseTab: {
            int64 id;
            if (message->FindInt64("id", &id) != B_OK) id = fSelected;
            if (message->what == kSelectTab) SelectTab(id); else CloseTab(id); break;
        }
        case kBack:
#if SUMMIT_MODERN_WEBKIT
            // From an error page to the page before it, which is still there.
            if (tab && tab->errorView && LeaveLoadErrorView(*tab)) break;
            if (!tab || !tab->back || !PrepareNavigation()) break;
#endif
            if (tab) tab->view->GoBack();
            break;
        case kForward:
#if SUMMIT_MODERN_WEBKIT
            if (!tab || !tab->forward || !PrepareNavigation()) break;
#endif
            if (tab) tab->view->GoForward();
            break;
        case kReload: if (tab) {
#if SUMMIT_MODERN_WEBKIT
            if (tab->processExited || tab->crashView) { ReloadCrashedTab(*tab); break; }
            if (tab->errorView && !tab->loading) { RetryFailedLoad(*tab); break; }
            if (!tab->loading && !PrepareNavigation()) break;
#endif
            if (tab->loading) {
#if SUMMIT_MODERN_WEBKIT
                tab->view->Stop();
#else
                tab->view->StopLoading();
#endif
            } else {
                tab->nextTransition = 9;
                tab->view->Reload();
            }
        } break;
        case kHome: Navigate(HomeAddress()); break;
        case kFocusAddress: fAddress->MakeFocus(); fAddress->TextView()->SelectAll(); break;
#if SUMMIT_MODERN_WEBKIT
        case kShowTabOfView: {
            BMessenger view;
            if (message->FindMessenger("view", &view) != B_OK) break;
            if (auto* tab = FindTab(view)) {
                SelectTab(tab->id);
                Activate();
            }
            break;
        }
#endif
        case kShowBookmarks: ShowBookmarkQuery(0); break;
        case kShowHistory: ShowInternalPage(kHistoryPage); break;
        case kShowExtensions: be_app->PostMessage(kShowExtensions); break;
#if SUMMIT_MODERN_WEBKIT
        case kShowCertificate: ShowCertificate(); break;
        case kToggleReader: ToggleReader(); break;
        case kTranslatePage: {
            auto* tab = message->HasInt64("tab") ? FindTabByID(message->GetInt64("tab", 0)) : ActiveTab();
            if (!tab) break;
            if (message->GetBool("offer", false)) {
                tab->translation.dismissed = false;
                if (!tab->translation.active) ProbeTranslation(*tab, true);
                else RefreshNoticeBar();
            } else StartTranslation(*tab);
            break;
        }
        case kRestoreTranslation: {
            if (auto* tab = FindTabByID(message->GetInt64("tab", fSelected))) RestoreTranslation(*tab);
            break;
        }
        case kDismissTranslation: {
            if (auto* tab = FindTabByID(message->GetInt64("tab", fSelected))) tab->translation.dismissed = true;
            RefreshNoticeBar(); break;
        }
        case kTranslationStatus: TranslationStatus(*message); break;
        case kTranslationTick: TranslationTick(); break;
        case B_WEBKIT_JAVASCRIPT_RESULT:
            if (message->GetUInt64("identifier", 0) == kBookmarkletQuery) {
                BMessenger view;
                if (tab && message->FindMessenger("view", &view) == B_OK && view == BMessenger(tab->view)
                    && message->GetUInt64("generation", 0) == tab->loadGeneration)
                    fStatus->SetText(message->GetString("error", "Bookmarklet finished."));
                break;
            }
            if (TranslationResult(*message)) break;
            if (message->GetUInt64("identifier", 0) == kWebAppQuery) {
                InstallWebAppResult(*message);
                break;
            }
            if (message->GetUInt64("identifier", 0) == kPictureInPictureQuery) {
                // window.__summitPipLargest(): the page's largest video.
                const char* token = nullptr;
                if (message->FindString("result", &token) == B_OK && token && *token) StartPictureInPicture(token);
                else fStatus->SetText("This page has no video for Picture in Picture.");
                break;
            }
            ReaderResult(*message);
            break;
        case B_WEBKIT_SCRIPT_MESSAGE: PageScriptMessage(*message); break;
        case B_WEBKIT_RESPONSIVENESS_CHANGED: ResponsivenessChanged(*message); break;
        case B_WEBKIT_PRINT_REQUESTED: {
            BMessenger view;
            if (message->FindMessenger("view", &view) == B_OK && FindTab(view) == ActiveTab())
                PrintPage(message->GetUInt64("frame_identifier", 0), message->GetUInt64("generation", 0));
            break;
        }
        case B_WEBKIT_PRINT_PAGES: PrintPagesReceived(*message); break;
        case kPrint: PrintPage(); break;
        case kPageSetup: PrintJob::ConfigurePage(); break;
        case kSaveAsPDF: SaveAsPDF(); break;
        case kSaveAsPDFChosen: SaveAsPDFChosen(*message); break;
        case kPrintPagesRequest: {
            BMessenger reply;
            auto* target = FindTabByID(message->GetInt64("tab", 0));
            if (message->FindMessenger("reply", &reply) != B_OK) break;
            if (!target || target->processExited || target->loading || target->loadGeneration != message->GetUInt64("generation", 0)) {
                BMessage failed(B_WEBKIT_PRINT_PAGES);
                failed.AddUInt64("identifier", message->GetUInt64("identifier", 0));
                failed.AddString("error", "The document changed or the tab closed.");
                reply.SendMessage(&failed);
                break;
            }
            BSize paper;
            BRect margins;
            message->FindSize("paper", &paper);
            message->FindRect("margins", &margins);
            target->view->DrawFramePagesForPrinting(reply, message->GetUInt64("identifier", 0),
                message->GetUInt64("frame_identifier", 0), message->GetUInt64("generation", 0), paper.width, paper.height, margins,
                1, message->GetInt32("first", 0), message->GetInt32("count", 1), message->GetFloat("scale", 2), false);
            break;
        }
        case kPictureInPicture: {
            if (!tab || tab->processExited) break;
            const char* token = nullptr;
            if (message->FindString("token", &token) == B_OK && token && *token) StartPictureInPicture(token);
            else tab->view->EvaluateJavaScript("window.__summitPipLargest ? window.__summitPipLargest() : ''",
                BMessenger(this), kPictureInPictureQuery);
            break;
        }
        case kPictureInPictureCommand: PictureInPictureCommand(*message); break;
        case kToggleTabMute: ToggleTabMute(message->GetInt64("id", fSelected)); break;
        case kCheckMemory: UnloadTabsIfMemoryIsLow(); break;
        case kDiscardTab:
            if (const auto error = DiscardTab(message->GetInt64("id", 0)); !error.empty()) fStatus->SetText(error.c_str());
            break;
        case kPinTab: SetTabPinned(message->GetInt64("id", fSelected), message->GetBool("pinned", true)); break;
        case kMoveTab: MoveTabTo(message->GetInt64("id", 0), static_cast<size_t>(std::max<int32>(0, message->GetInt32("index", 0)))); break;
        case kTabDragOut: {
            BPoint where;
            if (message->FindPoint("where", &where) == B_OK) TabDraggedOut(message->GetInt64("id", 0), where);
            break;
        }
        case kDetachTab: {
            BMessenger target;
            if (message->FindMessenger("target", &target) == B_OK) DetachTab(message->GetInt64("id", 0), target);
            break;
        }
        case kAdoptTab: AdoptTab(*message); break;
        case kCloseTabsToRight: {
            const int64 id = message->GetInt64("id", fSelected);
            std::vector<int64> right;
            bool after = false;
            for (const auto& candidate : fTabs) {
                if (after) right.push_back(candidate.id);
                if (candidate.id == id) after = true;
            }
            for (auto close : right) CloseTab(close);
            break;
        }
        case kOpenBlockedPopup: {
            auto* target = FindTabByID(message->GetInt64("tab", 0));
            if (!target) break;
            const auto url = std::exchange(target->blockedPopup, {});
            target->blockedPopups = 0;
            RefreshNoticeBar();
            if (!message->GetBool("dismiss", false) && !url.empty()) {
                int32 index = 0;
                for (size_t i = 0; i < fTabs.size(); ++i) if (fTabs[i].id == target->id) index = int32(i) + 1;
                CreateTab(url, true, nullptr, index);
            }
            break;
        }
        case kAllowSitePopups: {
            auto* target = FindTabByID(message->GetInt64("tab", 0));
            if (!target) break;
            const auto url = target->url;
            const auto scheme = url.find("://");
            const std::string origin = scheme == std::string::npos ? std::string() : url.substr(0, url.find_first_of("/?#", scheme + 3));
            if (fPrivate) {
                // A private window's choice lasts as long as it does.
                fWebKitContext->SetSitePermission("popups", origin.c_str(), 1);
            } else {
                BMessage change(kSitePermissionChange);
                change.AddString("permission", "popups");
                change.AddString("origin", origin.c_str());
                change.AddInt32("state", 1);
                be_app->PostMessage(&change);
            }
            const auto popup = std::exchange(target->blockedPopup, {});
            target->blockedPopups = 0;
            RefreshNoticeBar();
            if (!popup.empty()) CreateTab(popup, true);
            break;
        }
        case kWaitForPage: case kStopPage: {
            auto* target = FindTabByID(message->GetInt64("tab", 0));
            if (!target) break;
            if (message->what == kStopPage) target->view->TerminateWebProcess();
            else target->waitedForPage = true;
            RefreshNoticeBar();
            break;
        }
        case kSpellingCommand: SpellingCommand(*message); break;
#if SUMMIT_MODERN_WEBKIT
        case kToggleSidebar: {
            const std::string identifier = message->GetString("identifier", "");
            if (identifier == fSidebarExtension) { CloseSidebar(); break; }
            const auto [url, title] = SidebarPage(identifier);
            BMessage open(kOpenSidebarOnApp);
            open.AddMessenger("window", BMessenger(this));
            open.AddString("identifier", identifier.c_str());
            open.AddString("url", url.empty() ? message->GetString("url", "") : url.c_str());
            open.AddString("title", title.empty() ? message->GetString("title", "") : title.c_str());
            be_app->PostMessage(&open);
            break;
        }
        case kCloseSidebar: CloseSidebar(); break;
        case kToggleCaretBrowsing:
        case kToggleReducedMotion:
        case kToggleIncreasedContrast:
        case kToggleOverrideColors: {
            // One saved setting for the whole browser (#118): every window
            // applies it when the profile tells it of the change.
            const uint32 what = message->what;
            fShared->Change([what](Profile& profile) -> uint32 {
                bool& aid = what == kToggleCaretBrowsing ? profile.caretBrowsing : what == kToggleReducedMotion ? profile.reduceMotion
                    : what == kToggleIncreasedContrast ? profile.increaseContrast : profile.overrideColors;
                aid = !aid;
                return SharedProfile::kSettingsChanged;
            });
            break;
        }
#endif
        case kToggleZoomTextOnly:
            fShared->Change([](Profile& profile) -> uint32 {
                profile.zoomTextOnly = !profile.zoomTextOnly;
                return SharedProfile::kSettingsChanged;
            }, BMessenger(this));
            break;
        case kToggleSpellChecking: {
            const bool enabled = !fShared->Read([](const Profile& profile) { return profile.spellChecking; });
            BMessage changed(kPreferencesChanged);
            changed.AddBool("spell_checking", enabled);
            be_app->PostMessage(&changed);
            break;
        }
        case kPasswordBarAnswer: PasswordBarAnswer(*message); break;
        case kRetryFailedLoad:
            if (auto* target = FindTabByID(message->GetInt64("tab", 0))) RetryFailedLoad(*target);
            break;
        case kCheckNetwork: CheckNetworkForErrorPages(); break;
        case kWindowNotice: case kDismissWindowNotice:
            fWindowNotice = message->what == kWindowNotice ? message->GetString("text", "") : "";
            RefreshNoticeBar();
            break;
        case kDismissSoftwareRenderingNotice:
            fSoftwareRenderingNoticePending = false;
            RefreshNoticeBar();
            break;
        case kBookmarkAllTabs: {
            // A folder named after the day, in Other Bookmarks.
            char name[64] = "Tabs";
            const std::time_t now = std::time(nullptr);
            if (std::tm local; localtime_r(&now, &local)) std::strftime(name, sizeof name, "Tabs %Y-%m-%d %H:%M", &local);
            size_t added = 0;
            fShared->Change([&](Profile& profile) -> uint32 {
                const int64 folder = profile.bookmarks.AddFolder(kOtherBookmarksFolder, name);
                if (!folder) return 0;
                for (const auto& page : fTabs)
                    if (IsBookmarkableURL(page.url) && profile.bookmarks.Add({page.url, page.title}, folder)) ++added;
                if (!added) profile.bookmarks.Remove(folder);
                return SharedProfile::kBookmarksChanged;
            });
            fStatus->SetText(added ? (std::to_string(added) + " tabs bookmarked in Other Bookmarks › " + name).c_str()
                : "None of these tabs can be bookmarked.");
            break;
        }
        case kSelectTabNumber: {
            const int32 number = message->GetInt32("number", 0);
            if (number >= 9 && !fTabs.empty()) SelectTab(fTabs.back().id);
            else if (number >= 1 && size_t(number) <= fTabs.size()) SelectTab(fTabs[number - 1].id);
            break;
        }
        case kInstallWebApp:
            if (tab && (tab->url.rfind("https://", 0) == 0 || tab->url.rfind("http://", 0) == 0) && !tab->processExited)
                tab->view->EvaluateJavaScript(kWebAppPageScript, BMessenger(this), kWebAppQuery, true);
            else ShowError("Only web pages can be installed as apps.");
            break;
        case kWebAppInstalled: WebAppInstalled(*message); break;
        case kOpenInBrowser:
            if (tab) {
                RequestNewWindow({ tab->url }, fPrivate);
                if (!fWebApp.empty() && tab->back) tab->view->GoBack();
            }
            break;
        case kPasswordChecked: {
            auto* target = FindTabByID(message->GetInt64("tab", 0));
            if (!target || message->GetBool("same", false)) break;
            target->pendingPassword = Tab::PendingPassword { message->GetString("origin", ""), message->GetString("username", ""),
                message->GetString("password", ""), message->GetBool("update", false) };
            RefreshNoticeBar();
            break;
        }
        case B_WEBKIT_AUTHENTICATION_REQUESTED: AuthenticationRequested(*message); break;
        case kAuthenticationAccounts: AuthenticationAccounts(*message); break;
        case kAuthenticationAnswer: AuthenticationAnswered(*message); break;
        case B_WEBKIT_PASSKEY_REQUESTED: PasskeyRequested(*message); break;
        case kPasskeyAnswer: PasskeyAnswered(*message); break;
        case kPasswordSaved: {
            const std::string error = message->GetString("error", "");
            if (!error.empty()) ShowError("Could not save the password: " + error);
            else fStatus->SetText("The password was saved.");
            break;
        }
        case B_WEBKIT_URL_SCHEME_REQUEST: ReaderResource(*message); break;
        case B_WEBKIT_URL_SCHEME_REQUEST_STOPPED: break;
        case kExtensionStoreInstall: {
            const auto item = fExtensionsEnabled && tab ? ParseExtensionStoreURL(tab->url) : std::nullopt;
            if (!item) break;
            if (const auto identifier = StoreItemIdentifier(*item); fShared->InstalledExtensions().contains(identifier)) {
                BMessage show(kShowExtensions);
                show.AddString("extension_identifier", identifier.c_str());
                be_app->PostMessage(&show);
                break;
            }
            BMessage request(kExtensionStoreInstall);
            request.AddMessenger("window", BMessenger(this));
            request.AddString("url", tab->url.c_str());
            be_app->PostMessage(&request);
            break;
        }
#endif
        case kOpenBookmark: {
            const char* url = nullptr;
            if (message->FindString("url", &url) != B_OK || !url) break;
            if (IsBookmarkQueryURL(url)) {
                const auto parent = message->GetInt64("query_parent", 0);
                const auto section = message->GetUInt32("query_section", 0);
                if (parent || section) {
                    const auto target = fShared->Read([&](const Profile& profile) {
                        return ResolveBookmarkQueryTarget(profile.bookmarks, profile.history, parent, section);
                    });
                    if (parent && section && target.error.empty() && target.node.url == url) ShowBookmarkQuery(parent, section);
                    else fStatus->SetText("This smart folder has been removed or changed.");
                    break;
                }
                const auto id = fShared->Read([&](const Profile& profile) {
                    const auto* node = profile.bookmarks.Find(message->GetInt64("id", 0));
                    return node && node->url == url ? node->id : int64_t(0);
                });
                if (id) ShowBookmarkQuery(id); else Navigate(url);
                break;
            }
            if (IsBookmarkletURL(url)) {
                // This message is also used by history and tab-strip drops.
                // Only a deliberately saved bookmark may run a script.
                if (fShared->Read([&](const Profile& profile) { return profile.bookmarks.Contains(url); }))
                    RunBookmarklet(url);
                break;
            }
            if (message->GetBool("new_tab", false)) CreateTab(url);
            else {
                // The same message opens history entries and dropped links.
                if (auto* active = ActiveTab(); active && fShared->Read([&](const Profile& profile) { return profile.bookmarks.Contains(url); }))
                    active->nextTransition = 3;
                Navigate(url);
            }
            break;
        }
        case kBookmarkButton: ShowBookmarkMenu(); break;
        case kBookmark: case kAddToBookmarksBar: case kRemoveFromBookmarksBar: case kRemoveBookmark: {
            // From a bookmarks bar menu for that bookmark, otherwise for the current page.
            const char* url = nullptr;
            const std::string target = message->FindString("url", &url) == B_OK && url ? url : tab ? tab->url : "";
            if (!Bookmarkable(target)) break;
            const std::string title = tab && tab->url == target ? tab->title : "";
            const uint32 what = message->what;
            const char* status = nullptr;
            // Every window hears of the change (ProfileChanged) and redraws its bar and star.
            fShared->Change([&](Profile& profile) -> uint32 {
                auto& bookmarks = profile.bookmarks;
                if (what == kRemoveBookmark) {
                    if (!profile.RemoveBookmark(target)) return 0;
                    status = "Bookmark removed";
                } else if (what == kRemoveFromBookmarksBar) {
                    // Off the bar, into Other Bookmarks.
                    int64 id = 0;
                    for (const auto& child : bookmarks.Bar().children)
                        if (!child.folder && child.url == target) { id = child.id; break; }
                    if (!id || !bookmarks.Move(id, kOtherBookmarksFolder, kBookmarkEnd)) return 0;
                    status = "Moved to Other Bookmarks";
                } else {
                    const bool bar = what == kAddToBookmarksBar;
                    if (bookmarks.Contains(target) && (!bar || bookmarks.OnBar(target))) {
                        status = bar ? "This page is already on the bookmarks bar" : "This page is already bookmarked";
                        return 0;
                    }
                    profile.AddBookmark({target, title}, bar);
                    if (!bookmarks.Contains(target)) { status = "You have reached the bookmark limit."; return 0; }
                    status = bar ? "Added to the bookmarks bar" : "Bookmark saved";
                }
                return SharedProfile::kBookmarksChanged;
            }, BMessenger(this));
            if (status) fStatus->SetText(status);
            break;
        }
        case kBookmarkLink: {
            const char* url = nullptr;
            const char* title = "";
            if (message->FindString("url", &url) != B_OK || !Bookmarkable(url)) break;
            message->FindString("title", &title);
            fShared->Change([&](Profile& profile) -> uint32 {
                if (profile.bookmarks.Contains(url)) return 0;
                profile.AddBookmark({url, title}, false);
                return profile.bookmarks.Contains(url) ? SharedProfile::kBookmarksChanged : 0;
            }, BMessenger(this));
            fStatus->SetText("Bookmark saved");
            break;
        }
        case kToggleBookmarksBar: SetBookmarksBarVisible(!fBookmarksBarVisible); break;
        case kEditBookmark: {
            int64 id = message->GetInt64("id", 0);
            if (!id && tab) id = fShared->Read([&](const Profile& profile) {
                const auto* node = profile.bookmarks.FindURL(tab->url);
                return node ? node->id : int64(0);
            });
            OpenBookmarkEditor(id);
            break;
        }
        case kNewBookmarkFolder: OpenNewBookmarkEditor(message->GetInt64("folder", kOtherBookmarksFolder), true); break;
        case kAddBookmarkToFolder: OpenNewBookmarkEditor(kOtherBookmarksFolder, false); break;
        case kBookmarkEdited: BookmarkEdited(*message); break;
        case kDeleteBookmarkNode: DeleteBookmark(message->GetInt64("id", 0), true); break;
        case kDeleteBookmarkReply:
            if (message->GetInt32("which", 0) == 1) DeleteBookmark(message->GetInt64("id", 0), false);
            break;
        case kMoveBookmark: case kDropBookmark: {
            // From the bookmarks bar: one of its buttons, or a page dropped on it.
            const int64 folder = message->GetInt64("folder", kBookmarksBarFolder);
            const size_t index = message->HasInt64("index") ? size_t(std::max<int64>(0, message->GetInt64("index", 0))) : kBookmarkEnd;
            const std::string url = message->GetString("url", "");
            const std::string title = message->GetString("title", "");
            const int64 id = message->GetInt64("id", 0);
            const uint32 what = message->what;
            if (what == kDropBookmark && !Bookmarkable(url)) break;
            const char* status = nullptr;
            fShared->Change([&](Profile& profile) -> uint32 {
                auto& bookmarks = profile.bookmarks;
                int64 moving = id;
                // A page already bookmarked moves to where it was dropped.
                if (what == kDropBookmark)
                    if (const auto* existing = bookmarks.FindURL(url)) moving = existing->id;
                if (moving) return bookmarks.Move(moving, folder, index) ? SharedProfile::kBookmarksChanged : 0;
                if (!bookmarks.Add({url, title.empty() ? url : title}, folder, index, std::time(nullptr))) {
                    status = "You have reached the bookmark limit.";
                    return 0;
                }
                status = "Bookmark saved";
                return SharedProfile::kBookmarksChanged;
            }, BMessenger(this));
            if (status) fStatus->SetText(status);
            break;
        }
        case kOpenBookmarkFolder: {
            const int64 id = message->GetInt64("id", 0);
            const auto urls = fShared->Read([&](const Profile& profile) {
                std::vector<std::string> urls;
                if (const auto* folder = profile.bookmarks.Find(id)) {
                    const auto children = IsBookmarkQueryURL(folder->url)
                        ? QueryBookmarks(folder->url, profile.bookmarks, profile.history, folder->origin).items : folder->children;
                    for (const auto& child : children)
                        if (!child.folder && !child.separator && !IsBookmarkletURL(child.url) && !IsBookmarkQueryURL(child.url) && urls.size() < 50) urls.push_back(child.url);
                } else if (!id) {
                    const char* url = nullptr;
                    for (int32 i = 0; i < 50 && message->FindString("url", i, &url) == B_OK; ++i)
                        if (IsBookmarkableURL(url) && !IsBookmarkletURL(url) && !IsBookmarkQueryURL(url)) urls.push_back(url);
                }
                return urls;
            });
            for (size_t i = 0; i < urls.size(); ++i) CreateTab(urls[i], i == 0);
            break;
        }
        case kImportBookmarks: ShowBookmarkImportWindow(fShared, Frame()); break;
        case kExportBookmarks: ExportBookmarks(); break;
        case kExportBookmarksChosen: ExportBookmarksTo(*message); break;
        case kClearHistory: {
            auto* alert = new BAlert("Clear History", "Remove every page from your history?\n\nBookmarks and open tabs are kept.",
                "Cancel", "Clear History", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
            alert->SetShortcut(0, B_ESCAPE);
            alert->Go(new BInvoker(new BMessage(kClearHistoryReply), this));
            break;
        }
#if SUMMIT_MODERN_WEBKIT
        case kCertificateDecision: CertificateDecision(*message); break;
#endif
        case kClearHistoryReply:
            if (message->GetInt32("which", 0) != 1) break;
            fShared->ClearHistory(BMessenger(this));
            fStatus->SetText("History cleared");
            break;
        case kShowPreferences: be_app->PostMessage(kShowPreferences); break;
#if SUMMIT_MODERN_WEBKIT
        case kCheckForUpdates: be_app->PostMessage(kCheckForUpdates); break;
#endif
        case kPreferencesUseCurrentPage:
            // From the application's Preferences window, for the front window's page.
            if (tab && Bookmarkable(tab->url)) {
                const std::string url = tab->url;
                fShared->Change([&](Profile& profile) -> uint32 {
                    if (profile.homeURL == url) return 0;
                    profile.homeURL = url;
                    return SharedProfile::kSettingsChanged;
                }, BMessenger(this));
            } else fStatus->SetText("This page cannot be used as the home page.");
            break;
        case kProfileChanged: ProfileChanged(*message); break;
        case kFind: if (fFindBar->IsHidden(fFindBar)) fFindBar->Show(); fFindText->MakeFocus(); fFindText->TextView()->SelectAll(); break;
        case kCloseFind:
            if (!fFindBar->IsHidden(fFindBar)) fFindBar->Hide();
            if (tab) {
#if SUMMIT_MODERN_WEBKIT
                tab->view->HideFindUI();
                fStatus->SetText(tab->loading ? "Loading…" : "Ready");
#endif
                tab->view->MakeFocus();
            }
            break;
        case kFindNext: case kFindPrevious:
            if (tab) {
                tab->view->FindString(fFindText->Text(), message->what == kFindNext);
#if SUMMIT_MODERN_WEBKIT
                fStatus->SetText(*fFindText->Text() ? "Searching…" : "Ready");
#endif
            }
            break;
        case kZoomIn: ChangeZoom(1); break;
        case kZoomOut: ChangeZoom(-1); break;
        case kZoomReset: ChangeZoom(0); break;
        case kSaveSession:
            SaveSession();
#if SUMMIT_MODERN_WEBKIT
            // Checked with each session save (every five seconds): pages of a
            // window whose daemon is gone draw through app_server again.
            if (!fDirectDaemonLostHandled && DirectDaemonLost()) {
                fDirectDaemonLostHandled = true;
                std::fprintf(stderr, "Summit: the window's direct daemon is gone; pages draw through app_server\n");
                BWebKitView::WindowDirectConnected(this, nullptr);
            }
#endif
            break;
        case kNextTab: case kPreviousTab:
            for (size_t i = 0; i < fTabs.size(); ++i) if (fTabs[i].id == fSelected) {
                SelectTab(fTabs[(i + (message->what == kNextTab ? 1 : fTabs.size() - 1)) % fTabs.size()].id); break;
            } break;
        case kReopenTab:
            if (!fClosedTabs.empty()) {
                auto page = fClosedTabs.back();
                fClosedTabs.pop_back();
#if SUMMIT_MODERN_WEBKIT
                ForgetClosedSessionTab(page.url);
#endif
                CreateTab(page.url);
            } break;
        case kOpenFile:
            if (!fOpenPanel) {
                BMessenger target(this);
                fOpenPanel = std::make_unique<BFilePanel>(B_OPEN_PANEL, &target);
            }
            fOpenPanel->Show(); break;
        case B_SIMPLE_DATA:
            // Files dropped on the tab strip or the toolbar (a page takes its
            // own drops) open in new tabs, as in other browsers.
            if (!message->WasDropped() || !message->HasRef("refs")) {
                BrowserWindowBase::MessageReceived(message);
                break;
            }
            [[fallthrough]];
        case B_REFS_RECEIVED: {
            entry_ref ref;
            for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; ++i)
                if (const auto url = RefURL(ref); !url.empty()) CreateTab(url);
            break;
        }
#if SUMMIT_MODERN_WEBKIT
        case kToggleDownloadsPanel: ToggleDownloadsPanel(); break;
        case kDownloadsPanelClosed: {
            BMessenger panel;
            if (message->FindMessenger("panel", &panel) != B_OK || panel != fDownloadsPanel) break;
            fDownloadsPanel = {};
            if (message->GetBool("deactivated", false)) fDownloadsPanelClosedAt = system_time();
            break;
        }
        case kDownloadsChanged: RefreshDownloadsButton(); break;
        case kDownloadRetry: {
            const char* url = nullptr;
            auto* active = ActiveTab();
            if (message->FindString("url", &url) != B_OK || !url || !*url || !active) break;
            active->view->DownloadURL(url);
            break;
        }
        case kStoreItemResolved: StoreItemResolved(*message); break;
#endif
        case kShowDownloads: {
            BPath path;
            std::string error;
            if (!FindDownloads(path, error)) { ShowError(error); break; }
            entry_ref ref;
            status_t status = get_ref_for_path(path.Path(), &ref);
            if (status == B_OK) status = be_roster->Launch(&ref);
            // Tracker receives the folder reference even when it is already running.
            if (status != B_OK && status != B_ALREADY_RUNNING)
                ShowError("Could not open Downloads: " + std::string(std::strerror(status)));
            break;
        }
#if !SUMMIT_MODERN_WEBKIT
        case B_DOWNLOAD_ADDED: {
            BWebDownload* download = nullptr;
            if (message->FindPointer("download", reinterpret_cast<void**>(&download)) != B_OK || !download) break;
            BPath path;
            std::string error;
            if (!FindDownloads(path, error)) {
                download->Cancel();
                ShowError(error);
                break;
            }
            fDownloads.push_back(download);
            download->SetProgressListener(BMessenger(this));
            download->Start(path);
            fStatus->SetText("Downloading to your Downloads folder…"); break;
        }
        case B_DOWNLOAD_PROGRESS: {
            int64 current = 0; message->FindInt64("current size", &current);
            std::string status = "Downloaded " + std::to_string(current / 1024) + " KiB";
            fStatus->SetText(status.c_str()); break;
        }
        case B_DOWNLOAD_REMOVED: {
            BWebDownload* download = nullptr;
            message->FindPointer("download", reinterpret_cast<void**>(&download));
            fDownloads.erase(std::remove(fDownloads.begin(), fDownloads.end(), download), fDownloads.end());
            int32 status = -1;
            if (message->FindInt32("status", &status) != B_OK) status = -1;
            switch (status) {
                case B_DOWNLOAD_FINISHED: fStatus->SetText("Download complete — open Downloads to view the file"); break;
                case B_DOWNLOAD_FAILED: fStatus->SetText("Download failed"); break;
                case B_DOWNLOAD_BLOCKED: fStatus->SetText("Download blocked"); break;
                case B_DOWNLOAD_CANNOT_SHOW_URL: fStatus->SetText("This address could not be downloaded"); break;
                default: fStatus->SetText("Download ended — open Downloads to view the file"); break;
            }
            message->SendReply(B_REPLY); break;
        }
#else
        case B_WEBKIT_DOWNLOAD_STARTED: {
            uint64 identifier = 0;
            if (message->FindUInt64("identifier", &identifier) != B_OK || !identifier) break;
            fDownloads.insert(identifier);
            if (const char* url = nullptr; message->FindString("url", &url) == B_OK)
                if (auto target = fSaveAsTargets.find(url); target != fSaveAsTargets.end()) {
                    fSaveAsDownloads[identifier] = target->second;
                    fSaveAsTargets.erase(target);
                }
            // A download manager extension turned this off (downloads.setUiOptions).
            if (fShared->DownloadUIHidden()) break;
            fStatus->SetText("Downloading to your Downloads folder…");
            // As in Firefox, a download started here shows the panel.
            if (IsActive() && !fDownloadsPanel.IsValid() && fDownloadsButton
                && !fDownloadsButton->IsHidden(fDownloadsButton))
                ToggleDownloadsPanel(true);
            break;
        }
        case B_WEBKIT_DOWNLOAD_PROGRESS: {
            if (fShared->DownloadUIHidden()) break;
            uint64 current = 0;
            message->FindUInt64("current_size", &current);
            std::string status = "Downloaded " + std::to_string(current / 1024) + " KiB";
            // A resumed download whose server sent the whole file again.
            if (message->GetBool("restarted", false))
                status = "The server could not resume the download, so it started over — " + status;
            fStatus->SetText(status.c_str());
            break;
        }
        case B_WEBKIT_DOWNLOAD_FINISHED: {
            uint64 identifier = 0;
            uint32 result = B_WEBKIT_DOWNLOAD_FAILED;
            if (message->FindUInt64("identifier", &identifier) != B_OK || !identifier) break;
            fDownloads.erase(identifier);
            message->FindUInt32("result", &result);
            if (fSaveAsDownloads.contains(identifier)) DownloadFinishedForSave(*message);
            else if (fShared->DownloadUIHidden()) break;
            else if (result == B_WEBKIT_DOWNLOAD_SUCCEEDED)
                fStatus->SetText("Download complete — open Downloads to view the file");
            else if (result == B_WEBKIT_DOWNLOAD_CANCELLED)
                fStatus->SetText("Download cancelled");
            else if (result == B_WEBKIT_DOWNLOAD_PAUSED)
                fStatus->SetText("Download paused — resume it in the downloads panel");
            else {
                const char* description = nullptr;
                message->FindString("error_description", &description);
                std::string status = "Download failed";
                if (description && *description) status += ": " + std::string(description);
                fStatus->SetText(status.c_str());
            }
            break;
        }
        case B_WEBKIT_SCHEME_OPEN_REQUESTED:
        case B_WEBKIT_SCHEME_REGISTRATION: {
            BMessenger view;
            if (message->FindMessenger("view", &view) != B_OK) break;
            auto* tab = FindTab(view);
            if (!tab || tab != ActiveTab()) break;
            const std::string source = message->GetString("source_url", "");
            if (message->what == B_WEBKIT_SCHEME_OPEN_REQUESTED && message->GetBool("main_frame", true)
                && BookmarkPageRequest(*tab, source, message->GetString("url", "")))
                break;
            // An app link's own checks are in SchemeOpenRequested: a redirect
            // reports the page it left as committed_url.
            if (source != tab->url && message->what != B_WEBKIT_SCHEME_OPEN_REQUESTED) break;
            if (message->what == B_WEBKIT_SCHEME_OPEN_REQUESTED) {
                SchemeOpenRequested(*message, *tab);
                break;
            }
            fProtocolHandlers->Register(message->GetString("scheme", ""), message->GetString("url", ""),
                message->GetBool("remove", false), tab->id, source);
            break;
        }
        case B_WEBKIT_FULLSCREEN_REQUESTED: FullScreenRequested(*message); break;
        case B_WEBKIT_WEBAUTHN_UNHANDLED: {
            // The page asked for a passkey, no extension answered, and
            // Summit's own passkeys could not (turned off with
            // SUMMIT_PLATFORM_PASSKEYS=0, or the site wants a security key),
            // so the page saw an error.
            BMessenger messenger;
            if (message->FindMessenger("view", &messenger) != B_OK) break;
            auto* tab = FindTab(messenger);
            if (!tab || tab->id != fSelected) break;
            std::string notice = message->GetBool("create", false)
                ? "This page tried to create a passkey, but neither Summit nor a password manager could."
                : "This page asked for a passkey, but neither Summit nor a password manager answered.";
            notice += " Security keys (USB) are not supported yet; an extension such as 1Password provides its own passkeys";
            notice += fPrivate ? ", and must be allowed in private windows (Extensions)." : ".";
            // Shown for a while: the page's own reaction (a title or a reload)
            // would otherwise put "Ready" back at once.
            tab->notice = notice;
            tab->noticeUntil = system_time() + 30 * 1000000LL;
            ShowTabStatus(*tab);
            std::fprintf(stderr, "Summit: %s\n", notice.c_str());
            break;
        }
        case 'fsnh':
            if (fFullScreenTab && !fFullScreenNotice->IsHidden(fFullScreenNotice))
                fFullScreenNotice->Hide();
            break;
        case kKeepScreenAwake:
            KeepScreenAwake();
            break;
        case B_WEBKIT_STATE_CHANGED: case B_WEBKIT_PROCESS_EXITED:
            WebKitStateChanged(*message);
            break;
        case B_WEBKIT_FIND_RESULT:
            WebKitFindResult(*message);
            break;
        case B_WEBKIT_ICON_LOADED:
            IconLoaded(*message);
            break;
        case B_WEBKIT_CONTEXT_MENU: ShowPageContextMenu(*message); break;
        case B_WEBKIT_CONTEXT_MENU_EXTENSIONS: RefreshPageMenuExtensions(*message); break;
        case kPageMenuOpened: PageMenuOpened(*message); break;
        case kPageMenuClosed: PageMenuClosed(*message); break;
        case B_WEBKIT_NEW_PAGE_REQUESTED: NewPageRequested(*message); break;
        case B_WEBKIT_LINK_OPEN_REQUESTED: LinkOpenRequested(*message); break;
        case B_WEBKIT_LINK_HOVERED: LinkHovered(*message); break;
        case kOpenLink: case kOpenLinkInNewTab: case kOpenLinkInNewWindow: case kOpenLinkInNewPrivateWindow:
        case kDownloadLink: case kSaveLinkAs: case kSearchFor: {
            const char* url = nullptr;
            if (message->FindString("url", &url) != B_OK || !*url) break;
            if (message->what == kOpenLink) Navigate(url);
            else if (message->what == kOpenLinkInNewTab) CreateTab(url, false, nullptr, BackgroundTabIndex());
            else if (message->what == kOpenLinkInNewWindow) RequestNewWindow({ url }, fPrivate);
            else if (message->what == kOpenLinkInNewPrivateWindow) RequestNewWindow({ url }, true);
            // Selected text is searched for even when it looks like an address.
            else if (message->what == kSearchFor) CreateTab(SearchURL(url), true, nullptr, BackgroundTabIndex());
            else if (message->what == kDownloadLink) {
                if (tab) tab->view->DownloadURL(url);
                fStatus->SetText("Downloading to your Downloads folder…");
            } else SaveLinkAs(url, message->GetString("filename", ""));
            break;
        }
        case kSaveLinkAsChosen: {
            // The save panel's answer: a folder and a name for fSavePanelURL.
            entry_ref directory;
            const char* name = nullptr;
            if (message->FindRef("directory", &directory) != B_OK || message->FindString("name", &name) != B_OK
                || fSavePanelURL.empty() || !tab) break;
            BPath path(&directory);
            if (path.InitCheck() != B_OK || path.Append(name) != B_OK) break;
            fSaveAsTargets[fSavePanelURL] = path.Path();
            tab->view->DownloadURL(fSavePanelURL.c_str());
            fStatus->SetText(("Saving " + std::string(name) + "…").c_str());
            fSavePanelURL.clear();
            break;
        }
        case kExtensionMenuItem:
            BWebKitView::PerformContextMenuExtensionItem(message->GetUInt64("token", 0));
            break;
        case kShowDeveloperTools: ShowDeveloperTools(message->GetString("panel", nullptr), message); break;
        case kViewPageSource: ShowPageSource(); break;
        case kDeveloperToolsClosed: {
            BMessenger window;
            if (message->FindMessenger("window", &window) != B_OK) break;
            for (auto& open : fTabs)
                if (open.devTools == window) {
                    open.devTools = BMessenger();
                    open.devToolsDocked = false;
                }
            PlaceDockedDeveloperTools();
            break;
        }
        case kDockDeveloperTools: {
            const int64 id = message->GetInt64("tab", -1);
            const bool docked = message->GetBool("docked", false);
            for (auto& open : fTabs) {
                if (open.id != id || !open.devTools.IsValid()) continue;
                open.devToolsDocked = docked;
                open.devToolsDockSide = message->GetInt32("side", 0) == 1 ? 1 : 0;
                // A window let go is told so; a docked one where it goes.
                if (!docked) {
                    BMessage place(kDeveloperToolsDock);
                    place.AddBool("docked", false);
                    open.devTools.SendMessage(&place);
                }
            }
            PlaceDockedDeveloperTools();
            break;
        }
        case kDockSlotChanged: PlaceDockedDeveloperTools(); break;
        case kCopyText: {
            const char* text = nullptr;
            if (message->FindString("text", &text) != B_OK || !be_clipboard->Lock()) break;
            be_clipboard->Clear();
            if (BMessage* clip = be_clipboard->Data())
                clip->AddData("text/plain", B_MIME_TYPE, text, std::strlen(text));
            be_clipboard->Commit();
            be_clipboard->Unlock();
            break;
        }
#endif
        case B_ABOUT_REQUESTED: {
#if SUMMIT_MODERN_WEBKIT
            // The packages (and commit) say which build this is.
            std::string info = "Summit — a web browser for air/OS and Haiku\nBuilt for " SUMMIT_TARGET_NAME " ("
                + std::string(PackageArchitecture()) + ")\n\n" + BuildDescription()
                + "\n\nWebKit " + std::string(BWebKitVersion()) + "\nHaiku port " + BWebKitPortVersion();
#else
            std::string info = "Summit — a web browser for air/OS and Haiku\nBuilt for " SUMMIT_TARGET_NAME
                "\nDevelopment build\n\nWebKit "
                + std::string(WebKitInfo::WebKitVersion().String()) + "\nHaiku port "
                + WebKitInfo::HaikuWebKitVersion().String();
#endif
            (new BAlert("About Summit", info.c_str(), "OK"))->Go(nullptr); break;
        }
        case B_CUT: case B_COPY: case B_PASTE: case B_SELECT_ALL: case B_UNDO: case B_REDO:
#if SUMMIT_MODERN_WEBKIT
            if (tab && (CurrentFocus() == tab->view || message->GetBool("page", false))) {
                tab->view->ExecuteEditCommand(message->what);
                break;
            }
#endif
            if (CurrentFocus()) PostMessage(message, CurrentFocus());
            break;
#if SUMMIT_MODERN_WEBKIT
        case kMcpRequest: {
            BMessage reply(B_REPLY);
            reply.AddString("reply", McpRequest(message->GetString("request", "{}")).c_str());
            message->SendReply(&reply);
            break;
        }
#endif
        case kSimulateScroll: {
            BMessage reply(B_REPLY);
            SimulateScroll(*message, reply);
            message->SendReply(&reply);
            break;
        }
        case kFrameStats: {
            BMessage reply(B_REPLY);
#if SUMMIT_MODERN_WEBKIT
            if (tab) {
                if (auto* statsView = dynamic_cast<FrameStatsWebKitView*>(tab->view))
                    statsView->AppendFrameStats(reply);
            }
            if (fScrollBurstCompletionFrameStatsAvailable)
                reply.AddMessage("at_scroll_completion", &fScrollBurstCompletionFrameStats);
#endif
            message->SendReply(&reply);
            break;
        }
        case kScrollBurstCompleted:
            fScrollBurstSent = message->GetInt32("sent", 0);
            fScrollBurstStatus = message->GetInt32("status", B_ERROR);
            fScrollBurstDuration = message->GetInt64("duration_us", 0);
#if SUMMIT_MODERN_WEBKIT
            if (tab) {
                if (auto* statsView = dynamic_cast<FrameStatsWebKitView*>(tab->view)) {
                    fScrollBurstCompletionFrameStats.MakeEmpty();
                    fScrollBurstCompletionFrameStatsAvailable = statsView->AppendFrameStats(fScrollBurstCompletionFrameStats);
                }
            }
#endif
            fScrollBurstActive = false;
            break;
        case kBrowserState: {
            BMessage reply(B_REPLY);
            reply.AddInt32("count", fTabs.size()); reply.AddInt64("selected", fSelected);
            reply.AddBool("scroll_active", fScrollBurstActive);
            reply.AddInt32("scroll_requested", fScrollBurstRequested);
            reply.AddInt32("scroll_sent", fScrollBurstSent);
            reply.AddInt32("scroll_status", fScrollBurstStatus);
            reply.AddInt64("scroll_duration_us", fScrollBurstDuration);
            reply.AddString("address", fAddress->Text());
            {
                int32 start = 0, end = 0;
                fAddress->TextView()->GetSelection(&start, &end);
                reply.AddInt32("address_selection_start", start);
                reply.AddInt32("address_selection_end", end);
                reply.AddBool("address_focused", fAddress->TextView()->IsFocus());
                reply.AddString("address_typed", fAddressTyped.c_str());
                reply.AddString("autofill_url", fAutofill.url.c_str());
                reply.AddBool("suggestions_showing", fSuggestions->IsShowing());
                reply.AddInt32("suggestion_selected", fSuggestions->Selected());
                for (const auto& row : fSuggestions->Rows()) {
                    reply.AddString("suggestion_title", row.title.c_str());
                    reply.AddString("suggestion_url", row.url.c_str());
                }
            }
            reply.AddString("status", fStatus->Text());
            reply.AddInt64("now", system_time());
            reply.AddRect("frame", Frame());
#if SUMMIT_MODERN_WEBKIT
            reply.AddString("backend", "modern");
            reply.AddBool("fullscreen", fFullScreenTab != 0);
            reply.AddBool("private", fPrivate);
            reply.AddInt32("download_count", fDownloads.size());
            reply.AddUInt64("extension_action_snapshot", fExtensionActionSnapshot);
            reply.AddUInt64("extension_action_result_identifier", fExtensionActionResultIdentifier);
            reply.AddInt32("extension_action_result_error", fExtensionActionResultError);
            if (fExtensionActionSnapshot)
                for (const auto& action : fExtensionActionState) reply.AddMessage("extension_action", &action);
            reply.AddBool("closing", fClosingWindow || fWindowCloseQueued || fCloseCommitPending
                || std::any_of(fTabs.begin(), fTabs.end(), [](const Tab& tab) {
                    return tab.closeRequested || tab.closeQueued || tab.closeApproved;
                }));
            reply.AddBool("engine_version_available", true);
            reply.AddString("webkit", BWebKitVersion());
            reply.AddString("haiku_webkit", BWebKitPortVersion());
            reply.AddString("webkit_revision", BWebKitSourceRevision());
#else
            reply.AddString("backend", "legacy");
            reply.AddString("webkit", WebKitInfo::WebKitVersion());
            reply.AddString("haiku_webkit", WebKitInfo::HaikuWebKitVersion());
#endif
            for (const auto& page : fTabs) {
                BMessage item; item.AddInt64("id", page.id); item.AddString("url", page.url.c_str());
                item.AddString("title", page.title.c_str()); item.AddBool("loading", page.loading);
#if SUMMIT_MODERN_WEBKIT
                item.AddDouble("pageZoom", page.pageZoom);
                item.AddString("zoomKey", page.zoomKey.c_str());
                if (page.devTools.IsValid()) item.AddMessenger("devtools", page.devTools);
                item.AddDouble("textZoom", page.textZoom);
                item.AddBool("loadError", !page.loadError.empty());
                item.AddString("loadErrorText", page.loadError.c_str());
                item.AddString("loadErrorDescription", page.loadErrorDescription.c_str());
                item.AddString("loadErrorDomain", page.loadErrorDomain.c_str());
                item.AddString("loadErrorURL", page.loadErrorURL.c_str());
                item.AddInt32("loadErrorCode", page.loadErrorCode);
                item.AddBool("loadErrorProvisional", page.loadErrorProvisional);
                item.AddUInt64("loadGeneration", page.loadGeneration);
                item.AddBool("readerAvailable", page.readerAvailable);
                item.AddBool("readerActive", page.readerActive);
                item.AddBool("readerBusy", page.readerExtracting);
                item.AddString("loadOutcome", page.loadOutcome.c_str());
                item.AddUInt64("loadSuccessSequence", page.loadSuccessSequence);
                item.AddInt64("loadStartedAt", page.loadStartedAt);
                item.AddInt64("loadFinishedAt", page.loadFinishedAt);
                if (auto* stats = dynamic_cast<FrameStatsWebKitView*>(page.view)) {
                    item.AddInt64("shownAt", stats->ShownAt());
                    item.AddInt64("firstFrameAfterShow", stats->FirstFrameAfterShow());
                }
#endif
                reply.AddMessage("tab", &item);
            }
            message->SendReply(&reply); break;
        }
        default: BrowserWindowBase::MessageReceived(message); break;
    }
}
#if SUMMIT_MODERN_WEBKIT
int32 BrowserWindow::BackgroundTabIndex()
{
    int32 active = -1;
    for (size_t i = 0; i < fTabs.size(); ++i) if (fTabs[i].id == fSelected) active = int32(i);
    fBackgroundInsert = std::min<int32>(std::max(fBackgroundInsert, active) + 1, int32(fTabs.size()));
    return fBackgroundInsert;
}

static std::string ShortLabel(const std::string& text, size_t limit = 32)
{
    std::string label;
    for (char character : text) {
        if (static_cast<unsigned char>(character) < 0x20) character = ' ';
        if (character == ' ' && (label.empty() || label.back() == ' ')) continue;
        label += character;
    }
    while (!label.empty() && label.back() == ' ') label.pop_back();
    if (label.size() <= limit) return label;
    size_t cut = limit;
    while (cut && (static_cast<unsigned char>(label[cut]) & 0xC0) == 0x80) --cut;
    return label.substr(0, cut) + "…";
}

static std::string FileNameFor(const std::string& url, const std::string& suggested)
{
    if (!suggested.empty()) return suggested;
    std::string path = url.substr(0, url.find_first_of("?#"));
    if (auto slash = path.find_last_of('/'); slash != std::string::npos) path = path.substr(slash + 1);
    return path.empty() ? "download" : path;
}

void BrowserWindow::ShowPageContextMenu(const BMessage& message)
{
    BMessenger sender;
    BPoint where;
    if (message.FindMessenger("view", &sender) != B_OK || message.FindPoint("where", &where) != B_OK) return;
    auto* tab = FindTab(sender);
    if (!tab) return;
    if (tab->id != fSelected) {
        tab->view->ContextMenuClosed();
        return;
    }
    const std::string link = message.GetString("link_url", "");
    const std::string image = message.GetString("image_url", "");
    const std::string media = message.GetString("media_url", "");
    const std::string selection = message.GetString("selected_text", "");
    const bool editable = message.GetBool("editable", false);
    auto withURL = [](uint32 what, const std::string& url, const char* filename = nullptr) {
        auto* invocation = new BMessage(what);
        invocation->AddString("url", url.c_str());
        if (filename) invocation->AddString("filename", filename);
        return invocation;
    };
    auto copy = [](const std::string& text) {
        auto* invocation = new BMessage(kCopyText);
        invocation->AddString("text", text.c_str());
        return invocation;
    };
    if (IsClosing()) return;
    auto* menu = new CancellableMenu("page", fMenusCancelled);
    auto separate = [menu] { if (menu->CountItems() && !dynamic_cast<BSeparatorItem*>(menu->ItemAt(menu->CountItems() - 1))) menu->AddSeparatorItem(); };
    const bool web = [](const std::string& url) {
        return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0 || url.rfind("file:", 0) == 0
            || url.rfind("data:", 0) == 0 || url.rfind("blob:", 0) == 0;
    }(link);
    if (message.GetBool("misspelled", false)) {
        // Spelling suggestions for the word under the pointer (#66).
        const char* guess = nullptr;
        int32 guesses = 0;
        for (; message.FindString("spelling_guess", guesses, &guess) == B_OK && guesses < 6; ++guesses) {
            auto* replace = new BMessage(kSpellingCommand);
            replace->AddUInt32("command", B_WEBKIT_SPELLING_REPLACE);
            replace->AddString("word", guess);
            menu->AddItem(new BMenuItem(guess, replace));
        }
        if (!guesses) {
            auto* none = new BMenuItem("No Guesses Found", nullptr);
            none->SetEnabled(false);
            menu->AddItem(none);
        }
        menu->AddSeparatorItem();
        auto spelling = [](uint32 command) {
            auto* invocation = new BMessage(kSpellingCommand);
            invocation->AddUInt32("command", command);
            return invocation;
        };
        menu->AddItem(new BMenuItem("Ignore Spelling", spelling(B_WEBKIT_SPELLING_IGNORE)));
        menu->AddItem(new BMenuItem("Add to Dictionary", spelling(B_WEBKIT_SPELLING_LEARN)));
        menu->AddSeparatorItem();
    }
    if (!link.empty()) {
        const bool script = link.rfind("javascript:", 0) == 0;
        auto* open = new BMenuItem("Open Link", withURL(kOpenLink, link));
        open->SetEnabled(!script);
        menu->AddItem(open);
        auto* tabItem = new BMenuItem("Open Link in New Tab", withURL(kOpenLinkInNewTab, link));
        tabItem->SetEnabled(!script);
        menu->AddItem(tabItem);
        // A private window's links open in private windows.
        auto* windowItem = new BMenuItem(fPrivate ? "Open Link in New Private Window" : "Open Link in New Window",
            withURL(kOpenLinkInNewWindow, link));
        windowItem->SetEnabled(!script);
        menu->AddItem(windowItem);
        if (!fPrivate) {
            auto* privateItem = new BMenuItem("Open Link in New Private Window", withURL(kOpenLinkInNewPrivateWindow, link));
            privateItem->SetEnabled(!script);
            menu->AddItem(privateItem);
        }
        menu->AddSeparatorItem();
        const std::string filename = FileNameFor(link, message.GetString("link_filename", ""));
        auto* download = new BMenuItem("Download Linked File", withURL(kDownloadLink, link));
        download->SetEnabled(web);
        menu->AddItem(download);
        auto* saveAs = new BMenuItem("Save Linked File As…", withURL(kSaveLinkAs, link, filename.c_str()));
        saveAs->SetEnabled(web);
        menu->AddItem(saveAs);
        menu->AddItem(new BMenuItem("Copy Link", copy(link)));
        if (Bookmarkable(link)) {
            auto* bookmark = withURL(kBookmarkLink, link);
            bookmark->AddString("title", ShortLabel(message.GetString("link_label", ""), 200).c_str());
            menu->AddItem(new BMenuItem("Bookmark Link", bookmark));
        }
    }
    if (!image.empty()) {
        separate();
        menu->AddItem(new BMenuItem("Open Image in New Tab", withURL(kOpenLinkInNewTab, image)));
        const std::string filename = FileNameFor(image, message.GetString("image_filename", ""));
        menu->AddItem(new BMenuItem("Save Image As…", withURL(kSaveLinkAs, image, filename.c_str())));
        menu->AddItem(new BMenuItem("Download Image", withURL(kDownloadLink, image)));
        menu->AddItem(new BMenuItem("Copy Image Address", copy(image)));
    }
    if (!tab->pipToken.empty()) {
        // The page's script named the video under the pointer (#74).
        separate();
        auto* picture = new BMessage(kPictureInPicture);
        picture->AddString("token", tab->pipToken.c_str());
        menu->AddItem(new BMenuItem("Picture in Picture", picture));
        tab->pipToken.clear();
    }
    if (!media.empty() && media.rfind("blob:", 0) != 0) {
        // Streamed (Media Source) video has a blob: address nothing else can open.
        separate();
        menu->AddItem(new BMenuItem("Open Media in New Tab", withURL(kOpenLinkInNewTab, media)));
        menu->AddItem(new BMenuItem("Save Media As…", withURL(kSaveLinkAs, media, FileNameFor(media, "").c_str())));
        menu->AddItem(new BMenuItem("Copy Media Address", copy(media)));
    }
    // Editing commands go to the page even when another view has the focus.
    auto edit = [menu](const char* label, uint32 what) {
        auto* invocation = new BMessage(what);
        invocation->AddBool("page", true);
        menu->AddItem(new BMenuItem(label, invocation));
    };
    if (editable) {
        separate();
        edit("Undo", B_UNDO);
        edit("Redo", B_REDO);
        menu->AddSeparatorItem();
        edit("Cut", B_CUT);
        edit("Copy", B_COPY);
        edit("Paste", B_PASTE);
        edit("Select All", B_SELECT_ALL);
    } else if (!selection.empty()) {
        separate();
        menu->AddItem(new BMenuItem("Copy", copy(selection)));
    }
    if (!selection.empty()) {
        const std::string label = std::string("Search ") + CurrentSearchEngine().name + " for “" + ShortLabel(selection) + "”";
        menu->AddItem(new BMenuItem(label.c_str(), withURL(kSearchFor, ShortLabel(selection, 400))));
    }
    if (link.empty() && image.empty() && media.empty() && selection.empty() && !editable) {
        // The page itself.
        auto* back = new BMenuItem("Back", new BMessage(kBack));
        back->SetEnabled(tab->back);
        menu->AddItem(back);
        auto* forward = new BMenuItem("Forward", new BMessage(kForward));
        forward->SetEnabled(tab->forward);
        menu->AddItem(forward);
        menu->AddItem(new BMenuItem(tab->loading ? "Stop" : "Reload", new BMessage(kReload)));
        menu->AddSeparatorItem();
        auto* bookmark = new BMenuItem("Bookmark This Page", new BMessage(kBookmark));
        bookmark->SetEnabled(Bookmarkable(tab->url));
        menu->AddItem(bookmark);
        auto* copyAddress = new BMenuItem("Copy Page Address", copy(tab->url));
        copyAddress->SetEnabled(!DisplayURL(tab->url).empty());
        menu->AddItem(copyAddress);
        edit("Select All", B_SELECT_ALL);
        menu->AddSeparatorItem();
        menu->AddItem(new BMenuItem("Print…", new BMessage(kPrint)));
        menu->AddItem(new BMenuItem("View Page Source", new BMessage(kViewPageSource)));
        menu->AddSeparatorItem();
        menu->AddItem(new BMenuItem("Developer Tools", new BMessage(kShowDeveloperTools)));
    }
    // The element under the pointer, in the Developer Tools.
    separate();
    auto* inspect = new BMessage(kShowDeveloperTools);
    inspect->AddString("panel", "elements");
    inspect->AddPoint("inspect", where);
    menu->AddItem(new BMenuItem("Inspect Element", inspect));
    // Extensions' items come last, so menus.refresh can replace them in the
    // menu's own thread (the window learns its messenger when it opens).
    const int32 extensionsStart = menu->CountItems();
    AddExtensionMenuItems(menu, message, this);
    fPageMenu = BMessenger();
    fPageMenuTab = tab->id;
    fPageMenuShowing = true;
    fPendingMenuRefresh = BMessage();
    BMessage opened(kPageMenuOpened), closed(kPageMenuClosed);
    opened.AddUInt64("serial", ++fPageMenuSerial);
    closed.AddUInt64("serial", fPageMenuSerial);
    menu->SetNotices(BMessenger(this), opened, closed);
    menu->SetRefreshHook([extensionsStart, target = BMessenger(this)](BMenu* open, const BMessage& refreshed) {
        while (open->CountItems() > extensionsStart) delete open->RemoveItem(extensionsStart);
        BLooper* looper = nullptr;
        auto* handler = target.Target(&looper);
        AddExtensionMenuItems(open, refreshed, handler ? handler : looper);
    });
    menu->SetTargetForItems(this);
    menu->SetAsyncAutoDestruct(true);
    menu->Go(tab->view->ConvertToScreen(where), true, false, true);
}

void BrowserWindow::AddExtensionMenuItems(BMenu* menu, const BMessage& message, BHandler* target)
{
    // Items extensions add with the menus API, under the extension's name
    // unless it has just one (as Safari shows them).
    auto separate = [menu] { if (menu->CountItems() && !dynamic_cast<BSeparatorItem*>(menu->ItemAt(menu->CountItems() - 1))) menu->AddSeparatorItem(); };
    std::function<void(BMenu*, const BMessage&)> addExtensionItems = [&](BMenu* into, const BMessage& from) {
        BMessage entry;
        for (int32 i = 0; from.FindMessage("item", i, &entry) == B_OK; ++i) {
            if (entry.GetBool("separator", false)) { into->AddSeparatorItem(); continue; }
            const std::string title = ShortLabel(entry.GetString("title", ""), 60);
            BMenuItem* item;
            if (entry.HasMessage("item")) {
                auto* submenu = new BMenu(title.c_str());
                addExtensionItems(submenu, entry);
                item = new BMenuItem(submenu);
            } else {
                auto* invocation = new BMessage(kExtensionMenuItem);
                invocation->AddUInt64("token", entry.GetUInt64("token", 0));
                item = new BMenuItem(title.c_str(), invocation);
                item->SetTarget(target);
                item->SetMarked(entry.GetBool("checkable", false) && entry.GetBool("checked", false));
            }
            item->SetEnabled(entry.GetBool("enabled", true));
            into->AddItem(item);
        }
    };
    BMessage extension;
    for (int32 i = 0; message.FindMessage("extension", i, &extension) == B_OK; ++i) {
        separate();
        BMessage only;
        const bool single = extension.FindMessage("item", 1, &only) != B_OK && extension.FindMessage("item", 0, &only) == B_OK
            && !only.HasMessage("item");
        if (single) addExtensionItems(menu, extension);
        else {
            auto* submenu = new BMenu(ShortLabel(extension.GetString("name", "Extension"), 40).c_str());
            addExtensionItems(submenu, extension);
            menu->AddItem(new BMenuItem(submenu));
        }
    }
    for (int32 i = 0; i < menu->CountItems(); ++i)
        if (!menu->ItemAt(i)->Target()) menu->ItemAt(i)->SetTarget(target);
}

void BrowserWindow::RefreshPageMenuExtensions(const BMessage& message)
{
    // menus.refresh while the page's menu is open: its extension items
    // again, rebuilt by the menu itself (a gone menu's messenger fails).
    BMessenger sender;
    auto* tab = message.FindMessenger("view", &sender) == B_OK ? FindTab(sender) : nullptr;
    if (!tab || fPageMenuTab != tab->id || !fPageMenuShowing) return;
    BMessage refresh(message);
    refresh.what = CancellableMenu::kMenuRefresh;
    // onShown listeners usually answer before the menu has opened: the
    // refresh waits for it (PageMenuOpened).
    if (!fPageMenu.IsValid()) fPendingMenuRefresh = refresh;
    else fPageMenu.SendMessage(&refresh);
}

void BrowserWindow::PageMenuOpened(const BMessage& message)
{
    if (message.GetUInt64("serial", 0) != fPageMenuSerial || message.FindMessenger("menu", &fPageMenu) != B_OK) return;
    if (fPendingMenuRefresh.what) fPageMenu.SendMessage(&fPendingMenuRefresh);
    fPendingMenuRefresh = BMessage();
}

void BrowserWindow::PageMenuClosed(const BMessage& message)
{
    if (message.GetUInt64("serial", 0) != fPageMenuSerial) return;
    fPageMenu = BMessenger();
    fPageMenuShowing = false;
    fPendingMenuRefresh = BMessage();
    if (auto* tab = FindTabByID(fPageMenuTab)) tab->view->ContextMenuClosed();
}

// The inspector's name for the tab's page, which a session that is not the
// page's first is not told.
static std::string PageTarget(uint64 pageIdentifier)
{
    return pageIdentifier ? "page-" + std::to_string(pageIdentifier) : std::string();
}

void BrowserWindow::ShowDeveloperTools(const char* panel, const BMessage* request)
{
    auto* tab = ActiveTab();
    if (!tab || !tab->view || fClosingWindow || tab->closeQueued) return;
    BMessage show(kShowDeveloperTools);
    if (panel) show.AddString("panel", panel);
    BPoint where;
    if (request && request->FindPoint("inspect", &where) == B_OK) {
        // In the page view's pixels; the tools need the page's.
        show.AddPoint("inspect", where);
        show.AddDouble("zoom", tab->pageZoom);
    }
    if (request && request->GetBool("pick", false)) show.AddBool("pick", true);
    if (tab->devTools.IsValid()) {
        tab->devTools.SendMessage(&show);
        return;
    }
    auto* window = new DevToolsWindow(BMessenger(this), tab->id, tab->title, Frame());
    // The window hears of the page's messages, so it is made first; it reads
    // them only once it runs.
    window->SetInspector(BWebKitInspectorSession::Create(*tab->view, BMessenger(window), static_cast<uint64>(tab->id)),
        PageTarget(tab->pageIdentifier));
    tab->devTools = BMessenger(window);
    if (!show.IsEmpty()) window->PostMessage(&show);
    window->Show();
    // Docked if the tools were last docked, where they were.
    tab->devToolsDocked = DevToolsWindow::DockedByDefault();
    tab->devToolsDockSide = DevToolsWindow::DockSideByDefault();
    PlaceDockedDeveloperTools();
}

void BrowserWindow::ShowPageSource()
{
    auto* tab = ActiveTab();
    if (!tab || !tab->view || fClosingWindow || tab->closeQueued) return;
    auto* window = new SourceWindow("Source of " + tab->url, Frame());
    window->FetchPageSource(BWebKitInspectorSession::Create(*tab->view, BMessenger(window), static_cast<uint64>(tab->id)),
        PageTarget(tab->pageIdentifier));
    window->Show();
}

// The dock under the pages is shown while the selected tab's tools are
// docked; those tools are placed over it, and other tabs' docked tools are
// hidden until their tab is selected.
void BrowserWindow::PlaceDockedDeveloperTools()
{
    if (!fDock || !fSideDock || !fPageSplit || !fSideSplit || !fContentSplit) return;
    const auto* active = ActiveTab();
    const bool show = active && active->devTools.IsValid() && active->devToolsDocked && !fClosingWindow && !IsHidden() && !IsMinimized();
    const int32 side = show ? active->devToolsDockSide : -1;
    // The slot of the side the tools are on is shown, the other hidden.
    bool changed = false;
    if (side == 0 && fDock->IsHidden(fDock)) {
        fDock->Show();
        // Room for the tools: about two fifths of the page's height.
        fPageSplit->SetItemWeight(0, 0.6f, false);
        fPageSplit->SetItemWeight(1, 0.4f, true);
        changed = true;
    } else if (side != 0 && !fDock->IsHidden(fDock)) {
        fDock->Hide();
        changed = true;
    }
    if (side == 1 && fSideDock->IsHidden(fSideDock)) {
        fSideDock->Show();
        changed = true;
    } else if (side != 1 && !fSideDock->IsHidden(fSideDock)) {
        fSideDock->Hide();
        changed = true;
    }
    if (changed) {
        UpdateSplitters();
        Layout(true);
    }
    BView* slot = side == 1 ? fSideDock : fDock;
    for (auto& tab : fTabs) {
        if (!tab.devTools.IsValid() || !tab.devToolsDocked) continue;
        BMessage place(kDeveloperToolsDock);
        const bool here = show && &tab == active;
        place.AddBool("docked", true);
        place.AddBool("shown", here);
        place.AddInt32("side", tab.devToolsDockSide);
        place.AddPointer("owner", this);
        if (here) place.AddRect("frame", slot->ConvertToScreen(slot->Bounds()));
        tab.devTools.SendMessage(&place);
    }
}

void BrowserWindow::CloseDeveloperTools(Tab& tab, bool wait)
{
    if (tab.devTools.IsValid()) {
        tab.devTools.SendMessage(B_QUIT_REQUESTED);
        for (int i = 0; wait && i < 200 && tab.devTools.IsValid(); ++i) snooze(10000);
    }
    tab.devTools = BMessenger();
}

void BrowserWindow::SaveLinkAs(const std::string& url, const std::string& filename)
{
    if (!fSavePanel) {
        BMessenger target(this);
        fSavePanel = std::make_unique<BFilePanel>(B_SAVE_PANEL, &target, nullptr, 0, false, new BMessage(kSaveLinkAsChosen));
        BPath downloads;
        std::string error;
        if (FindDownloads(downloads, error)) fSavePanel->SetPanelDirectory(downloads.Path());
    }
    fSavePanelURL = url;
    fSavePanel->SetSaveText(filename.c_str());
    fSavePanel->Window()->SetTitle("Summit: Save As");
    fSavePanel->Show();
}

void BrowserWindow::DownloadFinishedForSave(const BMessage& message)
{
    const uint64 identifier = message.GetUInt64("identifier", 0);
    const auto target = fSaveAsDownloads[identifier];
    fSaveAsDownloads.erase(identifier);
    if (message.GetUInt32("result", B_WEBKIT_DOWNLOAD_FAILED) != B_WEBKIT_DOWNLOAD_SUCCEEDED) {
        fStatus->SetText("The file could not be saved.");
        return;
    }
    // Downloads land in the Downloads folder; move the file where it was asked for.
    const std::filesystem::path source = message.GetString("path", "");
    std::error_code error;
    if (source.empty()) error = std::make_error_code(std::errc::no_such_file_or_directory);
    else if (std::filesystem::equivalent(source, target, error) && !error) {
        fStatus->SetText(("Saved " + std::filesystem::path(target).filename().string()).c_str());
        return;
    }
    if (!source.empty()) {
        error.clear();
        std::filesystem::rename(source, target, error);
        if (error) {
            // Another volume: copy, then remove the download.
            error.clear();
            std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, error);
            if (!error) std::filesystem::remove(source, error);
        }
    }
    if (error) fStatus->SetText(("Could not save the file: " + error.message()).c_str());
    else fStatus->SetText(("Saved " + std::filesystem::path(target).filename().string()).c_str());
}

void BrowserWindow::NewPageRequested(const BMessage& message)
{
    const uint64 identifier = message.GetUInt64("identifier", 0);
    if (!identifier) return;
    BMessenger sender;
    message.FindMessenger("view", &sender);
    auto* opener = FindTab(sender);
    // The sidebar's pages open theirs as the shown tab would.
    if (!opener && fSidebarView && sender == BMessenger(fSidebarView)) opener = ActiveTab();
    const std::string url = message.GetString("url", "about:blank");
    const int32 modifiers = message.GetInt32("modifiers", 0);
    const int32 button = message.GetInt32("button", 0);
    if (!opener || fClosingWindow) { BWebKitView::DeclineNewPage(identifier); return; }
    // A web app's window has no tabs: what its pages open goes in a window.
    if (message.GetBool("popup", false) || (modifiers & B_SHIFT_KEY && !(modifiers & B_COMMAND_KEY)) || !fWebApp.empty()) {
        // A sized pop-up (sign-in and payment windows) or Shift-click: its own window.
        BMessage request(kNewWindow);
        request.AddUInt64("new_page", identifier);
        bool sized = false;
        request.AddString("new_page_url", url.c_str());
        // The page belongs to the opener's context, so it opens in a window of the same kind.
        request.AddBool("private", fPrivate);
        BRect frame = Frame().OffsetByCopy(24, 24);
        float width, height;
        if (message.FindFloat("width", &width) == B_OK && message.FindFloat("height", &height) == B_OK) {
            // Room for the page plus the window's own bars: this window's,
            // less those a pop-up does not have or show yet.
            float chrome = Bounds().Height() - fPages->Bounds().Height();
            for (BView* bar : std::initializer_list<BView*>{fBookmarksBar, fNoticeBar, fFindBar})
                if (bar && !bar->IsHidden(bar)) chrome -= bar->Bounds().Height() + 1;
            // A BRect's width is one pixel short of the pixels it covers.
            frame.right = frame.left + std::clamp(width, 200.0f, 4000.0f) - 1;
            frame.bottom = frame.top + std::clamp(height, 150.0f, 4000.0f) - 1 + chrome;
            sized = message.GetBool("popup", false);
            float x, y;
            if (message.FindFloat("x", &x) == B_OK && message.FindFloat("y", &y) == B_OK) frame.OffsetTo(x, y);
        }
        request.AddRect("frame", frame);
        request.AddBool("popup", sized);
        if (be_app->PostMessage(&request) != B_OK) BWebKitView::DeclineNewPage(identifier);
        return;
    }
    // target=_blank and window.open(): a tab after the opener, in front unless
    // opened with the middle button or Command.
    const bool background = button == B_TERTIARY_MOUSE_BUTTON || (modifiers & B_COMMAND_KEY);
    int32 index = -1;
    for (size_t i = 0; i < fTabs.size(); ++i) if (fTabs[i].id == opener->id) index = int32(i) + 1;
    if (background && opener->id == fSelected) index = BackgroundTabIndex();
    CreateTab(url, !background, nullptr, index, 0, 0, identifier);
}

void BrowserWindow::LinkOpenRequested(const BMessage& message)
{
    const char* url = nullptr;
    if (message.FindString("url", &url) != B_OK || !*url) return;
    const int32 modifiers = message.GetInt32("modifiers", 0);
    // Shift brings the new tab to the front, as in other browsers.
    if (modifiers & B_SHIFT_KEY) {
        int32 index = -1;
        for (size_t i = 0; i < fTabs.size(); ++i) if (fTabs[i].id == fSelected) index = int32(i) + 1;
        CreateTab(url, true, nullptr, index);
    } else CreateTab(url, false, nullptr, BackgroundTabIndex());
}

void BrowserWindow::LinkHovered(const BMessage& message)
{
    BMessenger sender;
    if (message.FindMessenger("view", &sender) != B_OK) return;
    auto* tab = FindTab(sender);
    if (!tab || tab->id != fSelected) return;
    fHoveredLink = message.GetString("url", "");
    // The status bar shows where a link goes, as in WebPositive.
    if (fHoveredLink.empty()) ShowTabStatus(*tab);
    else fStatus->SetText(fHoveredLink.c_str());
}

void BrowserWindow::WebKitFindResult(const BMessage& message)
{
    BMessenger sender;
    if (message.FindMessenger("view", &sender) != B_OK) return;
    auto* tab = FindTab(sender);
    const char* query = nullptr;
    if (!tab || tab->id != fSelected || message.FindString("query", &query) != B_OK
        || !query || std::strcmp(query, fFindText->Text())) return;
    int32 error;
    if (message.FindInt32("error", &error) != B_OK) return;
    if (error != B_OK) {
        fStatus->SetText("Could not search this page.");
        return;
    }
    bool found;
    if (message.FindBool("found", &found) == B_OK)
        fStatus->SetText(found ? "Match found" : "No matches");
}

void BrowserWindow::WebKitStateChanged(const BMessage& message)
{
    BMessenger sender;
    if (message.FindMessenger("view", &sender) != B_OK) return;
    auto* tab = FindTab(sender);
    if (!tab) return;
    if (message.what == B_WEBKIT_PROCESS_EXITED && message.HasString("reason")) {
        // The engine's second notice says why the process ended.
        ProcessExitReason(*tab, message.GetString("reason", ""));
        return;
    }
    if (message.what == B_WEBKIT_PROCESS_EXITED) {
        CancelTranslation(*tab);
        tab->translation = {};
        tab->processExited = true;
        tab->loading = false;
        tab->progress = 0;
        tab->keepScreenAwake = false;
        if (tab->id == fSelected) UpdateKeepScreenAwake();
        int32 error;
        tab->processError = message.FindInt32("error", &error) == B_OK && error != B_OK
            ? "Could not initialize the web page: " + std::string(std::strerror(error))
            : "The page process exited. Reload to try again.";
        if (tab->id == fSelected) fStatus->SetText(tab->processError.c_str());
        RefreshChrome();
        return;
    }
    if (int32 process = 0; message.FindInt32("processID", &process) == B_OK) tab->processID = process;
    if (uint64 page = 0; message.FindUInt64("pageIdentifier", &page) == B_OK) tab->pageIdentifier = page;
    bool closeInvalidated = false;
    if (message.FindBool("closeApprovalInvalidated", &closeInvalidated) == B_OK && closeInvalidated)
        InvalidateWindowClose();
    const char* value = nullptr;
    uint64 generation;
    // Error fields are a copied snapshot, never a native pointer into WebKit.
    // Older generations may still carry ordinary progress/close state, but
    // cannot restore a previous navigation's load error.
    if (message.FindUInt64("loadGeneration", &generation) == B_OK && generation >= tab->loadGeneration) {
        if (generation != tab->loadGeneration) {
            CancelTranslation(*tab);
            tab->translation = {};
            tab->readerRequest = 0;
            tab->readerExtracting = false;
            tab->readerAvailable = false;
            // A new page: the last one's blocked pop-ups and its video are gone.
            tab->blockedPopup.clear();
            tab->blockedPopups = 0;
            tab->pipToken.clear();
            if (tab->id == fSelected) RefreshNoticeBar();
        }
        tab->loadGeneration = generation;
        tab->connectionCertificate.MakeEmpty();
        message.FindMessage("connectionCertificate", &tab->connectionCertificate);
        if (message.FindString("loadOutcome", &value) == B_OK && value) tab->loadOutcome = value;
        // A navigation that failed or was cancelled leaves no visit to classify.
        if (tab->loadOutcome == "failed" || tab->loadOutcome == "cancelled" || tab->loadOutcome == "process-exited") tab->nextTransition = 0;
        bool failed = false;
        if (message.FindBool("loadError", &failed) == B_OK) {
            if (!failed) tab->certificate.reset();
            tab->loadError.clear();
            tab->loadErrorDescription.clear();
            tab->loadErrorDomain.clear();
            tab->loadErrorURL.clear();
            tab->loadErrorCode = 0;
            tab->loadErrorProvisional = false;
            if (failed) {
                if (message.FindString("loadErrorDescription", &value) == B_OK && value) tab->loadErrorDescription = value;
                if (message.FindString("loadErrorDomain", &value) == B_OK && value) tab->loadErrorDomain = value;
                if (message.FindString("loadErrorURL", &value) == B_OK && value) tab->loadErrorURL = value;
                message.FindInt32("loadErrorCode", &tab->loadErrorCode);
                message.FindBool("loadErrorProvisional", &tab->loadErrorProvisional);
            }
            if (failed && MediaTookOverLoad(*tab, message, generation)) {
                tab->loadErrorDescription.clear();
                tab->loadErrorDomain.clear();
                tab->loadErrorURL.clear();
                tab->loadErrorCode = 0;
                tab->loadErrorProvisional = false;
            } else if (failed) {
                tab->loadError = tab->loadErrorProvisional ? "Could not load " : "Loading was interrupted for ";
                tab->loadError += tab->loadErrorURL.empty() ? "this page" : tab->loadErrorURL;
                bool certificateFailed = false;
                if (message.FindBool("loadErrorCertificate", &certificateFailed) == B_OK && certificateFailed) {
                    Tab::CertificateProblem problem;
                    problem.url = tab->loadErrorURL;
                    problem.host = message.GetString("loadErrorCertificateHost", "");
                    problem.sha256 = message.GetString("loadErrorCertificateSHA256", "");
                    problem.subject = message.GetString("loadErrorCertificateSubject", "");
                    problem.issuer = message.GetString("loadErrorCertificateIssuer", "");
                    problem.problem = message.GetString("loadErrorCertificateProblem", "");
                    problem.names = message.GetString("loadErrorCertificateNames", "");
                    problem.validFrom = message.GetDouble("loadErrorCertificateValidFrom", 0);
                    problem.validUntil = message.GetDouble("loadErrorCertificateValidUntil", 0);
                    problem.generation = generation;
                    // Only a page the user asked for is worth a question; a
                    // failed request after the page committed stays in the status.
                    problem.asked = !tab->loadErrorProvisional || problem.host.empty() || problem.sha256.empty();
                    // Every later snapshot of the same load repeats its error;
                    // it was already asked about (or answered).
                    if (tab->certificate && tab->certificate->generation == generation
                        && tab->certificate->sha256 == problem.sha256)
                        problem.asked = tab->certificate->asked;
                    tab->certificate = std::move(problem);
                    tab->loadError += ": the server's certificate is not trusted"
                        + (tab->certificate->problem.empty() ? std::string() : " (" + tab->certificate->problem + ")") + ".";
                } else {
                    tab->loadError += ": " + (tab->loadErrorDescription.empty()
                        ? std::string("The request failed.") : tab->loadErrorDescription);
                }
                tab->loadError += " Enter the address again to retry.";
                // Keep native status text on one line; retain the original
                // diagnostic strings separately in the state probe.
                for (auto& character : tab->loadError)
                    if (static_cast<unsigned char>(character) < 0x20 || character == 0x7f) character = ' ';
            }
        }
    }
    if (message.FindString("url", &value) == B_OK && value && *value) {
        tab->readerActive = !tab->readerURL.empty() && tab->readerURL == value;
        tab->url = StoredURL(value);
        UpdateBookmarkQueryLocation(*tab, value);
        ApplySiteZoom(*tab);
    }
    // An unloaded tab keeps the title it had until its page loads.
    if (message.FindString("title", &value) == B_OK && value && (*value || tab->deferredURL.empty())) {
        const auto before = tab->title;
        tab->title = *value ? value : tab->url == "summit:home" ? "Start Page" : tab->url;
        if (tab->title != before && tab->devTools.IsValid()) {
            BMessage page(kDeveloperToolsPage);
            page.AddString("title", tab->title.c_str());
            tab->devTools.SendMessage(&page);
        }
    }
    const bool wasLoading = tab->loading;
    message.FindBool("loading", &tab->loading);
    if (tab->loading && !wasLoading) { tab->loadStartedAt = system_time(); tab->loadFinishedAt = 0; }
    else if (!tab->loading && wasLoading) tab->loadFinishedAt = system_time();
    message.FindBool("canGoBack", &tab->back);
    message.FindBool("canGoForward", &tab->forward);
    message.FindBool("capturingCamera", &tab->capturingCamera);
    message.FindBool("capturingMicrophone", &tab->capturingMicrophone);
    message.FindBool("capturingScreen", &tab->capturingScreen);
    message.FindBool("playingAudio", &tab->playingAudio);
    message.FindBool("audioMuted", &tab->audioMuted);
    message.FindBool("softwareRendering", &tab->softwareRendering);
    if (tab->softwareRendering && !fSoftwareRenderingNoticeShown) {
        fSoftwareRenderingNoticeShown = true;
        fSoftwareRenderingNoticePending = true;
        RefreshNoticeBar();
    }
    if (message.FindBool("keepScreenAwake", &tab->keepScreenAwake) == B_OK && tab->id == fSelected)
        UpdateKeepScreenAwake();
    double progress;
    if (message.FindDouble("progress", &progress) == B_OK && std::isfinite(progress))
        tab->progress = static_cast<float>(std::clamp(progress, 0.0, 1.0));
    // pageZoom is the window's own: it sets every change, and the engine's
    // report can predate the latest one.
    double zoom;
    if (message.FindDouble("textZoom", &zoom) == B_OK && std::isfinite(zoom)) tab->textZoom = zoom;
    if (tab->loading) {
        tab->processExited = false;
        tab->processError.clear();
        if (tab->crashView) RemoveCrashView(*tab);
        tab->exitReason.clear();
        tab->exitedInBackground = false;
    }
    uint64 successSequence;
    bool internalPageLoaded = false;
    if (message.FindUInt64("loadSuccessSequence", &successSequence) == B_OK
        && successSequence > tab->loadSuccessSequence) {
        tab->loadSuccessSequence = successSequence;
        const char* successURL = nullptr;
        const char* successTitle = nullptr;
        if (message.FindString("loadSuccessURL", &successURL) == B_OK && successURL && *successURL
            && message.FindString("loadSuccessTitle", &successTitle) == B_OK && successTitle) {
            const auto stored = StoredURL(successURL);
            const PageRecord visit { stored, successTitle };
            // Private windows leave no history. A navigation the window did
            // not start (a link, the page's own) is a link visit.
            const uint32 transition = std::exchange(tab->nextTransition, 0);
            if (!fPrivate && tab->readerURL != successURL)
                RecordVisit(visit, transition ? transition : 1);
            internalPageLoaded = stored == kHistoryPage || stored == kBookmarksPage;
        }
    }
    const char* successfulURL = nullptr;
    if (!fPrivate && !tab->readerActive && tab->loadOutcome == "succeeded"
        && message.FindString("loadSuccessURL", &successfulURL) == B_OK && successfulURL
        && StoredURL(successfulURL) == tab->url) {
        const std::string url = tab->url, title = tab->title;
        fShared->Change([&](Profile& profile) -> uint32 {
            uint32 changes = 0;
            for (auto& page : profile.history)
                if (page.url == url && page.title != title) { page.title = title; changes = SharedProfile::kHistoryChanged; }
            return changes;
        }, BMessenger(this), false);
    }
    UpdateLoadErrorView(*tab);
    if (tab->id == fSelected) {
        if (!fAddress->TextView()->IsFocus()) fAddress->SetText(DisplayURL(tab->ShownURL()).c_str());
        ShowTabStatus(*tab);
        if (tab->certificate && !tab->certificate->asked) AskAboutCertificate(*tab);
        ProbeReader(*tab);
        ProbeTranslation(*tab);
    }
    // Back or Forward to a built-in page shows the copy from when it was
    // written (or the page cache's); bring it up to date once it has loaded.
    // Only then: any other state change may be the start of a navigation away.
    if (internalPageLoaded && !tab->loading) RefreshInternalPage(*tab);
    UpdateStoreOffer(*tab);
    RefreshChrome();
    if (NavigationTraceEnabled() && tab->id == fSelected && NavigationTraceStart()
        && system_time() - NavigationTraceStart() < 60000000) {
        // Each change of what the tab shows while a traced navigation runs.
        static std::string last;
        char line[400];
        std::snprintf(line, sizeof(line), "loading=%d progress=%.2f outcome=%s url=%.200s", tab->loading, tab->progress,
            tab->loadOutcome.c_str(), tab->url.c_str());
        if (last != line) {
            last = line;
            NavigationTrace("tab state %s", line);
        }
    }
}

void BrowserWindow::ProbeReader(Tab& tab)
{
    if (fClosingWindow || tab.closeQueued || tab.closeRequested || tab.closeApproved
        || tab.loading || tab.processExited || !tab.loadError.empty() || tab.readerActive
        || tab.loadOutcome != "succeeded" || !tab.loadGeneration
        || tab.readerProbedGeneration == tab.loadGeneration
        || !(tab.url.starts_with("https://") || tab.url.starts_with("http://"))) return;
    tab.readerProbedGeneration = tab.loadGeneration;
    const auto& source = ReaderProbeScript();
    if (source.empty()) return;
    tab.readerRequest = ++fReaderRequest;
    tab.readerGeneration = tab.loadGeneration;
    tab.readerRequestURL = tab.url;
    tab.readerExtracting = false;
    tab.view->EvaluateJavaScriptIsolated(source.c_str(), BMessenger(this), tab.readerRequest, tab.readerGeneration);
}

void BrowserWindow::RefreshDownloadsButton()
{
    if (fDownloadsButton) fDownloadsButton->SetSummary(DownloadList::Default().Summarize(fPrivate));
}

void BrowserWindow::ToggleDownloadsPanel(bool open)
{
    if (fDownloadsPanel.IsValid()) {
        if (!open) CloseDownloadsPanel();
        return;
    }
    // A click on the button while the panel was open took the focus from the
    // panel, which closed; the click is done with.
    if (!open && fDownloadsPanelClosedAt && system_time() - fDownloadsPanelClosedAt < 500000) {
        fDownloadsPanelClosedAt = 0;
        return;
    }
    if (!fDownloadsButton) return;
    // Under the button, or at the window's corner while the toolbar is hidden.
    BRect anchor = fDownloadsButton->IsHidden(fDownloadsButton)
        ? BRect(Frame().right - 8, Frame().top + 8, Frame().right - 8, Frame().top + 8)
        : fDownloadsButton->ConvertToScreen(fDownloadsButton->Bounds());
    fDownloadsPanelMenus = std::make_shared<std::atomic<bool>>(false);
    auto* panel = new DownloadsPanel(this, fPrivate, anchor, fDownloadsPanelMenus);
    fDownloadsPanel = BMessenger(panel);
    panel->Show();
}

void BrowserWindow::CloseDownloadsPanel(bool wait)
{
    if (fDownloadsPanelMenus) fDownloadsPanelMenus->store(true);
    if (wait) {
        if (fDownloadsPanel.LockTarget()) {
            BLooper* looper = nullptr;
            fDownloadsPanel.Target(&looper);
            looper->Quit();
        }
    } else if (fDownloadsPanel.IsValid()) fDownloadsPanel.SendMessage(B_QUIT_REQUESTED);
    fDownloadsPanel = {};
}

std::string BrowserWindow::StoreItemIdentifier(const ExtensionStoreItem& item)
{
    if (item.store == ExtensionStoreItem::Store::Chrome) return item.identifier;
    auto found = fStoreItems.find(item.identifier);
    return found == fStoreItems.end() ? std::string() : found->second.identifier;
}

void BrowserWindow::UpdateStoreOffer(Tab& tab)
{
    // A new document has no card; one is put in once it has loaded.
    if (tab.loading || tab.processExited) {
        tab.storeOffer.clear();
        return;
    }
    const auto item = fExtensionsEnabled && tab.loadError.empty() && tab.storeOfferDismissed != tab.url
        ? ParseExtensionStoreURL(tab.url) : std::nullopt;
    std::optional<StoreOffer> offer;
    std::string wanted;
    if (item) {
        std::string name = StoreItemNameFromTitle(tab.title);
        bool known = true;
        if (item->store == ExtensionStoreItem::Store::Firefox) {
            auto& identity = fStoreItems[item->identifier];
            if (!identity.asked) {
                identity.asked = true;
                std::thread([item = *item, target = BMessenger(this)] {
                    std::string identifier, name, error;
                    bool extension = true;
                    BMessage resolved(kStoreItemResolved);
                    resolved.AddString("slug", item.identifier.c_str());
                    if (ResolveFirefoxStoreItem(item, identifier, name, extension, error)) {
                        resolved.AddString("identifier", identifier.c_str());
                        resolved.AddString("name", name.c_str());
                        resolved.AddBool("extension", extension);
                    } else std::fprintf(stderr, "Summit: could not look up %s on Firefox Add-ons: %s\n",
                        item.identifier.c_str(), error.c_str());
                    target.SendMessage(&resolved);
                }).detach();
            }
            known = identity.resolved && identity.extension;
            if (!identity.name.empty()) name = identity.name;
        }
        if (known) {
            const auto installed = fShared->InstalledExtensions();
            offer = StoreOffer { name, false, false, fStoreOfferToken };
            if (auto found = installed.find(StoreItemIdentifier(*item)); found != installed.end()) {
                offer->installed = true;
                offer->disabled = !found->second.enabled;
                if (offer->name.empty()) offer->name = found->second.name;
            }
            wanted = tab.url + '\n' + offer->name + '\n' + (offer->installed ? offer->disabled ? "off" : "on" : "-");
        }
    }
    if (wanted == tab.storeOffer) return;
    tab.storeOffer = wanted;
    const auto script = StoreOfferScript(offer ? &*offer : nullptr);
    tab.view->EvaluateJavaScriptIsolated(script.c_str(), BMessenger(this), ++fReaderRequest, tab.loadGeneration);
}

void BrowserWindow::StoreItemResolved(const BMessage& message)
{
    const std::string slug = message.GetString("slug", "");
    auto& identity = fStoreItems[slug];
    identity.resolved = true;
    identity.identifier = message.GetString("identifier", "");
    identity.name = message.GetString("name", "");
    identity.extension = message.GetBool("extension", true);
    for (auto& tab : fTabs) {
        const auto item = ParseExtensionStoreURL(tab.url);
        if (item && item->store == ExtensionStoreItem::Store::Firefox && item->identifier == slug) UpdateStoreOffer(tab);
    }
    RefreshChrome();
}

bool BrowserWindow::StoreOfferChosen(Tab& tab, const std::string& url)
{
    if (url.compare(0, std::strlen(kStoreOfferScheme) + 1, std::string(kStoreOfferScheme) + ":") != 0) return false;
    const auto choice = ParseStoreOfferLink(url);
    const auto item = ParseExtensionStoreURL(tab.url);
    // Only the card's own links, on the page it was put in.
    if (!choice || choice->token != fStoreOfferToken || !item || !fExtensionsEnabled || tab.storeOffer.empty()) return true;
    if (choice->action == "dismiss") {
        tab.storeOfferDismissed = tab.url;
        UpdateStoreOffer(tab);
    } else if (choice->action == "install") {
        BMessage request(kExtensionStoreInstall);
        request.AddMessenger("window", BMessenger(this));
        request.AddString("url", tab.url.c_str());
        be_app->PostMessage(&request);
    } else {
        BMessage show(kShowExtensions);
        show.AddString("extension_identifier", StoreItemIdentifier(*item).c_str());
        be_app->PostMessage(&show);
    }
    return true;
}

void BrowserWindow::ToggleReader()
{
    auto* tab = ActiveTab();
    if (!tab || tab->loading || tab->processExited || tab->readerExtracting) return;
    if (tab->readerActive) {
        Navigate(tab->readerSource);
        return;
    }
    if (!tab->readerAvailable || !PrepareNavigation()) return;
    const auto& source = ReaderExtractScript();
    if (source.empty()) { fStatus->SetText("Reader mode resources are unavailable."); return; }
    tab->readerRequest = ++fReaderRequest;
    tab->readerGeneration = tab->loadGeneration;
    tab->readerRequestURL = tab->url;
    tab->readerExtracting = true;
    tab->view->EvaluateJavaScriptIsolated(source.c_str(), BMessenger(this), tab->readerRequest, tab->readerGeneration);
    fStatus->SetText("Preparing Reader mode…");
    RefreshChrome();
}

void BrowserWindow::ReaderResult(const BMessage& message)
{
    BMessenger view;
    if (message.FindMessenger("view", &view) != B_OK) return;
    auto* tab = FindTab(view);
    if (!tab || !tab->readerRequest || tab->readerRequest != message.GetUInt64("identifier", 0)) return;
    const bool extracting = std::exchange(tab->readerExtracting, false);
    tab->readerRequest = 0;
    if (fClosingWindow || tab->closeQueued || tab->closeRequested || tab->closeApproved
        || tab->loading || tab->processExited || tab->readerGeneration != tab->loadGeneration
        || tab->readerGeneration != message.GetUInt64("generation", 0) || tab->readerRequestURL != tab->url) {
        RefreshChrome();
        return;
    }
    const std::string value = message.GetString("result", "");
    if (!extracting) {
        tab->readerAvailable = !message.HasString("error") && value == "true";
        // The first check answers openInReaderMode, article or not.
        if (std::exchange(tab->openInReader, false) && tab->readerAvailable && tab->id == fSelected && !tab->readerActive) {
            ToggleReader();
            return;
        }
        RefreshChrome();
        return;
    }
    auto unavailable = [&] {
        tab->readerAvailable = false;
        if (tab->id == fSelected) fStatus->SetText("Reader mode could not find an article on this page.");
        RefreshChrome();
    };
    if (message.HasString("error") || value.empty() || value.size() > 4000000) { unavailable(); return; }
    const auto article = nlohmann::json::parse(value, nullptr, false);
    if (!article.is_object() || !article.contains("url") || !article["url"].is_string()
        || article["url"].get<std::string>() != tab->url || !article.contains("html") || !article["html"].is_string()) {
        unavailable(); return;
    }
    auto html = article["html"].get<std::string>();
    auto url = tab->readerSource == tab->url ? tab->readerURL : NewReaderURL();
    if (html.empty() || html.size() > 3500000 || url.empty()) { unavailable(); return; }
    // Keep one article per tab, in memory. Stale requests cannot retrieve a
    // previous tab's article, and private browsing creates no reader files.
    tab->readerSource = tab->url;
    tab->readerURL = std::move(url);
    tab->readerOriginals[tab->readerURL] = tab->readerSource;
    tab->readerHTML = std::move(html);
    tab->view->LoadURL(tab->readerURL.c_str());
}

void BrowserWindow::ReaderResource(const BMessage& message)
{
    const auto identifier = message.GetUInt64("identifier", 0);
    BMessenger view;
    auto* tab = message.FindMessenger("view", &view) == B_OK ? FindTab(view) : nullptr;
    const char* url = message.GetString("url", "");
    if (!tab || !message.GetBool("main_frame", false) || std::strcmp(message.GetString("method", ""), "GET")) {
        BWebKitView::FailURLSchemeRequest(identifier, "This Reader article is no longer available.");
        return;
    }
    if (tab->readerHTML.empty() || tab->readerURL != url) {
        if (auto found = tab->readerOriginals.find(url); found != tab->readerOriginals.end())
            tab->view->LoadURL(found->second.c_str());
        BWebKitView::FailURLSchemeRequest(identifier, "Opening the original article.");
        return;
    }
    BMessage headers;
    for (auto [name, value] : { std::pair { "Cache-Control", "no-store" },
            { "Content-Security-Policy", "default-src 'none'; img-src https: http:; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'" },
            { "Referrer-Policy", "no-referrer" }, { "X-Content-Type-Options", "nosniff" } }) {
        headers.AddString("name", name);
        headers.AddString("value", value);
    }
    BWebKitView::RespondToURLSchemeRequest(identifier, 200, "text/html; charset=utf-8", &headers,
        tab->readerHTML.data(), tab->readerHTML.size());
}

void BrowserWindow::ShowCertificate()
{
    auto* tab = ActiveTab();
    if (!tab || fCertificateButton->IsHidden(fCertificateButton)) return;
    if (fCertificateWindow.IsValid()) { fCertificateWindow.SendMessage(kShowCertificate); return; }
    auto* window = new CertificateInfoWindow(tab->connectionCertificate, Frame());
    fCertificateWindow = BMessenger(window);
    fCertificateTab = tab->id;
    fCertificateGeneration = tab->loadGeneration;
    fCertificateVerified = tab->connectionCertificate.GetBool("verified", false);
    fCertificateMixedContent = tab->connectionCertificate.GetBool("mixedContent", false);
    window->Show();
}

void BrowserWindow::AskAboutCertificate(Tab& tab)
{
    if (!tab.certificate || tab.certificate->asked) return;
    auto& problem = *tab.certificate;
    problem.asked = true;
    auto date = [](double seconds) {
        if (!(seconds > 0)) return std::string("?");
        const time_t time = static_cast<time_t>(seconds);
        struct tm local {};
        char text[64];
        if (!localtime_r(&time, &local) || !std::strftime(text, sizeof text, "%e %B %Y", &local)) return std::string("?");
        std::string result = text;
        return result.erase(0, result.find_first_not_of(' '));
    };
    std::string fingerprint;
    for (size_t i = 0; i + 1 < problem.sha256.size(); i += 2) {
        if (!fingerprint.empty()) fingerprint += i % 32 ? ":" : "\n    ";
        for (char digit : problem.sha256.substr(i, 2)) fingerprint += static_cast<char>(std::toupper(static_cast<unsigned char>(digit)));
    }
    std::string text = "This connection to " + problem.host + " is not private.\n\n"
        "Summit could not verify the server's certificate";
    text += problem.problem.empty() ? "." : ": " + problem.problem + ".";
    text += " Someone could be impersonating " + problem.host + " to read what you send it.\n\n"
        "Continue only if you know this device or site, such as your own router, and trust the network you "
        "are on. Summit will then remember this certificate for " + problem.host + " and not ask again "
        "unless the certificate changes.\n\n";
    if (!problem.subject.empty()) text += "Issued to: " + problem.subject + "\n";
    if (!problem.issuer.empty()) text += "Issued by: " + problem.issuer + "\n";
    if (!problem.names.empty()) text += "Valid for: " + problem.names + "\n";
    text += "Valid from " + date(problem.validFrom) + " to " + date(problem.validUntil) + "\n";
    if (!fingerprint.empty()) text += "SHA-256: " + fingerprint + "\n";
    auto* alert = new BAlert("Certificate Not Trusted", text.c_str(), "Go Back",
        ("Continue to " + problem.host).c_str(), nullptr, B_WIDTH_FROM_LABEL, B_STOP_ALERT);
    alert->SetShortcut(0, B_ESCAPE);
    alert->SetDefaultButton(alert->ButtonAt(0));
    auto* reply = new BMessage(kCertificateDecision);
    reply->AddInt64("tab", tab.id);
    reply->AddUInt64("generation", problem.generation);
    alert->Go(new BInvoker(reply, this));
}

void BrowserWindow::CertificateDecision(const BMessage& message)
{
    const int64 id = message.GetInt64("tab", -1);
    auto found = std::find_if(fTabs.begin(), fTabs.end(), [id](const Tab& tab) { return tab.id == id; });
    if (found == fTabs.end() || !found->certificate
        || found->certificate->generation != message.GetUInt64("generation", 0)) return;
    const auto problem = *found->certificate;
    if (message.GetInt32("which", 0) != 1) {
        if (found->id == fSelected) fStatus->SetText(("Did not continue to " + problem.host + ".").c_str());
        return;
    }
    // Trust it in this window's context before the page is asked for again
    // (both go to the engine's thread in this order), then everywhere else.
    fWebKitContext->AllowServerCertificate(problem.host.c_str(), problem.sha256.c_str());
    BMessage trust(kTrustCertificate);
    trust.AddString("host", problem.host.c_str());
    trust.AddString("sha256", problem.sha256.c_str());
    trust.AddString("subject", problem.subject.c_str());
    trust.AddBool("private", fPrivate);
    be_app->PostMessage(&trust);
    found->certificate.reset();
    found->loadError.clear();
    found->view->LoadURL(problem.url.c_str());
    if (found->id == fSelected) ShowTabStatus(*found);
}

void BrowserWindow::ShowTabStatus(const Tab& tab)
{
    const bool noticed = !tab.notice.empty() && system_time() < tab.noticeUntil;
    const char* text = !fHoveredLink.empty() && tab.id == fSelected ? fHoveredLink.c_str()
        : tab.processExited ? tab.processError.c_str()
        : !tab.loadError.empty() ? tab.loadError.c_str()
        : noticed ? tab.notice.c_str()
        : tab.loading ? "Loading…" : "Ready";
    fStatus->SetText(text);
    fStatus->SetToolTip(tab.loadError.empty() && !tab.processExited && !noticed ? nullptr : text);
}
#else
void BrowserWindow::NavigationRequested(const BString& url, BWebView* view)
{
    // This is a notification for a navigation WebKit has already started.
    // Starting another load here would continually restart the same request.
    if (view == CurrentWebView() && !fAddress->TextView()->IsFocus())
        fAddress->SetText(DisplayURL(StoredURL(url)).c_str());
}
void BrowserWindow::NewWindowRequested(const BString& url, bool primary) { CreateTab(url.String(), primary); }
void BrowserWindow::NewPageCreated(BWebView* view, BRect, bool, bool, bool activate) { CreateTab("about:blank", activate, view); }
void BrowserWindow::CloseWindowRequested(BWebView* view) { if (auto* tab = FindTab(view)) CloseTab(tab->id); }
void BrowserWindow::LoadNegotiating(const BString& url, BWebView* view)
{
    if (auto* tab = FindTab(view)) { tab->loading = true; tab->progress = 0.03f; }
    (void)url;
    if (view == CurrentWebView()) fStatus->SetText("Connecting…");
    RefreshChrome();
}
void BrowserWindow::LoadCommitted(const BString& url, BWebView* view)
{
    if (auto* tab = FindTab(view)) { tab->url = StoredURL(url); UpdateBookmarkQueryLocation(*tab, url); }
    if (view == CurrentWebView() && !fAddress->TextView()->IsFocus())
        fAddress->SetText(DisplayURL(StoredURL(url)).c_str());
}
void BrowserWindow::LoadProgress(float progress, BWebView* view)
{
    if (auto* tab = FindTab(view)) {
        tab->progress = std::clamp(progress / 100.0f, 0.0f, 1.0f);
        if (tab->progress >= 1.0f) tab->loading = false;
    }
    if (progress >= 100 && view == CurrentWebView()) {
        if (auto* tab = FindTab(view)) ShowTabStatus(*tab);
        else fStatus->SetText("Ready");
    }
    RefreshChrome();
}
void BrowserWindow::LoadFailed(const BString&, BWebView* view)
{
    if (auto* tab = FindTab(view)) tab->loading = false;
    if (view == CurrentWebView()) fStatus->SetText("The page could not be loaded");
    RefreshChrome();
}
void BrowserWindow::LoadFinished(const BString& url, BWebView* view)
{
    if (auto* tab = FindTab(view)) {
        // BWebWindow calls this at DOM readiness, before images and other
        // resources finish. Completion arrives through LoadProgress(100).
        tab->url = StoredURL(url);
        UpdateBookmarkQueryLocation(*tab, url);
        const PageRecord visit { tab->url, tab->title };
        RecordVisit(visit);
        RefreshInternalPage(*tab);
    }
    RefreshChrome();
}
void BrowserWindow::MainDocumentError(const BString&, const BString& error, BWebView* view)
{
    if (view == CurrentWebView()) fStatus->SetText(error.String());
}
void BrowserWindow::TitleChanged(const BString& title, BWebView* view)
{
    if (auto* tab = FindTab(view)) {
        tab->title = title.String();
        const std::string url = tab->url, title = tab->title;
        fShared->Change([&](Profile& profile) -> uint32 {
            for (auto& page : profile.history) if (page.url == url) page.title = title;
            return 0;
        }, BMessenger(this), false);
    }
    RefreshChrome();
}
void BrowserWindow::StatusChanged(const BString& status, BWebView* view)
{
    if (view == CurrentWebView()) fStatus->SetText(status.String());
}
void BrowserWindow::NavigationCapabilitiesChanged(bool back, bool forward, bool, BWebView* view)
{
    // The loader can still report "can stop" while dispatching its final
    // progress notification. It is not a new loading transition.
    if (auto* tab = FindTab(view)) { tab->back = back; tab->forward = forward; }
    RefreshChrome();
}
#endif
}

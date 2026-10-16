#include "Chrome.h"
#include "Messages.h"
#include <Window.h>
#include <Message.h>
#include <Bitmap.h>
#include <Application.h>
#include <Resources.h>
#include <IconUtils.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <ControlLook.h>
#include <MessageRunner.h>
#if SUMMIT_MODERN_WEBKIT
#include "ExtensionInstaller.h"
#include <WebKit/WebKitView.h>
#include <cstring>
#include <dlfcn.h>
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>

namespace summit {
static std::atomic<bool> sHaikuStyle { true };
void SetInterfaceStyle(bool haiku) { sHaikuStyle = haiku; }
bool HaikuInterfaceStyle() { return sHaikuStyle; }

static rgb_color Mix(rgb_color a, rgb_color b, float amount)
{
    auto channel = [amount](uint8 x, uint8 y) { return uint8(std::lround(x + (y - x) * amount)); };
    return { channel(a.red, b.red), channel(a.green, b.green), channel(a.blue, b.blue), 255 };
}

namespace {
std::mutex sPrivateWindowsLock;
std::set<const BWindow*> sPrivateWindows;
}
void SetPrivateWindow(const BWindow* window, bool privateBrowsing)
{
    std::lock_guard lock(sPrivateWindowsLock);
    if (privateBrowsing) sPrivateWindows.insert(window);
    else sPrivateWindows.erase(window);
}
bool IsPrivateWindow(const BWindow* window)
{
    std::lock_guard lock(sPrivateWindowsLock);
    return window && sPrivateWindows.contains(window);
}
ChromeColors ChromeColorsFor(bool privateBrowsing)
{
    if (privateBrowsing)
        return { {64, 42, 104, 255}, {243, 238, 252, 255}, {104, 76, 156, 255}, {250, 247, 255, 255}, true };
    return { ui_color(B_PANEL_BACKGROUND_COLOR), ui_color(B_PANEL_TEXT_COLOR),
        ui_color(B_CONTROL_BACKGROUND_COLOR), ui_color(B_CONTROL_TEXT_COLOR), false };
}
ChromeColors ChromeColorsFor(const BView* view)
{
    return ChromeColorsFor(view && IsPrivateWindow(view->Window()));
}

// A real Haiku push button: frame and background from BControlLook.
static void DrawHaikuButton(BControl* control, BRect update, bool hover)
{
    BRect rect = control->Bounds();
    const auto colors = ChromeColorsFor(control);
    const rgb_color background = colors.panel;
    const rgb_color base = colors.control;
    uint32 flags = be_control_look->Flags(control);
    if (hover && control->IsEnabled()) flags |= BControlLook::B_HOVER;
    be_control_look->DrawButtonFrame(control, rect, update, base, background, flags);
    be_control_look->DrawButtonBackground(control, rect, update, base, flags);
}

static BSize ToolButtonSize() { return HaikuInterfaceStyle() ? BSize(30, 28) : BSize(34, 32); }

ToolButton::ToolButton(const char* name, const char* tooltip, Icon icon, uint32 message)
    : BButton(name, "", new BMessage(message)), fIcon(icon)
{
    SetToolTip(tooltip);
}
BSize ToolButton::MinSize() { return ToolButtonSize(); }
BSize ToolButton::MaxSize() { return ToolButtonSize(); }
BSize ToolButton::PreferredSize() { return ToolButtonSize(); }
void ToolButton::MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
{
    const bool hover = transit == B_ENTERED_VIEW || transit == B_INSIDE_VIEW;
    if (hover != fHover) { fHover = hover; Invalidate(); }
    BButton::MouseMoved(where, transit, drag);
}
// The Safari-like look's flat button: a rounded highlight when pressed or focused.
static void DrawFlatButton(BView* view, bool pressed, bool focused, const ChromeColors& colors)
{
    view->SetHighColor(colors.panel);
    view->FillRect(view->Bounds());
    if (pressed || focused) {
        view->SetHighColor(Mix(colors.panel, colors.text, pressed ? 0.16f : 0.09f));
        view->FillRoundRect(view->Bounds().InsetByCopy(2, 2), 5, 5);
    }
}

// air/OS draws a bitmap given B_DEVICE_PIXEL_BITMAP (the value of air/OS's
// InterfaceDefs.h) one to one in frame buffer pixels, from the pixel its
// destination starts on. Releases that can have BScreen::DeviceScale(); Haiku
// R1/beta6 and older air/OS do not, so it is looked up at run time.
static constexpr uint32 kDevicePixelBitmap = 0x00001000;

static bool DevicePixelBitmaps()
{
    static const bool available = dlsym(RTLD_DEFAULT, "_ZN7BScreen11DeviceScaleEv") != nullptr;
    return available;
}

// Font Awesome silhouettes are native vectors, rasterized at the display's
// density and tinted with the control's current theme/state color. Keep their
// 20-point canvas: trimming would enlarge narrow glyphs and erase optical sizing.
ToolButton::~ToolButton() = default;
void ToolButton::UpdateIconBitmap(rgb_color ink)
{
    float scale = 1;
#if SUMMIT_MODERN_WEBKIT
    scale = BWebKitDisplayScale();
#endif
    const int size = std::max(1, int(std::ceil(20 * scale)));
    if (size == fBitmapSize && fBitmapIcon == fIcon && fBitmapInk == ink) return;
    fBitmapSize = size;
    fBitmapIcon = fIcon;
    fBitmapInk = ink;
    fBitmap.reset();
    // BResources is shared across window threads.
    static std::mutex lock;
    std::lock_guard guard(lock);
    auto* resources = BApplication::AppResources();
    size_t bytes = 0;
    auto* data = resources ? static_cast<const uint8*>(resources->LoadResource('VICN', 201 + int(fIcon), &bytes)) : nullptr;
    auto bitmap = std::make_unique<BBitmap>(BRect(0, 0, size - 1, size - 1), B_RGBA32);
    if (!data || bitmap->InitCheck() != B_OK || BIconUtils::GetVectorIcon(data, bytes, bitmap.get()) != B_OK)
        return;
    // B_RGBA32 stores BGRA on Haiku's supported little-endian architectures.
    // Preserve vector antialiasing in the alpha channel; RGB is straight alpha.
    for (int y = 0; y < size; ++y) {
        auto* pixel = static_cast<uint8*>(bitmap->Bits()) + y * bitmap->BytesPerRow();
        for (int x = 0; x < size; ++x, pixel += 4) {
            pixel[0] = ink.blue;
            pixel[1] = ink.green;
            pixel[2] = ink.red;
        }
    }
    fBitmap = std::move(bitmap);
}

void ToolButton::Draw(BRect update)
{
    rgb_color ink;
    const auto colors = ChromeColorsFor(this);
    if (HaikuInterfaceStyle()) {
        DrawHaikuButton(this, update, fHover);
        ink = IsEnabled() ? colors.controlText : Mix(colors.controlText, colors.control, 0.6f);
    } else {
        DrawFlatButton(this, Value(), IsFocus() || (fHover && IsEnabled()), colors);
        ink = IsEnabled() ? colors.text : Mix(colors.text, colors.panel, 0.55f);
    }
    if (IsEnabled()) {
        const auto background = HaikuInterfaceStyle() ? colors.control : colors.panel;
        const bool dark = int(background.red) + background.green + background.blue < 384;
        if (fIcon == Icon::BookmarkFilled || fIcon == Icon::LockWarning)
            ink = dark ? rgb_color{255, 208, 112, 255} : rgb_color{151, 96, 0, 255};
        else if (fIcon == Icon::ReaderActive)
            ink = dark ? rgb_color{131, 222, 191, 255} : rgb_color{30, 111, 85, 255};
    }
    UpdateIconBitmap(ink);
    if (fBitmap) {
        const float x = std::floor((Bounds().Width() + 1 - 20) / 2) + (Value() ? 1 : 0);
        const float y = std::floor((Bounds().Height() + 1 - 20) / 2) + (Value() ? 1 : 0);
        SetDrawingMode(B_OP_ALPHA);
        SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        // The icon has the screen's density (20 points are a whole number of
        // pixels at every air/OS scale): drawn one to one it stays sharp at
        // 125, 150 and 175 percent, where scaling it to fit blurred it.
        if (fBitmapSize > 20 && DevicePixelBitmaps())
            DrawBitmap(fBitmap.get(), fBitmap->Bounds(), BRect(x, y, x + 19, y + 19), kDevicePixelBitmap);
        else
            DrawBitmap(fBitmap.get(), fBitmap->Bounds(), BRect(x, y, x + 19, y + 19), B_FILTER_BITMAP_BILINEAR);
        SetDrawingMode(B_OP_COPY);
        return;
    }
    SetHighColor(ink);
    SetDrawingMode(B_OP_ALPHA);
    SetPenSize(1.6f);
    BPoint c(std::floor(Bounds().Width() / 2) + 0.5f, std::floor(Bounds().Height() / 2) + 0.5f);
    if (HaikuInterfaceStyle() && Value()) c += BPoint(1, 1);
    auto line = [&](float x1, float y1, float x2, float y2) {
        StrokeLine(c + BPoint(x1, y1), c + BPoint(x2, y2));
    };
    switch (fIcon) {
        case Icon::Reader: case Icon::ReaderActive:
            if (fIcon == Icon::ReaderActive) SetHighColor(rgb_color { 44, 125, 104, 255 });
            line(-8, -7, -8, 6); line(8, -7, 8, 6); line(0, -5, 0, 8);
            line(-8, -7, -3, -7); line(-3, -7, 0, -5); line(0, -5, 3, -7); line(3, -7, 8, -7);
            line(-8, 6, -3, 6); line(-3, 6, 0, 8); line(0, 8, 3, 6); line(3, 6, 8, 6);
            line(-6, -2, -2, -2); line(2, -2, 6, -2); line(-6, 1, -2, 1); line(2, 1, 6, 1);
            break;
        case Icon::More:
            for (float x : {-6.0f, 0.0f, 6.0f}) FillEllipse(c + BPoint(x, 0), 1.3f, 1.3f);
            break;
        case Icon::Back: line(3, -6, -3, 0); line(-3, 0, 3, 6); break;
        case Icon::Forward: line(-3, -6, 3, 0); line(3, 0, -3, 6); break;
        case Icon::Plus: line(-6, 0, 6, 0); line(0, -6, 0, 6); break;
        case Icon::Go: line(-6, 0, 5, 0); line(1, -5, 6, 0); line(6, 0, 1, 5); break;
        case Icon::Stop: line(-5, -5, 5, 5); line(5, -5, -5, 5); break;
        case Icon::Reload:
            StrokeArc(BRect(c.x - 6, c.y - 6, c.x + 6, c.y + 6), 40, 290);
            line(6, -7, 6, -1); line(6, -1, 1, -2); break;
        case Icon::Bookmark: case Icon::BookmarkFilled: {
            BPoint points[10];
            for (int i = 0; i < 10; ++i) {
                const float angle = -1.5707963f + i * 0.6283185f;
                const float radius = i % 2 ? 3.5f : 8.0f;
                points[i] = c + BPoint(std::cos(angle) * radius, std::sin(angle) * radius);
            }
            if (fIcon == Icon::BookmarkFilled && IsEnabled()) {
                // The current page is bookmarked: green here, gold in the Haiku look.
                SetHighColor(HaikuInterfaceStyle() ? rgb_color{255, 203, 0, 255} : rgb_color{44, 125, 104, 255});
                FillPolygon(points, 10);
                SetHighColor(ink);
            }
            StrokePolygon(points, 10); break;
        }
        case Icon::Downloads:
            line(0, -8, 0, 3); line(-4, -1, 0, 3); line(0, 3, 4, -1);
            line(-7, 3, -7, 7); line(-7, 7, 7, 7); line(7, 7, 7, 3); break;
        case Icon::Home:
            line(-8, 1, 0, -7); line(0, -7, 8, 1);
            line(-5, -1, -5, 7); line(-5, 7, 5, 7); line(5, 7, 5, -1); break;
        case Icon::Lock: case Icon::LockWarning:
            StrokeArc(BRect(c.x - 4, c.y - 8, c.x + 4, c.y), 0, 180);
            StrokeRoundRect(BRect(c.x - 6, c.y - 1, c.x + 6, c.y + 8), 2, 2); break;
    }
    SetPenSize(1);
    SetDrawingMode(B_OP_COPY);
}

ZoomButton::ZoomButton() : BButton("zoom", "100%", new BMessage(kZoomReset))
{
    SetToolTip("Reset the zoom to 100%");
}
void ZoomButton::SetZoom(double zoom)
{
    const std::string label = std::to_string(std::lround(zoom * 100)) + "%";
    if (label != Label()) { SetLabel(label.c_str()); Invalidate(); }
}
BSize ZoomButton::MinSize() { return BSize(std::ceil(StringWidth("888%")) + 18, ToolButtonSize().Height()); }
BSize ZoomButton::MaxSize() { return MinSize(); }
BSize ZoomButton::PreferredSize() { return MinSize(); }
void ZoomButton::MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
{
    const bool hover = transit == B_ENTERED_VIEW || transit == B_INSIDE_VIEW;
    if (hover != fHover) { fHover = hover; Invalidate(); }
    BButton::MouseMoved(where, transit, drag);
}
void ZoomButton::Draw(BRect update)
{
    const auto colors = ChromeColorsFor(this);
    rgb_color ink;
    if (HaikuInterfaceStyle()) {
        DrawHaikuButton(this, update, fHover);
        ink = colors.controlText;
    } else {
        // A pill, like Firefox's zoom indicator in the address bar.
        SetHighColor(colors.panel);
        FillRect(Bounds());
        SetHighColor(colors.privateBrowsing ? Mix(colors.panel, colors.control, Value() ? 1.0f : 0.7f)
            : Value() ? rgb_color{200, 216, 212, 255} : fHover ? rgb_color{212, 225, 222, 255} : rgb_color{225, 232, 230, 255});
        FillRoundRect(Bounds().InsetByCopy(2, 4), 8, 8);
        ink = colors.privateBrowsing ? colors.text : rgb_color{49, 69, 66, 255};
    }
    font_height metrics;
    GetFontHeight(&metrics);
    const float width = StringWidth(Label());
    BPoint origin(std::floor((Bounds().Width() - width) / 2) + 0.5f,
        std::floor((Bounds().Height() + metrics.ascent - metrics.descent) / 2));
    if (HaikuInterfaceStyle() && Value()) origin += BPoint(1, 1);
    SetHighColor(ink);
    SetDrawingMode(B_OP_OVER);
    DrawString(Label(), origin);
    SetDrawingMode(B_OP_COPY);
}

PrivateBadge::PrivateBadge() : BView("private-badge", B_WILL_DRAW)
{
    SetViewColor(B_TRANSPARENT_COLOR);
    SetToolTip("Private window: Summit forgets its pages, cookies and site data when the last private window closes.");
}
BSize PrivateBadge::MinSize()
{
    BFont font(be_bold_font);
    return BSize(std::ceil(font.StringWidth("Private")) + 44, ToolButtonSize().Height());
}
BSize PrivateBadge::MaxSize() { return MinSize(); }
BSize PrivateBadge::PreferredSize() { return MinSize(); }
void PrivateBadge::Draw(BRect)
{
    const auto colors = ChromeColorsFor(this);
    SetHighColor(colors.panel);
    FillRect(Bounds());
    const BRect pill = Bounds().InsetByCopy(1, 3);
    const rgb_color fill = colors.privateBrowsing ? rgb_color{132, 96, 196, 255} : rgb_color{110, 80, 170, 255};
    SetHighColor(fill);
    FillRoundRect(pill, pill.Height() / 2, pill.Height() / 2);
    // A domino mask.
    SetHighColor(255, 255, 255);
    const float cy = std::floor((pill.top + pill.bottom) / 2) + 0.5f;
    const float x = pill.left + 9;
    FillRoundRect(BRect(x, cy - 4.5f, x + 18, cy + 4.5f), 4.5f, 4.5f);
    SetHighColor(fill);
    FillEllipse(BPoint(x + 5, cy), 2.8f, 2.1f);
    FillEllipse(BPoint(x + 13, cy), 2.8f, 2.1f);
    BFont font(be_bold_font);
    SetFont(&font);
    font_height metrics;
    font.GetHeight(&metrics);
    SetHighColor(255, 255, 255);
    SetDrawingMode(B_OP_OVER);
    DrawString("Private", BPoint(x + 24, std::floor(cy + (metrics.ascent - metrics.descent) / 2)));
    SetDrawingMode(B_OP_COPY);
}

#if SUMMIT_MODERN_WEBKIT
ExtensionActionButton::ExtensionActionButton(const char* identifier)
    : BButton((std::string("extension-action-") + identifier).c_str(), "", nullptr)
{
}
BSize ExtensionActionButton::MinSize() { return ToolButtonSize(); }
BSize ExtensionActionButton::MaxSize() { return ToolButtonSize(); }
BSize ExtensionActionButton::PreferredSize() { return ToolButtonSize(); }
ExtensionActionButton::~ExtensionActionButton() = default;

void ExtensionActionButton::SetAction(const BMessage& action, uint64 snapshot)
{
    const char* title = "";
    action.FindString("title", &title);
    if (!*title) action.FindString("name", &title);
    auto label = ExtensionDisplayText(title);
    SetLabel(label.c_str());
    const char* badge = "";
    action.FindString("badge", &badge);
    fBadge = ExtensionDisplayText(badge);
    const auto unpack = [](uint32 color) {
        return rgb_color { uint8(color >> 24), uint8(color >> 16), uint8(color >> 8), uint8(color) };
    };
    fBadgeBackground = unpack(action.GetUInt32("badge_background_rgba", 0xD90000FF));
    fBadgeTextColor = unpack(action.GetUInt32("badge_text_rgba", 0xFFFFFFFF));
    SetToolTip((label + (fBadge.empty() ? "" : " — " + fBadge)).c_str());
    auto* message = new BMessage(action);
    message->what = kActivateExtensionAction;
    message->AddUInt64("snapshot", snapshot);
    SetMessage(message);
    fPreload = *message;
    fPreload.what = kPreloadExtensionAction;
    fBitmap.reset();
    const void* pixels = nullptr;
    ssize_t length = 0;
    // On a screen drawn at twice the density the 32 pixel icon fills the
    // same 16 point square pixel for pixel.
    int32 size = 16;
    if (BWebKitDisplayScale() > 1.25f && action.FindData("icon_bgra_32", B_RAW_TYPE, &pixels, &length) == B_OK
        && length == 32 * 32 * 4)
        size = 32;
    else if (action.FindData("icon_bgra", B_RAW_TYPE, &pixels, &length) != B_OK || length != 16 * 16 * 4)
        pixels = nullptr;
    if (pixels) {
        auto bitmap = std::make_unique<BBitmap>(BRect(0, 0, size - 1, size - 1), B_RGBA32);
        if (bitmap->InitCheck() == B_OK) {
            for (int row = 0; row < size; ++row)
                std::memcpy(static_cast<uint8*>(bitmap->Bits()) + row * bitmap->BytesPerRow(),
                    static_cast<const uint8*>(pixels) + row * size * 4, size * 4);
            fBitmap = std::move(bitmap);
        }
    }
    SetEnabled(action.GetBool("enabled", false));
    Invalidate();
}

// Loading a popup page takes longer than showing it, so the page starts
// loading as soon as the pointer reaches the button, as Firefox does; the
// click that follows only has to show the window.
void ExtensionActionButton::Preload()
{
    if (IsEnabled() && Window() && fPreload.what == kPreloadExtensionAction) Window()->PostMessage(&fPreload);
}

void ExtensionActionButton::MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
{
    BButton::MouseMoved(where, transit, drag);
    if (transit == B_ENTERED_VIEW && !drag) Preload();
}

void ExtensionActionButton::MouseDown(BPoint where)
{
    int32 buttons = 0;
    if (Window() && Window()->CurrentMessage()) Window()->CurrentMessage()->FindInt32("buttons", &buttons);
    if (buttons & B_SECONDARY_MOUSE_BUTTON) {
        // The button's own menu (Unpin from Toolbar), not the action.
        BMessage menu(kExtensionActionMenu);
        menu.AddString("extension_identifier", Name() + std::strlen("extension-action-"));
        menu.AddPoint("where", ConvertToScreen(where));
        Window()->PostMessage(&menu);
        return;
    }
    Preload();
    BButton::MouseDown(where);
}

void ExtensionActionButton::Draw(BRect update)
{
    SetDrawingMode(B_OP_COPY);
    if (HaikuInterfaceStyle()) DrawHaikuButton(this, update, false);
    else {
        SetHighColor(ui_color(B_PANEL_BACKGROUND_COLOR));
        FillRect(Bounds());
        if (Value() || IsFocus()) {
            SetHighColor(Value() ? rgb_color{212, 225, 222, 255} : rgb_color{225, 232, 230, 255});
            FillRoundRect(Bounds().InsetByCopy(2, 2), 5, 5);
        }
    }
    const BPoint origin((Bounds().Width() - 15) / 2, (Bounds().Height() - 15) / 2);
    if (fBitmap) {
        SetDrawingMode(B_OP_ALPHA);
        SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        DrawBitmap(fBitmap.get(), fBitmap->Bounds(), BRect(origin, origin + BPoint(15, 15)), B_FILTER_BITMAP_BILINEAR);
        SetDrawingMode(B_OP_COPY);
    } else {
        SetHighColor(IsEnabled() ? rgb_color{49, 99, 84, 255} : rgb_color{163, 170, 168, 255});
        SetPenSize(1.5f);
        StrokeRoundRect(BRect(origin, origin + BPoint(15, 15)), 3, 3);
        StrokeLine(origin + BPoint(4, 7.5f), origin + BPoint(11, 7.5f));
        StrokeLine(origin + BPoint(7.5f, 4), origin + BPoint(7.5f, 11));
        SetPenSize(1);
    }
    if (!IsEnabled() && fBitmap) {
        SetDrawingMode(B_OP_ALPHA);
        SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
        auto color = ui_color(HaikuInterfaceStyle() ? B_CONTROL_BACKGROUND_COLOR : B_PANEL_BACKGROUND_COLOR); color.alpha = 160;
        SetHighColor(color);
        FillRect(BRect(origin, origin + BPoint(15, 15)));
        SetDrawingMode(B_OP_COPY);
    }
    if (!fBadge.empty()) {
        BFont font(be_bold_font); font.SetSize(9);
        SetFont(&font);
        BString badge(fBadge.c_str());
        TruncateString(&badge, B_TRUNCATE_END, 21);
        float width = std::max(12.0f, StringWidth(badge.String()) + 4);
        BRect rect(Bounds().right - width, 1, Bounds().right, 13);
        auto background = fBadgeBackground;
        auto foreground = fBadgeTextColor;
        if (!IsEnabled()) {
            background.alpha = uint8(background.alpha * 0.55f);
            foreground.alpha = uint8(foreground.alpha * 0.55f);
        }
        SetDrawingMode(B_OP_ALPHA);
        SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        SetHighColor(background);
        FillRoundRect(rect, 3, 3);
        SetHighColor(foreground);
        DrawString(badge.String(), BPoint(rect.left + 2, 10));
        SetDrawingMode(B_OP_COPY);
        SetFont(be_plain_font);
    }
}
#endif

CancellableMenu::CancellableMenu(const char* name, std::shared_ptr<std::atomic<bool>> cancelled)
    : BPopUpMenu(name, false, false), fCancelled(std::move(cancelled))
{
    SetAsyncAutoDestruct(true);
    SetTrackingHook([](BMenu*, void* state) {
        return static_cast<std::atomic<bool>*>(state)->load();
    }, fCancelled.get());
}
CancellableMenu::~CancellableMenu() = default;
void CancellableMenu::AttachedToWindow()
{
    BPopUpMenu::AttachedToWindow();
    BMessage tick('cmnt');
    fTimer = std::make_unique<BMessageRunner>(BMessenger(this), &tick, 50000);
}
void CancellableMenu::DetachedFromWindow()
{
    fTimer.reset();
    BPopUpMenu::DetachedFromWindow();
}
void CancellableMenu::MessageReceived(BMessage* message)
{
    if (message->what == 'cmnt') {
        // The tracking hook is not consulted while the menu waits for the
        // mouse; Escape wakes it (as the extension actions menu does).
        if (fCancelled->load()) { const char escape = B_ESCAPE; KeyDown(&escape, 1); }
        return;
    }
    BPopUpMenu::MessageReceived(message);
}

void DrawSiteIcon(BView* view, const BBitmap* icon, BPoint leftTop)
{
    if (icon) {
        view->PushState();
        view->SetDrawingMode(B_OP_ALPHA);
        view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        // Icons are kept at 32 pixels: exact on a 200% screen, filtered down otherwise.
        view->DrawBitmap(icon, icon->Bounds(), BRect(leftTop, leftTop + BPoint(15, 15)), B_FILTER_BITMAP_BILINEAR);
        view->PopState();
        return;
    }
    view->PushState();
    view->SetHighColor(128, 142, 135);
    view->SetPenSize(1.2f);
    const BPoint centre = leftTop + BPoint(7.5f, 7.5f);
    view->StrokeEllipse(centre, 6.5f, 6.5f);
    view->StrokeEllipse(centre, 2.8f, 6.5f);
    view->StrokeLine(centre + BPoint(-6.5f, 0), centre + BPoint(6.5f, 0));
    view->PopState();
}

TabStrip::TabStrip() : BView("tabs", B_WILL_DRAW | B_FRAME_EVENTS)
{
    SetViewColor(B_TRANSPARENT_COLOR);
}
float TabStrip::Height() const
{
    if (!HaikuInterfaceStyle()) return 36;
    font_height metrics;
    be_plain_font->GetHeight(&metrics);
    return std::max(26.0f, std::ceil(metrics.ascent + metrics.descent) + 14);
}
BSize TabStrip::MinSize() { return BSize(200, Height()); }
BSize TabStrip::MaxSize() { return BSize(B_SIZE_UNLIMITED, Height()); }
BSize TabStrip::PreferredSize() { return BSize(400, Height()); }
void TabStrip::SetTabs(std::vector<TabLabel> tabs, int64 selected)
{
    fTabs = std::move(tabs); fSelected = selected; Invalidate();
}
size_t TabStrip::VisibleCount() const
{
    const float reserved = HaikuInterfaceStyle() ? 36 : 60;
    return std::min(fTabs.size(), std::max(size_t(1), size_t(std::max(100.0f, Bounds().Width() - reserved) / 120)));
}
size_t TabStrip::FirstVisible() const
{
    size_t selected = 0;
    for (size_t i = 0; i < fTabs.size(); ++i) if (fTabs[i].id == fSelected) selected = i;
    const size_t count = VisibleCount();
    return selected >= count ? selected - count + 1 : 0;
}
BRect TabStrip::TabRect(size_t index) const
{
    const size_t count = VisibleCount();
    if (HaikuInterfaceStyle()) {
        // Adjacent tabs, as in BTabView and WebPositive.
        const float width = std::floor(std::min(250.0f, (Bounds().Width() - 36) / std::max(size_t(1), count)));
        const float left = (index - FirstVisible()) * width;
        return BRect(left, 0, left + width - 1, Bounds().bottom);
    }
    float width = std::min(250.0f, (Bounds().Width() - 60) / std::max(size_t(1), count));
    const float left = 8 + (index - FirstVisible()) * width;
    return BRect(left, 3, left + width - 3, Bounds().bottom - 3);
}
BRect TabStrip::CloseRect(size_t index) const
{
    const BRect tab = TabRect(index);
    if (!HaikuInterfaceStyle()) return BRect(tab.right - 24, tab.top, tab.right, tab.bottom);
    const bool selected = fTabs[index].id == fSelected;
    const float middle = std::floor((tab.top + (selected ? 2 : 4) + tab.bottom) / 2);
    return BRect(tab.right - 21, middle - 7, tab.right - 7, middle + 7);
}
BRect TabStrip::NewTabRect() const
{
    const BRect bounds = Bounds();
    if (!HaikuInterfaceStyle()) return BRect(bounds.right - 44, bounds.top, bounds.right, bounds.bottom);
    return BRect(bounds.right - 29, bounds.top + 3, bounds.right - 5, bounds.bottom - 3);
}
void TabStrip::Draw(BRect update)
{
    if (HaikuInterfaceStyle()) DrawHaiku(update);
    else DrawSafari(update);
}
void TabStrip::DrawHaiku(BRect update)
{
    const auto colors = ChromeColorsFor(this);
    const rgb_color base = colors.panel;
    const rgb_color text = colors.text;
    const uint32 borders = BControlLook::B_TOP_BORDER | BControlLook::B_BOTTOM_BORDER;
    BRect frame = Bounds();
    be_control_look->DrawTabFrame(this, frame, update, base, 0, borders, B_NO_BORDER);
    const size_t first = FirstVisible(), last = first + VisibleCount();
    int32 selectedIndex = -1;
    for (size_t i = first; i < last; ++i) if (fTabs[i].id == fSelected) selectedIndex = int32(i);
    font_height metrics;
    GetFontHeight(&metrics);
    for (size_t i = first; i < last; ++i) {
        const BRect rect = TabRect(i);
        const bool selected = int32(i) == selectedIndex;
        BRect tab = rect;
        tab.right++;
        tab.bottom++;
        if (i + 1 == last) tab.right -= 2;
        if (selected)
            be_control_look->DrawActiveTab(this, tab, update, base, 0, borders, BControlLook::B_TOP_BORDER,
                int32(i), selectedIndex, int32(first), int32(last - 1));
        else
            be_control_look->DrawInactiveTab(this, tab, update, base, 0, borders, BControlLook::B_TOP_BORDER,
                int32(i), selectedIndex, int32(first), int32(last - 1));
        // Contents sit lower on tabs behind the selected one, as BTabView draws them.
        BRect content = rect;
        content.top += selected ? 2 : 4;
        content.left += 8;
        content.right = CloseRect(i).left - 4;
        const float middle = std::floor((content.top + content.bottom) / 2);
        const BPoint icon(content.left, middle - 8);
        if (fTabs[i].loading && !fTabs[i].icon) {
            SetHighColor(Mix(text, base, 0.35f));
            FillEllipse(icon + BPoint(7.5f, 7.5f), 2.5f, 2.5f);
        } else DrawSiteIcon(this, fTabs[i].icon, icon);
        if (fTabs[i].loading && fTabs[i].icon) {
            SetHighColor(ui_color(B_CONTROL_HIGHLIGHT_COLOR));
            FillEllipse(icon + BPoint(15, 15), 2.5f, 2.5f);
        }
        if (fTabs[i].capturing) {
            SetHighColor(214, 40, 40);
            FillEllipse(icon + BPoint(15, 1), 3, 3);
        }
        BString title(fTabs[i].title.empty() ? "New Tab" : fTabs[i].title.c_str());
        const float textLeft = content.left + 22;
        TruncateString(&title, B_TRUNCATE_END, std::max(0.0f, content.right - textLeft));
        SetHighColor(selected ? text : Mix(text, base, 0.25f));
        SetLowColor(base);
        SetDrawingMode(B_OP_OVER);
        DrawString(title.String(), BPoint(textLeft, std::floor(middle + (metrics.ascent - metrics.descent) / 2)));
        SetDrawingMode(B_OP_COPY);
        // A small close cross, darker under the pointer (WebPositive's tab close box).
        const BRect close = CloseRect(i);
        const bool hover = fHoverClose == fTabs[i].id;
        if (hover) {
            SetHighColor(tint_color(base, colors.privateBrowsing ? B_LIGHTEN_1_TINT : B_DARKEN_2_TINT));
            FillRoundRect(close, 3, 3);
        }
        SetHighColor(hover ? text : Mix(text, base, 0.45f));
        SetPenSize(1.4f);
        const BPoint c((close.left + close.right) / 2, (close.top + close.bottom) / 2);
        StrokeLine(c + BPoint(-3, -3), c + BPoint(3, 3));
        StrokeLine(c + BPoint(3, -3), c + BPoint(-3, 3));
        SetPenSize(1);
    }
    // The new-tab "+" at the end of the tabs: just the sign, darker under the pointer.
    const BRect plus = NewTabRect();
    SetHighColor(fHoverNewTab ? text : Mix(text, base, 0.35f));
    SetPenSize(fHoverNewTab ? 2.0f : 1.6f);
    const BPoint c(std::floor((plus.left + plus.right) / 2) + 0.5f, std::floor((plus.top + plus.bottom) / 2) + 0.5f);
    StrokeLine(c + BPoint(-5, 0), c + BPoint(5, 0));
    StrokeLine(c + BPoint(0, -5), c + BPoint(0, 5));
    SetPenSize(1);
}
void TabStrip::DrawSafari(BRect)
{
    const auto colors = ChromeColorsFor(this);
    const bool dark = colors.privateBrowsing;
    const rgb_color strip = dark ? tint_color(colors.panel, B_DARKEN_1_TINT) : rgb_color{230, 233, 232, 255};
    const rgb_color selectedTab = dark ? colors.control : rgb_color{252, 253, 252, 255};
    const rgb_color selectedText = dark ? colors.controlText : rgb_color{30, 82, 72, 255};
    const rgb_color otherText = dark ? Mix(colors.text, strip, 0.2f) : rgb_color{78, 86, 83, 255};
    SetHighColor(strip); FillRect(Bounds());
    const size_t first = FirstVisible(), last = first + VisibleCount();
    for (size_t i = first; i < last; ++i) {
        BRect rect = TabRect(i);
        const bool selected = fTabs[i].id == fSelected;
        SetHighColor(selected ? selectedTab : strip);
        FillRoundRect(rect, 5, 5);
        SetHighColor(selected ? selectedText : otherText);
        BString text(fTabs[i].title.empty() ? "New Tab" : fTabs[i].title.c_str());
        TruncateString(&text, B_TRUNCATE_END, rect.Width() - 61);
        const BPoint icon(rect.left + 9, rect.top + std::floor((rect.Height() - 15) / 2));
        if (fTabs[i].loading && !fTabs[i].icon) FillEllipse(icon + BPoint(7.5f, 7.5f), 2.5f, 2.5f);
        else DrawSiteIcon(this, fTabs[i].icon, icon);
        if (fTabs[i].loading && fTabs[i].icon) {
            // A loading page keeps its icon, with a dot on its corner.
            SetHighColor(44, 125, 104);
            FillEllipse(icon + BPoint(15, 15), 2.5f, 2.5f);
            SetHighColor(selected ? selectedText : otherText);
        }
        if (fTabs[i].capturing) {
            // Camera, microphone or screen in use: a red dot on the other corner.
            SetHighColor(214, 40, 40);
            FillEllipse(icon + BPoint(15, 1), 3, 3);
            SetHighColor(selected ? selectedText : otherText);
        }
        DrawString(text, BPoint(rect.left + 31, 22));
        StrokeLine(BPoint(rect.right - 16, 14), BPoint(rect.right - 10, 20));
        StrokeLine(BPoint(rect.right - 10, 14), BPoint(rect.right - 16, 20));
    }
    SetHighColor(dark ? colors.text : rgb_color{63, 83, 76, 255});
    const float x = Bounds().right - 24;
    StrokeLine(BPoint(x - 5, 18), BPoint(x + 5, 18));
    StrokeLine(BPoint(x, 13), BPoint(x, 23));
}
void TabStrip::MouseDown(BPoint point)
{
    uint32 buttons = 0;
    int32 clicks = 1;
    if (auto* message = Window()->CurrentMessage()) {
        message->FindInt32("buttons", reinterpret_cast<int32*>(&buttons));
        message->FindInt32("clicks", &clicks);
    }
    if (NewTabRect().Contains(point)) { Window()->PostMessage(kNewTab); return; }
    for (size_t i = FirstVisible(); i < FirstVisible() + VisibleCount(); ++i) {
        const auto rect = TabRect(i);
        if (!rect.Contains(point)) continue;
        if (buttons & B_SECONDARY_MOUSE_BUTTON) {
            BMessage menu(kTabMenu);
            menu.AddInt64("id", fTabs[i].id);
            menu.AddPoint("where", ConvertToScreen(point));
            Window()->PostMessage(&menu);
            return;
        }
        const bool close = CloseRect(i).Contains(point) || buttons & B_TERTIARY_MOUSE_BUTTON;
        BMessage message(close ? kCloseTab : kSelectTab);
        message.AddInt64("id", fTabs[i].id);
        Window()->PostMessage(&message);
        return;
    }
    // A double click on the empty part of the strip opens a tab.
    if (clicks == 2 && (buttons & B_PRIMARY_MOUSE_BUTTON)) Window()->PostMessage(kNewTab);
}
void TabStrip::MouseMoved(BPoint where, uint32 transit, const BMessage*)
{
    int64 hoverClose = 0;
    bool hoverNewTab = false;
    if (HaikuInterfaceStyle() && transit != B_EXITED_VIEW && transit != B_OUTSIDE_VIEW) {
        hoverNewTab = NewTabRect().Contains(where);
        for (size_t i = FirstVisible(); i < FirstVisible() + VisibleCount(); ++i)
            if (CloseRect(i).Contains(where)) hoverClose = fTabs[i].id;
    }
    if (hoverClose == fHoverClose && hoverNewTab == fHoverNewTab) return;
    fHoverClose = hoverClose;
    fHoverNewTab = hoverNewTab;
    Invalidate();
}
void TabStrip::FrameResized(float, float) { Invalidate(); }
static constexpr int32 kOverflowItem = -2;
BookmarksBar::BookmarksBar() : BView("bookmarks-bar", B_WILL_DRAW | B_FRAME_EVENTS)
{
    SetViewColor(B_TRANSPARENT_COLOR);
    SetExplicitMinSize(BSize(200, 27));
    SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 27));
}
void BookmarksBar::SetBookmarks(std::vector<BookmarkButton> bookmarks)
{
    fBookmarks = std::move(bookmarks);
    fHover = fPressed = -1;
    LayoutItems();
    Invalidate();
}
void BookmarksBar::LayoutItems()
{
    fRects.clear();
    const float height = Bounds().Height();
    float x = 6;
    for (const auto& bookmark : fBookmarks) {
        float width = 8 + 16 + 8;
        if (!bookmark.title.empty()) width += 5 + std::min(150.0f, std::ceil(StringWidth(bookmark.title.c_str())));
        fRects.push_back(BRect(x, 2, x + width - 1, height - 3));
        x += width + 2;
    }
    fVisible = fRects.size();
    fOverflow = BRect();
    if (!fRects.empty() && fRects.back().right > Bounds().right - 6) {
        // Keep room for a » button that lists the rest.
        fOverflow = BRect(Bounds().right - 30, 2, Bounds().right - 6, height - 3);
        fVisible = 0;
        while (fVisible < fRects.size() && fRects[fVisible].right < fOverflow.left - 4) ++fVisible;
    }
}
void BookmarksBar::FrameResized(float, float) { LayoutItems(); Invalidate(); }
int32 BookmarksBar::ItemAt(BPoint where) const
{
    if (fOverflow.IsValid() && fOverflow.Contains(where)) return kOverflowItem;
    for (size_t i = 0; i < fVisible; ++i) if (fRects[i].Contains(where)) return int32(i);
    return -1;
}
void BookmarksBar::Draw(BRect update)
{
    const BRect bounds = Bounds();
    const bool haiku = HaikuInterfaceStyle();
    const auto colors = ChromeColorsFor(this);
    const bool dark = colors.privateBrowsing;
    const rgb_color panel = colors.panel;
    const rgb_color ink = haiku || dark ? colors.text : rgb_color{49, 69, 66, 255};
    SetHighColor(panel);
    FillRect(bounds);
    SetHighColor(dark ? tint_color(panel, B_DARKEN_1_TINT) : haiku ? tint_color(panel, B_DARKEN_2_TINT) : rgb_color{221, 227, 224, 255});
    StrokeLine(BPoint(bounds.left, bounds.bottom), BPoint(bounds.right, bounds.bottom));
    font_height metrics;
    GetFontHeight(&metrics);
    const float baseline = std::floor((bounds.Height() + metrics.ascent - metrics.descent) / 2);
    if (fBookmarks.empty()) {
        SetHighColor(haiku || dark ? Mix(ink, panel, 0.5f) : rgb_color{135, 147, 141, 255});
        DrawString("Add favourite pages here with Bookmarks › Add to Bookmarks Bar", BPoint(12, baseline));
        return;
    }
    for (size_t i = 0; i < fVisible; ++i) {
        const BRect rect = fRects[i];
        if (int32(i) == fPressed || int32(i) == fHover) {
            if (haiku) {
                // Flat buttons that rise under the pointer, like a Haiku toolbar.
                BRect button = rect;
                const uint32 flags = int32(i) == fPressed ? BControlLook::B_ACTIVATED : BControlLook::B_HOVER;
                be_control_look->DrawButtonFrame(this, button, update, colors.control, panel, flags);
                be_control_look->DrawButtonBackground(this, button, update, colors.control, flags);
            } else {
                SetHighColor(dark ? Mix(panel, colors.control, int32(i) == fPressed ? 1.0f : 0.6f)
                    : int32(i) == fPressed ? rgb_color{212, 225, 222, 255} : rgb_color{225, 232, 230, 255});
                FillRoundRect(rect, 5, 5);
            }
        }
        DrawSiteIcon(this, fBookmarks[i].icon, BPoint(rect.left + 8, std::floor(rect.top + (rect.Height() - 15) / 2)));
        if (fBookmarks[i].title.empty()) continue;
        BString title(fBookmarks[i].title.c_str());
        TruncateString(&title, B_TRUNCATE_END, 150);
        SetHighColor(ink);
        SetDrawingMode(B_OP_OVER);
        DrawString(title.String(), BPoint(rect.left + 29, baseline));
        SetDrawingMode(B_OP_COPY);
    }
    if (fOverflow.IsValid()) {
        if (fHover == kOverflowItem) {
            SetHighColor(dark ? Mix(panel, colors.control, 0.6f) : haiku ? tint_color(panel, B_DARKEN_1_TINT) : rgb_color{225, 232, 230, 255});
            FillRoundRect(fOverflow, 5, 5);
        }
        SetHighColor(ink);
        SetPenSize(1.4f);
        const BPoint c(std::floor((fOverflow.left + fOverflow.right) / 2), std::floor((fOverflow.top + fOverflow.bottom) / 2));
        for (float dx : {-3.0f, 2.0f}) {
            StrokeLine(c + BPoint(dx - 2, -4), c + BPoint(dx + 2, 0));
            StrokeLine(c + BPoint(dx + 2, 0), c + BPoint(dx - 2, 4));
        }
        SetPenSize(1);
    }
}
void BookmarksBar::MouseDown(BPoint where)
{
    uint32 buttons = 0;
    if (auto* message = Window()->CurrentMessage())
        message->FindInt32("buttons", reinterpret_cast<int32*>(&buttons));
    const int32 index = ItemAt(where);
    if (index == kOverflowItem) { ShowOverflow(); return; }
    if (index < 0) return;
    if (buttons & B_SECONDARY_MOUSE_BUTTON) { ShowContextMenu(index, where); return; }
    fPressed = index;
    fPressedButtons = buttons;
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    Invalidate();
}
void BookmarksBar::MouseUp(BPoint where)
{
    if (fPressed >= 0 && ItemAt(where) == fPressed)
        Open(fPressed, (fPressedButtons & B_TERTIARY_MOUSE_BUTTON) || (modifiers() & B_COMMAND_KEY));
    fPressed = -1;
    Invalidate();
}
void BookmarksBar::MouseMoved(BPoint where, uint32 transit, const BMessage*)
{
    const int32 hover = transit == B_EXITED_VIEW || transit == B_OUTSIDE_VIEW ? -1 : ItemAt(where);
    if (hover == fHover) return;
    fHover = hover;
    if (hover >= 0) SetToolTip(fBookmarks[hover].url.c_str());
    else SetToolTip(hover == kOverflowItem ? "More bookmarks" : static_cast<const char*>(nullptr));
    Invalidate();
}
void BookmarksBar::Open(int32 index, bool newTab)
{
    if (index < 0 || size_t(index) >= fBookmarks.size()) return;
    BMessage open(kOpenBookmark);
    open.AddString("url", fBookmarks[index].url.c_str());
    open.AddBool("new_tab", newTab);
    Window()->PostMessage(&open);
}
static BMessage* BookmarkMessage(uint32 what, const std::string& url, bool newTab = false)
{
    auto* message = new BMessage(what);
    message->AddString("url", url.c_str());
    if (newTab) message->AddBool("new_tab", true);
    return message;
}
BPopUpMenu* BookmarksBar::NewMenu(const char* name)
{
    if (fMenusCancelled && *fMenusCancelled) return new CancellableMenu(name, *fMenusCancelled);
    auto* menu = new BPopUpMenu(name, false, false);
    menu->SetAsyncAutoDestruct(true);
    return menu;
}
void BookmarksBar::ShowOverflow()
{
    auto* menu = NewMenu("more-bookmarks");
    for (size_t i = fVisible; i < fBookmarks.size(); ++i) {
        const auto& title = fBookmarks[i].title.empty() ? fBookmarks[i].url : fBookmarks[i].title;
        menu->AddItem(new BMenuItem(title.c_str(), BookmarkMessage(kOpenBookmark, fBookmarks[i].url)));
    }
    menu->SetTargetForItems(Window());
    menu->SetAsyncAutoDestruct(true);
    menu->Go(ConvertToScreen(BPoint(fOverflow.left, fOverflow.bottom + 1)), true, false,
        ConvertToScreen(fOverflow), true);
}
void BookmarksBar::ShowContextMenu(int32 index, BPoint where)
{
    const auto& url = fBookmarks[index].url;
    auto* menu = NewMenu("bookmark");
    menu->AddItem(new BMenuItem("Open", BookmarkMessage(kOpenBookmark, url)));
    menu->AddItem(new BMenuItem("Open in New Tab", BookmarkMessage(kOpenBookmark, url, true)));
    menu->AddSeparatorItem();
    menu->AddItem(new BMenuItem("Remove from Bookmarks Bar", BookmarkMessage(kRemoveFromBookmarksBar, url)));
    menu->AddItem(new BMenuItem("Delete Bookmark", BookmarkMessage(kRemoveBookmark, url)));
    menu->SetTargetForItems(Window());
    menu->SetAsyncAutoDestruct(true);
    menu->Go(ConvertToScreen(where), true, false, true);
}

ProgressLine::ProgressLine() : BView("progress", B_WILL_DRAW)
{
    SetViewColor(B_TRANSPARENT_COLOR);
    SetExplicitMinSize(BSize(0, 2));
    SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 2));
}
void ProgressLine::SetProgress(float value) { fProgress = std::clamp(value, 0.0f, 1.0f); Invalidate(); }
void ProgressLine::Draw(BRect)
{
    const auto colors = ChromeColorsFor(this);
    SetHighColor(colors.privateBrowsing ? tint_color(colors.panel, B_DARKEN_1_TINT) : rgb_color{214, 221, 217, 255});
    FillRect(Bounds());
    if (fProgress > 0 && fProgress < 1) {
        SetHighColor(44, 125, 104);
        BRect progress = Bounds(); progress.right *= fProgress; FillRect(progress);
    }
}
}

#pragma once
#if SUMMIT_MODERN_WEBKIT
// Developer Tools › Responsive Design Mode (#155, docs/developer-tools.md):
// a tab's page laid out at a phone's, tablet's or chosen size inside the
// window, centred over a neutral background, with a bar above it to choose
// the device, size and user agent. The window keeps its size.
//
// It is the browser's alone: the page view is simply given that size, so
// the page's viewport (innerWidth, media queries) is the device's. The
// engine has no device scale override, so the page's devicePixelRatio stays
// the display's.
#include <CardLayout.h>
#include <GroupView.h>
#include <Size.h>
#include <functional>
#include <optional>
#include <span>
#include <string>

class BButton;
class BMenuField;
class BPopUpMenu;
class BStringView;
class BTextControl;

namespace summit {
struct ResponsiveDevice {
    const char* name;
    int32 width, height;
    // The ResponsiveAgents() identifier of the device's own browser.
    const char* agent;
};
// The presets of the bar's device menu.
std::span<const ResponsiveDevice> ResponsiveDevices();

struct ResponsiveAgent {
    const char* id;
    const char* name;
    // Empty: the tab's normal user agent (Preferences').
    const char* agent;
};
// "device" (the chosen device's own browser), "summit" (the normal one) and
// mobile browsers.
std::span<const ResponsiveAgent> ResponsiveAgents();

// A tab's Responsive Design Mode.
struct ResponsiveState {
    bool active = false;
    // An index of ResponsiveDevices(), or -1 for a size typed in.
    int32 device = 0;
    int32 width = 393, height = 852;
    // A ResponsiveAgents() identifier.
    std::string agent = "device";
};
// The user agent the state gives its page; null when the page has its
// normal one (inactive, or "summit").
const char* ResponsiveUserAgent(const ResponsiveState& state);
// Typed sizes are kept within these.
constexpr int32 kResponsiveMinimum = 50, kResponsiveMaximum = 8192;

// The bar's controls and View › Responsive Design Mode, to the browser
// window: kResponsiveDesign toggles the selected tab's mode ("tab" int64 for
// another tab, "active" bool to set it rather than toggle); kResponsiveDevice
// ("device" int32, -1 custom), kResponsiveSize ("width", "height" int32;
// without them the bar's fields), kResponsiveRotate and kResponsiveAgent
// ("agent") change the selected tab's. A tab's Developer Tools window is
// told the tab's mode (kResponsiveDesignState: "active").
constexpr uint32 kResponsiveDesign = 'rspt';
constexpr uint32 kResponsiveDevice = 'rspd';
constexpr uint32 kResponsiveSize = 'rsps';
constexpr uint32 kResponsiveRotate = 'rspr';
constexpr uint32 kResponsiveAgent = 'rspa';
constexpr uint32 kResponsiveDesignState = 'rspS';
// From PageCards: the size a page in the mode was given ("width", "height").
constexpr uint32 kResponsiveShown = 'rspz';

// The browser window's page cards: a card with a fixed size (a page in
// Responsive Design Mode) is laid out at that size, centred in the cards'
// area; where the area is smaller, at the area's size in that direction
// (never growing the window's minimum size). shown hears the size such a
// card was given when it changes.
class PageCards final : public BCardLayout {
public:
    using FixedSize = std::function<std::optional<BSize>(BView*)>;
    using Shown = std::function<void(BSize)>;
    PageCards(FixedSize fixed, Shown shown) : fFixed(std::move(fixed)), fShown(std::move(shown)) { }
    void DoLayout() override;

private:
    FixedSize fFixed;
    Shown fShown;
    BSize fLastShown { -1, -1 };
};

// The bar above the page.
class ResponsiveBar final : public BGroupView {
public:
    ResponsiveBar();
    void AttachedToWindow() override;
    // Shows state's choices.
    void SetState(const ResponsiveState& state);
    // Says whether the window had room for state's size (shown: the size given).
    void SetShown(const ResponsiveState& state, BSize shown);
    // The width and height fields, when both are numbers.
    std::optional<std::pair<int32, int32>> TypedSize() const;

private:
    BPopUpMenu* fDevices;
    BPopUpMenu* fAgents;
    BTextControl* fWidth;
    BTextControl* fHeight;
    BStringView* fScale;
    BStringView* fShownSize;
};
}
#endif

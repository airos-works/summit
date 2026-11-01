#if SUMMIT_MODERN_WEBKIT
#include "ResponsiveDesign.h"
#include <WebKit/WebKitView.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace summit {
namespace {
// Sizes in CSS pixels, portrait, as the devices' browsers report them.
constexpr std::array<ResponsiveDevice, 4> kDevices {{
    {"iPhone 15", 393, 852, "safari-ios"},
    // iPadOS Safari asks for desktop sites: it sends the Mac's agent.
    {"iPad", 820, 1180, "safari-ipados"},
    {"Pixel 8", 412, 915, "chrome-android"},
    {"Desktop", 1280, 800, "summit"},
}};

// Fixed identities, as Preferences' (core/UserAgent.cpp): iOS Safari froze
// its OS token at 18_6, Chrome's reduced agent its Android version and model.
constexpr std::array<ResponsiveAgent, 5> kAgents {{
    {"device", "The device's", ""},
    {"summit", "Summit (normal)", ""},
    {"safari-ios", "Safari — iPhone", "Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Mobile/15E148 Safari/604.1"},
    {"safari-ipados", "Safari — iPad", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15"},
    {"chrome-android", "Chrome — Android", "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Mobile Safari/537.36"},
}};

const ResponsiveAgent* FindAgent(const std::string& id)
{
    for (const auto& agent : kAgents)
        if (id == agent.id) return &agent;
    return nullptr;
}

BTextControl* SizeField(const char* name)
{
    auto* field = new BTextControl(name, nullptr, "", new BMessage(kResponsiveSize));
    // Room for four digits, no more.
    const float width = field->StringWidth("00000") + 16;
    field->SetExplicitMinSize(BSize(width, B_SIZE_UNSET));
    field->SetExplicitMaxSize(BSize(width, B_SIZE_UNSET));
    field->TextView()->SetMaxBytes(4);
    for (uint32 c = 32; c < 128; ++c)
        if (c < '0' || c > '9') field->TextView()->DisallowChar(c);
    return field;
}
}

std::span<const ResponsiveDevice> ResponsiveDevices() { return kDevices; }
std::span<const ResponsiveAgent> ResponsiveAgents() { return kAgents; }

const char* ResponsiveUserAgent(const ResponsiveState& state)
{
    if (!state.active) return nullptr;
    std::string id = state.agent;
    if (id == "device")
        id = state.device >= 0 && state.device < static_cast<int32>(kDevices.size()) ? kDevices[state.device].agent : "summit";
    const auto* agent = FindAgent(id);
    return agent && *agent->agent ? agent->agent : nullptr;
}

void PageCards::DoLayout()
{
    BCardLayout::DoLayout();
    auto* item = VisibleItem();
    if (!item || !item->View() || !fFixed) return;
    const auto size = fFixed(item->View());
    if (!size) return;
    const BRect area = LayoutArea();
    // Never past the area: the engine puts pages straight into the screen
    // wherever their view's frame is in the window, over the status line and
    // whatever else is below or beside it. BRect widths are a pixel short of
    // the size they cover.
    const float width = std::min(size->width, area.Width() + 1), height = std::min(size->height, area.Height() + 1);
    const float left = area.left + std::floor((area.Width() + 1 - width) / 2);
    const float top = area.top + std::floor((area.Height() + 1 - height) / 2);
    item->SetFrame(BRect(left, top, left + width - 1, top + height - 1));
    if (fShown && (width != fLastShown.width || height != fLastShown.height)) {
        fLastShown = BSize(width, height);
        fShown(BSize(width, height));
    }
}

ResponsiveBar::ResponsiveBar()
    : BGroupView("responsive-design", B_HORIZONTAL, 6)
{
    SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
    fDevices = new BPopUpMenu("device");
    for (int32 i = 0; i < static_cast<int32>(kDevices.size()); ++i) {
        auto* message = new BMessage(kResponsiveDevice);
        message->AddInt32("device", i);
        char label[96];
        std::snprintf(label, sizeof label, "%s (%d × %d)", kDevices[i].name, static_cast<int>(kDevices[i].width),
            static_cast<int>(kDevices[i].height));
        fDevices->AddItem(new BMenuItem(label, message));
    }
    fDevices->AddSeparatorItem();
    auto* custom = new BMessage(kResponsiveDevice);
    custom->AddInt32("device", -1);
    fDevices->AddItem(new BMenuItem("Custom", custom));
    fAgents = new BPopUpMenu("agent");
    for (const auto& agent : kAgents) {
        auto* message = new BMessage(kResponsiveAgent);
        message->AddString("agent", agent.id);
        fAgents->AddItem(new BMenuItem(agent.name, message));
    }
    fWidth = SizeField("responsive-width");
    fWidth->SetToolTip("Width of the page in CSS pixels");
    fHeight = SizeField("responsive-height");
    fHeight->SetToolTip("Height of the page in CSS pixels");
    auto* rotate = new BButton("responsive-rotate", "Rotate", new BMessage(kResponsiveRotate));
    rotate->SetToolTip("Swap width and height (portrait or landscape)");
    // No device scale override in the engine: the page's devicePixelRatio is
    // the display's, which the bar says after the size.
    fScale = new BStringView("responsive-scale", "");
    fScale->SetToolTip("Device pixel ratio: the display's (Summit cannot emulate another)");
    // Said only while the window is too small for the size.
    fShownSize = new BStringView("responsive-shown", "");
    fShownSize->SetHighUIColor(B_FAILURE_COLOR);
    fShownSize->SetToolTip("The window is too small for this size: the page is shown smaller, and its viewport is too");
    auto* exit = new BButton("responsive-exit", "Exit", new BMessage(kResponsiveDesign));
    exit->SetToolTip("Exit Responsive Design Mode");
    BLayoutBuilder::Group<>(this).SetInsets(8, 3, 8, 3)
        .Add(new BMenuField("responsive-device", "Device:", fDevices))
        .Add(fWidth)
        .Add(new BStringView("responsive-by", "×"))
        .Add(fHeight)
        .Add(fScale)
        .Add(rotate)
        .Add(new BMenuField("responsive-agent", "User agent:", fAgents))
        .Add(fShownSize)
        .AddGlue()
        .Add(exit);
    // The window keeps its own minimum width: in a narrow window the bar's
    // last controls are cut off rather than the window made wider.
    SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
}

void ResponsiveBar::AttachedToWindow()
{
    BGroupView::AttachedToWindow();
    fDevices->SetTargetForItems(Window());
    fAgents->SetTargetForItems(Window());
}

void ResponsiveBar::SetState(const ResponsiveState& state)
{
    const int32 index = state.device >= 0 && state.device < static_cast<int32>(kDevices.size())
        ? state.device : fDevices->CountItems() - 1;
    if (auto* item = fDevices->ItemAt(index)) item->SetMarked(true);
    for (int32 i = 0; i < fAgents->CountItems(); ++i)
        if (auto* item = fAgents->ItemAt(i); item && item->Message()
            && !std::strcmp(item->Message()->GetString("agent", ""), state.agent.c_str()))
            item->SetMarked(true);
    // Not while the user types in it.
    const auto show = [](BTextControl* field, int32 value) {
        const std::string text = std::to_string(value);
        if (std::strcmp(field->Text(), text.c_str()) && !field->TextView()->IsFocus()) field->SetText(text.c_str());
    };
    show(fWidth, state.width);
    show(fHeight, state.height);
    char scale[64];
    std::snprintf(scale, sizeof scale, "@%g×", std::round(BWebKitDisplayScale() * 100) / 100);
    fScale->SetText(scale);
}

void ResponsiveBar::SetShown(const ResponsiveState& state, BSize shown)
{
    const int32 width = static_cast<int32>(shown.width), height = static_cast<int32>(shown.height);
    std::string text;
    if (width < state.width || height < state.height)
        text = "Window too small: " + std::to_string(width) + " × " + std::to_string(height);
    if (text != fShownSize->Text()) fShownSize->SetText(text.c_str());
}

std::optional<std::pair<int32, int32>> ResponsiveBar::TypedSize() const
{
    const auto number = [](const BTextControl* field) -> std::optional<int32> {
        const char* text = field->Text();
        if (!text || !*text) return std::nullopt;
        return std::clamp<int32>(std::atoi(text), kResponsiveMinimum, kResponsiveMaximum);
    };
    const auto width = number(fWidth), height = number(fHeight);
    if (!width || !height) return std::nullopt;
    return std::make_pair(*width, *height);
}
}
#endif

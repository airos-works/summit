#if SUMMIT_MODERN_WEBKIT
// Developer Tools › Responsive Design Mode (#155, ResponsiveDesign.h): the
// tab's page view is laid out at the chosen device's size (PageCards) under
// a bar of choices, and given the device's user agent if one is chosen.
#include "BrowserWindow.h"
#include "SharedProfile.h"
#include "core/UserAgent.h"
#include <MenuItem.h>
#include <algorithm>
#include <cstring>
#include <tuple>

namespace summit {
bool BrowserWindow::ResponsiveMessage(BMessage& message)
{
    switch (message.what) {
        case kResponsiveDesign: {
            Tab* tab = message.HasInt64("tab") ? FindTabByID(message.GetInt64("tab", 0)) : ActiveTab();
            if (tab && !fClosingWindow) SetResponsiveDesign(*tab, message.GetBool("active", !tab->responsive.active));
            return true;
        }
        case kResponsiveShown:
            if (auto* tab = ActiveTab(); tab && tab->responsive.active)
                fResponsiveBar->SetShown(tab->responsive, BSize(message.GetFloat("width", 0), message.GetFloat("height", 0)));
            return true;
        case kResponsiveDevice:
        case kResponsiveSize:
        case kResponsiveRotate:
        case kResponsiveAgent:
            break;
        default:
            return false;
    }
    auto* tab = ActiveTab();
    if (!tab || !tab->responsive.active || fClosingWindow) return true;
    auto& state = tab->responsive;
    const auto devices = ResponsiveDevices();
    switch (message.what) {
        case kResponsiveDevice: {
            const int32 device = message.GetInt32("device", -1);
            // A device starts upright, as in Safari; Custom keeps the size.
            if (device >= 0 && device < static_cast<int32>(devices.size())) {
                state.width = devices[device].width;
                state.height = devices[device].height;
            }
            state.device = device >= 0 && device < static_cast<int32>(devices.size()) ? device : -1;
            break;
        }
        case kResponsiveSize: {
            std::optional<std::pair<int32, int32>> size;
            if (message.HasInt32("width") && message.HasInt32("height"))
                size = std::make_pair(std::clamp(message.GetInt32("width", 0), kResponsiveMinimum, kResponsiveMaximum),
                    std::clamp(message.GetInt32("height", 0), kResponsiveMinimum, kResponsiveMaximum));
            else size = fResponsiveBar->TypedSize();
            if (!size || (size->first == state.width && size->second == state.height)) break;
            std::tie(state.width, state.height) = *size;
            // Still the device when it is its size, either way up.
            if (state.device >= 0) {
                const auto& device = devices[state.device];
                if (!((state.width == device.width && state.height == device.height)
                        || (state.width == device.height && state.height == device.width)))
                    state.device = -1;
            }
            break;
        }
        case kResponsiveRotate: std::swap(state.width, state.height); break;
        case kResponsiveAgent: {
            const char* agent = message.GetString("agent", "");
            for (const auto& known : ResponsiveAgents())
                if (!std::strcmp(known.id, agent)) state.agent = agent;
            break;
        }
    }
    // The next tab put in the mode starts with the same choices.
    fResponsiveLast = state;
    ApplyResponsiveDesign(*tab);
    return true;
}

void BrowserWindow::SetResponsiveDesign(Tab& tab, bool active)
{
    // A full-screen page has the screen, not a device's size.
    if (fFullScreenTab) ExitPageFullScreen();
    if (tab.responsive.active != active) {
        if (active) tab.responsive = fResponsiveLast;
        tab.responsive.active = active;
        ApplyResponsiveDesign(tab);
    }
    if (tab.devTools.IsValid()) {
        BMessage state(kResponsiveDesignState);
        state.AddBool("active", active);
        tab.devTools.SendMessage(&state);
    }
    if (&tab == ActiveTab()) {
        ShowResponsiveDesign();
        tab.view->MakeFocus();
    }
}

void BrowserWindow::ApplyResponsiveDesign(Tab& tab)
{
    const char* agent = ResponsiveUserAgent(tab.responsive);
    if (const std::string wanted = agent ? agent : ""; wanted != tab.responsiveAgent) {
        tab.responsiveAgent = wanted;
        tab.view->SetCustomUserAgent(agent ? agent : fShared->Read([](const Profile& profile) {
            return FindUserAgentPreset(profile.userAgent).agent;
        }));
        // As Safari does: the page loads again as the device's browser would
        // be sent it (navigator.userAgent, and the server's own choice).
        if (tab.deferredURL.empty() && !tab.url.empty() && !tab.url.starts_with("summit:")
            && !tab.crashView && !tab.errorView && !tab.safetyView)
            tab.view->Reload();
    }
    if (&tab == ActiveTab()) ShowResponsiveDesign();
}

void BrowserWindow::ShowResponsiveDesign()
{
    if (!fResponsiveBar) return;
    const auto* tab = ActiveTab();
    const bool active = tab && tab->responsive.active;
    if (active) fResponsiveBar->SetState(tab->responsive);
    // Full screen hides the bar itself, and shows it again after.
    if (!fFullScreenTab) {
        if (active && fResponsiveBar->IsHidden(fResponsiveBar)) fResponsiveBar->Show();
        else if (!active && !fResponsiveBar->IsHidden(fResponsiveBar)) fResponsiveBar->Hide();
    }
    if (fResponsiveItem)
        fResponsiveItem->SetLabel(active ? "Exit Responsive Design Mode" : "Enter Responsive Design Mode");
    // A neutral background around the page, and the pages' own colour back
    // after: it shows only while a page is resized.
    if (active && !fPagesColor) {
        fPagesColor = fPages->ViewColor();
        fPages->SetViewUIColor(B_PANEL_BACKGROUND_COLOR, B_DARKEN_2_TINT);
        fPages->Invalidate();
    } else if (!active && fPagesColor) {
        fPages->SetViewColor(*fPagesColor);
        fPagesColor.reset();
        fPages->Invalidate();
    }
    // The page's size: the device's, or the whole area again.
    fPages->InvalidateLayout();
    fPages->Layout(true);
    // Later layouts (the window resized) say so with kResponsiveShown.
    if (active) {
        const BRect frame = tab->view->Frame();
        fResponsiveBar->SetShown(tab->responsive, BSize(frame.Width() + 1, frame.Height() + 1));
    }
}

std::optional<BSize> BrowserWindow::ResponsivePageSize(BView* view)
{
    for (const auto& tab : fTabs)
        if (tab.view == view) {
            if (!tab.responsive.active || tab.id == fFullScreenTab) return std::nullopt;
            return BSize(tab.responsive.width, tab.responsive.height);
        }
    return std::nullopt;
}
}
#endif

#if SUMMIT_MODERN_WEBKIT
// Browser profiles (#157, Profiles.h) in the browser window: File › New
// Window in Profile, and the window's profile shown in its title and as a
// badge in the toolbar while there is more than one profile.
#include "BrowserWindow.h"
#include "Chrome.h"
#include "Messages.h"
#include "Profiles.h"
#include <Application.h>
#include <Menu.h>
#include <MenuItem.h>

namespace summit {
void BrowserWindow::AddProfileMenu(BMenu* file)
{
    fProfileMenu = new BMenu("New Window in Profile");
    file->AddItem(fProfileMenu);
    RefreshProfileMenu();
}

void BrowserWindow::RefreshProfileMenu()
{
    if (!fProfileMenu) return;
    while (fProfileMenu->CountItems() > 0) delete fProfileMenu->RemoveItem(int32(0));
    const auto current = CurrentProfileID();
    for (const auto& entry : KnownProfiles().entries) {
        auto* message = new BMessage(kOpenProfileWindow);
        message->AddString("id", entry.id.c_str());
        auto* item = NewColorMenuItem(entry.name.c_str(), message, ProfileRGB(entry.color));
        item->SetTarget(be_app);
        item->SetMarked(entry.id == current);
        fProfileMenu->AddItem(item);
    }
    fProfileMenu->AddSeparatorItem();
    auto* manage = new BMessage(kShowPreferences);
    manage->AddString("tab", "profiles");
    auto* item = new BMenuItem("Manage Profiles…", manage);
    item->SetTarget(be_app);
    fProfileMenu->AddItem(item);
}

void BrowserWindow::ShowProfile()
{
    const auto label = CurrentProfileLabel();
    fProfileTitle = label.shown ? " (" + label.name + ")" : std::string();
    if (fProfileBadge) {
        fProfileBadge->SetLabel(label);
        if (label.shown && fProfileBadge->IsHidden(fProfileBadge)) fProfileBadge->Show();
        else if (!label.shown && !fProfileBadge->IsHidden(fProfileBadge)) fProfileBadge->Hide();
    }
    RefreshProfileMenu();
}

bool BrowserWindow::ProfileMessage(BMessage& message)
{
    if (message.what != kProfilesChanged) return false;
    ShowProfile();
    RefreshChrome();
    return true;
}
}
#endif

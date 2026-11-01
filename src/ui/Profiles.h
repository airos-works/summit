#pragma once
#include "core/BrowserProfiles.h"
#include <GraphicsDefs.h>
#include <GroupView.h>
#include <Messenger.h>
#include <OS.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class BButton;
class BListView;
class BMessageRunner;
class BStringView;

namespace summit {
// Browser profiles (#157). Each profile runs in a Summit process of its own,
// started with --profile and its folder (BrowserProfiles.h): its windows,
// engine context (cookies and website data), extensions, history, bookmarks
// and settings are that process's, as with one profile per Summit before.
// Summit is B_MULTIPLE_LAUNCH so that one executable can run several; one
// process per profile folder is kept by the folder's lock (ProfileLock), a
// second start handing its pages to the first. Processes find each other by
// the team the lock file names.

// To the application (and from it to its windows and the Preferences page):
// profiles.json changed.
constexpr uint32 kProfilesChanged = 'bpfc';
// To the application: a new window of the profile "id".
constexpr uint32 kOpenProfileWindow = 'bpfw';
// From another profile's Summit that quits (File › Quit): quit as well.
constexpr uint32 kQuitWithOtherProfiles = 'bpfq';
// A start that only opens a new window (in the running Summit of the profile).
constexpr const char* kNewWindowArgument = "--new-window";

// This process's profile, set once as Summit starts (application thread).
void SetCurrentProfile(const std::filesystem::path& base, const std::string& id);
std::filesystem::path ProfileBase();
std::string CurrentProfileID();
std::filesystem::path ProfileDirectory(const std::string& id);
// The list as last loaded (ReloadProfiles, on kProfilesChanged). Any thread.
BrowserProfiles KnownProfiles();
void ReloadProfiles();
// What windows show of their profile: nothing while there is only one.
struct ProfileLabel {
    bool shown = false;
    std::string name;
    rgb_color color { 0, 0, 0, 255 };
};
ProfileLabel CurrentProfileLabel();
rgb_color ProfileRGB(const std::string& color);
// The team of the Summit that has profile id open (this one's too), or -1.
team_id RunningProfileTeam(const std::string& id);
// Opens a window of profile id in its Summit, which is started if it is not
// running (another profile's), or activated only (newWindow false). Not for
// this process's own profile.
status_t StartProfile(const std::string& id, bool newWindow, std::string& error);
// Sends kProfilesChanged to every running profile's Summit, this one's too.
void BroadcastProfilesChanged();
// File › Quit quits every profile's Summit: asks the others.
void QuitOtherProfiles();
// Records whether this profile's windows open again with the default
// profile's (quitting with windows open: true; starting: false).
void SetReopenProfile(bool reopen);

// The toolbar's mark of the window's profile: its name on its colour.
class ProfileBadge final : public BView {
public:
    ProfileBadge();
    void SetLabel(const ProfileLabel&);
    void Draw(BRect update) override;
    BSize MinSize() override;
    BSize MaxSize() override;
    BSize PreferredSize() override;
private:
    ProfileLabel fLabel;
};

// Preferences › Profiles: the profiles, with their colours and which are
// open; New Profile…, Edit… (name and colour), Open Window and Delete… (its
// folder goes to the Trash, after a question; not while it is open).
class ProfilesPreferences final : public BGroupView {
public:
    ProfilesPreferences();
    ~ProfilesPreferences() override;
    void AttachedToWindow() override;
    void DetachedFromWindow() override;
    void MessageReceived(BMessage* message) override;
private:
    void Fill();
    void UpdateButtons();
    std::string SelectedID() const;
    void Edit(const std::string& id);
    void Delete(const std::string& id);
    BListView* fList;
    BButton* fEdit;
    BButton* fOpen;
    BButton* fDelete;
    BStringView* fStatus;
    std::vector<std::string> fIDs;
    // What the list shows, to leave it alone when nothing changed.
    std::string fSignature;
    std::unique_ptr<BMessageRunner> fRefresh;
};
}

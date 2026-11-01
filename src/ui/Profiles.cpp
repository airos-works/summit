#include "Profiles.h"
#include "Chrome.h"
#include "DefaultBrowser.h"
#include "McpService.h"
#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Invoker.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <ScrollView.h>
#include <String.h>
#include <StringView.h>
#include <TextControl.h>
#include <Volume.h>
#include <Window.h>
#include <image.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace summit {
namespace {
std::mutex sLock;
std::filesystem::path sBase;
std::string sID = BrowserProfiles::kDefaultID;
BrowserProfiles sProfiles;

std::string ExecutablePath()
{
    int32 cookie = 0;
    image_info image;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK)
        if (image.type == B_APP_IMAGE) return image.name;
    return { };
}

// The team named by a profile folder's lock file, when a Summit holds the
// lock: a file left by a Summit that crashed names a team that is gone, or
// another one by now.
team_id LockHolder(const std::filesystem::path& directory)
{
    const int descriptor = ::open((directory / "lock").c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return -1;
    char text[32] = { };
    const bool read = ::pread(descriptor, text, sizeof text - 1, 0) > 0;
    // A shared lock is only had while nobody holds the exclusive one.
    const bool held = ::flock(descriptor, LOCK_SH | LOCK_NB) != 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
    ::close(descriptor);
    if (!read || !held) return -1;
    const team_id team = static_cast<team_id>(std::strtol(text, nullptr, 10));
    app_info info;
    if (team <= 0 || be_roster->GetRunningAppInfo(team, &info) != B_OK || strcasecmp(info.signature, kSummitSignature)) return -1;
    return team;
}

void SendToTeam(team_id team, BMessage& message)
{
    BMessenger application(nullptr, team);
    if (application.IsValid()) application.SendMessage(&message, static_cast<BHandler*>(nullptr), 1000000);
}
}

void SetCurrentProfile(const std::filesystem::path& base, const std::string& id)
{
    {
        std::lock_guard lock(sLock);
        sBase = base;
        sID = id;
    }
    ReloadProfiles();
}

std::filesystem::path ProfileBase()
{
    std::lock_guard lock(sLock);
    return sBase;
}

std::string CurrentProfileID()
{
    std::lock_guard lock(sLock);
    return sID;
}

std::filesystem::path ProfileDirectory(const std::string& id) { return BrowserProfiles::Directory(ProfileBase(), id); }

BrowserProfiles KnownProfiles()
{
    std::lock_guard lock(sLock);
    return sProfiles;
}

void ReloadProfiles()
{
    const auto base = ProfileBase();
    auto profiles = base.empty() ? BrowserProfiles { } : BrowserProfiles::Load(base);
    std::lock_guard lock(sLock);
    // A profile started by its folder before it was listed (or after it was
    // deleted) still has a name: its folder's.
    if (!profiles.Find(sID)) profiles.entries.push_back({sID, sID, "grey", false});
    sProfiles = std::move(profiles);
}

rgb_color ProfileRGB(const std::string& name)
{
    const auto& color = FindProfileColor(name);
    return {color.red, color.green, color.blue, 255};
}

ProfileLabel CurrentProfileLabel()
{
    std::lock_guard lock(sLock);
    ProfileLabel label;
    const auto* entry = sProfiles.Find(sID);
    if (!entry) return label;
    label.shown = sProfiles.entries.size() > 1;
    label.name = entry->name;
    label.color = ProfileRGB(entry->color);
    return label;
}

team_id RunningProfileTeam(const std::string& id)
{
    if (id == CurrentProfileID()) return be_app ? be_app->Team() : -1;
    return LockHolder(ProfileDirectory(id));
}

status_t StartProfile(const std::string& id, bool newWindow, std::string& error)
{
    const auto profiles = KnownProfiles();
    const auto found = std::find_if(profiles.entries.begin(), profiles.entries.end(), [&](const auto& entry) { return entry.id == id; });
    if (found == profiles.entries.end() || id == CurrentProfileID()) {
        error = "There is no such profile.";
        return B_BAD_VALUE;
    }
    if (const team_id team = RunningProfileTeam(id); team >= 0) {
        BMessage start(newWindow ? uint32(B_ARGV_RECEIVED) : uint32(B_SILENT_RELAUNCH));
        if (newWindow) {
            start.AddInt32("argc", 2);
            start.AddString("argv", "Summit");
            start.AddString("argv", kNewWindowArgument);
        }
        SendToTeam(team, start);
        be_roster->ActivateApp(team);
        return B_OK;
    }
    const auto directory = ProfileDirectory(id);
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    const std::string executable = ExecutablePath();
    if (executable.empty()) {
        error = "Summit could not find its own executable.";
        return B_ENTRY_NOT_FOUND;
    }
    std::vector<std::string> arguments { executable, "--profile", directory.string() };
    // The environment of this Summit, except that a debugging server set up
    // in it (SUMMIT_MCP_PORT, as test runs do) moves to a port of the
    // profile's own: the base port plus the profile's place in the list.
    std::vector<std::string> environment;
    for (char** variable = environ; variable && *variable; ++variable) {
        std::string entry = *variable;
        if (entry.rfind("SUMMIT_MCP_PORT=", 0) == 0) {
            const int port = std::atoi(entry.c_str() + 16);
            if (port > 0) entry = "SUMMIT_MCP_PORT=" + std::to_string(port + int(found - profiles.entries.begin()));
        }
        environment.push_back(std::move(entry));
    }
    std::vector<char*> argv, envp;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    for (auto& entry : environment) envp.push_back(entry.data());
    envp.push_back(nullptr);
    // Only stdin, stdout and stderr: not this Summit's sockets and files
    // (its debugging server's port, its engine connections), which the
    // other would keep open after this one ends. stderr is where it went
    // before this Summit's log capture, whose pipe ends with it.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int fd = 3, limit = std::min(getdtablesize(), 4096); fd < limit; ++fd)
        if (const int flags = fcntl(fd, F_GETFD); flags >= 0 && !(flags & FD_CLOEXEC)) posix_spawn_file_actions_addclose(&actions, fd);
    if (const int original = mcplog::OriginalStderr(); original >= 0) posix_spawn_file_actions_adddup2(&actions, original, STDERR_FILENO);
    pid_t process = -1;
    const int status = posix_spawn(&process, executable.c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (status != 0) {
        error = std::strerror(status);
        return B_ERROR;
    }
    // A child that ends is reaped, not left a zombie of this team.
    std::thread([process] {
        int ignoredStatus;
        while (waitpid(process, &ignoredStatus, 0) < 0 && errno == EINTR) { }
    }).detach();
    std::fprintf(stderr, "Summit: started profile %s (team %d)\n", id.c_str(), static_cast<int>(process));
    return B_OK;
}

void BroadcastProfilesChanged()
{
    const auto profiles = BrowserProfiles::Load(ProfileBase());
    for (const auto& entry : profiles.entries) {
        if (entry.id == CurrentProfileID()) continue;
        if (const team_id team = RunningProfileTeam(entry.id); team >= 0) {
            BMessage changed(kProfilesChanged);
            SendToTeam(team, changed);
        }
    }
    if (be_app) be_app->PostMessage(kProfilesChanged);
}

void QuitOtherProfiles()
{
    for (const auto& entry : KnownProfiles().entries) {
        if (entry.id == CurrentProfileID()) continue;
        if (const team_id team = RunningProfileTeam(entry.id); team >= 0) {
            BMessage quit(kQuitWithOtherProfiles);
            SendToTeam(team, quit);
        }
    }
}

void SetReopenProfile(bool reopen)
{
    const auto id = CurrentProfileID();
    const auto base = ProfileBase();
    if (id == BrowserProfiles::kDefaultID || base.empty()) return;
    std::string error;
    if (!BrowserProfiles::Update(base, [&](BrowserProfiles& profiles) {
            auto* entry = profiles.Find(id);
            if (!entry || entry->reopen == reopen) return false;
            entry->reopen = reopen;
            return true;
        }, error))
        std::fprintf(stderr, "Summit: could not save the profiles: %s\n", error.c_str());
}

// The toolbar's badge.
ProfileBadge::ProfileBadge() : BView("profile-badge", B_WILL_DRAW)
{
    SetViewColor(B_TRANSPARENT_COLOR);
}

void ProfileBadge::SetLabel(const ProfileLabel& label)
{
    const bool resize = label.name != fLabel.name;
    fLabel = label;
    SetToolTip(("Profile: " + label.name + ". This window's history, bookmarks, cookies, website data and "
        "extensions are the profile's own.").c_str());
    if (resize) InvalidateLayout();
    Invalidate();
}

BSize ProfileBadge::MinSize()
{
    BFont font(be_bold_font);
    return BSize(std::min(160.0f, std::ceil(font.StringWidth(fLabel.name.c_str()))) + 30, 28);
}
BSize ProfileBadge::MaxSize() { return MinSize(); }
BSize ProfileBadge::PreferredSize() { return MinSize(); }

void ProfileBadge::Draw(BRect)
{
    const auto colors = ChromeColorsFor(this);
    SetHighColor(colors.panel);
    FillRect(Bounds());
    const BRect pill = Bounds().InsetByCopy(1, 3);
    SetHighColor(fLabel.color);
    FillRoundRect(pill, pill.Height() / 2, pill.Height() / 2);
    // A person's head and shoulders, as Safari's profile mark.
    SetHighColor(255, 255, 255);
    const float cy = std::floor((pill.top + pill.bottom) / 2) + 0.5f;
    const float x = pill.left + 10;
    FillEllipse(BPoint(x + 3, cy - 2.5f), 2.6f, 2.6f);
    FillArc(BRect(x - 2, cy + 1, x + 8, cy + 9), 0, 180);
    BFont font(be_bold_font);
    SetFont(&font);
    font_height metrics;
    font.GetHeight(&metrics);
    BString name(fLabel.name.c_str());
    font.TruncateString(&name, B_TRUNCATE_END, 160);
    SetDrawingMode(B_OP_OVER);
    DrawString(name.String(), BPoint(x + 12, std::floor(cy + (metrics.ascent - metrics.descent) / 2)));
    SetDrawingMode(B_OP_COPY);
}

// Preferences › Profiles.
namespace {
constexpr uint32 kSelected = 'pfsl', kAdd = 'pfad', kEditSelected = 'pfed', kOpen = 'pfop', kDelete = 'pfdl',
    kDeleteAnswered = 'pfda', kEditorSaved = 'pfsv', kRefresh = 'pfrf', kColorChosen = 'pfcc';

class ProfileItem final : public BListItem {
public:
    ProfileItem(std::string name, rgb_color color, std::string note)
        : fName(std::move(name)), fColor(color), fNote(std::move(note)) {}
    void DrawItem(BView* owner, BRect frame, bool) override
    {
        owner->SetHighUIColor(IsSelected() ? B_LIST_SELECTED_BACKGROUND_COLOR : B_LIST_BACKGROUND_COLOR);
        owner->FillRect(frame);
        owner->SetHighColor(fColor);
        const float cy = (frame.top + frame.bottom) / 2;
        owner->FillEllipse(BPoint(frame.left + 12, cy), 5, 5);
        font_height metrics;
        owner->GetFontHeight(&metrics);
        const float baseline = std::floor(cy + (metrics.ascent - metrics.descent) / 2);
        owner->SetHighUIColor(IsSelected() ? B_LIST_SELECTED_ITEM_TEXT_COLOR : B_LIST_ITEM_TEXT_COLOR);
        owner->SetDrawingMode(B_OP_OVER);
        owner->DrawString(fName.c_str(), BPoint(frame.left + 24, baseline));
        if (!fNote.empty()) {
            owner->SetHighUIColor(IsSelected() ? B_LIST_SELECTED_ITEM_TEXT_COLOR : B_LIST_ITEM_TEXT_COLOR, B_DARKEN_2_TINT);
            owner->DrawString(("   " + fNote).c_str());
        }
        owner->SetDrawingMode(B_OP_COPY);
    }
    void Update(BView* owner, const BFont* font) override
    {
        BListItem::Update(owner, font);
        SetHeight(std::max(Height(), 22.0f));
    }
private:
    std::string fName;
    rgb_color fColor;
    std::string fNote;
};

// New Profile… and Edit…: a name and a colour.
class ProfileEditor final : public BWindow {
public:
    ProfileEditor(BWindow* owner, BMessenger target, const std::string& id, const std::string& name, const std::string& color)
        : BWindow(BRect(0, 0, 360, 120), id.empty() ? "New Profile" : "Edit Profile", B_TITLED_WINDOW_LOOK,
              B_MODAL_SUBSET_WINDOW_FEEL, B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS
              | B_CLOSE_ON_ESCAPE),
          fTarget(target), fID(id), fColor(color)
    {
        fName = new BTextControl("name", "Name:", name.c_str(), nullptr);
        fName->TextView()->SetExplicitMinSize(BSize(220, B_SIZE_UNSET));
        auto* colors = new BPopUpMenu("colors");
        for (const auto& entry : ProfileColors()) {
            auto* message = new BMessage(kColorChosen);
            message->AddString("color", entry.name);
            auto* item = NewColorMenuItem(entry.label, message, {entry.red, entry.green, entry.blue, 255});
            item->SetMarked(color == entry.name);
            colors->AddItem(item);
        }
        auto* colorField = new BMenuField("color", "Colour:", colors);
        auto* save = new BButton("save", id.empty() ? "Create" : "Save", new BMessage('save'));
        BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
            .SetInsets(B_USE_WINDOW_SPACING)
            .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
                .AddTextControl(fName, 0, 0)
                .AddMenuField(colorField, 0, 1)
            .End()
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(new BButton("cancel", "Cancel", new BMessage(B_QUIT_REQUESTED)))
                .Add(save)
            .End();
        save->MakeDefault(true);
        fName->MakeFocus(true);
        if (owner) {
            AddToSubset(owner);
            CenterIn(owner->Frame());
        }
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == kColorChosen) {
            fColor = message->GetString("color", "blue");
            return;
        }
        if (message->what != 'save') return BWindow::MessageReceived(message);
        const std::string name = BrowserProfiles::CleanName(fName->Text());
        if (name.empty()) {
            fName->MakeFocus(true);
            return;
        }
        BMessage saved(kEditorSaved);
        saved.AddString("id", fID.c_str());
        saved.AddString("name", name.c_str());
        saved.AddString("color", fColor.c_str());
        fTarget.SendMessage(&saved);
        Quit();
    }
private:
    BMessenger fTarget;
    std::string fID;
    std::string fColor;
    BTextControl* fName;
};

BStringView* Hint(const char* name, const char* text)
{
    auto* hint = new BStringView(name, text);
    hint->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    return hint;
}

// Moves a deleted profile's folder to the Trash of its volume.
status_t MoveToTrash(const std::filesystem::path& directory, const std::string& name)
{
    BEntry folder(directory.c_str());
    entry_ref ref;
    status_t status = folder.GetRef(&ref);
    if (status != B_OK) return status;
    BVolume volume(ref.device);
    BPath trash;
    if ((status = find_directory(B_TRASH_DIRECTORY, &trash, true, &volume)) != B_OK) return status;
    BDirectory bin(trash.Path());
    std::string target = "Summit profile " + name;
    for (int number = 2; bin.Contains(target.c_str()) && number < 100; ++number)
        target = "Summit profile " + name + " " + std::to_string(number);
    for (auto& c : target) if (c == '/') c = '-';
    return folder.MoveTo(&bin, target.c_str());
}
}

ProfilesPreferences::ProfilesPreferences() : BGroupView("Profiles", B_VERTICAL, 8)
{
    auto* title = new BStringView("profiles-title", "Profiles");
    title->SetFont(be_bold_font);
    fList = new BListView("profiles", B_SINGLE_SELECTION_LIST);
    fList->SetSelectionMessage(new BMessage(kSelected));
    fList->SetInvocationMessage(new BMessage(kOpen));
    auto* scroll = new BScrollView("profiles-scroll", fList, 0, false, true);
    scroll->SetExplicitMinSize(BSize(320, 150));
    auto* add = new BButton("profile-add", "New Profile…", new BMessage(kAdd));
    fEdit = new BButton("profile-edit", "Edit…", new BMessage(kEditSelected));
    fOpen = new BButton("profile-open", "Open Window", new BMessage(kOpen));
    fDelete = new BButton("profile-delete", "Delete…", new BMessage(kDelete));
    fStatus = Hint("profiles-status", "");
    BLayoutBuilder::Group<>(this)
        .SetInsets(B_USE_WINDOW_SPACING)
        .Add(title)
        .Add(Hint("profiles-hint-1", "Each profile has its own history, bookmarks, cookies, website data,"))
        .Add(Hint("profiles-hint-2", "extensions and settings. Its windows show its name and colour."))
        .Add(Hint("profiles-hint-3", "File › New Window in Profile opens one."))
        .Add(scroll, 1)
        .AddGroup(B_HORIZONTAL, 8)
            .Add(add)
            .Add(fEdit)
            .Add(fOpen)
            .AddGlue()
            .Add(fDelete)
        .End()
        .Add(fStatus);
}

ProfilesPreferences::~ProfilesPreferences() = default;

void ProfilesPreferences::AttachedToWindow()
{
    BGroupView::AttachedToWindow();
    fList->SetTarget(this);
    for (const char* name : {"profile-add", "profile-edit", "profile-open", "profile-delete"})
        if (auto* button = dynamic_cast<BButton*>(FindView(name))) button->SetTarget(this);
    // Which profiles are open changes as their Summits start and quit.
    BMessage refresh(kRefresh);
    fRefresh = std::make_unique<BMessageRunner>(BMessenger(this), &refresh, 2000000);
    Fill();
}

void ProfilesPreferences::DetachedFromWindow()
{
    fRefresh.reset();
    BGroupView::DetachedFromWindow();
}

void ProfilesPreferences::Fill()
{
    const auto selected = SelectedID();
    const auto profiles = BrowserProfiles::Load(ProfileBase());
    const auto current = CurrentProfileID();
    std::vector<std::string> notes;
    for (const auto& entry : profiles.entries)
        notes.push_back(entry.id == current ? "this window's profile" : RunningProfileTeam(entry.id) >= 0 ? "open" : "");
    // Unchanged lists are not drawn again (every two seconds).
    std::vector<std::string> ids;
    std::string signature;
    for (size_t i = 0; i < profiles.entries.size(); ++i) {
        ids.push_back(profiles.entries[i].id);
        signature += profiles.entries[i].id + '\n' + profiles.entries[i].name + '\n' + profiles.entries[i].color + '\n' + notes[i] + '\n';
    }
    if (signature == fSignature) return UpdateButtons();
    fSignature = signature;
    while (fList->CountItems() > 0) delete fList->RemoveItem(int32(0));
    fIDs = ids;
    for (size_t i = 0; i < profiles.entries.size(); ++i)
        fList->AddItem(new ProfileItem(profiles.entries[i].name, ProfileRGB(profiles.entries[i].color), notes[i]));
    for (size_t i = 0; i < fIDs.size(); ++i) if (fIDs[i] == selected) fList->Select(int32(i));
    UpdateButtons();
}

std::string ProfilesPreferences::SelectedID() const
{
    const int32 index = fList->CurrentSelection();
    return index >= 0 && size_t(index) < fIDs.size() ? fIDs[size_t(index)] : std::string();
}

void ProfilesPreferences::UpdateButtons()
{
    const auto id = SelectedID();
    fEdit->SetEnabled(!id.empty());
    fOpen->SetEnabled(!id.empty());
    fDelete->SetEnabled(!id.empty() && id != BrowserProfiles::kDefaultID && id != CurrentProfileID());
}

void ProfilesPreferences::Edit(const std::string& id)
{
    const auto profiles = BrowserProfiles::Load(ProfileBase());
    const auto* entry = id.empty() ? nullptr : profiles.Find(id);
    if (!id.empty() && !entry) return;
    (new ProfileEditor(Window(), BMessenger(this), id, entry ? entry->name : "",
        entry ? entry->color : profiles.UnusedColor()))->Show();
}

void ProfilesPreferences::Delete(const std::string& id)
{
    const auto profiles = BrowserProfiles::Load(ProfileBase());
    const auto* entry = profiles.Find(id);
    if (!entry || id == BrowserProfiles::kDefaultID || id == CurrentProfileID()) return;
    if (RunningProfileTeam(id) >= 0) {
        fStatus->SetText(("Close the windows of “" + entry->name + "” first.").c_str());
        return;
    }
    const std::string question = "Delete the profile “" + entry->name + "”?\n\nIts history, bookmarks, cookies, website "
        "data, extensions and settings go to the Trash.";
    auto* alert = new BAlert("Delete Profile", question.c_str(), "Cancel", "Delete", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
    alert->SetShortcut(0, B_ESCAPE);
    auto* answer = new BMessage(kDeleteAnswered);
    answer->AddString("id", id.c_str());
    alert->Go(new BInvoker(answer, BMessenger(this)));
}

void ProfilesPreferences::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kSelected:
            fStatus->SetText("");
            UpdateButtons();
            return;
        case kRefresh:
        case kProfilesChanged:
            Fill();
            return;
        case kAdd:
            Edit({ });
            return;
        case kEditSelected:
            if (const auto id = SelectedID(); !id.empty()) Edit(id);
            return;
        case kOpen: {
            const auto id = SelectedID();
            if (id.empty()) return;
            if (id == CurrentProfileID()) {
                BMessage open(kOpenProfileWindow);
                open.AddString("id", id.c_str());
                be_app->PostMessage(&open);
                return;
            }
            std::string error;
            if (StartProfile(id, true, error) != B_OK) fStatus->SetText(("Could not open the profile: " + error).c_str());
            return;
        }
        case kDelete:
            if (const auto id = SelectedID(); !id.empty()) Delete(id);
            return;
        case kDeleteAnswered: {
            if (message->GetInt32("which", 0) != 1) return;
            const std::string id = message->GetString("id", "");
            const auto profiles = BrowserProfiles::Load(ProfileBase());
            const auto* entry = profiles.Find(id);
            if (!entry || id == BrowserProfiles::kDefaultID || id == CurrentProfileID()) return;
            // It may have been opened while the question was asked.
            if (RunningProfileTeam(id) >= 0) {
                fStatus->SetText(("Close the windows of “" + entry->name + "” first.").c_str());
                return;
            }
            const auto directory = ProfileDirectory(id);
            std::error_code missing;
            if (std::filesystem::exists(directory, missing)) {
                if (const status_t status = MoveToTrash(directory, entry->name); status != B_OK) {
                    fStatus->SetText(("Could not move the profile to the Trash: " + std::string(std::strerror(status))).c_str());
                    return;
                }
            }
            std::string error;
            BrowserProfiles::Update(ProfileBase(), [&](BrowserProfiles& list) { return list.Remove(id); }, error);
            fStatus->SetText(error.empty() ? "" : ("Could not save the profiles: " + error).c_str());
            BroadcastProfilesChanged();
            return;
        }
        case kEditorSaved: {
            const std::string id = message->GetString("id", "");
            const std::string name = message->GetString("name", "");
            const std::string color = message->GetString("color", "blue");
            std::string error, made;
            const bool saved = BrowserProfiles::Update(ProfileBase(), [&](BrowserProfiles& list) {
                if (id.empty()) {
                    made = list.Add(name, color).id;
                    return true;
                }
                auto* entry = list.Find(id);
                if (!entry || (entry->name == name && entry->color == color)) return false;
                entry->name = name;
                entry->color = FindProfileColor(color).name;
                return true;
            }, error);
            if (!saved) {
                fStatus->SetText(("Could not save the profiles: " + error).c_str());
                return;
            }
            if (!made.empty()) {
                std::error_code ignored;
                std::filesystem::create_directories(ProfileDirectory(made), ignored);
                fStatus->SetText(("“" + name + "” was made. Open Window opens it.").c_str());
            }
            BroadcastProfilesChanged();
            Fill();
            if (!made.empty())
                for (size_t i = 0; i < fIDs.size(); ++i) if (fIDs[i] == made) fList->Select(int32(i));
            return;
        }
    }
    BGroupView::MessageReceived(message);
}
}

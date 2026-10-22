#include "SitePermissions.h"
#include "FaviconCache.h"
#include "Location.h"
#include "Messages.h"
#include <WebKit/WebKitContext.h>
#include <Alert.h>
#include <AppFileInfo.h>
#include <Application.h>
#include <Bitmap.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <ScrollView.h>
#include <StringView.h>
#include <Invoker.h>
#include <MessageRunner.h>
#include <Notification.h>
#include <Roster.h>
#include <Window.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace summit {
namespace {
constexpr uint32 kPromptAnswered = 'sppa';
constexpr uint32 kLocationFound = 'splf';
constexpr uint32 kLocationRefresh = 'splr';
// How long a position is reused, and how often watching pages get a new one.
constexpr bigtime_t kPositionLifetime = 60 * 1000000LL;
constexpr bigtime_t kRefreshInterval = 5 * 60 * 1000000LL;

// SUMMIT_PERMISSION_TRACE=1: requests, answers, notifications and clicks on stderr.
bool Tracing()
{
    static const bool enabled = [] {
        const char* value = std::getenv("SUMMIT_PERMISSION_TRACE");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

std::string HostOf(const std::string& origin)
{
    const size_t start = origin.find("://");
    return start == std::string::npos ? origin : origin.substr(start + 3);
}

const char* Question(const std::string& permission)
{
    return permission == "notifications" ? "wants to show you notifications."
        : permission == "geolocation" ? "wants to know your location."
        : permission == "camera" ? "wants to use your camera."
        : permission == "microphone" ? "wants to use your microphone."
        : permission == "camera-microphone" ? "wants to use your camera and microphone."
        : "wants a permission.";
}

// Permissions answered by choosing a device, for this browsing session only.
bool DeviceChooser(const std::string& permission)
{
    return permission == "usb" || permission == "hid" || permission == "bluetooth";
}

// getDisplayMedia(): which screen or window to share, asked every time.
// Answers the service with kPromptAnswered ("which" 2 and "device_id", or
// "which" 1).
class DevicePermissionPicker final : public BWindow {
public:
    DevicePermissionPicker(const BMessage& request, const std::string& question, BMessenger service)
        : BWindow(BRect(0, 0, 380, 260), std::strcmp(request.GetString("permission", ""), "bluetooth") == 0 ? "Connect Bluetooth Device"
            : std::strcmp(request.GetString("permission", ""), "usb") == 0 ? "Connect USB Device"
            : std::strcmp(request.GetString("permission", ""), "hid") == 0 ? "Connect HID Device" : "Share Your Screen", B_FLOATING_WINDOW_LOOK, B_FLOATING_APP_WINDOW_FEEL,
            B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
        , fService(service)
        , fPermission(request.GetString("permission", ""))
        , fOrigin(request.GetString("origin", ""))
        , fIdentifier(request.GetUInt64("identifier", 0))
    {
        auto* text = new BStringView("question", question.c_str());
        auto* note = new BStringView("note", DeviceChooser(fPermission)
            ? "Access lasts for this browsing session."
            : "The page sees everything in it while you share. A window is shared with whatever covers it.");
        BFont small(be_plain_font);
        small.SetSize(small.Size() * 0.9f);
        note->SetFont(&small);
        fList = new BListView("choices", B_SINGLE_SELECTION_LIST);
        const char* id = nullptr;
        const char* label = nullptr;
        for (int32 index = 0; request.FindString("device_id", index, &id) == B_OK; ++index) {
            if (request.FindString("device_label", index, &label) != B_OK) label = id;
            fList->AddItem(new BStringItem(label));
            fIds.emplace_back(id);
        }
        fList->Select(0);
        fList->SetInvocationMessage(new BMessage(kShare));
        auto* scroll = new BScrollView("choices-scroll", fList, 0, false, true);
        // Item heights are not initialized until the list is attached. Use
        // the UI font here so the picker has room for readable rows at Show().
        font_height metrics;
        be_plain_font->GetHeight(&metrics);
        const float row = metrics.ascent + metrics.descent + metrics.leading + 8;
        scroll->SetExplicitMinSize(BSize(340, row * std::min<int32>(std::max<int32>(fList->CountItems(), 3), 10) + 4));
        auto* share = new BButton("share", DeviceChooser(fPermission) ? "Connect" : "Share", new BMessage(kShare));
        auto* cancel = new BButton("cancel", "Cancel", new BMessage(B_QUIT_REQUESTED));
        BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
            .SetInsets(B_USE_WINDOW_INSETS)
            .Add(text)
            .Add(scroll)
            .Add(note)
            .AddGroup(B_HORIZONTAL)
                .AddGlue()
                .Add(cancel)
                .Add(share)
            .End();
        SetDefaultButton(share);
    }

    void MessageReceived(BMessage* message) override
    {
        if (message->what != kShare) {
            BWindow::MessageReceived(message);
            return;
        }
        const int32 selected = fList->CurrentSelection();
        if (selected < 0 || selected >= int32(fIds.size())) return;
        Answer(2, fIds[selected]);
        Quit();
    }

    bool QuitRequested() override
    {
        // Closed or cancelled: not shared.
        Answer(1, {});
        return true;
    }

private:
    static constexpr uint32 kShare = 'sssh';

    void Answer(int32 which, const std::string& id)
    {
        if (fAnswered) return;
        fAnswered = true;
        BMessage answer(kPromptAnswered);
        answer.AddString("permission", fPermission.c_str());
        answer.AddString("origin", fOrigin.c_str());
        answer.AddUInt64("identifier", fIdentifier);
        answer.AddInt32("which", which);
        answer.AddString("device_id", id.c_str());
        fService.SendMessage(&answer);
    }

    BMessenger fService;
    std::string fPermission, fOrigin;
    uint64 fIdentifier;
    BListView* fList = nullptr;
    std::vector<std::string> fIds;
    bool fAnswered = false;
};

// Centres a window on the asking page's window, near its top.
void PlaceOverView(BWindow* window, const BMessage& request)
{
    BMessenger view;
    if (request.FindMessenger("view", &view) != B_OK) return;
    BLooper* looper = nullptr;
    view.Target(&looper);
    auto* owner = dynamic_cast<BWindow*>(looper);
    if (!owner || owner->LockWithTimeout(100000) != B_OK) return;
    const BRect frame = owner->Frame();
    owner->Unlock();
    window->Lock();
    const BRect size = window->Frame();
    window->MoveTo(frame.left + (frame.Width() - size.Width()) / 2, frame.top + 90);
    window->Unlock();
}
}

SitePermissionService::SitePermissionService(std::shared_ptr<BWebKitContext> context, bool privateBrowsing,
    std::filesystem::path favicons, DecisionSaver save)
    : BHandler(privateBrowsing ? "private site permissions" : "site permissions")
    , fContext(context)
    , fPrivate(privateBrowsing)
    , fFavicons(std::make_unique<FaviconCache>(std::move(favicons), false))
    , fSave(std::move(save))
{
}

SitePermissionService::~SitePermissionService() = default;

void SitePermissionService::MessageReceived(BMessage* message)
{
    switch (message->what) {
    case B_WEBKIT_PERMISSION_REQUESTED: Ask(*message); break;
    case B_WEBKIT_PERMISSION_CANCELLED: Cancelled(*message); break;
    case kPromptAnswered: Answered(*message); break;
    case B_WEBKIT_NOTIFICATION_SHOW: ShowNotification(*message); break;
    case B_WEBKIT_NOTIFICATION_CLOSE:
        // Haiku can not take a notification back; it times out by itself.
        fNotifications.erase(message->GetUInt64("identifier", 0));
        break;
    case B_WEBKIT_GEOLOCATION_START: StartLocation(message->GetBool("high_accuracy", false)); break;
    case B_WEBKIT_GEOLOCATION_STOP: StopLocation(); break;
    case kLocationFound: LocationResult(*message); break;
    case kLocationRefresh:
        if (fWatching && !fLookingUp) {
            fLookingUp = true;
            LookUpLocation(BMessenger(this), kLocationFound);
        }
        break;
    default: BHandler::MessageReceived(message); break;
    }
}

// The question goes to the front of the asking page's window, and does not
// stop the user from doing something else first: it floats, it is not modal.
void SitePermissionService::Ask(const BMessage& request)
{
    const uint64 identifier = request.GetUInt64("identifier", 0);
    const std::string permission = request.GetString("permission", "");
    const std::string origin = request.GetString("origin", "");
    auto context = fContext.lock();
    if (!context || !identifier) return;
    if (permission.empty() || origin.empty()) {
        context->RespondToPermissionRequest(identifier, false);
        return;
    }
    if (Tracing())
        std::fprintf(stderr, "Summit permissions: %s asks for %s (request %llu)\n", origin.c_str(), permission.c_str(),
            static_cast<unsigned long long>(identifier));
    // Device requests may contain different filter/device sets. Never merge their
    // answers into an existing prompt for the same origin.
    if (DeviceChooser(permission) && fPrompts.count({permission, origin})) {
        context->RespondToMediaPermissionRequest(identifier, false, "");
        return;
    }
    auto& waiting = fPrompts[{permission, origin}];
    waiting.push_back(identifier);
    if (waiting.size() > 1) return;
    if (permission == "screen" || DeviceChooser(permission)) {
        std::string question = HostOf(origin) + (permission == "bluetooth" ? " wants to connect to a Bluetooth device."
            : permission == "usb" ? " wants to connect to a USB device."
            : permission == "hid" ? " wants to connect to a HID device."
            : " wants to see your screen.");
        if (request.GetBool("audio", false)) question += " (Its sound is not shared.)";
        auto* picker = new DevicePermissionPicker(request, question, BMessenger(this));
        picker->Lock();
        picker->Layout(true);
        picker->ResizeToPreferred();
        picker->Unlock();
        PlaceOverView(picker, request);
        auto& shown = fPromptWindows[{permission, origin}];
        shown.window = BMessenger(picker);
        const char* device = nullptr;
        for (int32 index = 0; request.FindString("device_id", index, &device) == B_OK; ++index)
            shown.devices.emplace_back(device);
        picker->Show();
        return;
    }
    std::string text = HostOf(origin) + " " + Question(permission);
    if (permission == "geolocation")
        text += "\n\nSummit asks BeaconDB (beacondb.net) where you are, from the Wi-Fi networks nearby and your "
            "network address.";
    if (fPrivate) text += "\n\nIn a private window, the answer is forgotten when the window closes.";
    auto* alert = new BAlert("Summit", text.c_str(), "Block", "Not Now", "Allow", B_WIDTH_FROM_LABEL, B_IDEA_ALERT);
    alert->SetShortcut(1, B_ESCAPE);
    alert->SetFeel(B_FLOATING_APP_WINDOW_FEEL);
    BMessenger view;
    if (request.FindMessenger("view", &view) == B_OK) {
        BLooper* looper = nullptr;
        view.Target(&looper);
        if (auto* window = dynamic_cast<BWindow*>(looper); window && window->LockWithTimeout(100000) == B_OK) {
            const BRect frame = window->Frame();
            window->Unlock();
            alert->Lock();
            const BRect size = alert->Frame();
            alert->MoveTo(frame.left + (frame.Width() - size.Width()) / 2, frame.top + 90);
            alert->Unlock();
        }
    }
    auto* answer = new BMessage(kPromptAnswered);
    answer->AddString("permission", permission.c_str());
    answer->AddString("origin", origin.c_str());
    answer->AddUInt64("identifier", identifier);
    fPromptWindows[{permission, origin}].window = BMessenger(alert);
    alert->Go(new BInvoker(answer, this));
}

bool SitePermissionService::AnswerOpenPrompt(int32 which, int32 deviceIndex)
{
    if (fPrompts.empty()) return false;
    const auto& [permission, origin] = fPrompts.begin()->first;
    BMessage answer(kPromptAnswered);
    answer.AddString("permission", permission.c_str());
    answer.AddString("origin", origin.c_str());
    answer.AddUInt64("identifier", fPrompts.begin()->second.front());
    answer.AddInt32("which", which);
    if ((permission == "screen" || DeviceChooser(permission)) && which == 2) {
        const auto shown = fPromptWindows.find(fPrompts.begin()->first);
        if (shown == fPromptWindows.end() || shown->second.devices.empty()) return false;
        const auto& devices = shown->second.devices;
        answer.AddString("device_id", devices[std::clamp<int32>(deviceIndex, 0, int32(devices.size()) - 1)].c_str());
    }
    Answered(answer);
    return true;
}

void SitePermissionService::Answered(const BMessage& message)
{
    const std::string permission = message.GetString("permission", ""), origin = message.GetString("origin", "");
    const int32 which = message.GetInt32("which", 1);
    auto found = fPrompts.find({permission, origin});
    if (found == fPrompts.end()) return;
    const uint64 identifier = message.GetUInt64("identifier", 0);
    if (std::find(found->second.begin(), found->second.end(), identifier) == found->second.end()) return;
    const auto identifiers = std::move(found->second);
    fPrompts.erase(found);
    // Answered from elsewhere (AnswerOpenPrompt) while its window is open;
    // a window that answered is closing already.
    if (auto shown = fPromptWindows.find({permission, origin}); shown != fPromptWindows.end()) {
        shown->second.window.SendMessage(B_QUIT_REQUESTED);
        fPromptWindows.erase(shown);
    }
    const bool allowed = which == 2;
    if (Tracing())
        std::fprintf(stderr, "Summit permissions: %s %s for %s\n", which == 2 ? "allowed" : which == 0 ? "blocked" : "not now",
            permission.c_str(), origin.c_str());
    // "Not Now" answers this request only; Block and Allow are remembered,
    // a camera-and-microphone answer as both. A shared screen is asked for
    // every time.
    std::vector<std::string> saved;
    if (permission == "camera-microphone") saved = {"camera", "microphone"};
    else if (permission != "screen" && !DeviceChooser(permission)) saved = {permission};
    if (which != 1) {
        for (const auto& name : saved) fSave(name, origin, allowed);
    }
    const std::string device = message.GetString("device_id", "");
    if (auto context = fContext.lock()) {
        if (which != 1 && fPrivate) {
            for (const auto& name : saved) context->SetSitePermission(name.c_str(), origin.c_str(), allowed ? 1 : 0);
        }
        for (const uint64 identifier : identifiers) {
            if (permission == "screen" || DeviceChooser(permission))
                context->RespondToMediaPermissionRequest(identifier, allowed && !device.empty(), device.c_str());
            else
                context->RespondToPermissionRequest(identifier, allowed);
        }
    }
}

void SitePermissionService::Cancelled(const BMessage& message)
{
    const uint64 identifier = message.GetUInt64("identifier", 0);
    for (auto found = fPrompts.begin(); found != fPrompts.end(); ++found) {
        auto& identifiers = found->second;
        auto request = std::find(identifiers.begin(), identifiers.end(), identifier);
        if (request == identifiers.end()) continue;
        identifiers.erase(request);
        if (!identifiers.empty()) return;
        if (auto shown = fPromptWindows.find(found->first); shown != fPromptWindows.end()) {
            shown->second.window.SendMessage(B_QUIT_REQUESTED);
            fPromptWindows.erase(shown);
        }
        fPrompts.erase(found);
        return;
    }
}

void SitePermissionService::ShowNotification(const BMessage& message)
{
    const uint64 identifier = message.GetUInt64("identifier", 0);
    const std::string origin = message.GetString("origin", ""), tag = message.GetString("tag", "");
    std::string title = message.GetString("title", "");
    std::string body = message.GetString("body", "");
    if (!identifier) return;
    if (title.size() > 200) title.resize(200);
    if (body.size() > 1000) body.resize(1000);
    BNotification notification(B_INFORMATION_NOTIFICATION);
    notification.SetGroup("Summit");
    notification.SetTitle(title.empty() ? HostOf(origin).c_str() : title.c_str());
    notification.SetContent((body.empty() ? HostOf(origin) : body + "\n" + HostOf(origin)).c_str());
    // A notification with the tag of an earlier one replaces it.
    notification.SetMessageID((tag.empty() ? "summit:" + std::to_string(identifier) : "summit:" + origin + "#" + tag).c_str());
    if (const BBitmap* icon = fFavicons->Icon(origin + "/")) notification.SetIcon(icon);
    // A click starts this executable with the identifier; as Summit is
    // running, the system hands the arguments to it (ArgvReceived).
    app_info info;
    if (be_app->GetAppInfo(&info) == B_OK) {
        notification.SetOnClickFile(&info.ref);
        notification.AddOnClickArg(kNotificationClickArgument);
        notification.AddOnClickArg(fPrivate ? "private" : "normal");
        notification.AddOnClickArg(std::to_string(identifier).c_str());
    }
    BMessenger view;
    message.FindMessenger("view", &view);
    fNotifications[identifier] = {view};
    if (Tracing())
        std::fprintf(stderr, "Summit permissions: notification %llu shown for %s (%s)\n",
            static_cast<unsigned long long>(identifier), origin.c_str(), fPrivate ? "private" : "normal");
    if (fNotifications.size() > 200) fNotifications.erase(fNotifications.begin());
    notification.Send();
}

void SitePermissionService::NotificationClicked(uint64 identifier)
{
    auto found = fNotifications.find(identifier);
    if (Tracing())
        std::fprintf(stderr, "Summit permissions: notification %llu clicked (%s)\n",
            static_cast<unsigned long long>(identifier), found == fNotifications.end() ? "unknown" : "known");
    if (found == fNotifications.end()) return;
    // The page's tab comes forward, then the page hears the click (its
    // handler usually focuses itself or navigates).
    if (found->second.view.IsValid()) {
        BMessage show(kShowTabOfView);
        show.AddMessenger("view", found->second.view);
        BLooper* looper = nullptr;
        found->second.view.Target(&looper);
        if (looper) BMessenger(looper).SendMessage(&show);
    }
    if (auto context = fContext.lock()) context->NotificationClicked(identifier);
}

void SitePermissionService::StartLocation(bool)
{
    fWatching = true;
    auto context = fContext.lock();
    if (fLast && system_time() - fLast->at < kPositionLifetime && context)
        context->SetGeolocationPosition(fLast->latitude, fLast->longitude, fLast->accuracy, fLast->timestamp);
    else if (!fLookingUp) {
        fLookingUp = true;
        LookUpLocation(BMessenger(this), kLocationFound);
    }
    if (!fRefresh) {
        BMessage refresh(kLocationRefresh);
        fRefresh = std::make_unique<BMessageRunner>(BMessenger(this), &refresh, kRefreshInterval);
    }
}

void SitePermissionService::StopLocation()
{
    fWatching = false;
    fRefresh.reset();
}

void SitePermissionService::LocationResult(const BMessage& message)
{
    fLookingUp = false;
    auto context = fContext.lock();
    if (!context) return;
    const char* error = nullptr;
    if (message.FindString("error", &error) == B_OK) {
        std::fprintf(stderr, "Summit location: %s\n", error);
        if (fWatching) context->GeolocationUnavailable(error);
        return;
    }
    Position position;
    position.latitude = message.GetDouble("latitude", 0);
    position.longitude = message.GetDouble("longitude", 0);
    position.accuracy = message.GetDouble("accuracy", 25000);
    position.timestamp = message.GetDouble("timestamp", 0);
    position.at = system_time();
    fLast = position;
    if (fWatching) context->SetGeolocationPosition(position.latitude, position.longitude, position.accuracy, position.timestamp);
}
}

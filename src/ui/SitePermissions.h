#pragma once
#include <Handler.h>
#include <Messenger.h>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

class BMessageRunner;
class BWebKitContext;

namespace summit {
class FaviconCache;

// What a page asked of one WebKit context (BWebKitContext::SetPermissionListener):
// permission prompts, web notifications shown as Haiku notifications, and
// positions for pages that asked for the location. One per context, run by
// the application looper.
class SitePermissionService final : public BHandler {
public:
    // Saves (or, with nullopt, forgets) a decision: the application stores it
    // in the profile and gives it to every context.
    using DecisionSaver = std::function<void(const std::string& permission, const std::string& origin, std::optional<bool> allowed)>;
    SitePermissionService(std::shared_ptr<BWebKitContext>, bool privateBrowsing, std::filesystem::path favicons, DecisionSaver);
    ~SitePermissionService() override;
    void MessageReceived(BMessage*) override;
    // A click on one of this context's notifications (from ArgvReceived).
    void NotificationClicked(uint64 identifier);
    // Answers the oldest open prompt as its window would, and closes the
    // window (kAnswerPermissionPrompt). False when none is open. shareAudio:
    // a shared screen takes the computer's sound with it when offered.
    bool AnswerOpenPrompt(int32 which, int32 deviceIndex, bool shareAudio = true);
    bool IsPrivate() const { return fPrivate; }

private:
    void Ask(const BMessage&);
    void Answered(const BMessage&);
    void Cancelled(const BMessage&);
    void ShowNotification(const BMessage&);
    void StartLocation(bool highAccuracy);
    void StopLocation();
    void LocationResult(const BMessage&);
    std::weak_ptr<BWebKitContext> fContext;
    const bool fPrivate;
    std::unique_ptr<FaviconCache> fFavicons;
    DecisionSaver fSave;
    // Open prompts by permission and origin; requests that arrive while one
    // is open wait for its answer.
    std::map<std::pair<std::string, std::string>, std::vector<uint64>> fPrompts;
    // Their windows, and for a screen the devices it offers and whether it
    // offers the computer's sound too.
    struct PromptWindow {
        BMessenger window;
        std::vector<std::string> devices;
        bool audio = false;
    };
    std::map<std::pair<std::string, std::string>, PromptWindow> fPromptWindows;
    struct Shown {
        BMessenger view;
    };
    std::map<uint64, Shown> fNotifications;
    // Location: pages are watching while fWatching; a lookup is running while
    // fLookingUp; the last answer is reused for a minute.
    bool fWatching = false;
    bool fLookingUp = false;
    std::unique_ptr<BMessageRunner> fRefresh;
    struct Position {
        double latitude = 0, longitude = 0, accuracy = 0, timestamp = 0;
        bigtime_t at = 0;
    };
    std::optional<Position> fLast;
};

// The click argument Summit's notifications carry: the application calls
// NotificationClicked on the service the notification came from.
constexpr const char* kNotificationClickArgument = "--summit-notification-click";
}

#include "ProtocolHandlers.h"
#include "DefaultBrowser.h"
#include "SharedProfile.h"
#include <Alert.h>
#include <AppFileInfo.h>
#include <Entry.h>
#include <File.h>
#include <FilePanel.h>
#include <Invoker.h>
#include <Message.h>
#include <Mime.h>
#include <Path.h>
#include <Roster.h>
#include <Window.h>
#include <cstring>
#include <strings.h>

namespace summit {
namespace {
constexpr uint32 answered = 'phas', selected = 'phse';
// Only an actual application can receive a link. Never pass a page's text
// through a shell or let a handler loop back to this browser.
bool Application(const std::string& path, entry_ref& ref)
{
    BEntry entry(path.c_str(), true);
    BFile file(&entry, B_READ_ONLY);
    BAppFileInfo info(&file);
    char signature[B_MIME_TYPE_LENGTH] = "";
    uint32 flags = 0;
    return entry.IsFile() && entry.GetRef(&ref) == B_OK && info.GetSignature(signature) == B_OK
        && *signature && strcasecmp(signature, kSummitSignature) && info.GetAppFlags(&flags) == B_OK;
}
ProtocolHandler Recommended(const std::string& scheme)
{
    BMimeType type(("application/x-vnd.Be.URL." + scheme).c_str());
    char signature[B_MIME_TYPE_LENGTH] = "";
    if (type.GetPreferredApp(signature, B_OPEN) != B_OK || !*signature) return {};
    BMimeType app(signature);
    entry_ref ref;
    if (app.GetAppHint(&ref) != B_OK) return {};
    BPath path(&ref);
    if (path.InitCheck() != B_OK || !Application(path.Path(), ref)) return {};
    return {"app", path.Path(), ""};
}
std::string AppName(const std::string& path)
{
    const auto slash = path.rfind('/');
    return path.substr(slash == std::string::npos ? 0 : slash + 1);
}
}
ProtocolHandlers::ProtocolHandlers(std::shared_ptr<SharedProfile> profile, bool privateBrowsing, SourceCheck check,
    std::function<void(const std::string&)> navigate, std::function<void(const std::string&)> error)
    : BHandler("link-handlers"), fProfile(std::move(profile)), fPrivate(privateBrowsing), fCheck(std::move(check)),
      fNavigate(std::move(navigate)), fError(std::move(error)) { }
ProtocolHandlers::~ProtocolHandlers() { Clear(); }
void ProtocolHandlers::Clear()
{
    fRequest.reset();
    if (fAlert.IsValid()) fAlert.SendMessage(B_QUIT_REQUESTED);
    fAlert = {};
    fPanel.reset();
}
std::optional<ProtocolHandler> ProtocolHandlers::Saved(const std::string& scheme) const
{
    if (fPrivate) {
        if (auto it = fPrivateHandlers.find(scheme); it != fPrivateHandlers.end()) return it->second;
        if (fPrivateRemoved.count(scheme)) return {};
    }
    return fProfile->Read([&](const Profile& profile) -> std::optional<ProtocolHandler> {
        if (auto it = profile.protocolHandlers.find(scheme); it != profile.protocolHandlers.end()) return it->second;
        return {};
    });
}
void ProtocolHandlers::Save(const Request& request)
{
    if (fPrivate) {
        fPrivateHandlers[request.scheme] = request.handler;
        fPrivateRemoved.erase(request.scheme);
    } else fProfile->Change([&](Profile& profile) -> uint32 {
        return profile.SetProtocolHandler(request.scheme, request.handler) ? SharedProfile::kSettingsChanged : 0;
    });
}
void ProtocolHandlers::Open(const std::string& url, int64 tab, const std::string& source, bool askAlways)
{
    const auto scheme = URLScheme(url);
    if (!IsExternalScheme(scheme) || !fCheck(tab, source) || fRequest) return;
    Request request { ++fNext, tab, scheme, url, source, {}, false };
    if (auto saved = Saved(scheme)) {
        request.handler = *saved;
        entry_ref ref;
        // A link no click asked for is confirmed even for a remembered choice.
        if (askAlways && (saved->kind == "web" || Application(saved->target, ref))) {
            fRequest = std::move(request);
            Confirm();
            return;
        }
        if (saved->kind == "web") { Launch(request); return; }
        if (Application(saved->target, ref)) { Launch(request); return; }
        // A moved or removed application gets a fresh chooser.
    }
    request.handler = Recommended(scheme);
    fRequest = std::move(request);
    Confirm();
}
void ProtocolHandlers::Register(const std::string& scheme, const std::string& target, bool remove,
    int64 tab, const std::string& source)
{
    ProtocolHandler handler {"web", target, ProtocolOrigin(target)};
    if (!ValidProtocolHandler(scheme, handler) || !fCheck(tab, source)) return;
    const auto saved = Saved(scheme);
    if (remove) {
        // A site can unregister only its own exact template.
        if (saved && *saved == handler) {
            if (fPrivate) { fPrivateHandlers.erase(scheme); fPrivateRemoved.insert(scheme); }
            else fProfile->Change([&](Profile& profile) -> uint32 {
                const auto it = profile.protocolHandlers.find(scheme);
                if (it == profile.protocolHandlers.end() || !(it->second == handler)) return 0;
                profile.protocolHandlers.erase(it);
                return SharedProfile::kSettingsChanged;
            });
        }
        if (fRequest && fRequest->registration && fRequest->scheme == scheme && fRequest->handler == handler) Clear();
        return;
    }
    if (fRequest || (saved && *saved == handler)) return;
    const bool declined = fPrivate ? fPrivateDeclined[scheme].count(target) > 0 : fProfile->Read([&](const Profile& profile) {
        const auto it = profile.declinedProtocolHandlers.find(scheme);
        return it != profile.declinedProtocolHandlers.end() && it->second.count(target) > 0;
    });
    if (declined) return;
    fRequest = Request { ++fNext, tab, scheme, "", source, std::move(handler), true };
    Confirm();
}
void ProtocolHandlers::Confirm()
{
    if (!fRequest) return;
    const auto& request = *fRequest;
    std::string text;
    if (request.registration) {
        text = request.handler.origin + " wants to open " + request.scheme + ": links.\n\n";
        if (Saved(request.scheme)) text += "This replaces your current choice.\n\n";
        text += fPrivate ? "Use this website for links in this private window?"
            : "Remember this website? You can remove it in Preferences › Link Handlers.";
    } else {
        auto source = ProtocolOrigin(request.source);
        text = source.empty() ? "Open this link in another application?\n\n" : source + " wants to open a link in another application.\n\n";
        text += request.url.substr(0, 500) + (request.url.size() > 500 ? "…" : "") + "\n\n";
        if (!request.handler.target.empty()) text += "Application: " + AppName(request.handler.target) + "\n\n";
        else text += "Choose an application for " + request.scheme + ": links.\n\n";
        text += fPrivate ? "Your choice lasts for this private window."
            : "Summit will remember your choice. Change it in Preferences › Link Handlers.";
    }
    const bool hasApp = !request.handler.target.empty();
    auto* alert = new BAlert("Open links — Summit", text.c_str(), request.registration ? "Don't Allow" : "Cancel",
        request.registration ? "Allow" : "Choose App…", !request.registration && hasApp ? "Open" : nullptr,
        B_WIDTH_AS_USUAL, B_INFO_ALERT);
    alert->SetShortcut(0, B_ESCAPE);
    fAlert = BMessenger(alert);
    auto* reply = new BMessage(answered);
    reply->AddUInt64("request", request.id);
    alert->Go(new BInvoker(reply, this));
}
void ProtocolHandlers::Choose()
{
    auto* reply = new BMessage(selected);
    reply->AddUInt64("request", fRequest->id);
    BMessenger target(this);
    fPanel = std::make_unique<BFilePanel>(B_OPEN_PANEL, &target, nullptr, B_FILE_NODE, false, reply);
    delete reply;
    fPanel->Window()->SetTitle("Choose an application for links — Summit");
    fPanel->Show();
}
void ProtocolHandlers::Launch(const Request& request)
{
    if (!fCheck(request.tab, request.source)) return;
    if (request.handler.kind == "web") {
        const auto url = ProtocolHandlerURL(request.handler, request.url);
        if (!url.empty()) fNavigate(url);
        return;
    }
    entry_ref ref;
    if (!Application(request.handler.target, ref)) { fError("That application is no longer available. Choose it again."); return; }
    const char* arguments[] = {request.url.c_str()};
    const auto status = be_roster->Launch(&ref, 1, arguments);
    if (status == B_OK || status == B_ALREADY_RUNNING) Save(request);
    else fError("Could not open " + AppName(request.handler.target) + ": " + std::string(strerror(status)));
}
void ProtocolHandlers::MessageReceived(BMessage* message)
{
    if (message->what != answered && message->what != selected && message->what != B_CANCEL) {
        BHandler::MessageReceived(message); return;
    }
    if (!fRequest) return;
    if (message->what == B_CANCEL) { Clear(); return; }
    if (message->GetUInt64("request", 0) != fRequest->id) return;
    if (!fCheck(fRequest->tab, fRequest->source)) { Clear(); return; }
    if (message->what == selected) {
        entry_ref ref;
        if (message->FindRef("refs", &ref) != B_OK) { Clear(); return; }
        BPath path(&ref);
        if (path.InitCheck() != B_OK || !Application(path.Path(), ref)) {
            fError("Choose a native application other than Summit.");
            fPanel.reset(); Choose(); return;
        }
        fRequest->handler = {"app", path.Path(), ""};
        fPanel.reset(); Confirm(); return;
    }
    fAlert = {};
    const int32 which = message->GetInt32("which", 0);
    if (fRequest->registration) {
        const auto request = *fRequest;
        Clear();
        if (which == 1) Save(request);
        else if (fPrivate) fPrivateDeclined[request.scheme].insert(request.handler.target);
        else fProfile->Change([&](Profile& profile) -> uint32 {
            if (!profile.declinedProtocolHandlers.count(request.scheme) && profile.declinedProtocolHandlers.size() >= 1000) return 0;
            auto& targets = profile.declinedProtocolHandlers[request.scheme];
            if (targets.size() >= 100) return 0;
            return targets.insert(request.handler.target).second ? SharedProfile::kSettingsChanged : 0;
        });
    } else if (which == 1) Choose();
    else if (which == 2) { const auto request = *fRequest; Clear(); Launch(request); }
    else Clear();
}
}

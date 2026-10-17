#include "ui/BrowserWindow.h"
#include "ui/PreferencesWindow.h"
#include "ui/DefaultBrowser.h"
#include <thread>
#include "ui/SharedProfile.h"
#include "ui/Messages.h"
#include "ui/ExtensionPermissionPrompt.h"
#include "ui/ExtensionController.h"
#include "ui/ExtensionTiming.h"
#include "ui/StallTrace.h"
#include "ui/ExtensionInstaller.h"
#include "ui/ExtensionManager.h"
#if SUMMIT_MODERN_WEBKIT
#include "ui/McpService.h"
#include "ui/SitePermissions.h"
#include <nlohmann/json.hpp>
#endif
#include "core/Address.h"
#include <Alert.h>
#include <Application.h>
#include <Entry.h>
#include <Invoker.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Roster.h>
#include <OS.h>
#include <image.h>
#include <unistd.h>
#if !SUMMIT_MODERN_WEBKIT
#include <WebPage.h>
#include <WebSettings.h>
#endif
#include <filesystem>
#include <functional>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <tuple>
#include <utility>
#include <string_view>
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/time.h>
#include <netdb.h>

class SummitApp : public BApplication {
public:
    explicit SummitApp(status_t& status) : BApplication("application/x-vnd.Kunanyi-Summit", &status) {}
    void DispatchMessage(BMessage* message, BHandler* handler) override
    {
        summit::StallScope scope("app", message ? message->what : 0);
#if SUMMIT_MODERN_WEBKIT
        // Before a shutdown or reboot the registrar asks each application to
        // quit, and a negative answer cancels the shutdown. Summit's quit
        // takes several turns that each answer "not yet" (QuitRequested), so
        // the registrar's request is kept, not answered: Summit quits, or
        // answers no when a page keeps its window open.
        if (message && message->what == B_QUIT_REQUESTED && message->GetBool("_shutdown_", false)) {
            QuitForShutdown(DetachCurrentMessage());
            return;
        }
#endif
        BApplication::DispatchMessage(message, handler);
    }
    void ArgvReceived(int32 argc, char** argv) override
    {
#if SUMMIT_MODERN_WEBKIT
        // A click on one of Summit's notifications (SitePermissionService).
        if (argc >= 2 && !std::strcmp(argv[1], summit::kNotificationClickArgument)) {
            if (const char* trace = std::getenv("SUMMIT_PERMISSION_TRACE"); trace && !std::strcmp(trace, "1"))
                std::fprintf(stderr, "Summit permissions: click arguments argc=%d %s %s\n", static_cast<int>(argc),
                    argc > 2 ? argv[2] : "", argc > 3 ? argv[3] : "");
        }
        if (argc == 4 && !std::strcmp(argv[1], summit::kNotificationClickArgument)) {
            const bool privateContext = !std::strcmp(argv[2], "private");
            auto& service = privateContext ? fPrivateSitePermissions : fSitePermissions;
            if (service) service->NotificationClicked(std::strtoull(argv[3], nullptr, 10));
            return;
        }
#endif
        // A start while Summit quits is made again once this instance is
        // gone (RunApplication): opening it in a closing window would lose it.
        if (fQuitting) {
            fRelaunch = true;
            for (int32 i = 1; i < argc; ++i) fRelaunchArguments.push_back(argv[i]);
            return;
        }
        for (int32 i = 1; i < argc; ++i) {
            std::string argument = argv[i];
            if (argument == "--profile" && i + 1 < argc) { fProfile = argv[++i]; continue; }
            if (!fWindows.empty()) OpenInNormalWindow(argument);
            else fURLs.push_back(argument);
        }
    }
    void RefsReceived(BMessage* message) override
    {
        if (fQuitting) {
            fRelaunch = true;
            entry_ref ref;
            for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; ++i) {
                BPath path(&ref);
                if (path.InitCheck() == B_OK) fRelaunchArguments.push_back(summit::FileURL(path.Path()));
            }
            return;
        }
        if (!fWindows.empty()) {
            if (FrontNormalWindow().IsValid()) FrontNormalWindow().SendMessage(message);
            else {
                entry_ref ref;
                for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; ++i) {
                    BPath path(&ref);
                    if (path.InitCheck() == B_OK) OpenInNormalWindow(summit::FileURL(path.Path()));
                }
            }
        } else {
            entry_ref ref;
            for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; ++i) {
                BPath path(&ref);
                if (path.InitCheck() == B_OK) fURLs.push_back(summit::FileURL(path.Path()));
            }
        }
    }
    void ReadyToRun() override
    {
        // If Summit is the system's browser, keep the system pointed at the
        // installed Summit (registrar round trips: off the startup path).
        std::thread(summit::RefreshDefaultBrowserHint).detach();
        SUMMIT_EXTENSION_TIMING("browser ready to run");
        if (fProfile.empty()) {
            BPath path;
            status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
            if (status == B_OK) status = path.Append(
#if SUMMIT_MODERN_WEBKIT
                "SummitModern"
#else
                "Summit"
#endif
            );
            if (status != B_OK) {
                StartupError("Could not locate your settings folder: " + std::string(std::strerror(status)));
                return;
            }
            fProfile = path.Path();
        }
        std::error_code error;
#if SUMMIT_MODERN_WEBKIT
        fProfile = std::filesystem::absolute(fProfile, error);
        if (error) {
            StartupError("Could not resolve the profile folder: " + error.message());
            return;
        }
#endif
        std::filesystem::create_directories(fProfile, error);
        if (error) {
            StartupError("Could not open the profile folder:\n" + fProfile.string() + "\n\n" + error.message());
            return;
        }
        fShared = std::make_shared<summit::SharedProfile>(fProfile / "profile.json");
        fShared->AddListener(BMessenger(this));
#if SUMMIT_MODERN_WEBKIT
        // The debugging server (Preferences › Developer) starts before the
        // engine, so that its log has the engine processes' output.
        ApplyMcpSettings();
        status_t initialization = BWebKitInitialize();
        if (initialization != B_OK) {
            StartupError("Could not initialize WebKit: " + std::string(std::strerror(initialization)));
            return;
        }
        fWebKitInitialized = true;
        fWebKitContext = std::make_shared<BWebKitContext>((fProfile / "WebKit").c_str());
        if (fWebKitContext->InitCheck() != B_OK) {
            StartupError("Could not open the WebKit profile: " + std::string(std::strerror(fWebKitContext->InitCheck())));
            return;
        }
        initialization = fWebKitContext->SetDownloadListener(BMessenger(this));
        if (initialization != B_OK) {
            StartupError("Could not initialize downloads: " + std::string(std::strerror(initialization)));
            return;
        }
        fSitePermissions = std::make_unique<summit::SitePermissionService>(fWebKitContext, false, fProfile / "Favicons",
            [this](const std::string& permission, const std::string& origin, std::optional<bool> allowed) {
                SaveSitePermission(permission, origin, allowed);
            });
        AddHandler(fSitePermissions.get());
        fWebKitContext->SetPermissionListener(BMessenger(fSitePermissions.get()));
        fPermissionPrompts = std::make_unique<summit::ExtensionPermissionPrompt>(
            [context = std::weak_ptr<BWebKitContext>(fWebKitContext)](const std::string& identifier, bool allowed) {
                if (auto liveContext = context.lock())
                    liveContext->RespondToExtensionPermissionRequest(identifier.c_str(), allowed);
            });
        AddHandler(fPermissionPrompts.get());
        initialization = fWebKitContext->SetExtensionPermissionListener(BMessenger(fPermissionPrompts.get()));
        if (initialization == B_NOT_SUPPORTED) {
            RemoveHandler(fPermissionPrompts.get());
            fPermissionPrompts.reset();
        } else if (initialization != B_OK) {
            StartupError("Could not initialize extension permissions: " + std::string(std::strerror(initialization)));
            return;
        }
        if (fPermissionPrompts) {
            initialization = fWebKitContext->SetExtensionActionListener(BMessenger(this));
            if (initialization != B_OK) {
                StartupError("Could not initialize extension actions: " + std::string(std::strerror(initialization)));
                return;
            }
            // Extensions change tabs and windows by asking this application.
            initialization = fWebKitContext->SetBrowserCommandListener(BMessenger(this));
            if (initialization != B_OK && initialization != B_NOT_SUPPORTED) {
                StartupError("Could not initialize extension tab commands: " + std::string(std::strerror(initialization)));
                return;
            }
        }
#else
        setenv("CURL_COOKIE_JAR_PATH", (fProfile / "cookies.sqlite").c_str(), 1);
        BWebPage::InitializeOnce();
        fWebKitInitialized = true;
        BWebPage::SetCacheModel(B_WEBKIT_CACHE_MODEL_WEB_BROWSER);
        BWebSettings::SetPersistentStoragePath((fProfile / "WebKit").c_str());
#endif
        app_info info; GetAppInfo(&info); BPath executable(&info.ref);
        auto home = std::filesystem::path(executable.Path()).parent_path() / "resources/start.html";
        if (!std::filesystem::exists(home, error)) home = "/boot/system/data/Summit/start.html";
        if (!std::filesystem::exists(home, error)) {
            std::fprintf(stderr, "Summit: missing start page at %s\n", home.c_str());
        }
        fStartURL = summit::FileURL(home.string());
#if SUMMIT_MODERN_WEBKIT
        TrustSavedCertificates(*fWebKitContext);
        GiveSitePermissions(*fWebKitContext);
#endif
        summit::SetSearchEngine(fShared->Read([](const summit::Profile& profile) { return profile.searchEngine; }));
        // Reopen every window of the last session, unless pages were named or
        // Preferences say to start afresh.
        const auto saved = fShared->SavedWindows();
        const bool restore = fShared->Read([](const summit::Profile& profile) { return profile.restoreSession; });
        if (!fURLs.empty() || saved.empty() || !restore) {
            summit::BrowserWindowOptions options;
            options.urls = fURLs;
            OpenWindow(options);
        } else {
            for (const auto& session : saved) {
                summit::BrowserWindowOptions options;
                options.session = session;
                OpenWindow(options);
            }
        }
        if (!fShared->LoadError().empty())
            (new BAlert("Summit", ("The saved profile could not be read. It has been preserved. " + fShared->LoadError()).c_str(),
                "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT))->Go(nullptr);
#if SUMMIT_MODERN_WEBKIT
        if (fPermissionPrompts) {
            fExtensions = std::make_unique<summit::ExtensionController>(fWebKitContext, fProfile / "Extensions",
                [this] { RefreshExtensions(); });
            AddHandler(fExtensions.get());
            fExtensions->Start();
            fInstaller = std::make_unique<summit::ExtensionInstaller>(fWebKitContext, fProfile / "Extensions",
                [this](summit::InstalledExtension entry, std::string baseURL, std::string error, bool installed, std::string newTabURL) {
                    fExtensions->AddLoaded(std::move(entry), std::move(baseURL), std::move(error), installed, std::move(newTabURL));
                }, [this] { RefreshExtensions(); });
            AddHandler(fInstaller.get());
            fInstaller->Start();
        }
#endif
    }
    void MessageReceived(BMessage* message) override
    {
        // Summit started again without arguments (its icon) while quitting.
        if (message->what == B_SILENT_RELAUNCH && fQuitting) {
            fRelaunch = true;
            return;
        }
#if SUMMIT_MODERN_WEBKIT
        if (message->what == B_WEBKIT_EXTENSION_ACTIONS_CHANGED) {
            for (const auto& window : fWindows) window.messenger.SendMessage(message);
            return;
        }
        if (message->what == B_WEBKIT_BROWSER_COMMAND || message->what == B_WEBKIT_BROWSER_COMMAND_CANCELLED) {
            BrowserCommand(message);
            return;
        }
        if (message->what == summit::kShowExtensions) {
            ShowExtensions();
            return;
        }
        if (message->what == summit::kBrowseExtensionStore || message->what == summit::kExtensionStoreInstall) {
            BMessenger sender;
            if (fQuitting || fWindows.empty() || !fInstaller || !fExtensions
                || message->FindMessenger("window", &sender) != B_OK) return;
            const bool fromManager = sender == fExtensionWindow;
            const bool fromBrowser = std::any_of(fWindows.begin(), fWindows.end(), [&](const auto& window) {
                return window.messenger == sender;
            });
            if (!fromManager && !fromBrowser) return;
            const std::string url = message->GetString("url", "");
            if (message->what == summit::kBrowseExtensionStore) {
                if (url == summit::kChromeExtensionStore || url == summit::kFirefoxExtensionStore) OpenInNormalWindow(url);
                return;
            }
            ShowExtensions();
            if (fExtensions->IsReady() && !fInstaller->IsBusy()) fInstaller->ImportStoreURL(url);
            else (new BAlert("Extensions", "Please finish the current extension operation before starting another installation.",
                "OK"))->Go(nullptr);
            return;
        }
        if (message->what == summit::kExtensionSetPinned) {
            // From a toolbar button's menu or the Extensions window.
            const char* identifier = nullptr;
            bool pinned = true;
            if (!fShared || message->FindString("extension_identifier", &identifier) != B_OK || !*identifier
                || message->FindBool("pinned", &pinned) != B_OK) return;
            const std::string id = identifier;
            fShared->Change([&](summit::Profile& profile) -> uint32 {
                const bool changed = pinned ? profile.unpinnedExtensions.erase(id) > 0 : profile.unpinnedExtensions.insert(id).second;
                return changed ? summit::SharedProfile::kExtensionsPinnedChanged : 0;
            });
            RefreshExtensions();
            return;
        }
        if (message->what == summit::kExtensionSetPrivate) {
            BMessenger sender;
            const char* identifier = nullptr;
            bool allowed = false;
            // Only the Extensions window, or a test driver where one is allowed.
            static const bool synthesis = [] {
                const char* value = std::getenv("SUMMIT_ENABLE_INPUT_SYNTHESIS");
                return value && !std::strcmp(value, "1");
            }();
            const bool fromManager = message->FindMessenger("window", &sender) == B_OK && sender == fExtensionWindow;
            if (!fExtensions || !(fromManager || synthesis)
                || message->FindString("extension_identifier", &identifier) != B_OK
                || message->FindBool("allowed", &allowed) != B_OK) return;
            fExtensions->SetAllowPrivateBrowsing(identifier, allowed);
            RefreshExtensions();
            return;
        }
        if (message->what == summit::kExtensionSelected || message->what == summit::kExtensionApprove
            || message->what == summit::kExtensionCancel || message->what == summit::kExtensionEnable
            || message->what == summit::kExtensionRemove || message->what == summit::kExtensionManagerClosed) {
            BMessenger sender;
            if (!fInstaller || !fExtensions || message->FindMessenger("window", &sender) != B_OK || sender != fExtensionWindow) return;
            uint64 generation = 0;
            message->FindUInt64("generation", &generation);
            if (message->what == summit::kExtensionManagerClosed) {
                fExtensionWindow = {};
                // The window may close before the first import snapshot has
                // reached it. Closing the owning manager cancels its current
                // import even when that window still has an older generation.
                fInstaller->Cancel(fInstaller->Generation());
                return;
            }
            if (fWindows.empty()) return;
            if (message->what == summit::kExtensionSelected && fExtensions->IsReady() && !fInstaller->IsBusy()) {
                entry_ref ref;
                if (message->FindRef("refs", &ref) == B_OK) {
                    BPath path(&ref);
                    if (path.InitCheck() == B_OK) fInstaller->Import(path.Path());
                }
            } else if (message->what == summit::kExtensionApprove) {
                bool files = false;
                if (message->FindBool("allow_files", &files) == B_OK) fInstaller->Approve(generation, files, false);
            } else if (message->what == summit::kExtensionCancel) fInstaller->Cancel(generation);
            else if (!fInstaller->IsBusy()) {
                const char* identifier = nullptr;
                if (message->FindString("extension_identifier", &identifier) == B_OK) {
                    if (message->what == summit::kExtensionRemove) fExtensions->Remove(identifier);
                    else {
                        bool enabled = false;
                        if (message->FindBool("enabled", &enabled) == B_OK) fExtensions->SetEnabled(identifier, enabled);
                    }
                }
            }
            RefreshExtensions();
            return;
        }
        if (message->what == B_WEBKIT_DOWNLOAD_STARTED || message->what == B_WEBKIT_DOWNLOAD_PROGRESS
            || message->what == B_WEBKIT_DOWNLOAD_FINISHED) {
            // The application remains the listener while closing windows and
            // draining cancellation replies on the WebKit main loop. Each
            // download is reported to the window whose page started it.
            const uint64 identifier = message->GetUInt64("identifier", 0);
            BMessenger target;
            if (auto found = fDownloads.find(identifier); found != fDownloads.end()) target = found->second;
            if (!target.IsValid()) {
                BMessenger view;
                if (message->FindMessenger("view", &view) == B_OK) target = WindowOf(view);
            }
            if (!target.IsValid() && !fWindows.empty()) target = FrontWindow();
            if (message->what == B_WEBKIT_DOWNLOAD_FINISHED) fDownloads.erase(identifier);
            else if (identifier) fDownloads[identifier] = target;
            if (target.IsValid()) target.SendMessage(message);
            if (message->what == B_WEBKIT_DOWNLOAD_FINISHED) ReleaseRetiredPrivateContexts();
            return;
        }
        if (message->what == summit::kDownloadQuitReply) {
            fDownloadPromptPending = false;
            if (message->GetInt32("which", 0) == 1) {
                fDownloadQuitApproved = true;
                PostMessage(B_QUIT_REQUESTED);
            }
            return;
        }
        if (message->what == summit::kWindowReadyToClose) {
            BMessenger sender;
            if (message->FindMessenger("window", &sender) != B_OK) return;
            auto record = std::find_if(fWindows.begin(), fWindows.end(), [&](const WindowRecord& window) { return window.messenger == sender; });
            if (record == fWindows.end()) return;
            const uint64 key = record->key;
            const bool privateWindow = record->privateBrowsing;
            fWindows.erase(record);
            if (sender.LockTarget()) {
                BLooper* looper = nullptr;
                auto* window = dynamic_cast<summit::BrowserWindow*>(sender.Target(&looper));
                if (window) window->Quit();
                else if (looper) looper->Unlock();
            }
            // The last private window takes the private session with it.
            if (privateWindow && std::none_of(fWindows.begin(), fWindows.end(),
                    [](const WindowRecord& window) { return window.privateBrowsing; }))
                EndPrivateSession();
            if (fQuitting) {
                SUMMIT_QUIT_TRACE("window closed, %zu left", fWindows.size());
                // Quitting keeps every window's tabs for the next start.
                fClosedWhileQuitting.push_back(key);
                if (fWindows.empty()) PostMessage(B_QUIT_REQUESTED);
                else CloseNextWindow();
            } else if (!fWindows.empty()) {
                // A window closed on its own: its tabs are not reopened.
                fShared->RemoveWindowSession(key);
                fShared->SaveSoon();
            } else PostMessage(B_QUIT_REQUESTED);
            return;
        }
        if (message->what == summit::kWindowCloseCancelled) {
            if (!fQuitting) return;
            SUMMIT_QUIT_TRACE("a window stayed open; not quitting");
            if (fShutdownRequest) {
                // The page's choice cancels the shutdown, as for any
                // application that does not quit.
                BMessage reply(B_REPLY);
                reply.AddBool("result", false);
                reply.AddInt32("thread", Thread());
                fShutdownRequest->SendReply(&reply);
                fShutdownRequest.reset();
            }
            // A page kept its window open: Summit keeps running, without the
            // windows that already closed.
            fQuitting = false;
            fDownloadQuitApproved = false;
            for (auto key : fClosedWhileQuitting) fShared->RemoveWindowSession(key);
            fClosedWhileQuitting.clear();
            fShared->SaveSoon();
            // What was started meanwhile opens here after all.
            fRelaunch = false;
            for (const auto& argument : std::exchange(fRelaunchArguments, { })) OpenInNormalWindow(argument);
            return;
        }
        if (message->what == summit::kCloseWindowRequest) {
            BMessenger sender;
            if (fQuitting || message->FindMessenger("window", &sender) != B_OK) return;
            // Closing the last window quits, which keeps its tabs for next time.
            if (fWindows.size() <= 1) { PostMessage(B_QUIT_REQUESTED); return; }
            BMessage request(summit::kRequestWindowClose);
            request.AddBool("quitting", false);
            sender.SendMessage(&request);
            return;
        }
#endif
        if (message->what == summit::kWindowActivated) {
            BMessenger sender;
            if (message->FindMessenger("window", &sender) != B_OK) return;
            auto record = std::find_if(fWindows.begin(), fWindows.end(), [&](const WindowRecord& window) { return window.messenger == sender; });
            if (record != fWindows.end()) std::rotate(fWindows.begin(), record, record + 1);
            return;
        }
        if (message->what == summit::kNewWindow) {
            summit::BrowserWindowOptions options;
            const char* url = nullptr;
            for (int32 i = 0; message->FindString("url", i, &url) == B_OK; ++i) options.urls.push_back(url);
            message->FindRect("frame", &options.frame);
            options.privateBrowsing = message->GetBool("private", false);
#if SUMMIT_MODERN_WEBKIT
            options.newPage = message->GetUInt64("new_page", 0);
            options.newPageURL = message->GetString("new_page_url", "about:blank");
            if (fQuitting) {
                if (options.newPage) BWebKitView::DeclineNewPage(options.newPage);
                return;
            }
#endif
            if (!options.frame.IsValid() && !fWindows.empty()) options.frame = CascadedFrame();
            OpenWindow(options);
            return;
        }
        if (message->what == summit::kShowPreferences) { ShowPreferences(message->GetString("tab", nullptr)); return; }
        if (message->what == summit::kMakeDefaultBrowser || message->what == summit::kDefaultBrowserState) {
            // From Preferences (or summitctl); kDefaultBrowserState only asks.
            std::string error;
            if (message->what == summit::kMakeDefaultBrowser) summit::MakeDefaultBrowser(error);
            BMessage state = DefaultBrowserState();
            if (!error.empty()) state.AddString("error", error.c_str());
            if (fPreferences.IsValid()) fPreferences.SendMessage(&state);
            if (message->IsSourceWaiting()) message->SendReply(&state);
            return;
        }
        if (message->what == summit::kPreferencesChanged) {
            const char* home = nullptr;
            const char* style = nullptr;
            const char* engine = nullptr;
            bool bar = false, restore = true;
            const bool hasHome = message->FindString("home_url", &home) == B_OK && home;
            const bool hasBar = message->FindBool("show_bookmarks_bar", &bar) == B_OK;
            const bool hasRestore = message->FindBool("restore_session", &restore) == B_OK;
            const bool hasStyle = message->FindString("interface_style", &style) == B_OK && style;
            const bool hasEngine = message->FindString("search_engine", &engine) == B_OK && engine;
            bool mcpEnabled = false, mcpLocal = false;
            int32 mcpPort = 0;
            const bool hasMcpEnabled = message->FindBool("mcp_enabled", &mcpEnabled) == B_OK;
            const bool hasMcpLocal = message->FindBool("mcp_local_only", &mcpLocal) == B_OK;
            const bool hasMcpPort = message->FindInt32("mcp_port", &mcpPort) == B_OK && mcpPort > 0 && mcpPort < 65536;
            const bool newMcpToken = message->GetBool("mcp_new_token", false);
            fShared->Change([&](summit::Profile& profile) -> uint32 {
                bool changed = false;
                if (hasMcpEnabled && profile.mcpEnabled != mcpEnabled) { profile.mcpEnabled = mcpEnabled; changed = true; }
                if (hasMcpLocal && profile.mcpLocalOnly != mcpLocal) { profile.mcpLocalOnly = mcpLocal; changed = true; }
                if (hasMcpPort && profile.mcpPort != mcpPort) { profile.mcpPort = mcpPort; changed = true; }
#if SUMMIT_MODERN_WEBKIT
                if (newMcpToken) { profile.mcpToken = summit::McpService::NewToken(); changed = true; }
#else
                (void)newMcpToken;
#endif
                if (hasHome && profile.homeURL != summit::Trim(home)) { profile.homeURL = summit::Trim(home); changed = true; }
                if (hasBar && profile.showBookmarksBar != bar) { profile.showBookmarksBar = bar; changed = true; }
                if (hasRestore && profile.restoreSession != restore) { profile.restoreSession = restore; changed = true; }
                if (hasStyle && profile.interfaceStyle != style) { profile.interfaceStyle = style; changed = true; }
                if (hasEngine && profile.searchEngine != engine) {
                    // Set before windows hear of the change, so they see the new engine.
                    summit::SetSearchEngine(engine);
                    profile.searchEngine = summit::CurrentSearchEngine().id;
                    changed = true;
                }
                return changed ? summit::SharedProfile::kSettingsChanged : 0;
            });
            return;
        }
#if SUMMIT_MODERN_WEBKIT
        if (message->what == summit::kProtocolHandlerRemove) {
            const std::string scheme = message->GetString("scheme", "");
            fShared->Change([&](summit::Profile& profile) -> uint32 {
                return profile.RemoveProtocolHandler(scheme) ? summit::SharedProfile::kSettingsChanged : 0;
            });
            SendPreferencesState();
            return;
        }
        if (message->what == summit::kSitePermissionChange) {
            // From Preferences › Site Permissions: "state" 1 allows, 0 blocks, -1 forgets.
            const std::string permission = message->GetString("permission", ""), origin = message->GetString("origin", "");
            const int32 state = message->GetInt32("state", -1);
            SaveSitePermission(permission, origin, state < 0 ? std::nullopt : std::optional<bool>(state > 0));
            SendPreferencesState();
            return;
        }
#endif
        if (message->what == summit::kClearHistoryRequest) {
            fShared->ClearHistory(BMessenger());
            BMessage cleared(summit::kDataCleared);
            cleared.AddInt32("kind", summit::kClearHistoryRequest);
            fPreferences.SendMessage(&cleared);
            return;
        }
#if SUMMIT_MODERN_WEBKIT
        if (message->what == summit::kAnswerPermissionPrompt) {
            const int32 which = message->GetInt32("which", 1), device = message->GetInt32("device_index", 0);
            bool answered = false;
            for (auto* service : { fSitePermissions.get(), fPrivateSitePermissions.get() }) {
                if (!answered && service) answered = service->AnswerOpenPrompt(which, device);
            }
            BMessage reply(B_REPLY);
            reply.AddBool("answered", answered);
            message->SendReply(&reply);
            return;
        }
        if (message->what == summit::kClearCacheRequest || message->what == summit::kClearSiteDataRequest) {
            // Extensions keep their own storage; only websites' is removed.
            const uint32 types = message->what == summit::kClearCacheRequest ? B_WEBKIT_DATA_CACHE
                : B_WEBKIT_DATA_COOKIES | B_WEBKIT_DATA_LOCAL_STORAGE | B_WEBKIT_DATA_INDEXED_DB | B_WEBKIT_DATA_SERVICE_WORKERS;
            fDataRequests[++fNextDataRequest] = message->what;
            fWebKitContext->RemoveWebsiteData(types, BMessenger(this), fNextDataRequest);
            return;
        }
        if (message->what == summit::kTrustCertificate) {
            const std::string host = message->GetString("host", ""), sha256 = message->GetString("sha256", "");
            if (host.empty() || sha256.empty()) return;
            for (const auto& context : {fWebKitContext, fPrivateContext})
                if (context) context->AllowServerCertificate(host.c_str(), sha256.c_str());
            // A choice made in a private window is forgotten with its session.
            if (!message->GetBool("private", false)) {
                summit::TrustedCertificate certificate { host, sha256, message->GetString("subject", ""),
                    static_cast<int64_t>(std::time(nullptr)) };
                fShared->Change([&](summit::Profile& profile) -> uint32 {
                    return profile.TrustCertificate(certificate) ? summit::SharedProfile::kSettingsChanged : 0;
                });
            }
            return;
        }
        if (message->what == summit::kForgetCertificatesRequest) {
            for (const auto& context : {fWebKitContext, fPrivateContext})
                if (context) context->ForgetServerCertificates();
            fShared->Change([](summit::Profile& profile) -> uint32 {
                if (profile.trustedCertificates.empty()) return 0;
                profile.trustedCertificates.clear();
                return summit::SharedProfile::kSettingsChanged;
            });
            BMessage cleared(summit::kDataCleared);
            cleared.AddInt32("kind", summit::kForgetCertificatesRequest);
            fPreferences.SendMessage(&cleared);
            return;
        }
        if (message->what == B_WEBKIT_WEBSITE_DATA_REMOVED) {
            auto request = fDataRequests.find(message->GetUInt64("identifier", 0));
            if (request == fDataRequests.end()) return;
            BMessage cleared(summit::kDataCleared);
            cleared.AddInt32("kind", request->second);
            if (const char* error = nullptr; message->FindString("error", &error) == B_OK) cleared.AddString("error", error);
            fDataRequests.erase(request);
            fPreferences.SendMessage(&cleared);
            return;
        }
#endif
        if (message->what == summit::kPreferencesUseCurrentPage) {
            if (!fWindows.empty()) FrontWindow().SendMessage(message);
            return;
        }
        if (message->what == summit::kPreferencesClosed) {
            BMessenger window;
            if (message->FindMessenger("window", &window) == B_OK && window == fPreferences) fPreferences = BMessenger();
            return;
        }
        if (message->what == summit::kProfileChanged) {
#if SUMMIT_MODERN_WEBKIT
            if (message->GetUInt32("changes", 0) & summit::SharedProfile::kSettingsChanged) ApplyMcpSettings();
#endif
            if (message->GetUInt32("changes", 0) & (summit::SharedProfile::kSettingsChanged | summit::SharedProfile::kHistoryChanged))
                SendPreferencesState();
            return;
        }
#if SUMMIT_MODERN_WEBKIT
        if (message->what == summit::kMcpRequest) {
            McpRequest(message);
            return;
        }
        if (message->what == summit::kMcpStatus) {
            // The Preferences window's refresh: only the server's status, so
            // what is being typed in other fields is left alone.
            SendPreferencesState(true);
            return;
        }
#endif
        if (message->what == summit::kCreateTabOnApp) {
#if SUMMIT_MODERN_WEBKIT
            // A restored session asks for its extension pages before their
            // extensions have loaded; they are created once loading ends.
            if (fExtensions && !fExtensions->IsReady() && fExtensions->CatalogError().empty()
                && std::string_view(message->GetString("url", "")).starts_with("webkit-extension:")
                && fPendingExtensionTabs.size() < 64) {
                fPendingExtensionTabs.push_back(*message);
                return;
            }
#endif
            BMessenger target;
            const char* url = nullptr;
            bool select = true;
            message->FindBool("select", &select);
#if SUMMIT_MODERN_WEBKIT
            const int32 index = message->GetInt32("index", -1);
            const uint64 command = message->GetUInt64("command", 0);
            const int64 replaces = message->GetInt64("replaces", 0);
#endif
            if (message->FindMessenger("window", &target) != B_OK
                || message->FindString("url", &url) != B_OK || !target.LockTarget()) {
#if SUMMIT_MODERN_WEBKIT
                // An extension is waiting for this tab; do not leave it to time out.
                if (command && fWebKitContext)
                    fWebKitContext->RespondToBrowserCommand(command, B_ERROR, { }, "The browser window is not available.");
#endif
                return;
            }
            BLooper* looper = nullptr;
            auto* window = dynamic_cast<summit::BrowserWindow*>(target.Target(&looper));
#if SUMMIT_MODERN_WEBKIT
            if (!window && command && fWebKitContext)
                fWebKitContext->RespondToBrowserCommand(command, B_ERROR, { }, "The browser window is not available.");
#endif
            if (window) {
#if SUMMIT_MODERN_WEBKIT
                std::string extensionIdentifier;
                if (fExtensions) {
                    for (const auto& entry : fExtensions->Entries()) {
                        // Load receipts contain the canonical origin with its
                        // trailing slash, so a hostname prefix cannot match.
                        if (entry.loaded && !entry.baseURL.empty() && entry.baseURL.back() == '/'
                            && std::string_view(url).starts_with(entry.baseURL)) {
                            extensionIdentifier = entry.installation.identifier;
                            break;
                        }
                    }
                }
                window->CreateTab(url, select, extensionIdentifier.c_str(), index, command, replaces);
#else
                window->CreateTab(url, select);
#endif
            }
            if (looper) looper->Unlock();
            return;
        }
        if (!fWindows.empty() && message->what == summit::kBrowserState) FrontWindow().SendMessage(message);
        else if (!fWindows.empty() && (message->what == summit::kNewTab || message->what == summit::kNavigate)) {
            // Pages from outside never open in a private window.
            if (FrontNormalWindow().IsValid()) FrontNormalWindow().SendMessage(message);
            else {
                summit::BrowserWindowOptions options;
                if (const char* url = nullptr; message->FindString("url", &url) == B_OK) options.urls.push_back(url);
                options.frame = CascadedFrame();
                OpenWindow(options);
            }
        } else BApplication::MessageReceived(message);
    }
    void AboutRequested() override { if (!fWindows.empty()) FrontWindow().SendMessage(B_ABOUT_REQUESTED); }
#if SUMMIT_MODERN_WEBKIT
    void Pulse() override
    {
        if (fWaitingForNativeUI) PostMessage(B_QUIT_REQUESTED);
    }
    bool QuitRequested() override
    {
        SUMMIT_QUIT_TRACE("quit requested (%zu windows%s)", fWindows.size(), fQuitting ? ", already quitting" : "");
        if (!fWindows.empty()) {
            if (fQuitting || fDownloadPromptPending) return false;
            if (!fDownloads.empty() && !fDownloadQuitApproved) {
                // Downloads belong to the application, not to one window.
                fDownloadPromptPending = true;
                auto* prompt = new BAlert("Downloads", "Downloads are still in progress. Quit and cancel them?",
                    "Keep Browsing", "Quit");
                prompt->SetShortcut(0, B_ESCAPE);
                prompt->Go(new BInvoker(new BMessage(summit::kDownloadQuitReply), this));
                return false;
            }
            // Windows close one at a time, so pages can ask about unsaved work.
            fQuitting = true;
            fClosedWhileQuitting.clear();
            CloseNextWindow();
            return false;
        }
        if (fWebKitContext) {
            fWebKitContext->SetExtensionActionListener({});
            fWebKitContext->SetBrowserCommandListener({});
            if (fExtensionWindow.IsValid()) fExtensionWindow.SendMessage(summit::kCloseExtensionManager);
            if (fInstaller) fInstaller->Shutdown();
            if (fExtensions) fExtensions->Shutdown();
            if (fExtensionWindows->load() || (fInstaller && fInstaller->HasPendingWork())
                || (fExtensions && fExtensions->HasPendingWork())) {
                fWaitingForNativeUI = true;
                SetPulseRate(100000);
                return false;
            }
            if (fInstaller) {
                RemoveHandler(fInstaller.get());
                fInstaller.reset();
            }
            if (fExtensions) {
                RemoveHandler(fExtensions.get());
                fExtensions.reset();
            }
            if (fPermissionPrompts) {
                fWebKitContext->CancelExtensionPermissionRequests();
                fPermissionPrompts->Shutdown();
                if (fPermissionPrompts->HasOpenWindows()) {
                    fWaitingForNativeUI = true;
                    SetPulseRate(100000);
                    return false;
                }
                RemoveHandler(fPermissionPrompts.get());
                fPermissionPrompts.reset();
            }
            if (fPrivateContext) EndPrivateSession();
            ReleaseRetiredPrivateContexts();
            if (fWebKitContext->HasPendingDownloads() || !fRetiredPrivateContexts.empty()) {
                if (!fCancellingDownloads) {
                    fCancellingDownloads = true;
                    fWebKitContext->CancelAllDownloads();
                }
                fWaitingForNativeUI = true;
                SetPulseRate(100000);
                return false;
            }
            SUMMIT_QUIT_TRACE("releasing the engine");
            fWebKitContext.reset();
            SUMMIT_QUIT_TRACE("engine released");
            PostMessage(B_QUIT_REQUESTED);
            return false;
        }
        if (BWebKitHasPendingNativeUI()) {
            fWaitingForNativeUI = true;
            fNativeUIExitReady = false;
            SetPulseRate(100000); // Native pulse intervals use 100 ms granularity.
            return false;
        }
        fWaitingForNativeUI = false;
        SetPulseRate(0);
        if (!fNativeUIExitReady) {
            fNativeUIExitReady = true;
            PostMessage(B_QUIT_REQUESTED);
            return false;
        }
        // What the profile's thread has still to write is written once the
        // application has left the registrar (RunApplication), so that a new
        // start does not wait on a slow disk.
        SUMMIT_QUIT_TRACE("the application quits");
        return true;
    }
    void QuitForShutdown(BMessage* request)
    {
        SUMMIT_QUIT_TRACE("the system shuts down");
        fShutdownRequest.reset(request);
        // The registrar kills an application that has not quit after three
        // seconds: the windows and tabs are written first.
        std::string error;
        if (fShared && !fShared->Save(error)) std::fprintf(stderr, "Summit: could not save the profile: %s\n", error.c_str());
        // Nobody is there to answer the downloads question.
        fDownloadQuitApproved = true;
        PostMessage(B_QUIT_REQUESTED);
    }
#endif
    bool WebKitInitialized() const { return fWebKitInitialized; }
    int ExitStatus() const { return fExitStatus; }
    std::shared_ptr<summit::SharedProfile> Profile() const { return fShared; }
    // Whether Summit was started again while it quit, and with what.
    bool TakeRelaunch(std::vector<std::string>& arguments)
    {
        arguments = std::exchange(fRelaunchArguments, { });
        return std::exchange(fRelaunch, false);
    }
private:
    struct WindowRecord {
        BMessenger messenger;
        summit::BrowserWindow* window;
        uint64 key;
        bool privateBrowsing = false;
    };
    // Most recently active first.
    std::vector<WindowRecord> fWindows;
    uint64 fNextWindowKey = 1;
    std::shared_ptr<summit::SharedProfile> fShared;
    std::string fStartURL;
    BMessenger fPreferences;
    bool fQuitting = false;
    bool fRelaunch = false;
    std::vector<std::string> fRelaunchArguments;
    std::vector<uint64> fClosedWhileQuitting;
    std::map<uint64, BMessenger> fDownloads;
    bool fDownloadQuitApproved = false;
    bool fDownloadPromptPending = false;
    // The registrar's quit request of a system shutdown, until Summit quits
    // or a page keeps its window open.
    std::unique_ptr<BMessage> fShutdownRequest;

    BMessenger FrontWindow() const { return fWindows.empty() ? BMessenger() : fWindows.front().messenger; }
    // The most recently active window that is not private.
    BMessenger FrontNormalWindow() const
    {
        for (const auto& window : fWindows) if (!window.privateBrowsing) return window.messenger;
        return BMessenger();
    }
    void OpenInNormalWindow(const std::string& url)
    {
        if (auto window = FrontNormalWindow(); window.IsValid()) {
            BMessage message(summit::kNewTab);
            message.AddString("url", url.c_str());
            window.SendMessage(&message);
            return;
        }
        summit::BrowserWindowOptions options;
        options.urls.push_back(url);
        options.frame = CascadedFrame();
        OpenWindow(options);
    }
#if SUMMIT_MODERN_WEBKIT
    // One private context serves every private window, as one private session
    // does in other browsers. It lives until the last private window closes.
    std::shared_ptr<BWebKitContext> PrivateContext()
    {
        if (fPrivateContext) return fPrivateContext;
        // Its pages are served by the normal profile's extensions: those
        // allowed in private windows work there with their own saved data.
        auto context = std::make_shared<BWebKitContext>(fWebKitContext);
        if (context->InitCheck() != B_OK || context->SetDownloadListener(BMessenger(this)) != B_OK) return nullptr;
        TrustSavedCertificates(*context);
        // Saved site permissions apply in private windows too; what is
        // answered there is forgotten with the session.
        GiveSitePermissions(*context);
        if (fPrivateSitePermissions) {
            RemoveHandler(fPrivateSitePermissions.get());
            fPrivateSitePermissions.reset();
        }
        fPrivateSitePermissions = std::make_unique<summit::SitePermissionService>(context, true, fProfile / "Favicons",
            [](const std::string&, const std::string&, std::optional<bool>) { });
        AddHandler(fPrivateSitePermissions.get());
        context->SetPermissionListener(BMessenger(fPrivateSitePermissions.get()));
        fPrivateContext = std::move(context);
        return fPrivateContext;
    }
    void GiveSitePermissions(BWebKitContext& context)
    {
        if (!fShared) return;
        fShared->Read([&](const summit::Profile& profile) {
            for (const auto& [permission, origins] : profile.sitePermissions)
                for (const auto& [origin, allowed] : origins)
                    context.SetSitePermission(permission.c_str(), origin.c_str(), allowed ? 1 : 0);
            return 0;
        });
    }
    // A decision from a prompt or from Preferences: saved, and given to the
    // normal and private contexts (a private window's own answers are not).
    void SaveSitePermission(const std::string& permission, const std::string& origin, std::optional<bool> allowed)
    {
        fShared->Change([&](summit::Profile& profile) -> uint32 {
            return profile.SetSitePermission(permission, origin, allowed) ? summit::SharedProfile::kSettingsChanged : 0;
        });
        for (const auto& context : {fWebKitContext, fPrivateContext})
            if (context) context->SetSitePermission(permission.c_str(), origin.c_str(), allowed ? (*allowed ? 1 : 0) : -1);
    }
    // Certificates the user trusted despite failed verification (saved in
    // the profile) are given to each context when it is created.
    void TrustSavedCertificates(BWebKitContext& context)
    {
        if (!fShared) return;
        fShared->Read([&](const summit::Profile& profile) {
            for (const auto& certificate : profile.trustedCertificates)
                context.AllowServerCertificate(certificate.host.c_str(), certificate.sha256.c_str());
            return 0;
        });
    }
#endif
    void EndPrivateSession()
    {
        fShared->ClearPrivateSession();
#if SUMMIT_MODERN_WEBKIT
        if (!fPrivateContext) return;
        // Its cookies, cache and storage are only in memory: releasing the
        // context forgets them. Downloads still running are cancelled first.
        if (fPrivateContext->HasPendingDownloads()) {
            fPrivateContext->CancelAllDownloads();
            fRetiredPrivateContexts.push_back(std::move(fPrivateContext));
        }
        fPrivateContext.reset();
#endif
    }
    void ReleaseRetiredPrivateContexts()
    {
#if SUMMIT_MODERN_WEBKIT
        std::erase_if(fRetiredPrivateContexts, [](const std::shared_ptr<BWebKitContext>& context) {
            return !context->HasPendingDownloads();
        });
#endif
    }
    // The browser window that owns a page view.
    BMessenger WindowOf(const BMessenger& view) const
    {
        BLooper* looper = nullptr;
        if (!view.IsValid() || !view.Target(&looper) || !looper) return BMessenger();
        for (const auto& window : fWindows)
            if (static_cast<BLooper*>(window.window) == looper) return window.messenger;
        return BMessenger();
    }
    BRect CascadedFrame() const
    {
        BRect frame;
        if (!fWindows.empty() && fWindows.front().messenger.LockTarget()) {
            frame = fWindows.front().window->Frame().OffsetByCopy(24, 24);
            fWindows.front().window->Unlock();
        }
        return frame;
    }
    summit::BrowserWindow* OpenWindow(summit::BrowserWindowOptions options)
    {
#if SUMMIT_MODERN_WEBKIT
        auto context = options.privateBrowsing ? PrivateContext() : fWebKitContext;
        if (!context) {
            if (options.newPage) BWebKitView::DeclineNewPage(options.newPage);
            (new BAlert("Summit", "Could not open a private window.", "OK", nullptr, nullptr,
                B_WIDTH_AS_USUAL, B_WARNING_ALERT))->Go(nullptr);
            return nullptr;
        }
#else
        options.privateBrowsing = false;
#endif
        options.key = fNextWindowKey++;
        auto* window = new summit::BrowserWindow(fShared, fStartURL, options
#if SUMMIT_MODERN_WEBKIT
            , context, bool(fPermissionPrompts)
#endif
        );
        fWindows.insert(fWindows.begin(), {BMessenger(window), window, options.key, options.privateBrowsing});
        window->Show();
        SUMMIT_EXTENSION_TIMING("browser window shown");
        return window;
    }
    void CloseNextWindow()
    {
        if (fWindows.empty()) return;
        BMessage request(summit::kRequestWindowClose);
        request.AddBool("quitting", true);
        fWindows.front().messenger.SendMessage(&request);
    }
    // tab: the Preferences tab to show ("sites", "developer"...), or null.
    void ShowPreferences(const char* tab = nullptr)
    {
        BMessage show(summit::kShowPreferences);
        if (tab) show.AddString("tab", tab);
        if (fPreferences.IsValid()) {
            fPreferences.SendMessage(&show);
            return;
        }
        summit::PreferencesState state = fShared->Read([](const summit::Profile& profile) {
            return summit::PreferencesState { profile.homeURL, profile.showBookmarksBar, profile.interfaceStyle,
                profile.searchEngine, profile.history.size(), { }, profile.trustedCertificates.size(),
                profile.restoreSession, profile.mcpEnabled, profile.mcpPort, profile.mcpLocalOnly };
        });
        state.webKitDirectory = fProfile / "WebKit";
        auto* window = new summit::PreferencesWindow(BMessenger(this), state);
        if (!fWindows.empty() && fWindows.front().messenger.LockTarget()) {
            const BRect front = fWindows.front().window->Frame();
            fWindows.front().window->Unlock();
            // Centred over the front window by hand: CenterIn() moves a window
            // that is still hidden to the monitor under the pointer, which on
            // a two-monitor desktop need not be the browser's.
            window->Lock();
            window->UpdateSizeLimits();
            const BRect frame = window->Frame();
            window->MoveTo(std::round(front.left + (front.Width() - frame.Width()) / 2),
                std::round(front.top + std::max(0.0f, (front.Height() - frame.Height()) / 2)));
            window->Unlock();
        }
        fPreferences = BMessenger(window);
        window->Show();
        if (tab) fPreferences.SendMessage(&show);
        BMessage browser = DefaultBrowserState();
        fPreferences.SendMessage(&browser);
        SendPreferencesState();
    }
    static BMessage DefaultBrowserState()
    {
        const auto browser = summit::QueryDefaultBrowser();
        BMessage state(summit::kDefaultBrowserState);
        state.AddBool("is_default", browser.isDefault);
        state.AddString("current", browser.current.c_str());
        return state;
    }
    // Repeated "site_permission", "site_origin" and "site_allowed" fields.
    static void AddSitePermissions(BMessage& state, const summit::Profile& profile)
    {
        state.AddBool("has_site_permissions", true);
        for (const auto& [permission, origins] : profile.sitePermissions)
            for (const auto& [origin, allowed] : origins) {
                state.AddString("site_permission", permission.c_str());
                state.AddString("site_origin", origin.c_str());
                state.AddBool("site_allowed", allowed);
            }
    }
    void SendPreferencesState(bool serverOnly = false)
    {
        if (!fPreferences.IsValid()) return;
        BMessage state(summit::kPreferencesState);
        if (!serverOnly) fShared->Read([&](const summit::Profile& profile) {
            state.AddString("home_url", profile.homeURL.c_str());
            state.AddBool("show_bookmarks_bar", profile.showBookmarksBar);
            state.AddBool("restore_session", profile.restoreSession);
            state.AddString("interface_style", profile.interfaceStyle.c_str());
            state.AddString("search_engine", profile.searchEngine.c_str());
            state.AddInt32("history_count", static_cast<int32>(profile.history.size()));
            state.AddInt32("trusted_certificates", static_cast<int32>(profile.trustedCertificates.size()));
            state.AddBool("mcp_enabled", profile.mcpEnabled);
            state.AddInt32("mcp_port", profile.mcpPort);
            state.AddBool("mcp_local_only", profile.mcpLocalOnly);
            AddSitePermissions(state, profile);
            state.AddBool("has_protocol_handlers", true);
            for (const auto& [scheme, handler] : profile.protocolHandlers) {
                state.AddString("protocol_scheme", scheme.c_str());
                state.AddString("protocol_target", handler.target.c_str());
            }
            for (const auto& [scheme, targets] : profile.declinedProtocolHandlers) {
                if (targets.empty() || profile.protocolHandlers.count(scheme)) continue;
                state.AddString("protocol_scheme", scheme.c_str());
                state.AddString("protocol_target", "Website registration declined");
            }
            return 0;
        });
#if SUMMIT_MODERN_WEBKIT
        // What the server is doing now, and how a client reaches it.
        if (fMcp) {
            state.AddString("mcp_status", fMcp->Status().c_str());
            state.AddString("mcp_endpoint", fMcp->Endpoint().c_str());
            state.AddString("mcp_configuration", fMcp->ClientConfiguration().c_str());
            state.AddBool("mcp_environment", std::getenv("SUMMIT_MCP") || std::getenv("SUMMIT_MCP_PORT") || std::getenv("SUMMIT_MCP_TOKEN"));
        } else state.AddString("mcp_status", "Off");
#endif
        fPreferences.SendMessage(&state);
    }
#if SUMMIT_MODERN_WEBKIT
    // The built-in debugging server follows the profile's settings, and the
    // environment's SUMMIT_MCP* overrides (McpService::Apply). Its token is
    // made when it is first turned on.
    void ApplyMcpSettings()
    {
        if (!fShared) return;
        auto settings = fShared->Read([](const summit::Profile& profile) {
            return summit::McpSettings { profile.mcpEnabled, static_cast<uint16>(profile.mcpPort), profile.mcpLocalOnly, profile.mcpToken };
        });
        const char* forced = std::getenv("SUMMIT_MCP");
        const bool wanted = forced && *forced ? !std::strcmp(forced, "1") : settings.enabled;
        if (wanted && settings.token.empty() && !std::getenv("SUMMIT_MCP_TOKEN")) {
            settings.token = summit::McpService::NewToken();
            const std::string token = settings.token;
            fShared->Change([&](summit::Profile& profile) -> uint32 {
                profile.mcpToken = token;
                return summit::SharedProfile::kSettingsChanged;
            });
        }
        if (!fMcp && !wanted) return;
        if (!fMcp) fMcp = std::make_unique<summit::McpService>(BMessenger(this), fProfile);
        fMcp->Apply(settings);
    }
    // The MCP server's questions for the application thread (McpService.h).
    void McpRequest(BMessage* message)
    {
        using nlohmann::json;
        const json request = json::parse(message->GetString("request", "{}"), nullptr, false);
        const std::string action = request.is_object() ? request.value("action", "") : "";
        if (action == "state") {
            json state = {{"quitting", fQuitting}, {"windows", fWindows.size()}, {"privateSession", bool(fPrivateContext)},
                {"downloads", fDownloads.size()}, {"profile", fProfile.string()},
                {"extensionsReady", fExtensions && fExtensions->IsReady()}};
            if (fExtensions && !fExtensions->CatalogError().empty()) state["extensionCatalogError"] = fExtensions->CatalogError();
            if (fMcp) state["mcp"] = {{"status", fMcp->Status()}, {"endpoint", fMcp->Endpoint()}};
            summit::McpReply(message, state);
            return;
        }
        if (action == "extensions") {
            json list = json::array();
            if (fExtensions) {
                for (const auto& entry : fExtensions->Entries()) {
                    json item = {{"identifier", entry.installation.identifier}, {"name", entry.installation.name},
                        {"version", entry.installation.version}, {"enabled", entry.installation.enabled}, {"loaded", entry.loaded},
                        {"installed", entry.installed}, {"baseURL", entry.baseURL}, {"privateWindows", entry.installation.allowPrivateBrowsing},
                        {"fileURLs", entry.installation.allowFileURLs}, {"order", entry.installation.installationOrder}};
                    if (!entry.error.empty()) item["error"] = entry.error;
                    if (!entry.notice.empty()) item["notice"] = entry.notice;
                    if (!entry.newTabURL.empty()) item["newTabPage"] = entry.newTabURL;
                    item["pinned"] = fShared && fShared->Read([&](const summit::Profile& profile) {
                        return !profile.unpinnedExtensions.contains(entry.installation.identifier);
                    });
                    list.push_back(item);
                }
            }
            summit::McpReply(message, {{"ready", fExtensions && fExtensions->IsReady()}, {"enabled", bool(fExtensions)},
                {"installerBusy", fInstaller && fInstaller->IsBusy()}, {"extensions", list}});
            return;
        }
        if (action == "extension_enable") {
            const std::string identifier = request.value("extension", "");
            if (!fExtensions || !fInstaller) { summit::McpReplyError(message, "extensions are not available in this build"); return; }
            if (!fExtensions->IsReady() || fInstaller->IsBusy()) { summit::McpReplyError(message, "the extensions are busy (loading or installing)"); return; }
            const bool done = fExtensions->SetEnabled(identifier, request.value("enabled", true));
            RefreshExtensions();
            if (!done) { summit::McpReplyError(message, "no extension " + identifier + " (or it already is that way)"); return; }
            summit::McpReply(message, {{"extension", identifier}, {"enabled", request.value("enabled", true)}, {"note", "loading or unloading continues in the background"}});
            return;
        }
        if (action == "preferences") {
            json preferences = fShared->Read([](const summit::Profile& profile) {
                json sites = json::object();
                for (const auto& [permission, origins] : profile.sitePermissions)
                    for (const auto& [origin, allowed] : origins) sites[permission][origin] = allowed;
                json handlers = json::object();
                for (const auto& [scheme, handler] : profile.protocolHandlers) handlers[scheme] = handler.target;
                json certificates = json::array();
                for (const auto& certificate : profile.trustedCertificates) certificates.push_back(certificate.host);
                return json{{"homeURL", profile.homeURL}, {"searchEngine", profile.searchEngine}, {"restoreSession", profile.restoreSession},
                    {"interfaceStyle", profile.interfaceStyle}, {"showBookmarksBar", profile.showBookmarksBar},
                    {"bookmarks", profile.bookmarks.size()}, {"history", profile.history.size()}, {"siteZoom", profile.siteZoom},
                    {"sitePermissions", sites}, {"protocolHandlers", handlers}, {"trustedCertificates", certificates},
                    {"mcp", {{"enabled", profile.mcpEnabled}, {"port", profile.mcpPort}, {"localOnly", profile.mcpLocalOnly}}}};
            });
            summit::McpReply(message, preferences);
            return;
        }
        summit::McpReplyError(message, "unknown request \"" + action + "\"");
    }
    // otherwise to the front window; windows.create() opens a real window.
    void BrowserCommand(BMessage* message)
    {
        uint64 identifier = 0;
        message->FindUInt64("identifier", &identifier);
        if (message->what == B_WEBKIT_BROWSER_COMMAND_CANCELLED) {
            for (const auto& window : fWindows) window.messenger.SendMessage(message);
            return;
        }
        auto fail = [&](const char* error) {
            if (fWebKitContext && identifier) fWebKitContext->RespondToBrowserCommand(identifier, B_ERROR, { }, error);
        };
        if (fQuitting) { fail("The browser is quitting."); return; }
        const uint32 command = message->GetUInt32("command", 0);
        if (command == B_WEBKIT_BROWSER_OPEN_WINDOW) {
            summit::BrowserWindowOptions options;
            options.empty = true;
            if (!fWindows.empty()) options.frame = CascadedFrame();
            auto* window = OpenWindow(options);
            if (!window || BMessenger(window).SendMessage(message) != B_OK) fail("The browser window is not available.");
            return;
        }
        BMessenger target;
        BMessenger named;
        if (message->FindMessenger("view", &named) == B_OK) target = WindowOf(named);
        if (!target.IsValid() && message->FindMessenger("window", &named) == B_OK)
            for (const auto& window : fWindows) if (window.messenger == named) target = named;
        // New tabs go to a normal window: only a tab or window named by an
        // extension allowed in private windows is a private one.
        if (!target.IsValid()) target = FrontNormalWindow();
        if (!target.IsValid() && command == B_WEBKIT_BROWSER_OPEN_TAB) {
            summit::BrowserWindowOptions options;
            options.empty = true;
            options.frame = CascadedFrame();
            if (auto* window = OpenWindow(options)) target = BMessenger(window);
        }
        if (!target.IsValid() || target.SendMessage(message) != B_OK) fail("The browser window is not available.");
    }
    void ShowExtensions()
    {
        if (fWindows.empty() || !fExtensions || !fInstaller || fQuitting) return;
        if (!fExtensionWindow.IsValid()) {
            auto* manager = new summit::ExtensionManager(BMessenger(this), fExtensionWindows);
            fExtensionWindow = BMessenger(manager);
            manager->Show();
        } else fExtensionWindow.SendMessage(summit::kShowExtensions);
        RefreshExtensions();
    }
    void RefreshExtensions()
    {
        if (fExtensions && fExtensions->IsReady())
            for (auto& pending : std::exchange(fPendingExtensionTabs, {})) PostMessage(&pending);
        if (fExtensions && fShared) {
            // Like Chrome, the most recently installed enabled extension that
            // overrides the new tab page supplies it.
            std::string newTab;
            uint64 order = 0;
            for (const auto& entry : fExtensions->Entries()) {
                if (entry.loaded && !entry.newTabURL.empty() && entry.installation.installationOrder >= order) {
                    newTab = entry.newTabURL;
                    order = entry.installation.installationOrder;
                }
            }
            fShared->SetNewTabOverride(std::move(newTab));
        }
        if (!fExtensionWindow.IsValid() || !fExtensions || !fInstaller) return;
        BMessage state = fInstaller->Snapshot();
        state.what = summit::kExtensionsState;
        state.AddBool("ready", fExtensions->IsReady());
        state.AddString("catalog_error", fExtensions->CatalogError().c_str());
        for (const auto& entry : fExtensions->Entries()) {
            BMessage item;
            item.AddString("identifier", entry.installation.identifier.c_str());
            item.AddString("name", entry.installation.name.c_str());
            item.AddString("version", entry.installation.version.c_str());
            item.AddString("error", entry.error.c_str());
            item.AddString("notice", entry.notice.c_str());
            item.AddBool("enabled", entry.installation.enabled);
            item.AddBool("loaded", entry.loaded);
            item.AddBool("installed", entry.installed);
            item.AddBool("private", entry.installation.allowPrivateBrowsing);
            item.AddBool("pinned", !fShared || fShared->Read([&](const summit::Profile& profile) {
                return !profile.unpinnedExtensions.contains(entry.installation.identifier);
            }));
            state.AddMessage("entry", &item);
        }
        fExtensionWindow.SendMessage(&state, static_cast<BHandler*>(nullptr), 0);
    }
#endif
    void StartupError(const std::string& message)
    {
        fExitStatus = 1;
        std::fprintf(stderr, "Summit: %s\n", message.c_str());
        (new BAlert("Summit", message.c_str(), "Quit", nullptr, nullptr,
            B_WIDTH_AS_USUAL, B_STOP_ALERT))->Go();
        PostMessage(B_QUIT_REQUESTED);
    }
#if SUMMIT_MODERN_WEBKIT
    std::shared_ptr<BWebKitContext> fWebKitContext;
    std::shared_ptr<BWebKitContext> fPrivateContext;
    // Closed private sessions whose downloads are still being cancelled.
    std::vector<std::shared_ptr<BWebKitContext>> fRetiredPrivateContexts;
    std::vector<BMessage> fPendingExtensionTabs;
    std::map<uint64, uint32> fDataRequests;
    uint64 fNextDataRequest = 0;
    std::unique_ptr<summit::ExtensionPermissionPrompt> fPermissionPrompts;
    std::unique_ptr<summit::SitePermissionService> fSitePermissions;
    std::unique_ptr<summit::SitePermissionService> fPrivateSitePermissions;
    std::unique_ptr<summit::ExtensionController> fExtensions;
    std::unique_ptr<summit::ExtensionInstaller> fInstaller;
    std::unique_ptr<summit::McpService> fMcp;
    BMessenger fExtensionWindow;
    std::shared_ptr<std::atomic<unsigned>> fExtensionWindows = std::make_shared<std::atomic<unsigned>>(0);
    bool fWaitingForNativeUI = false;
    bool fNativeUIExitReady = false;
    bool fCancellingDownloads = false;
#endif
    bool fWebKitInitialized = false;
    int fExitStatus = 0;
    std::filesystem::path fProfile;
    std::vector<std::string> fURLs;
};

// The executable this instance runs: Summit is single-launch per executable.
static std::string ExecutablePath()
{
    int32 cookie = 0;
    image_info image;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
        if (image.type == B_APP_IMAGE) return image.name;
    }
    return { };
}

// A quitting instance keeps a port of this name from the moment it leaves the
// registrar until its profile is written.
static std::string ExitingPortName()
{
    char name[B_OS_NAME_LENGTH];
    std::snprintf(name, sizeof(name), "Summit exits %016zx", std::hash<std::string>()(ExecutablePath()));
    return name;
}

// A start of the same Summit just after it quit would read the profile while
// that instance still writes it (seconds, on a Raspberry Pi's SD card): wait
// for it, up to 20 s.
static void WaitForExitingInstance()
{
    const std::string name = ExitingPortName();
    if (find_port(name.c_str()) < 0) return;
    const bigtime_t deadline = system_time() + 20000000;
    while (find_port(name.c_str()) >= 0 && system_time() < deadline) snooze(50000);
}

static void StartAgain(const std::vector<std::string>& arguments)
{
    std::vector<std::string> strings { ExecutablePath() };
    strings.insert(strings.end(), arguments.begin(), arguments.end());
    std::vector<const char*> argv;
    for (const auto& string : strings) argv.push_back(string.c_str());
    argv.push_back(nullptr);
    thread_id thread = load_image(static_cast<int32>(strings.size()), argv.data(), const_cast<const char**>(environ));
    if (thread >= 0) resume_thread(thread);
    SUMMIT_QUIT_TRACE("started again with %zu arguments: %s", arguments.size(), thread >= 0 ? "yes" : std::strerror(thread));
}

static int RunApplication()
{
    umask(0077);
    status_t status = B_NO_INIT;
    std::shared_ptr<summit::SharedProfile> profile;
    std::vector<std::string> relaunchArguments;
    bool relaunch = false;
    port_id exiting = -1;
    int exitStatus = 0;
    {
        SummitApp app(status);
        if (status == B_ALREADY_RUNNING)
            return 0; // The registrar delivered the launch arguments to the existing instance.
        if (status != B_OK) {
            std::fprintf(stderr, "Summit: could not initialize the native application: %s\n", std::strerror(status));
            return 1;
        }
        app.Run();
        SUMMIT_QUIT_TRACE("application loop ended");
#if !SUMMIT_MODERN_WEBKIT
        if (app.WebKitInitialized()) BWebPage::ShutdownOnce();
#endif
        exitStatus = app.ExitStatus();
        profile = app.Profile();
        relaunch = app.TakeRelaunch(relaunchArguments);
        if (profile) exiting = create_port(1, ExitingPortName().c_str());
    }
    // The application has left the registrar: a new start makes a new
    // instance, which waits for the port. The profile's last write (the
    // saver thread's, then its own) happens with the last reference.
    SUMMIT_QUIT_TRACE("application destroyed");
    profile = nullptr;
    SUMMIT_QUIT_TRACE("profile saved");
    if (exiting >= 0) delete_port(exiting);
    if (relaunch) StartAgain(relaunchArguments);
    return exitStatus;
}

// libnetwork's resolver re-reads nsswitch.conf whenever the path it looks at
// is newer than its last parse, builds that path in a static buffer without a
// lock, and its scanner calls exit() when it is handed the settings directory
// instead of the file (docs/kunanyios-platform-issues.md): a name lookup on
// another thread could end the network process or a web process. Until
// libnetwork is fixed, every Summit process makes its first lookup before it
// has other threads, and the file is kept newer than the directories a torn
// path can name, so that a later look at either finds nothing new to parse.
static void PrepareResolver()
{
    static const char* const file = "/boot/system/settings/network/nsswitch.conf";
    struct stat conf {};
    if (stat(file, &conf) != 0) {
        if (FILE* out = std::fopen(file, "wx")) {
            std::fputs("# Created by Summit: the sources libnetwork uses without this file.\n"
                "# See Summit docs/kunanyios-platform-issues.md before removing it.\nhosts: files dns\n", out);
            std::fclose(out);
        }
    } else {
        for (const char* directory : { "/boot/system/settings", "/boot/system/settings/network" }) {
            struct stat above {};
            if (!stat(directory, &above) && above.st_mtime > conf.st_mtime) {
                utimes(file, nullptr);
                break;
            }
        }
    }
    if (struct addrinfo* result = nullptr; !getaddrinfo("localhost", nullptr, nullptr, &result) && result)
        freeaddrinfo(result);
}

#if SUMMIT_MODERN_WEBKIT
namespace summit { int RunMcpDebugHelper(int argc, char** argv); }
#endif

int main(int argc, char** argv)
{
#if SUMMIT_MODERN_WEBKIT
    // The MCP server's stacks and profile tools run Summit again as a short
    // debugger of a team (McpDebugHelper.cpp), never as a second browser.
    if (argc >= 2 && !std::strcmp(argv[1], "--mcp-debug-helper")) return summit::RunMcpDebugHelper(argc, argv);
#else
    (void)argc;
    (void)argv;
#endif
    // Started by the system for a web link or a page (Summit is the default
    // browser), not by its launcher: take the launcher's environment.
    summit::ApplyLaunchEnvironment();
    PrepareResolver();
    WaitForExitingInstance();
    // Pages get 10 frames in error.stack, as in Chrome, not JavaScriptCore's
    // 100: every error walks the stack that far when it is made. One of
    // uBlock Origin's YouTube filters traps JSON.stringify and, for a value
    // that does not serialize, throws and catches a SyntaxError: 1.6 million
    // in 80 s of YouTube (issue #22). The web processes inherit this; set it
    // to override.
    setenv("JSC_defaultErrorStackTraceLimit", "10", 0);
    // The application thread runs WebKit's main loop: every frame, input
    // reply and IPC message of every tab passes through it. Under the load of
    // many tabs it competes with dozens of web processes, so it runs at the
    // priority of window threads. SUMMIT_UI_PRIORITY=normal leaves it alone.
    if (const char* priority = std::getenv("SUMMIT_UI_PRIORITY"); !priority || std::strcmp(priority, "normal"))
        set_thread_priority(find_thread(nullptr), B_DISPLAY_PRIORITY);
    const int status = RunApplication();
#if SUMMIT_MODERN_WEBKIT
    // RunApplication destroys the native application after its asynchronous
    // window/context cleanup. WebKit worker TLS destructors may still be
    // finishing, so do not race them with libbe's global handler-token table
    // destruction. This matches the Haiku WebKit helper-process exit path.
    SUMMIT_QUIT_TRACE("exiting");
    std::fflush(nullptr);
    std::_Exit(status);
#else
    return status;
#endif
}

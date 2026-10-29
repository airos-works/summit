#include "ui/Containers.h"
#include "ui/BrowserWindow.h"
#include "ui/PreferencesWindow.h"
#include "ui/DefaultBrowser.h"
#include <thread>
#include "ui/SharedProfile.h"
#include "ui/Messages.h"
#include "ui/FeatureMessages.h"
#if SUMMIT_MODERN_WEBKIT
#include "ui/WebApps.h"
#endif
#include "ui/ExtensionPermissionPrompt.h"
#include "ui/ExtensionController.h"
#include "ui/ExtensionTiming.h"
#include "ui/StallTrace.h"
#include "ui/ExtensionInstaller.h"
#include "ui/ExtensionManager.h"
#include "ui/ExtensionShortcuts.h"
#include "ui/ExtensionShortcutsWindow.h"
#include "ui/ExtensionUpdater.h"
#if SUMMIT_MODERN_WEBKIT
#include "ui/DownloadList.h"
#include "ui/DownloadListener.h"
#include "ui/McpService.h"
#include "ui/SitePermissions.h"
#include "ui/BookmarkImport.h"
#include "ui/UpdateChecker.h"
#include "ui/PageTranslation.h"
#include <Mime.h>
#include <nlohmann/json.hpp>
#endif
#include "core/Address.h"
#include "core/UserAgent.h"
#include "core/ProfileLock.h"
#include "core/Spelling.h"
#include <Alert.h>
#include <Application.h>
#include <Entry.h>
#include <File.h>
#include <Resources.h>
#include <Invoker.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Roster.h>
#include <Screen.h>
#include <OS.h>
#include <image.h>
#include <unistd.h>
#include <fcntl.h>
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

// The argument of a start after an update (UpdateChecker.h): reopen the last
// session's windows and tabs even when Preferences say to start afresh.
static constexpr const char* kRestoreSessionArgument = "--restore-session";
// A click on a notification an extension showed starts Summit with this, the
// extension, the notification, its title and its buttons' titles
// (BWebKitContext::ExtensionNotificationClicked).
static constexpr const char* kExtensionNotificationClickArgument = "--summit-extension-notification-click";
static constexpr uint32 kExtensionNotificationChoice = 'senc';
static constexpr uint32 kReleaseDownloadContexts = 'srdc';

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
        if (argc >= 4 && !std::strcmp(argv[1], kExtensionNotificationClickArgument)) {
            std::vector<std::string> buttons;
            for (int32 i = 5; i < std::min<int32>(argc, 7); ++i) buttons.push_back(argv[i]);
            ExtensionNotificationClicked(argv[2], argv[3], argc > 4 ? argv[4] : "", buttons);
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
#if SUMMIT_MODERN_WEBKIT
            if (argument == "--app" && i + 1 < argc) {
                if (!fWindows.empty()) OpenWebApp(argv[++i]);
                else fPendingWebApps.push_back(argv[++i]);
                continue;
            }
#endif
            if (argument == kRestoreSessionArgument) {
                if (fWindows.empty()) fRestoreAfterUpdate = true;
                continue;
            }
            if (!fWindows.empty()) OpenInNormalWindow(argument);
            else fURLs.push_back(argument);
        }
    }
    void RefsReceived(BMessage* message) override
    {
#if SUMMIT_MODERN_WEBKIT
        // A web app's launcher (Deskbar, Tracker) opens the app.
        {
            BMessage others(*message);
            others.RemoveName("refs");
            entry_ref ref;
            bool any = false;
            for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; ++i) {
                if (const auto app = summit::WebAppOfLauncher(ref); !app.empty() && !fQuitting) {
                    if (!fWindows.empty()) OpenWebApp(app);
                    else fPendingWebApps.push_back(app);
                } else {
                    others.AddRef("refs", &ref);
                    any = true;
                }
            }
            if (!any) return;
            *message = others;
        }
#endif
        if (fQuitting) {
            fRelaunch = true;
            entry_ref ref;
            for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; ++i)
                if (const auto url = summit::RefURL(ref); !url.empty()) fRelaunchArguments.push_back(url);
            return;
        }
        if (!fWindows.empty()) {
            if (FrontNormalWindow().IsValid()) FrontNormalWindow().SendMessage(message);
            else {
                entry_ref ref;
                for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; ++i)
                    if (const auto url = summit::RefURL(ref); !url.empty()) OpenInNormalWindow(url);
            }
        } else {
            entry_ref ref;
            for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; ++i)
                if (const auto url = summit::RefURL(ref); !url.empty()) fURLs.push_back(url);
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
        if (!LockProfile()) return;
        fShared = std::make_shared<summit::SharedProfile>(fProfile / "profile.json");
        fShared->AddListener(BMessenger(this));
#if SUMMIT_MODERN_WEBKIT
        // The debugging server (Preferences › Developer) starts before the
        // engine, so that its log has the engine processes' output.
        ApplyMcpSettings();
        if (!PrepareEmojiFont())
            std::fputs("Summit: could not prepare bundled emoji; using the installed fonts\n", stderr);
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
        initialization = ListenForDownloads(fWebKitContext, false);
        if (initialization != B_OK) {
            StartupError("Could not initialize downloads: " + std::string(std::strerror(initialization)));
            return;
        }
        // A failed download keeps its partial file for Retry to continue in;
        // the list of downloads is kept in the profile.
        fWebKitContext->SetKeepFailedDownloads(true);
        summit::DownloadList::Default().Open(fProfile / "downloads.json");
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
        ApplySpellChecking();
        // Firefox's containers: a context for each, before windows restore
        // their tabs in them.
        if (fWebKitContext->SetContainerListener(BMessenger(this)) == B_OK)
            summit::Containers::Shared().Sync(fWebKitContext);
#endif
        summit::SetSearchEngine(fShared->Read([](const summit::Profile& profile) { return profile.searchEngine; }));
        // Reopen every window of the last session unless Preferences say to
        // start afresh; pages named at the start open beside them (#94).
        const auto saved = fShared->SavedWindows();
        const bool restore = fRestoreAfterUpdate || fShared->Read([](const summit::Profile& profile) { return profile.restoreSession; });
        if (!fPendingWebApps.empty() && fURLs.empty() && !fRestoreAfterUpdate) {
            // Started from a web app's launcher: just the app.
            for (const auto& app : std::exchange(fPendingWebApps, {})) OpenWebApp(app);
            if (fWindows.empty()) {
                summit::BrowserWindowOptions options;
                OpenWindow(options);
            }
        } else if (saved.empty() || !restore) {
            summit::BrowserWindowOptions options;
            options.urls = fURLs;
            OpenWindow(options);
        } else {
            for (const auto& session : saved) {
                summit::BrowserWindowOptions options;
                options.session = session;
                OpenWindow(options);
            }
            // A link opened while Summit was not running (or asked for while
            // the old build quit) goes in a new tab of the window in front,
            // the last one restored; the session keeps its tabs.
            for (const auto& url : fURLs) OpenInNormalWindow(url);
        }
        // A damaged profile is said in the window, not in an alert that
        // holds every click until it is answered (#93).
        if (const auto notice = ProfileNotice(); !notice.empty()) {
            BMessage show(summit::kWindowNotice);
            show.AddString("text", notice.c_str());
            std::fprintf(stderr, "Summit: %s (%s)\n", notice.c_str(), fShared->LoadError().c_str());
            if (auto window = FrontNormalWindow(); window.IsValid()) window.SendMessage(&show);
            else if (auto front = FrontWindow(); front.IsValid()) front.SendMessage(&show);
        }
#if SUMMIT_MODERN_WEBKIT
        if (fPermissionPrompts) {
            fExtensions = std::make_unique<summit::ExtensionController>(fWebKitContext, fProfile / "Extensions",
                [this] { RefreshExtensions(); });
            AddHandler(fExtensions.get());
            fExtensions->Start();
            fInstaller = std::make_unique<summit::ExtensionInstaller>(fWebKitContext, fProfile / "Extensions",
                [this](summit::InstalledExtension entry, std::string baseURL, std::string error, bool installed, std::string newTabURL,
                    std::string sidebarURL, std::string sidebarTitle) {
                    fExtensions->AddLoaded(std::move(entry), std::move(baseURL), std::move(error), installed, std::move(newTabURL),
                        std::move(sidebarURL), std::move(sidebarTitle));
                }, [this] { RefreshExtensions(); });
            AddHandler(fInstaller.get());
            fInstaller->Start();
            // New versions from the extensions' stores (Extensions › Check for Updates).
            fExtensionUpdates = std::make_unique<summit::ExtensionUpdater>(fWebKitContext, fProfile / "Extensions", *fExtensions,
                [this] { RefreshExtensions(); });
            AddHandler(fExtensionUpdates.get());
            fExtensionUpdates->Start();
        }
        // File › Check for Updates… and the daily check (Preferences › General).
        fUpdates = std::make_unique<summit::UpdateChecker>(
            [shared = fShared] {
                return shared->Read([](const summit::Profile& profile) {
                    return summit::UpdateChecker::Settings { profile.checkForUpdates, profile.lastUpdateCheck };
                });
            },
            [shared = fShared](int64_t when) {
                shared->Change([when](summit::Profile& profile) -> uint32 {
                    profile.lastUpdateCheck = when;
                    return summit::SharedProfile::kUpdateCheckChanged;
                });
            },
            [this] {
                BRect frame;
                if (!fWindows.empty() && fWindows.front().messenger.LockTarget()) {
                    frame = fWindows.front().window->Frame();
                    fWindows.front().window->Unlock();
                }
                return frame;
            });
        AddHandler(fUpdates.get());
        fUpdates->Start();
#endif
    }
    void MessageReceived(BMessage* message) override
    {
#if SUMMIT_MODERN_WEBKIT
        if (summit::HandleTranslationContextMessage(message)) return;
        if (message->what == kReleaseDownloadContexts) {
            ReleaseRetiredPrivateContexts();
            return;
        }
#endif
        // Summit started again without arguments (its icon) while quitting.
        if (message->what == B_SILENT_RELAUNCH && fQuitting) {
            fRelaunch = true;
            return;
        }
#if SUMMIT_MODERN_WEBKIT
        if (message->what == kExtensionNotificationChoice) {
            // The last button is the notification itself.
            const int32 which = message->GetInt32("which", -1);
            const int32 buttons = message->GetInt32("buttons", 0);
            if (fWebKitContext && which >= 0 && which <= buttons)
                fWebKitContext->ExtensionNotificationClicked(message->GetString("extension", ""), message->GetString("notification", ""),
                    which < buttons ? which : -1);
            return;
        }
#if SUMMIT_MODERN_WEBKIT
        if (message->what == B_WEBKIT_CONTAINERS_CHANGED) {
            // An extension made, changed or removed a container: a context
            // for a new one, and the windows redraw their tabs (and close
            // those of a removed one).
            if (fWebKitContext && !fQuitting) summit::Containers::Shared().Sync(fWebKitContext);
            BMessage changed(summit::kContainersChanged);
            changed.AddString("event", message->GetString("event", ""));
            changed.AddUInt32("container", message->GetUInt32("container", 0));
            for (const auto& window : fWindows) window.messenger.SendMessage(&changed);
            return;
        }
#endif
        if (message->what == B_WEBKIT_EXTENSION_ACTIONS_CHANGED) {
            for (const auto& window : fWindows) window.messenger.SendMessage(message);
            // Loads, unloads and changed keys: the commands' shortcuts again.
            RefreshExtensionCommands();
            return;
        }
        if (message->what == B_WEBKIT_EXTENSION_COMMANDS) {
            ExtensionCommandsReceived(*message);
            return;
        }
        if (message->what == summit::kShowExtensionShortcuts || message->what == summit::kExtensionSetShortcut
            || message->what == summit::kExtensionShortcutsClosed || message->what == summit::kExtensionCheckUpdates
            || message->what == summit::kExtensionSetAutoUpdate || message->what == summit::kExtensionApproveUpdate) {
            ExtensionSettingsMessage(*message);
            return;
        }
        if (message->what == B_WEBKIT_BROWSER_COMMAND || message->what == B_WEBKIT_BROWSER_COMMAND_CANCELLED) {
            BrowserCommand(message);
            return;
        }
        if (message->what == summit::kShowExtensions) {
            // A store page's Manage Extension names the extension to show.
            ShowExtensions(message->GetString("extension_identifier", ""));
            return;
        }
        if (message->what == summit::kDownloadCancel || message->what == summit::kDownloadPause) {
            // From a downloads panel; the download is in one of the contexts.
            const auto entry = summit::DownloadList::Default().Find(message->GetUInt64("identifier", 0));
            if (!entry) return;
            if (!entry->engine) {
                // Paused: nothing downloads; cancelling removes its partial file.
                if (message->what == summit::kDownloadCancel) summit::DownloadList::Default().Discard(entry->identifier);
                return;
            }
            auto stop = [&](BWebKitContext& context) {
                if (message->what == summit::kDownloadCancel) context.CancelDownload(entry->engine);
                else context.PauseDownload(entry->engine);
            };
            if (fWebKitContext) stop(*fWebKitContext);
            if (fPrivateContext) stop(*fPrivateContext);
            for (const auto& context : fRetiredPrivateContexts) stop(*context);
            return;
        }
        if (message->what == summit::kDownloadResume) {
            BMessenger window;
            message->FindMessenger("window", &window);
            ResumeDownload(message->GetUInt64("identifier", 0), window);
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
                    if (message->what == summit::kExtensionRemove) {
                        // Nothing of it stays in the profile, its toolbar setting neither.
                        if (fExtensions->Remove(identifier) && fShared) {
                            const std::string id = identifier;
                            fShared->Change([&](summit::Profile& profile) -> uint32 {
                                return profile.unpinnedExtensions.erase(id) ? uint32(summit::SharedProfile::kExtensionsPinnedChanged) : 0;
                            });
                        }
                    }
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
            // Only a context's listener can identify the source and lifetime
            // of a download. Untagged application messages have neither.
            return;
        }
        if (message->what == summit::kDownloadQuitReply) {
            fDownloadPromptPending = false;
            if (message->GetInt32("which", 0) == 1) {
                fDownloadQuitApproved = true;
                PostMessage(B_QUIT_REQUESTED);
            } else if (std::exchange(fUpdateRestart, false) && fUpdates) fUpdates->RestartCancelled();
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
            // What was started meanwhile opens here after all; an update
            // starts the next time.
            fRelaunch = false;
            if (std::exchange(fUpdateRestart, false) && fUpdates) fUpdates->RestartCancelled();
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
            options.container = message->GetUInt32("container", 0);
            options.popup = options.newPage && message->GetBool("popup", false);
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
#if SUMMIT_MODERN_WEBKIT
        if (message->what == summit::kCheckForUpdates) {
            if (fUpdates && !fQuitting) fUpdates->Check(true);
            return;
        }
        if (message->what == summit::kUpdateRestart) {
            // As Quit does (pages may still ask about unsaved work), then
            // the new build starts with this profile's windows and tabs.
            if (fQuitting) return;
            fUpdateRestart = true;
            PostMessage(B_QUIT_REQUESTED);
            return;
        }
#endif
        if (message->what == summit::kOpenPageInTab) {
            if (const char* url = message->GetString("url", nullptr); url && *url && !fQuitting) OpenInNormalWindow(url);
            return;
        }
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
            const char* userAgent = nullptr;
            const bool hasUserAgent = message->FindString("user_agent", &userAgent) == B_OK && userAgent;
            bool mcpEnabled = false, mcpLocal = false;
            int32 mcpPort = 0;
            const bool hasMcpEnabled = message->FindBool("mcp_enabled", &mcpEnabled) == B_OK;
            const bool hasMcpLocal = message->FindBool("mcp_local_only", &mcpLocal) == B_OK;
            const bool hasMcpPort = message->FindInt32("mcp_port", &mcpPort) == B_OK && mcpPort > 0 && mcpPort < 65536;
            const bool newMcpToken = message->GetBool("mcp_new_token", false);
            const char* autoplay = nullptr;
            bool spelling = true, savePasswords = true;
            const bool hasAutoplay = message->FindString("autoplay", &autoplay) == B_OK && autoplay;
            const bool hasSpelling = message->FindBool("spell_checking", &spelling) == B_OK;
            const char* spellingLanguage = nullptr;
            const bool hasSpellingLanguage = message->FindString("spelling_language", &spellingLanguage) == B_OK && spellingLanguage
                && std::strlen(spellingLanguage) < 64;
            const bool hasSavePasswords = message->FindBool("save_passwords", &savePasswords) == B_OK;
            bool checkForUpdates = true;
            const bool hasCheckForUpdates = message->FindBool("check_for_updates", &checkForUpdates) == B_OK;
            // Preferences › Browsing › Accessibility (#118) and Memory (#116).
            std::vector<std::pair<const char*, bool summit::Profile::*>> flags {
                { "caret_browsing", &summit::Profile::caretBrowsing }, { "reduce_motion", &summit::Profile::reduceMotion },
                { "increase_contrast", &summit::Profile::increaseContrast }, { "unload_when_memory_is_low", &summit::Profile::unloadWhenMemoryIsLow },
                { "override_colors", &summit::Profile::overrideColors } };
            int32 minimumFontSize = 0;
            const bool hasMinimumFontSize = message->FindInt32("minimum_font_size", &minimumFontSize) == B_OK
                && minimumFontSize >= 0 && minimumFontSize <= 72;
            fShared->Change([&](summit::Profile& profile) -> uint32 {
                bool changed = false;
                for (const auto& [name, field] : flags) {
                    bool value = false;
                    if (message->FindBool(name, &value) == B_OK && profile.*field != value) {
                        profile.*field = value;
                        changed = true;
                    }
                }
                if (hasMinimumFontSize && profile.minimumFontSize != minimumFontSize) {
                    profile.minimumFontSize = minimumFontSize;
                    changed = true;
                }
                if (hasCheckForUpdates && profile.checkForUpdates != checkForUpdates) {
                    profile.checkForUpdates = checkForUpdates;
                    changed = true;
                }
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
                if (hasUserAgent && profile.userAgent != summit::FindUserAgentPreset(userAgent).id) {
                    profile.userAgent = summit::FindUserAgentPreset(userAgent).id;
                    changed = true;
                }
                if (hasAutoplay && profile.autoplay != autoplay && (!std::strcmp(autoplay, "allow")
                    || !std::strcmp(autoplay, "block-audio") || !std::strcmp(autoplay, "block"))) { profile.autoplay = autoplay; changed = true; }
                if (hasSpelling && profile.spellChecking != spelling) { profile.spellChecking = spelling; changed = true; }
                if (hasSpellingLanguage && profile.spellingLanguage != spellingLanguage) {
                    profile.spellingLanguage = spellingLanguage;
                    changed = true;
                }
                if (hasSavePasswords && profile.savePasswords != savePasswords) { profile.savePasswords = savePasswords; changed = true; }
                if (hasEngine && profile.searchEngine != engine) {
                    // Set before windows hear of the change, so they see the new engine.
                    summit::SetSearchEngine(engine);
                    profile.searchEngine = summit::CurrentSearchEngine().id;
                    changed = true;
                }
                return changed ? summit::SharedProfile::kSettingsChanged : 0;
            });
#if SUMMIT_MODERN_WEBKIT
            if (hasAutoplay) {
                const auto setting = fShared->Read([](const summit::Profile& profile) { return profile.autoplay; });
                for (const auto& context : {fWebKitContext, fPrivateContext})
                    if (context) context->SetSitePermission("autoplay", "*", AutoplayValue(setting));
            }
            if (hasSpelling || hasSpellingLanguage) ApplySpellChecking();
#endif
            return;
        }
#if SUMMIT_MODERN_WEBKIT
        if (message->what == summit::kTransferTab) {
            TransferTab(*message);
            return;
        }
        if (message->what == summit::kOpenWebApp) {
            OpenWebApp(message->GetString("id", ""));
            return;
        }
        if (message->what == summit::kRemoveWebApp) {
            const std::string id = message->GetString("id", "");
            std::optional<summit::WebApp> removed;
            fShared->Change([&](summit::Profile& profile) -> uint32 {
                auto found = std::find_if(profile.webApps.begin(), profile.webApps.end(), [&](const auto& app) { return app.id == id; });
                if (found == profile.webApps.end()) return 0;
                removed = *found;
                profile.webApps.erase(found);
                return summit::SharedProfile::kSettingsChanged;
            });
            if (removed) summit::RemoveWebAppFiles(*removed);
            return;
        }
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
            // A time range (Preferences › History and Data) forgets only its visits.
            if (const int64 since = message->GetInt64("since", 0); since > 0) fShared->ForgetHistorySince(since, BMessenger());
            else fShared->ClearHistory(BMessenger());
            BMessage cleared(summit::kDataCleared);
            cleared.AddInt32("kind", summit::kClearHistoryRequest);
            fPreferences.SendMessage(&cleared);
            return;
        }
#if SUMMIT_MODERN_WEBKIT
        if (message->what == summit::kAnswerPermissionPrompt) {
            const int32 which = message->GetInt32("which", 1), device = message->GetInt32("device_index", 0);
            const bool shareAudio = message->GetBool("share_audio", true);
            bool answered = false;
            for (auto* service : { fSitePermissions.get(), fPrivateSitePermissions.get() }) {
                if (!answered && service) answered = service->AnswerOpenPrompt(which, device, shareAudio);
            }
            BMessage reply(B_REPLY);
            reply.AddBool("answered", answered);
            message->SendReply(&reply);
            return;
        }
        if (message->what == summit::kSiteDataSizesRequest) {
            BMessenger reply;
            if (message->FindMessenger("reply", &reply) == B_OK && fWebKitContext)
                fWebKitContext->FetchWebsiteDataSizes(B_WEBKIT_DATA_COOKIES | B_WEBKIT_DATA_CACHE | B_WEBKIT_DATA_LOCAL_STORAGE
                    | B_WEBKIT_DATA_INDEXED_DB | B_WEBKIT_DATA_SERVICE_WORKERS, reply, 0);
            return;
        }
        if (message->what == summit::kClearCacheRequest || message->what == summit::kClearSiteDataRequest) {
            // Extensions keep their own storage; only websites' is removed.
            const uint32 types = message->what == summit::kClearCacheRequest ? B_WEBKIT_DATA_CACHE
                : B_WEBKIT_DATA_COOKIES | B_WEBKIT_DATA_LOCAL_STORAGE | B_WEBKIT_DATA_INDEXED_DB | B_WEBKIT_DATA_SERVICE_WORKERS;
            fDataRequests[++fNextDataRequest] = message->what;
            // Chosen sites of the Site data list: all they keep, caches too.
            std::vector<std::string> sites;
            const char* site = nullptr;
            for (int32 i = 0; message->FindString("site", i, &site) == B_OK; ++i) sites.push_back(site);
            if (!sites.empty()) {
                fWebKitContext->RemoveWebsiteDataOfSites(types | B_WEBKIT_DATA_CACHE, sites, BMessenger(this), fNextDataRequest);
                return;
            }
            // A time range: cookies and caches by their times, the storage of
            // the sites visited then.
            if (const int64 since = message->GetInt64("since", 0); since > 0) {
                const auto visited = fShared->HostsVisitedSince(since);
                fWebKitContext->RemoveWebsiteDataSince(types, since, std::vector<std::string>(visited.begin(), visited.end()),
                    BMessenger(this), fNextDataRequest);
            } else
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
            if (message->GetUInt32("changes", 0) & (summit::SharedProfile::kSettingsChanged | summit::SharedProfile::kHistoryChanged
                    | summit::SharedProfile::kUpdateCheckChanged))
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
#if SUMMIT_MODERN_WEBKIT
        if (message->what == summit::kOpenSidebarOnApp) {
            // The extension's view is made on WebKit's application thread, with
            // the window locked, as an extension tab's is.
            BMessenger target;
            if (message->FindMessenger("window", &target) != B_OK || !target.LockTarget()) return;
            BLooper* looper = nullptr;
            if (auto* window = dynamic_cast<summit::BrowserWindow*>(target.Target(&looper)))
                window->ShowSidebar(message->GetString("identifier", ""), message->GetString("url", ""), message->GetString("title", ""));
            if (looper) looper->Unlock();
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
            std::string lost = fDownloadQuitApproved ? std::string() : DownloadsLostOnQuit();
            if (!lost.empty()) {
                // Downloads belong to the application, not to one window.
                fDownloadPromptPending = true;
                auto* prompt = new BAlert("Downloads", lost.c_str(), "Keep Browsing", "Quit");
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
            if (fShortcutsWindow.IsValid()) fShortcutsWindow.SendMessage(B_QUIT_REQUESTED);
            if (fInstaller) fInstaller->Shutdown();
            if (fExtensionUpdates) fExtensionUpdates->Shutdown();
            if (fExtensions) fExtensions->Shutdown();
            if (fExtensionWindows->load() || (fInstaller && fInstaller->HasPendingWork())
                || (fExtensionUpdates && fExtensionUpdates->HasPendingWork())
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
            // After the controller, which may still report to it.
            if (fExtensionUpdates) {
                RemoveHandler(fExtensionUpdates.get());
                fExtensionUpdates.reset();
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
                    // Paused, they are listed to be resumed the next time.
                    fCancellingDownloads = true;
                    fWebKitContext->PauseAllDownloads();
                }
                fWaitingForNativeUI = true;
                SetPulseRate(100000);
                return false;
            }
            SUMMIT_QUIT_TRACE("releasing the engine");
            fWebKitContext->SetContainerListener({});
            summit::Containers::Shared().Sync(nullptr);
            fWebKitContext.reset();
            SUMMIT_QUIT_TRACE("engine released");
            PostMessage(B_QUIT_REQUESTED);
            return false;
        }
        // load_image() workers still need to resume or cancel their helpers
        // and deliver completion on this loop after the context is released.
        if (BWebKitHasPendingNativeUI() || BWebKitHasPendingProcessLaunches()) {
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
#if SUMMIT_MODERN_WEBKIT
        for (const auto& item : fDownloadListeners) RemoveHandler(item.second.get());
        fDownloadListeners.clear();
#endif
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
    // Held until the profile's last save, after the application is gone.
    std::unique_ptr<summit::ProfileLock> TakeProfileLock() { return std::move(fProfileLock); }
    // Whether Summit was started again while it quit, and with what.
    bool TakeRelaunch(std::vector<std::string>& arguments)
    {
        arguments = std::exchange(fRelaunchArguments, { });
#if SUMMIT_MODERN_WEBKIT
        if (std::exchange(fUpdateRestart, false)) {
            // The new build: this profile, with its windows and tabs.
            arguments.insert(arguments.begin(), {"--profile", fProfile.string(), kRestoreSessionArgument});
            fRelaunch = false;
            return true;
        }
#endif
        return std::exchange(fRelaunch, false);
    }
private:
    struct WindowRecord {
        BMessenger messenger;
        summit::BrowserWindow* window;
        uint64 key;
        bool privateBrowsing = false;
        // A web app's window (#75), which links from elsewhere never open in.
        std::string webApp;
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
        for (const auto& window : fWindows) if (!window.privateBrowsing && window.webApp.empty()) return window.messenger;
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
    status_t ListenForDownloads(const std::shared_ptr<BWebKitContext>& context, bool privateBrowsing)
    {
        auto listener = std::make_unique<summit::DownloadListener>(privateBrowsing, context,
            [this](const BMessage& message, bool isPrivate, bool active) { DownloadReceived(message, isPrivate, active); });
        AddHandler(listener.get());
        const auto status = context->SetDownloadListener(BMessenger(listener.get()));
        if (status != B_OK) { RemoveHandler(listener.get()); return status; }
        fDownloadListeners.emplace(context.get(), std::move(listener));
        return B_OK;
    }
    void DownloadReceived(const BMessage& message, bool privateBrowsing, bool active)
    {
        // Cleanup runs on a later application turn, after this listener's
        // callback has returned. A retired private context cannot repopulate
        // either the public profile or a newer private session.
        if (message.what == B_WEBKIT_DOWNLOAD_FINISHED) PostMessage(kReleaseDownloadContexts);
        if (!active) return;
        const uint64 identifier = message.GetUInt64("identifier", 0);
        if (!identifier) return;
        BMessenger target;
        if (auto found = fDownloads.find(identifier); found != fDownloads.end()) target = found->second;
        if (!target.IsValid()) {
            BMessenger view;
            if (message.FindMessenger("view", &view) == B_OK) target = WindowOf(view);
        }
        // A view may already have closed, or a background extension may have
        // started this download. Choose a window only in its own context.
        if (std::none_of(fWindows.begin(), fWindows.end(), [&](const WindowRecord& window) {
                return window.messenger == target && window.privateBrowsing == privateBrowsing;
            })) {
            target = BMessenger();
            for (const auto& window : fWindows) if (window.privateBrowsing == privateBrowsing) {
                target = window.messenger;
                break;
            }
        }
        if (auto change = summit::DownloadList::Default().Update(message, privateBrowsing); change && fShared) {
            fShared->Change([&](summit::Profile& profile) -> uint32 {
                const bool changed = change->newVisit
                    ? profile.VisitDownload({change->url, change->title}, change->started)
                    : profile.SetDownloadTitle(change->url, change->started, change->title);
                return changed ? uint32(summit::SharedProfile::kHistoryChanged) : 0;
            }, BMessenger(), false);
            fShared->SaveSoon();
        }
        if (message.what == B_WEBKIT_DOWNLOAD_FINISHED
            && message.GetUInt32("result", B_WEBKIT_DOWNLOAD_FAILED) == B_WEBKIT_DOWNLOAD_SUCCEEDED)
            if (const char* path = message.GetString("path", nullptr); path && *path)
                update_mime_info(path, false, false, B_UPDATE_MIME_INFO_NO_FORCE);
        if (message.what == B_WEBKIT_DOWNLOAD_FINISHED) fDownloads.erase(identifier);
        else fDownloads[identifier] = target;
        if (target.IsValid()) {
            BMessage forwarded(message);
            target.SendMessage(&forwarded);
        }
    }
    // One private context serves every private window, as one private session
    // does in other browsers. It lives until the last private window closes.
    std::shared_ptr<BWebKitContext> PrivateContext()
    {
        if (fPrivateContext) return fPrivateContext;
        // Its pages are served by the normal profile's extensions: those
        // allowed in private windows work there with their own saved data.
        auto context = std::make_shared<BWebKitContext>(fWebKitContext);
        if (context->InitCheck() != B_OK || ListenForDownloads(context, true) != B_OK) return nullptr;
        context->SetKeepFailedDownloads(true);
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
    // Where the engine looks for dictionaries (TextCheckerHaiku): the
    // profile's Dictionaries folder, then data/hunspell and data/myspell.
    std::vector<std::string> SpellingDirectories() const
    {
        std::vector<std::string> result{(fProfile / "Dictionaries").string()};
        for (auto which : {B_USER_NONPACKAGED_DATA_DIRECTORY, B_USER_DATA_DIRECTORY, B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
                B_SYSTEM_DATA_DIRECTORY}) {
            BPath path;
            if (find_directory(which, &path) != B_OK) continue;
            result.push_back(std::string(path.Path()) + "/hunspell");
            result.push_back(std::string(path.Path()) + "/myspell");
        }
        return result;
    }
    // The system's preferred languages ("de", "en_AU"), from the Locale
    // preferences' settings (BLocaleRoster is in liblocale, which Summit
    // does not link).
    static std::vector<std::string> SystemLanguages()
    {
        std::vector<std::string> result;
        BPath path;
        BFile file;
        BMessage settings;
        if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK && path.Append("Locale settings") == B_OK
            && file.SetTo(path.Path(), B_READ_ONLY) == B_OK && settings.Unflatten(&file) == B_OK) {
            const char* language = nullptr;
            for (int32 i = 0; settings.FindString("language", i, &language) == B_OK && language; ++i)
                result.push_back(language);
        }
        return result;
    }
    // Keep fontconfig's system/user configuration and add Summit's own color
    // font. FONTCONFIG_FILE is inherited by WebProcess, before Skia creates
    // its font manager. No system font installation or configuration changes.
    bool PrepareEmojiFont()
    {
        app_info info;
        if (GetAppInfo(&info) != B_OK) return false;
        BFile executable(&info.ref, B_READ_ONLY);
        BResources resources(&executable);
        const auto directory = fProfile / "Fonts";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return false;
        auto store = [](const std::filesystem::path& path, const void* data, size_t size) {
            if (!data || !size) return false;
            BFile existing(path.c_str(), B_READ_ONLY);
            off_t length = 0;
            bool matches = existing.GetSize(&length) == B_OK && length == static_cast<off_t>(size);
            std::vector<char> chunk(65536);
            for (size_t offset = 0; matches && offset < size;) {
                size_t count = std::min(chunk.size(), size - offset);
                matches = existing.Read(chunk.data(), count) == static_cast<ssize_t>(count)
                    && !std::memcmp(chunk.data(), static_cast<const char*>(data) + offset, count);
                offset += count;
            }
            if (matches) return true;
            const auto temporary = path.string() + ".tmp-" + std::to_string(getpid());
            BFile output(temporary.c_str(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
            bool written = output.InitCheck() == B_OK
                && output.Write(data, size) == static_cast<ssize_t>(size) && output.Sync() == B_OK;
            output.Unset();
            if (written && !std::rename(temporary.c_str(), path.c_str())) return true;
            std::remove(temporary.c_str());
            return false;
        };
        for (const auto& [name, file] : { std::pair { "emoji-font", "Noto-COLRv1.ttf" },
                std::pair { "emoji-license", "OFL.txt" }, std::pair { "emoji-text-font", "NotoEmoji.ttf" },
                std::pair { "emoji-text-license", "NotoEmoji-OFL.txt" } }) {
            size_t size = 0;
            const void* data = resources.LoadResource('RAWT', name, &size);
            if (!store(directory / file, data, size)) return false;
        }
        BPath system;
        if (find_directory(B_SYSTEM_SETTINGS_DIRECTORY, &system) != B_OK) return false;
        // A web app launched by Summit inherits this environment too. Remember
        // the original config so its new profile does not include an old
        // profile's generated file (or recursively include itself).
        const char* inherited = std::getenv("SUMMIT_BASE_FONTCONFIG_FILE");
        if (!inherited) inherited = std::getenv("FONTCONFIG_FILE");
        const std::string base = inherited && *inherited ? inherited
            : std::string(system.Path()) + "/fonts/fonts.conf";
        auto xml = [](const std::string& value) {
            std::string escaped;
            for (char c : value) {
                if (c == '&') escaped += "&amp;";
                else if (c == '<') escaped += "&lt;";
                else if (c == '>') escaped += "&gt;";
                else escaped += c;
            }
            return escaped;
        };
        const std::string configuration = "<?xml version=\"1.0\"?>\n"
            "<!DOCTYPE fontconfig SYSTEM \"urn:fontconfig:fonts.dtd\">\n<fontconfig>\n"
            "<include ignore_missing=\"yes\">" + xml(base) + "</include>\n"
            "<dir>" + xml(directory.string()) + "</dir>\n"
            "<alias><family>emoji</family><prefer><family>Noto Color Emoji</family></prefer></alias>\n"
            "</fontconfig>\n";
        const auto configPath = directory / "fonts.conf";
        if (!store(configPath, configuration.data(), configuration.size())) return false;
        return !setenv("SUMMIT_BASE_FONTCONFIG_FILE", base.c_str(), 1)
            && !setenv("FONTCONFIG_FILE", configPath.c_str(), 1);
    }
    // Spell checking (#66) with the en_US dictionary Summit carries, copied
    // to the profile's Dictionaries folder, or one installed in data/hunspell:
    // the one chosen in Preferences, else each field/page's language, with
    // the system language as fallback (#110).
    // SUMMIT_SPELLING_LANGUAGES names others first ("de_DE,en_US").
    void ApplySpellChecking()
    {
        const bool enabled = fShared->Read([](const summit::Profile& profile) { return profile.spellChecking; });
        const auto directory = fProfile / "Dictionaries";
        std::error_code error;
        if (enabled) {
            std::filesystem::create_directories(directory, error);
            app_info info;
            if (be_app->GetAppInfo(&info) == B_OK) {
                BFile file(&info.ref, B_READ_ONLY);
                BResources resources(&file);
                for (const char* name : { "en_US.aff", "en_US.dic" }) {
                    size_t size = 0;
                    const void* data = resources.LoadResource('RAWT', ("dictionary-" + std::string(name)).c_str(), &size);
                    const auto target = directory / name;
                    if (!data || !size || (std::filesystem::exists(target, error) && std::filesystem::file_size(target, error) == size)) continue;
                    BFile out(target.c_str(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
                    if (out.InitCheck() == B_OK) out.Write(data, size);
                }
            }
        }
        const char* override = std::getenv("SUMMIT_SPELLING_LANGUAGES");
        const auto chosen = fShared->Read([](const summit::Profile& profile) { return profile.spellingLanguage; });
        const std::string languages = override ? std::string(override)
            : summit::SpellingLanguages(chosen, SystemLanguages(), summit::SpellingDictionaries(SpellingDirectories()));
        BWebKitSetSpellChecking(enabled, languages.c_str(), directory.c_str(), (directory / "words.txt").c_str(), chosen.empty() && !override);
    }
    // The engine's value of a saved decision: for autoplay, allowed means
    // media play by themselves (0) and blocked that they wait for the user
    // (2); the others are allowed (1) or blocked (0).
    static int32 SitePermissionValue(const std::string& permission, bool allowed)
    {
        if (permission == "autoplay") return allowed ? 0 : 2;
        return allowed ? 1 : 0;
    }
    // Media that start by themselves, for sites without their own choice.
    static int32 AutoplayValue(const std::string& setting)
    {
        return setting == "allow" ? 0 : setting == "block" ? 2 : 1;
    }
    void GiveSitePermissions(BWebKitContext& context)
    {
        if (!fShared) return;
        fShared->Read([&](const summit::Profile& profile) {
            for (const auto& [permission, origins] : profile.sitePermissions)
                for (const auto& [origin, allowed] : origins)
                    context.SetSitePermission(permission.c_str(), origin.c_str(), SitePermissionValue(permission, allowed));
            context.SetSitePermission("autoplay", "*", AutoplayValue(profile.autoplay));
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
            if (context) context->SetSitePermission(permission.c_str(), origin.c_str(), allowed ? SitePermissionValue(permission, *allowed) : -1);
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
        if (fPrivateContext) fDownloadListeners.at(fPrivateContext.get())->Deactivate();
        for (const auto& entry : summit::DownloadList::Default().Entries(true)) fDownloads.erase(entry.engine);
        summit::DownloadList::Default().ForgetPrivate();
        if (!fPrivateContext) return;
        // Its cookies, cache and storage are only in memory: releasing the
        // context forgets them. Downloads still running are cancelled first.
        if (fPrivateContext->HasPendingDownloads()) {
            fPrivateContext->CancelAllDownloads();
        }
        fRetiredPrivateContexts.push_back(std::move(fPrivateContext));
        ReleaseRetiredPrivateContexts();
#endif
    }
    // What quitting would cost the downloads in progress, to ask about it
    // first; empty when nothing (they are paused and resume next time).
    std::string DownloadsLostOnQuit() const
    {
        size_t restarting = 0, cancelled = 0;
        for (bool privateBrowsing : {false, true})
            for (const auto& entry : summit::DownloadList::Default().Entries(privateBrowsing)) {
                if (entry.state != summit::DownloadStatus::State::Active) continue;
                if (privateBrowsing) ++cancelled;
                else if (entry.received && !entry.acceptsRanges) ++restarting;
            }
        if (!restarting && !cancelled) return {};
        std::string text;
        if (cancelled)
            text = cancelled == 1 ? "A download of a private window is still in progress: quitting cancels it."
                : "Downloads of private windows are still in progress: quitting cancels them.";
        if (restarting) {
            if (!text.empty()) text += " ";
            text += restarting == 1
                ? "A download is still in progress whose server cannot resume it: quitting pauses it, but it will have to start over."
                : std::to_string(restarting) + " downloads are still in progress whose servers cannot resume them: quitting "
                    "pauses them, but they will have to start over.";
        }
        return text + "\n\nQuit anyway?";
    }
    // Continues a paused or failed download of the list in its partial file
    // (or afresh), in the context it belongs to.
    void ResumeDownload(uint64 identifier, const BMessenger& window)
    {
        const auto entry = summit::DownloadList::Default().Find(identifier);
        if (!entry || entry->state == summit::DownloadStatus::State::Active) return;
        BWebKitContext* context = entry->privateBrowsing ? fPrivateContext.get() : fWebKitContext.get();
        if (!context) {
            summit::DownloadList::Default().ResumeFailed(identifier, "Its private session has ended");
            return;
        }
        const std::string& url = entry->requestURL.empty() ? entry->url : entry->requestURL;
        uint64 engine = 0;
        const status_t status = context->ResumeDownload(url.c_str(), entry->partial ? entry->path.c_str() : "",
            entry->validator.c_str(), entry->expected, window.IsValid() ? window : FrontWindow(), &engine);
        if (status != B_OK) {
            summit::DownloadList::Default().ResumeFailed(identifier, "Could not resume: " + std::string(std::strerror(status)));
            return;
        }
        summit::DownloadList::Default().Resumed(identifier, engine);
    }
    void ReleaseRetiredPrivateContexts()
    {
#if SUMMIT_MODERN_WEBKIT
        std::erase_if(fRetiredPrivateContexts, [this](const std::shared_ptr<BWebKitContext>& context) {
            if (context->HasPendingDownloads()) return false;
            if (auto found = fDownloadListeners.find(context.get()); found != fDownloadListeners.end()) {
                RemoveHandler(found->second.get());
                fDownloadListeners.erase(found);
            }
            return true;
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
#if SUMMIT_MODERN_WEBKIT
    // Brings a web app's window forward, or opens it at its start address.
    void OpenWebApp(const std::string& id)
    {
        for (const auto& record : fWindows) {
            if (record.webApp != id) continue;
            if (record.window->Lock()) {
                record.window->Activate();
                record.window->Unlock();
            }
            return;
        }
        const auto start = fShared->Read([&](const summit::Profile& profile) {
            const auto* app = profile.FindWebApp(id);
            return app ? app->startURL : std::string();
        });
        if (start.empty()) {
            (new BAlert("Summit", "This web app is no longer installed. Remove its launcher, or install the site again "
                "with File › Install as App….", "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT))->Go(nullptr);
            return;
        }
        summit::BrowserWindowOptions options;
        options.webApp = id;
        options.urls.push_back(start);
        options.frame = CascadedFrame();
        OpenWindow(options);
    }
    // A tab dragged out of its window (#68): to the browser window under the
    // drop point (of the same kind, private or not), or to a new window there.
    void TransferTab(const BMessage& message)
    {
        BMessenger source;
        if (message.FindMessenger("window", &source) != B_OK) return;
        const int64 id = message.GetInt64("id", 0);
        const bool privateTab = message.GetBool("private", false);
        BPoint where;
        BMessenger target;
        if (message.FindMessenger("target", &target) != B_OK && message.FindPoint("where", &where) == B_OK) {
            for (const auto& record : fWindows) {
                // Not a web app's window, which shows only its own pages.
                if (record.messenger == source || record.privateBrowsing != privateTab || !record.webApp.empty()) continue;
                if (!record.window->LockWithTimeout(200000)) continue;
                const bool under = record.window->Frame().Contains(where) && !record.window->IsHidden();
                record.window->Unlock();
                if (under) { target = record.messenger; break; }
            }
        }
        if (!target.IsValid()) {
            // A window's only tab dropped outside every window stays put.
            if (message.GetBool("last", false)) return;
            summit::BrowserWindowOptions options;
            options.empty = true;
            options.privateBrowsing = privateTab;
            BRect frame = CascadedFrame();
            if (message.FindPoint("where", &where) == B_OK) frame.OffsetTo(where - BPoint(80, 12));
            // On the screen, title bar and all.
            const BRect screen = BScreen().Frame();
            frame.OffsetTo(std::clamp(frame.left, screen.left + 5, std::max(screen.left + 5, screen.right - frame.Width() - 5)),
                std::clamp(frame.top, screen.top + 30, std::max(screen.top + 30, screen.bottom - frame.Height() - 5)));
            options.frame = frame;
            if (auto* window = OpenWindow(options)) target = BMessenger(window);
        }
        if (!target.IsValid()) return;
        BMessage detach(summit::kDetachTab);
        detach.AddInt64("id", id);
        detach.AddMessenger("target", target);
        source.SendMessage(&detach);
    }
#endif
#if SUMMIT_MODERN_WEBKIT
    // Haiku's notifications have no buttons of their own: a click on one that
    // has buttons offers them, and the notification itself.
    void ExtensionNotificationClicked(const std::string& extension, const std::string& notification, const std::string& title,
        const std::vector<std::string>& buttons)
    {
        if (!fWebKitContext) return;
        if (buttons.empty()) {
            fWebKitContext->ExtensionNotificationClicked(extension.c_str(), notification.c_str());
            return;
        }
        BMessage choice(kExtensionNotificationChoice);
        choice.AddString("extension", extension.c_str());
        choice.AddString("notification", notification.c_str());
        choice.AddInt32("buttons", static_cast<int32>(buttons.size()));
        // Named for its extension: anything can start Summit with these arguments.
        std::string name = extension;
        if (fExtensions)
            for (const auto& entry : fExtensions->Entries())
                if (entry.installation.identifier == extension) name = entry.installation.name;
        const std::string text = summit::ExtensionDisplayText(name) + "\n\n"
            + (title.empty() ? std::string("Its notification offers:") : summit::ExtensionDisplayText(title));
        const std::string first = summit::ExtensionDisplayText(buttons[0]),
            second = buttons.size() > 1 ? summit::ExtensionDisplayText(buttons[1]) : std::string();
        auto* alert = new BAlert("Notification", text.c_str(), first.c_str(),
            buttons.size() > 1 ? second.c_str() : "Open", buttons.size() > 1 ? "Open" : nullptr,
            B_WIDTH_AS_USUAL, B_INFO_ALERT);
        alert->Go(new BInvoker(new BMessage(choice), this));
    }
#endif
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
        fWindows.insert(fWindows.begin(), {BMessenger(window), window, options.key, options.privateBrowsing, options.webApp});
        window->Show();
#if SUMMIT_MODERN_WEBKIT
        // The daemon thread of a new direct window clones the frame buffer over
        // the application's app_server link before its first DirectConnected(),
        // while app_server waits for that, half a second at most. This thread
        // held the link creating the next window, so with several windows to
        // restore one of them lost its daemon at every launch and its pages
        // drew through app_server for good (#97). The next window waits.
        window->WaitForDirectConnection(300000);
#endif
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
                profile.restoreSession, profile.mcpEnabled, profile.mcpPort, profile.mcpLocalOnly, profile.checkForUpdates,
                profile.lastUpdateCheck, profile.autoplay, profile.spellChecking, profile.savePasswords };
        });
        state.webKitDirectory = fProfile / "WebKit";
        state.userAgent = fShared->Read([](const summit::Profile& profile) { return profile.userAgent; });
        state.spellingLanguage = fShared->Read([](const summit::Profile& profile) { return profile.spellingLanguage; });
        state.aids = fShared->Read([](const summit::Profile& profile) {
            return summit::BrowsingAids { profile.caretBrowsing, profile.reduceMotion, profile.increaseContrast,
                profile.minimumFontSize, profile.unloadWhenMemoryIsLow, profile.overrideColors };
        });
        state.dictionaries = summit::SpellingDictionaries(SpellingDirectories());
        fShared->Read([&](const summit::Profile& profile) {
            for (const auto& app : profile.webApps) state.webApps.emplace_back(app.id, app.name, app.startURL);
            return 0;
        });
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
            state.AddBool("check_for_updates", profile.checkForUpdates);
            state.AddInt64("last_update_check", profile.lastUpdateCheck);
            state.AddBool("caret_browsing", profile.caretBrowsing);
            state.AddBool("reduce_motion", profile.reduceMotion);
            state.AddBool("increase_contrast", profile.increaseContrast);
            state.AddInt32("minimum_font_size", profile.minimumFontSize);
            state.AddBool("unload_when_memory_is_low", profile.unloadWhenMemoryIsLow);
            state.AddBool("override_colors", profile.overrideColors);
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
        if (action == "downloads") {
            // The downloads panel's list and its buttons, for test scripts.
            const std::string what = request.value("do", "list");
            const uint64 identifier = request.value("id", uint64(0));
            auto& downloads = summit::DownloadList::Default();
            const auto entry = downloads.Find(identifier);
            if (what == "start") {
                const std::string url = request.value("url", "");
                const auto window = FrontNormalWindow();
                if (url.empty() || !window.IsValid()) { summit::McpReplyError(message, "start needs a url and a normal browser window"); return; }
                BMessage start(summit::kDownloadRetry);
                start.AddString("url", url.c_str());
                window.SendMessage(&start);
            } else if (what == "clear") downloads.ClearFinished(false);
            else if (what != "list") {
                if (!entry) { summit::McpReplyError(message, "no download " + std::to_string(identifier) + " (ids are in list)"); return; }
                if (what == "pause" || what == "cancel") {
                    BMessage stop(what == "pause" ? summit::kDownloadPause : summit::kDownloadCancel);
                    stop.AddUInt64("identifier", identifier);
                    PostMessage(&stop);
                } else if (what == "resume" || (what == "retry" && entry->CanContinue())) ResumeDownload(identifier, FrontWindow());
                else if (what == "retry") {
                    const auto window = FrontNormalWindow();
                    if (!window.IsValid()) { summit::McpReplyError(message, "retry needs a normal browser window"); return; }
                    BMessage retry(summit::kDownloadRetry);
                    retry.AddString("url", (entry->requestURL.empty() ? entry->url : entry->requestURL).c_str());
                    window.SendMessage(&retry);
                    downloads.Remove(identifier);
                } else if (what == "remove") downloads.Remove(identifier);
                else { summit::McpReplyError(message, "unknown do: " + what); return; }
            }
            json list = json::array();
            for (bool privateBrowsing : {false, true})
                for (const auto& item : downloads.Entries(privateBrowsing)) {
                    static const char* const states[] = {"active", "paused", "succeeded", "failed", "cancelled"};
                    json record = {{"id", item.identifier}, {"engine", item.engine}, {"state", states[int(item.state)]},
                        {"url", item.url}, {"requestURL", item.requestURL}, {"path", item.path}, {"filename", item.filename},
                        {"received", item.received}, {"expected", item.expected}, {"partial", item.partial},
                        {"interrupted", item.interrupted}, {"restarted", item.restarted}, {"validator", item.validator},
                        {"acceptsRanges", item.acceptsRanges}, {"canResume", item.CanResume()}, {"private", privateBrowsing},
                        {"status", summit::DownloadStatusText(item.Status())}};
                    if (!item.error.empty()) record["error"] = item.error;
                    list.push_back(std::move(record));
                }
            summit::McpReply(message, {{"downloads", list}});
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
        if (action == "bookmarks") {
            // Imports and exports as the File menu does, for test scripts.
            const std::string what = request.value("do", "list");
            const std::string path = request.value("path", "");
            if (what == "import") {
                const auto outcome = summit::ImportBookmarksFrom(*fShared, path, request.value("history", true));
                if (!outcome.ok) { summit::McpReplyError(message, outcome.message); return; }
                summit::McpReply(message, {{"message", outcome.message}, {"bookmarks", outcome.bookmarks},
                    {"history", outcome.history}, {"folder", outcome.folder}});
                return;
            }
            if (what == "export") {
                std::string error;
                const size_t count = summit::ExportBookmarksFile(*fShared, path.c_str(), error);
                if (!error.empty()) { summit::McpReplyError(message, error); return; }
                summit::McpReply(message, {{"exported", count}, {"path", path}});
                return;
            }
            std::function<json(const summit::BookmarkNode&)> node = [&](const summit::BookmarkNode& item) {
                json result = {{"id", item.id}, {"title", item.title}};
                if (!item.metadata.description.empty()) result["description"] = item.metadata.description;
                if (!item.metadata.tags.empty()) result["tags"] = item.metadata.tags;
                if (!item.metadata.keywords.empty()) {
                    result["keywords"] = json::array();
                    for (const auto& keyword : item.metadata.keywords)
                        result["keywords"].push_back({{"name", keyword.name}, {"postData", keyword.postData}, {"charset", keyword.charset}});
                }
                if (item.separator) result["separator"] = true;
                else if (!item.folder) result["url"] = item.url;
                else {
                    result["children"] = json::array();
                    for (const auto& child : item.children) result["children"].push_back(node(child));
                }
                return result;
            };
            summit::McpReply(message, fShared->Read([&](const summit::Profile& profile) {
                return json{{"bookmarks", profile.bookmarks.Count()}, {"folders", profile.bookmarks.FolderCount()},
                    {"separators", profile.bookmarks.SeparatorCount()},
                    {"bar", node(profile.bookmarks.Bar())}, {"other", node(profile.bookmarks.Other())}};
            }));
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
                    {"bookmarks", profile.bookmarks.Count()}, {"history", profile.history.size()}, {"siteZoom", profile.siteZoom},
                    {"sitePermissions", sites}, {"protocolHandlers", handlers}, {"trustedCertificates", certificates},
                    {"mcp", {{"enabled", profile.mcpEnabled}, {"port", profile.mcpPort}, {"localOnly", profile.mcpLocalOnly}}},
                    {"updates", {{"dailyCheck", profile.checkForUpdates}, {"lastCheck", profile.lastUpdateCheck}}}};
            });
            summit::McpReply(message, preferences);
            return;
        }
        if (action == "updates") {
            if (!fUpdates) { summit::McpReplyError(message, "the update checker is not running"); return; }
            const std::string what = request.value("do", "status");
            if (what == "check") fUpdates->Check(true);
            else if (what == "install" || what == "later" || what == "changes") {
                if (!fUpdates->Answer(what)) { summit::McpReplyError(message, "no update window is open"); return; }
            } else if (what != "status") { summit::McpReplyError(message, "unknown updates request \"" + what + "\""); return; }
            summit::McpReply(message, fUpdates->State());
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
        if (command == B_WEBKIT_BROWSER_DOWNLOADS_UI) {
            // downloads.setUiOptions: the whole browser's, no window needed.
            if (fShared) fShared->SetDownloadUIHidden(message->GetString("extension_identifier", ""), !message->GetBool("enabled", true));
            if (fWebKitContext && identifier) fWebKitContext->RespondToBrowserCommandWithData(identifier, B_OK, "null");
            return;
        }
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
    // identifier: an extension to select in the window.
    void ShowExtensions(const std::string& identifier = std::string())
    {
        if (fWindows.empty() || !fExtensions || !fInstaller || fQuitting) return;
        BMessage show(summit::kShowExtensions);
        if (!identifier.empty()) show.AddString("extension_identifier", identifier.c_str());
        if (!fExtensionWindow.IsValid()) {
            auto* manager = new summit::ExtensionManager(BMessenger(this), fExtensionWindows);
            fExtensionWindow = BMessenger(manager);
            manager->Show();
        }
        fExtensionWindow.SendMessage(&show);
        RefreshExtensions();
    }
    void RefreshExtensionCommands()
    {
        // Badges change often (CHANGED each time): one request at a time.
        if (fCommandsInFlight) {
            fCommandsAgain = true;
            return;
        }
        if (fWebKitContext && fExtensions && !fQuitting)
            fCommandsInFlight = fWebKitContext->GetExtensionCommands(BMessenger(this), ++fCommandsRequest) == B_OK;
    }
    void ExtensionCommandsReceived(const BMessage& message)
    {
        const uint64 identifier = message.GetUInt64("identifier", 0);
        if (identifier && identifier == fShortcutRequest) {
            // The answer to a change made in the Keyboard Shortcuts window.
            if (const status_t error = message.GetInt32("error", B_OK); error != B_OK && fShortcutsWindow.IsValid()) {
                BMessage state(summit::kExtensionShortcutsState);
                const char* reason = message.GetString("description", nullptr);
                state.AddString("status", (std::string("The keys were not changed: ") + (reason ? reason : std::strerror(error))).c_str());
                fShortcutsWindow.SendMessage(&state);
            }
            RefreshExtensionCommands();
            return;
        }
        if (identifier != fCommandsRequest) return;
        fCommandsInFlight = false;
        if (std::exchange(fCommandsAgain, false)) RefreshExtensionCommands();
        if (message.GetInt32("error", B_ERROR) != B_OK || !fExtensions) return;
        std::vector<summit::ExtensionCommandBinding> bindings;
        BMessage command;
        for (int32 i = 0; i < 1024 && message.FindMessage("command", i, &command) == B_OK; ++i)
            bindings.push_back({ command.GetString("extension_identifier", ""), command.GetString("extension_name", ""),
                command.GetString("name", ""), command.GetString("description", ""), command.GetString("shortcut", ""),
                command.GetString("suggested_shortcut", ""), {}, {} });
        // The earlier installation keeps keys a later one asks for too.
        std::map<std::string, uint64> order;
        for (const auto& entry : fExtensions->Entries()) order[entry.installation.identifier] = entry.installation.installationOrder;
        std::stable_sort(bindings.begin(), bindings.end(), [&](const auto& a, const auto& b) {
            return order[a.extension] < order[b.extension];
        });
        summit::ExtensionShortcuts::Default().Set(std::move(bindings));
        if (fShortcutsWindow.IsValid()) fShortcutsWindow.SendMessage(summit::kExtensionShortcutsState);
    }
    // From the Extensions and Keyboard Shortcuts windows.
    void ExtensionSettingsMessage(const BMessage& message)
    {
        BMessenger sender;
        if (!fExtensions || message.FindMessenger("window", &sender) != B_OK
            || (sender != fExtensionWindow && sender != fShortcutsWindow)) return;
        const char* identifier = message.GetString("extension_identifier", "");
        switch (message.what) {
            case summit::kShowExtensionShortcuts:
                if (!fShortcutsWindow.IsValid()) {
                    auto* window = new summit::ExtensionShortcutsWindow(BMessenger(this), fExtensionWindows);
                    fShortcutsWindow = BMessenger(window);
                    window->Show();
                } else {
                    BMessage state(summit::kExtensionShortcutsState);
                    state.AddBool("activate", true);
                    fShortcutsWindow.SendMessage(&state);
                }
                RefreshExtensionCommands();
                break;
            case summit::kExtensionShortcutsClosed:
                if (sender == fShortcutsWindow) fShortcutsWindow = {};
                break;
            case summit::kExtensionSetShortcut: {
                const char* command = message.GetString("command", "");
                const char* shortcut = message.GetString("shortcut", nullptr);
                if (*identifier && *command && fWebKitContext)
                    fWebKitContext->SetExtensionCommandShortcut(identifier, command, shortcut, BMessenger(this), ++fShortcutRequest);
                break;
            }
            case summit::kExtensionCheckUpdates:
                if (fExtensionUpdates) fExtensionUpdates->CheckNow();
                break;
            case summit::kExtensionSetAutoUpdate: {
                const bool enabled = message.GetBool("enabled", true);
                fExtensions->ModifyInstallation(identifier, [enabled](summit::InstalledExtension& record) { record.autoUpdate = enabled; });
                break;
            }
            case summit::kExtensionApproveUpdate:
                if (fExtensionUpdates) fExtensionUpdates->Approve(identifier);
                break;
        }
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
            std::vector<summit::SharedProfile::ExtensionSidebar> sidebars;
            for (const auto& entry : fExtensions->Entries())
                if (entry.loaded && !entry.sidebarURL.empty())
                    sidebars.push_back({entry.installation.identifier, entry.sidebarTitle, entry.sidebarURL});
            fShared->SetExtensionSidebars(std::move(sidebars));
            std::set<std::string> loaded;
            for (const auto& entry : fExtensions->Entries())
                if (entry.loaded) loaded.insert(entry.installation.identifier);
            fShared->KeepDownloadUIHiddenBy(loaded);
            // For the cards on store pages: Install, or Manage when installed.
            std::map<std::string, summit::SharedProfile::InstalledExtensionInfo> installed;
            for (const auto& entry : fExtensions->Entries())
                installed[entry.installation.identifier] = {entry.installation.name, entry.loaded || entry.installation.enabled};
            fShared->SetInstalledExtensions(std::move(installed));
        }
        if (!fExtensionWindow.IsValid() || !fExtensions || !fInstaller) return;
        BMessage state = fInstaller->Snapshot();
        state.what = summit::kExtensionsState;
        state.AddBool("ready", fExtensions->IsReady());
        state.AddString("catalog_error", fExtensions->CatalogError().c_str());
        state.AddString("removal_error", fExtensions->RemovalError().c_str());
        if (fExtensionUpdates) {
            state.AddString("update_status", fExtensionUpdates->Status().c_str());
            state.AddBool("checking", fExtensionUpdates->IsChecking());
        }
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
            item.AddString("source", entry.installation.source.c_str());
            item.AddBool("auto_update", entry.installation.autoUpdate);
            item.AddInt64("updated", entry.installation.updated);
            if (fExtensionUpdates) {
                item.AddString("update_note", fExtensionUpdates->Note(entry.installation.identifier).c_str());
                item.AddBool("update_waiting", fExtensionUpdates->IsWaiting(entry.installation.identifier));
            }
            // Where its icon is (the window reads the manifest's icons).
            item.AddString("package", summit::ExtensionCatalog(fProfile / "Extensions").PackagePath(entry.installation).c_str());
            item.AddBool("pinned", !fShared || fShared->Read([&](const summit::Profile& profile) {
                return !profile.unpinnedExtensions.contains(entry.installation.identifier);
            }));
            state.AddMessage("entry", &item);
        }
        fExtensionWindow.SendMessage(&state, static_cast<BHandler*>(nullptr), 0);
    }
#endif
    // One Summit per profile folder (#93). Another build or package of
    // Summit (the registrar keeps one of each executable) that finds the
    // folder in use hands what it was asked to open to the one using it, and
    // quits; a copy that does not answer gets a message, never a wait.
    bool LockProfile()
    {
        auto lock = std::make_unique<summit::ProfileLock>();
        const bigtime_t deadline = system_time() + 8000000;
        while (true) {
            int64_t holder = 0;
            std::string error;
            const auto result = lock->Take(fProfile, Team(), holder, error);
            if (result == summit::ProfileLock::Result::Taken) {
                fProfileLock = std::move(lock);
                return true;
            }
            if (result == summit::ProfileLock::Result::Unsupported) {
                std::fprintf(stderr, "Summit: the profile folder cannot be locked (%s); using it anyway\n", error.c_str());
                return true;
            }
            if (holder > 0 && holder != Team()) {
                const status_t handed = HandToRunningCopy(static_cast<team_id>(holder));
                if (handed == B_OK) {
                    std::fprintf(stderr, "Summit: the profile is in use by team %lld; handed this start to it\n",
                        static_cast<long long>(holder));
                    PostMessage(B_QUIT_REQUESTED);
                    return false;
                }
                // There, and not answering: the pages wait in its queue,
                // and are not sent again.
                if (handed != B_BAD_TEAM_ID && handed != B_BAD_PORT_ID) break;
            }
            // One that is quitting lets go in a moment.
            if (system_time() > deadline) break;
            snooze(250000);
        }
        StartupError("Another copy of Summit is using this profile and does not answer:\n" + fProfile.string()
            + "\n\nQuit it first, or start this one with --profile and another folder.");
        return false;
    }
    // B_BAD_TEAM_ID or B_BAD_PORT_ID: that copy is gone.
    status_t HandToRunningCopy(team_id team)
    {
        status_t status = B_ERROR;
        BMessenger running(nullptr, team, &status);
        if (status != B_OK || !running.IsValid()) return B_BAD_TEAM_ID;
        BMessage start(B_ARGV_RECEIVED);
        std::vector<std::string> arguments { "Summit" };
        for (const auto& app : fPendingWebApps) arguments.insert(arguments.end(), {"--app", app});
        arguments.insert(arguments.end(), fURLs.begin(), fURLs.end());
        if (arguments.size() == 1) start.what = B_SILENT_RELAUNCH;
        else {
            start.AddInt32("argc", static_cast<int32>(arguments.size()));
            for (const auto& argument : arguments) start.AddString("argv", argument.c_str());
        }
        // The reply (B_NO_REPLY once it has been handled) says that it was.
        BMessage reply;
        if (const status_t sent = running.SendMessage(&start, &reply, 2000000, 6000000); sent != B_OK) return sent;
        be_roster->ActivateApp(team);
        return B_OK;
    }
    // What the window says about a profile that could not be read.
    std::string ProfileNotice() const
    {
        if (!fShared || fShared->LoadError().empty()) return { };
        const auto damaged = fShared->DamagedCopy().filename().string();
        const std::string kept = damaged.empty() ? std::string() : " The damaged file was kept as " + damaged + ".";
        if (!fShared->Writable())
            return "Summit could not read your profile and has left it as it is; changes in this session are not saved.";
        if (fShared->RestoredFrom().empty())
            return "Your profile could not be read and there was no earlier copy, so Summit started a new one." + kept;
        const time_t when = static_cast<time_t>(fShared->RestoredTime());
        const time_t now = std::time(nullptr);
        struct tm local { };
        char text[64] = "";
        if (when > 0 && localtime_r(&when, &local))
            std::strftime(text, sizeof text, now - when < 6 * 86400 ? "%A, %H:%M" : "%e %B, %H:%M", &local);
        std::string date = text;
        date.erase(0, date.find_first_not_of(' '));
        return "Your profile was damaged. Summit restored it from " + (date.empty() ? std::string("an earlier copy") : date)
            + " and saves it again." + kept;
    }
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
    std::map<BWebKitContext*, std::unique_ptr<summit::DownloadListener>> fDownloadListeners;
    std::vector<BMessage> fPendingExtensionTabs;
    std::map<uint64, uint32> fDataRequests;
    uint64 fNextDataRequest = 0;
    std::unique_ptr<summit::ExtensionPermissionPrompt> fPermissionPrompts;
    std::unique_ptr<summit::SitePermissionService> fSitePermissions;
    std::unique_ptr<summit::SitePermissionService> fPrivateSitePermissions;
    std::unique_ptr<summit::ExtensionController> fExtensions;
    std::unique_ptr<summit::ExtensionInstaller> fInstaller;
    std::unique_ptr<summit::ExtensionUpdater> fExtensionUpdates;
    // Extensions › Keyboard Shortcuts…, and the engine requests for the
    // commands' keys and for a change of them.
    BMessenger fShortcutsWindow;
    uint64 fCommandsRequest = 0, fShortcutRequest = 0;
    bool fCommandsInFlight = false, fCommandsAgain = false;
    std::unique_ptr<summit::McpService> fMcp;
    std::unique_ptr<summit::UpdateChecker> fUpdates;
    // The update window installed a new build: the quit under way starts
    // Summit again (TakeRelaunch).
    bool fUpdateRestart = false;
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
    // Web apps asked for before the first window opened (#75).
    std::vector<std::string> fPendingWebApps;
    std::unique_ptr<summit::ProfileLock> fProfileLock;
    // Started again after an update (kRestoreSessionArgument): the last
    // session's windows and tabs open whatever Preferences say.
    bool fRestoreAfterUpdate = false;
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
    // The new Summit inherits this one's descriptors: the debugging server's
    // listening socket would keep its port taken, unanswered, and stderr is
    // the log capture's pipe, which nobody reads once this one is gone.
#if SUMMIT_MODERN_WEBKIT
    summit::mcplog::RestoreStderr();
#endif
    for (int fd = 3, limit = std::min(getdtablesize(), 65536); fd < limit; ++fd)
        if (const int flags = fcntl(fd, F_GETFD); flags >= 0 && !(flags & FD_CLOEXEC)) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
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
    std::unique_ptr<summit::ProfileLock> lock;
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
        lock = app.TakeProfileLock();
        relaunch = app.TakeRelaunch(relaunchArguments);
        if (profile) exiting = create_port(1, ExitingPortName().c_str());
    }
    // The application has left the registrar: a new start makes a new
    // instance, which waits for the port. The profile's last write (the
    // saver thread's, then its own) happens with the last reference.
    SUMMIT_QUIT_TRACE("application destroyed");
    profile = nullptr;
    SUMMIT_QUIT_TRACE("profile saved");
    lock.reset();
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

#include "ExtensionUpdater.h"
#if SUMMIT_MODERN_WEBKIT
#include "ExtensionInstaller.h"
#include "ExtensionStoreDownload.h"
#include "ExtensionTiming.h"
#include "core/ExtensionIdentity.h"
#include "core/ExtensionUpdate.h"
#include <WebKit/WebKitContext.h>
#include <MessageRunner.h>
#include <OS.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>

namespace summit {
namespace {
constexpr uint32 tick = 'exut', workDone = 'exuw';
constexpr int64_t day = 24 * 60 * 60;
std::string field(const BMessage& message, const char* name)
{
    const char* value = nullptr;
    return message.FindString(name, &value) == B_OK && value ? value : "";
}
std::vector<std::string> fields(const BMessage& message, const char* name)
{
    std::vector<std::string> result;
    const char* value = nullptr;
    for (int32 i = 0; i < 4096 && message.FindString(name, i, &value) == B_OK; ++i)
        if (value && *value && std::strlen(value) <= 4096) result.emplace_back(value);
    return result;
}
// SUMMIT_EXTENSION_TEST_UPDATE=IDENTIFIER=PACKAGE: tests take that local
// package for the store's new version of that extension (no network).
std::pair<std::string, std::string> testUpdate()
{
    const char* value = std::getenv("SUMMIT_EXTENSION_TEST_UPDATE");
    const std::string text = value ? value : "";
    const auto equals = text.find('=');
    if (equals == std::string::npos || equals == 0 || text.size() <= equals + 2 || text[equals + 1] != '/') return {};
    return { text.substr(0, equals), text.substr(equals + 1) };
}
// Whether a site the new version asks for was approved already, alone or
// under a pattern for every site.
bool covered(const std::vector<std::string>& approved, const std::string& origin)
{
    if (std::find(approved.begin(), approved.end(), origin) != approved.end()) return true;
    const bool web = origin.rfind("http", 0) == 0 || origin.rfind("ws", 0) == 0 || origin.rfind("*://", 0) == 0;
    return web && std::any_of(approved.begin(), approved.end(), [](const std::string& pattern) {
        return pattern == "<all_urls>" || pattern == "*://*/*";
    });
}
}

struct ExtensionUpdater::Work {
    std::vector<Candidate> candidates;
    std::vector<Found> results;
    std::atomic<bool> cancelled { false };
};

ExtensionUpdater::ExtensionUpdater(std::shared_ptr<BWebKitContext> context, std::filesystem::path root, ExtensionController& controller,
    std::function<void()> changed)
    : BHandler("Summit extension updater"), fContext(std::move(context)), fCatalog(root), fDownloadRoot(root / "downloads")
    , fController(controller), fChanged(std::move(changed)) { }
ExtensionUpdater::~ExtensionUpdater()
{
    if (fWork) fWork->cancelled = true;
    if (fThread.joinable()) fThread.join();
}
void ExtensionUpdater::Changed() { if (fChanged) fChanged(); }
void ExtensionUpdater::Start()
{
    fStarted = system_time();
    BMessage message(tick);
    // Every minute: the launch check, the daily ones, and updates waiting
    // for the extensions to be idle.
    fTimer = std::make_unique<BMessageRunner>(BMessenger(this), &message, 60000000);
}
void ExtensionUpdater::Shutdown()
{
    fStopping = true;
    fTimer.reset();
    if (fWork) fWork->cancelled = true;
    if (fReady) {
        fContext->DiscardPreparedExtension(fReady->token.c_str());
        fReady.reset();
    }
    fQueue.clear();
    fWaiting.clear();
}
bool ExtensionUpdater::HasPendingWork() const
{
    return fWork != nullptr || fPreparing != nullptr || fApplying;
}
std::string ExtensionUpdater::Note(const std::string& identifier) const
{
    auto found = fNotes.find(identifier);
    return found == fNotes.end() ? std::string() : found->second;
}
void ExtensionUpdater::CheckNow()
{
    if (fStopping) return;
    if (IsChecking()) {
        fStatus = "Already looking for updates…";
        Changed();
        return;
    }
    Round(true, false);
}
void ExtensionUpdater::Tick()
{
    if (fStopping) return;
    if (fReady && !fApplying) Apply();
    if (IsChecking() || !fController.IsReady()) return;
    if (!fLaunchChecked && system_time() - fStarted >= 60000000) {
        fLaunchChecked = true;
        Round(false, true);
        return;
    }
    if (fLaunchChecked) Round(false, false);
}
void ExtensionUpdater::Round(bool manual, bool launch)
{
    const int64_t now = std::time(nullptr);
    auto work = std::make_shared<Work>();
    const auto test = testUpdate();
    for (const auto& entry : fController.Entries()) {
        const auto& installation = entry.installation;
        if (!entry.installed || !entry.loaded || fWaiting.count(installation.identifier)
            || (installation.source.empty() && installation.identifier != test.first)) continue;
        if (!manual && (!installation.autoUpdate || (!launch && now - installation.checked < day))) continue;
        work->candidates.push_back({ installation.identifier, installation.name, installation.version, installation.source });
    }
    fManual = manual;
    fUpdated = fFailed = 0;
    if (work->candidates.empty()) {
        if (manual) {
            fStatus = "No installed extension comes from the Chrome Web Store or Firefox Add-ons.";
            Changed();
        }
        return;
    }
    fStatus = "Looking for updates…";
    for (const auto& candidate : work->candidates) fNotes.erase(candidate.identifier);
    fWork = work;
    try {
        fThread = std::thread([work, catalog = fCatalog, downloadRoot = fDownloadRoot, test, target = BMessenger(this)] {
            for (const auto& candidate : work->candidates) {
                if (work->cancelled) break;
                Found found;
                found.candidate = candidate;
                std::string text, error, expected;
                long status = 0;
                if (candidate.identifier == test.first) {
                    found.package = catalog.Stage(test.second, error);
                    if (!found.package) found.error = error;
                    found.version = "(test package)";
                    work->results.push_back(std::move(found));
                    continue;
                }
                if (candidate.source == "chrome") {
                    if (!FetchStoreText(ChromeUpdateCheckURL(candidate.identifier, candidate.version), 256 * 1024, work->cancelled, text, error))
                        found.error = error;
                    else if (auto version = ChromeUpdateCheckVersion(text, candidate.identifier))
                        found.version = *version;
                } else if (!FetchStoreText(FirefoxAddonURL(candidate.identifier), 2 * 1024 * 1024, work->cancelled, text, error, &status)) {
                    if (status == 404) found.unlisted = true;
                    else found.error = "Firefox Add-ons did not answer (" + error + ")";
                } else {
                    try {
                        const auto addon = nlohmann::json::parse(text);
                        if (addon.at("guid").get<std::string>() != candidate.identifier) found.error = "Firefox Add-ons answered for another add-on";
                        else found.version = addon.at("current_version").at("version").get<std::string>();
                    } catch (const std::exception&) {
                        found.error = "Firefox Add-ons sent an answer Summit cannot read";
                    }
                }
                if (found.error.empty() && !found.version.empty() && CompareExtensionVersions(found.version, candidate.version) > 0) {
                    ExtensionStoreItem item { candidate.source == "chrome" ? ExtensionStoreItem::Store::Chrome : ExtensionStoreItem::Store::Firefox,
                        candidate.identifier, candidate.source == "chrome"
                            ? "https://clients2.google.com/service/update2/crx?response=redirect&prodversion=130.0&acceptformat=crx3&x=id%3D"
                                + candidate.identifier + "%26uc"
                            : FirefoxAddonURL(candidate.identifier) };
                    found.package = DownloadStoreExtension(item, downloadRoot, catalog, work->cancelled, expected, error);
                    if (!found.package) found.error = error;
                    else if (expected != candidate.identifier) {
                        found.package.reset();
                        found.error = "The store's package is for another extension";
                    }
                } else if (found.error.empty())
                    found.version.clear();
                work->results.push_back(std::move(found));
            }
            BMessage done(workDone);
            target.SendMessage(&done, static_cast<BHandler*>(nullptr), 0);
        });
    } catch (const std::exception& error) {
        fWork.reset();
        fStatus = std::string("Could not look for updates: ") + error.what();
    }
    Changed();
}
void ExtensionUpdater::Collect()
{
    if (!fWork) return;
    if (fThread.joinable()) fThread.join();
    auto work = std::exchange(fWork, {});
    if (fStopping) return;
    const int64_t now = std::time(nullptr);
    unsigned newer = 0;
    for (auto& found : work->results) {
        const auto& identifier = found.candidate.identifier;
        fController.ModifyInstallation(identifier, [now](InstalledExtension& record) { record.checked = now; });
        if (found.unlisted) {
            fNotes[identifier] = "Firefox Add-ons does not list it, so it cannot be updated from there.";
            SUMMIT_EXTENSION_TIMING("%s is not on Firefox Add-ons", identifier.c_str());
        } else if (!found.error.empty()) {
            ++fFailed;
            fNotes[identifier] = "Could not look for updates: " + found.error + ".";
            std::fprintf(stderr, "Summit extension %s: update check failed: %s\n", identifier.c_str(), found.error.c_str());
        } else if (found.package) {
            ++newer;
            fNotes[identifier] = "Version " + found.version + " is downloaded; installing it…";
            fQueue.push_back(std::move(found));
        } else
            SUMMIT_EXTENSION_TIMING("%s %s is up to date", identifier.c_str(), found.candidate.version.c_str());
    }
    if (!newer) fStatus = fFailed ? "Some extensions could not be checked for updates." : fManual ? "All extensions are up to date." : "";
    PrepareNext();
    Changed();
}
void ExtensionUpdater::PrepareNext()
{
    if (fStopping || fPreparing || fReady || fApplying || fQueue.empty()) return;
    fPreparing = std::make_unique<Found>(std::move(fQueue.front()));
    fQueue.pop_front();
    fStatus = "Checking the new version of " + ExtensionDisplayText(fPreparing->candidate.name) + "…";
    auto status = fContext->PrepareExtension(fPreparing->package->Path().c_str(), BMessenger(this), ++fRequest);
    if (status != B_OK) {
        fNotes[fPreparing->candidate.identifier] = std::string("The new version could not be checked: ") + std::strerror(status) + ".";
        fPreparing.reset();
        PrepareNext();
    }
}
void ExtensionUpdater::Prepared(const BMessage& message)
{
    auto found = std::move(fPreparing);
    if (fStopping) {
        if (const auto token = field(message, "token"); !token.empty()) fContext->DiscardPreparedExtension(token.c_str());
        return;
    }
    const auto& candidate = found->candidate;
    auto fail = [&](const std::string& reason) {
        ++fFailed;
        fNotes[candidate.identifier] = "Version " + found->version + " was not installed: " + reason;
        std::fprintf(stderr, "Summit extension %s: update not installed: %s\n", candidate.identifier.c_str(), reason.c_str());
        if (const auto token = field(message, "token"); !token.empty()) fContext->DiscardPreparedExtension(token.c_str());
        fStatus = ExtensionDisplayText(candidate.name) + " could not be updated.";
        PrepareNext();
        Changed();
    };
    if (message.GetInt32("error", B_ERROR) != B_OK) {
        const auto reason = field(message, "description");
        fail(reason.empty() ? std::string(std::strerror(message.GetInt32("error", B_ERROR))) : reason);
        return;
    }
    auto installed = std::find_if(fController.Entries().begin(), fController.Entries().end(), [&](const auto& entry) {
        return entry.installation.identifier == candidate.identifier;
    });
    if (installed == fController.Entries().end()) { fail("it is no longer installed."); return; }
    auto update = std::make_unique<ExtensionController::Update>();
    update->identifier = candidate.identifier;
    update->token = field(message, "token");
    update->name = field(message, "name");
    update->version = field(message, "version");
    update->fingerprint = field(message, "fingerprint");
    update->permissions = fields(message, "permission");
    update->origins = fields(message, "origin");
    std::string identity;
    try {
        const auto manifest = nlohmann::json::parse(field(message, "manifest_json"));
        const auto verified = field(message, "verified_crx_id");
        identity = ExtensionIdentity(manifest, "summit-" + found->package->Path().parent_path().filename().string(), verified);
        if (candidate.source == "chrome" && verified != candidate.identifier) { fail("its package is not signed for this extension."); return; }
        // As at installation: a store package carries the store's publisher
        // proof, and a Mozilla signature that does not verify is refused.
        if (candidate.source == "chrome") {
            std::ifstream crx(found->package->Path(), std::ios::binary);
            std::string header(12 + 1024 * 1024, '\0');
            crx.read(header.data(), static_cast<std::streamsize>(header.size()));
            header.resize(static_cast<size_t>(crx.gcount()));
            if (!CRXHasWebStorePublisherProof(header)) { fail("its package is not signed by the Chrome Web Store."); return; }
        }
        if (field(message, "xpi_signature") == "invalid") { fail("its Mozilla signature is not valid: " + field(message, "xpi_signature_error") + "."); return; }
        update->source = ExtensionUpdateSource(manifest, identity, verified);
        if (manifest.contains("update_url") && manifest["update_url"].is_string()) update->updateURL = manifest["update_url"].get<std::string>();
    } catch (const std::exception& error) {
        fail(error.what());
        return;
    }
    if (identity != candidate.identifier) { fail("the package is for another extension."); return; }
    if (update->token.empty() || update->name.empty() || update->version.empty() || update->fingerprint.size() != 64) {
        fail("its package is incomplete.");
        return;
    }
    if (CompareExtensionVersions(update->version, installed->installation.version) <= 0) {
        fail("it is not newer than version " + installed->installation.version + ".");
        return;
    }
    // More access than the user approved for the installed version waits
    // for approval. Unknown approvals (older catalogs) compare as none.
    const auto& approved = installed->installation;
    std::string access;
    if (!found->approved && (!approved.permissions.empty() || !approved.origins.empty())) {
        for (const auto& permission : update->permissions)
            if (std::find(approved.permissions.begin(), approved.permissions.end(), permission) == approved.permissions.end())
                access += "  • " + ExtensionDisplayText(permission) + "\n";
        for (const auto& origin : update->origins)
            if (!covered(approved.origins, origin)) access += "  • " + ExtensionDisplayText(origin) + "\n";
    }
    if (!access.empty()) {
        fContext->DiscardPreparedExtension(update->token.c_str());
        fNotes[candidate.identifier] = "Version " + update->version + " is ready. It asks for more access:\n" + access
            + "Choose Update to approve it and install it.";
        found->version = update->version;
        fStatus = ExtensionDisplayText(candidate.name) + " " + update->version + " waits for your approval.";
        const auto identifier = candidate.identifier;
        fWaiting[identifier] = std::move(*found);
        PrepareNext();
        Changed();
        return;
    }
    update->package = std::move(found->package);
    fReady = std::move(update);
    Apply();
}
void ExtensionUpdater::Apply()
{
    if (!fReady || fApplying || fStopping) return;
    // Installs wait for other extension work to finish (a minute's tick tries again).
    if (!fController.IsReady()) return;
    const auto identifier = fReady->identifier, version = fReady->version, name = fReady->name;
    fApplying = true;
    auto update = std::move(fReady);
    const auto token = update->token;
    if (!fController.ApplyUpdate(std::move(update), [this, identifier, version, name](bool updated, std::string detail) {
            fApplying = false;
            if (updated) {
                ++fUpdated;
                fNotes[identifier] = "Updated from version " + detail + ".";
                fStatus = ExtensionDisplayText(name) + " was updated to version " + version + ".";
            } else {
                ++fFailed;
                fNotes[identifier] = "Version " + version + " was not installed: " + detail;
                fStatus = ExtensionDisplayText(name) + " could not be updated.";
            }
            PrepareNext();
            Changed();
        })) {
        // Not running any more (disabled or removed meanwhile).
        fApplying = false;
        fContext->DiscardPreparedExtension(token.c_str());
        fNotes[identifier] = "Version " + version + " was not installed: the extension is not running.";
        PrepareNext();
    }
    Changed();
}
void ExtensionUpdater::Approve(const std::string& identifier)
{
    auto found = fWaiting.find(identifier);
    if (found == fWaiting.end() || fStopping) return;
    found->second.approved = true;
    fNotes[identifier] = "Installing version " + found->second.version + "…";
    fQueue.push_back(std::move(found->second));
    fWaiting.erase(found);
    PrepareNext();
    Changed();
}
void ExtensionUpdater::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case tick: Tick(); return;
        case workDone: Collect(); return;
        case B_WEBKIT_EXTENSION_PACKAGE_PREPARED:
            if (fPreparing && message->GetUInt64("identifier", 0) == fRequest) {
                Prepared(*message);
                return;
            }
            break;
    }
    BHandler::MessageReceived(message);
}
}
#endif

#include "ExtensionController.h"
#include "ExtensionTiming.h"
#if SUMMIT_MODERN_WEBKIT
#include "core/ExtensionUpdate.h"
#include <WebKit/WebKitContext.h>
#include <Message.h>
#include <Messenger.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <utility>

namespace summit {
namespace {
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
bool developerModeAcceptsChangedPackages()
{
    const char* value = std::getenv("SUMMIT_EXTENSION_DEVELOPER_MODE");
    return value && !std::strcmp(value, "1");
}
}
ExtensionController::ExtensionController(std::shared_ptr<BWebKitContext> context,
    std::filesystem::path root, std::function<void()> changed)
    : BHandler("Summit extension controller"), fContext(std::move(context)), fCatalog(std::move(root))
    , fChanged(std::move(changed)) { }

void ExtensionController::Start()
{
    if (fStarted || fStopping) return;
    fStarted = true;
    std::vector<InstalledExtension> installed;
    if (!fCatalog.Load(installed, fCatalogError)) {
        std::fprintf(stderr, "Summit extension catalog: %s\n", fCatalogError.c_str());
        Changed();
        return;
    }
    for (auto& entry : installed) fEntries.push_back({ std::move(entry), false, { }, { }, true, { }, { } });
    Changed();
    Next();
}
void ExtensionController::Changed()
{
    if (fChanged) fChanged();
}
void ExtensionController::DiscardToken()
{
    if (fToken.empty()) return;
    fContext->DiscardPreparedExtension(fToken.c_str());
    fToken.clear();
}
void ExtensionController::Fail(const std::string& error)
{
    auto& entry = fEntries[fIndex];
    entry.error = error;
    std::fprintf(stderr, "Summit extension %s: %s\n", entry.installation.identifier.c_str(), error.c_str());
    DiscardToken();
    fPending = Pending::None;
    Advance();
}
void ExtensionController::Advance()
{
    if (fOperation != Operation::None) {
        fOperation = Operation::None;
        fIndex = fStopping ? 0 : fEntries.size();
    } else ++fIndex;
    Changed();
    Next();
}
bool ExtensionController::IsReady() const
{
    return fStarted && !fStopping && fCatalogError.empty() && fIndex == fEntries.size() && !HasPendingWork();
}
void ExtensionController::AddLoaded(InstalledExtension entry, std::string baseURL, std::string error, bool installed,
    std::string newTabURL)
{
    // The app serializes installer operations with this controller.
    fEntries.push_back({ std::move(entry), true, std::move(baseURL), std::move(error), installed, std::move(newTabURL), { } });
    if (!fStopping) fIndex = fEntries.size();
    Changed();
    if (fStopping) Next();
}
bool ExtensionController::SetEnabled(const std::string& identifier, bool enabled)
{
    if (!IsReady()) return false;
    auto found = std::find_if(fEntries.begin(), fEntries.end(), [&](const auto& entry) { return entry.installation.identifier == identifier; });
    if (found == fEntries.end() || !found->installed) return false;
    if (enabled && found->loaded) return true;
    fIndex = found - fEntries.begin();
    fOperation = enabled ? Operation::Enable : Operation::Disable;
    found->error.clear();
    if (enabled) {
        if (!fCatalog.SetEnabled(identifier, true, found->error)) { Advance(); return false; }
        found->installation.enabled = true;
    }
    Changed();
    Next();
    return true;
}
bool ExtensionController::SetAllowPrivateBrowsing(const std::string& identifier, bool allowed)
{
    if (!IsReady()) return false;
    auto found = std::find_if(fEntries.begin(), fEntries.end(), [&](const auto& entry) { return entry.installation.identifier == identifier; });
    if (found == fEntries.end() || !found->installed) return false;
    if (found->installation.allowPrivateBrowsing == allowed) return true;
    if (!fCatalog.SetAllowPrivateBrowsing(identifier, allowed, found->error)) {
        Changed();
        return false;
    }
    found->installation.allowPrivateBrowsing = allowed;
    // A running extension changes at once; one that is not loads with it.
    if (found->loaded) fContext->SetExtensionPrivateAccess(identifier.c_str(), allowed);
    Changed();
    return true;
}
bool ExtensionController::Remove(const std::string& identifier)
{
    if (!IsReady()) return false;
    auto found = std::find_if(fEntries.begin(), fEntries.end(), [&](const auto& entry) { return entry.installation.identifier == identifier; });
    if (found == fEntries.end()) return false;
    fIndex = found - fEntries.begin();
    fOperation = Operation::Remove;
    found->error.clear();
    Changed();
    Next();
    return true;
}
void ExtensionController::FinishRemovalOrDisable()
{
    auto& entry = fEntries[fIndex];
    if (fOperation == Operation::Remove) {
        if (entry.installed && !fCatalog.Forget(entry.installation.identifier, entry.error)) { Advance(); return; }
        // Then what it kept in the profile: its storage, its website data
        // and its unpacked package.
        const auto package = entry.installed ? fCatalog.PackagePath(entry.installation).string() : std::string();
        fPending = Pending::RemoveData;
        auto status = fContext->RemoveExtensionData(entry.installation.identifier.c_str(), package.empty() ? nullptr : package.c_str(),
            BMessenger(this), ++fRequest);
        if (status != B_OK) {
            fPending = Pending::None;
            FinishRemoval(status);
        }
        return;
    } else {
        if (fCatalog.SetEnabled(entry.installation.identifier, false, entry.error)) entry.installation.enabled = false;
    }
    Advance();
}
void ExtensionController::FinishRemoval(status_t status)
{
    auto& entry = fEntries[fIndex];
    fRemovalError.clear();
    std::string error;
    if (status != B_OK)
        fRemovalError = "Some of the saved data of " + entry.installation.name + " could not be deleted: " + std::strerror(status);
    else if (entry.installed && !fCatalog.RemovePackage(entry.installation.package, error))
        fRemovalError = "The package of " + entry.installation.name + " could not be deleted: " + error;
    if (!fRemovalError.empty())
        std::fprintf(stderr, "Summit extension %s: %s\n", entry.installation.identifier.c_str(), fRemovalError.c_str());
    else
        SUMMIT_EXTENSION_TIMING("removed %s and its data", entry.installation.identifier.c_str());
    fEntries.erase(fEntries.begin() + fIndex);
    Advance();
}
void ExtensionController::RecordPackageDetails(Entry& entry, const BMessage& prepared)
{
    // Where its updates come from, and what this version asks for: an
    // update that asks for more needs approval again.
    auto permissions = fields(prepared, "permission"), origins = fields(prepared, "origin");
    auto source = entry.installation.source, updateURL = entry.installation.updateURL;
    try {
        const auto manifest = nlohmann::json::parse(field(prepared, "manifest_json"));
        source = ExtensionUpdateSource(manifest, entry.installation.identifier, field(prepared, "verified_crx_id"));
        updateURL = manifest.contains("update_url") && manifest["update_url"].is_string() ? manifest["update_url"].get<std::string>() : std::string();
    } catch (const std::exception&) { }
    const bool approvalKnown = !entry.installation.permissions.empty() || !entry.installation.origins.empty();
    if (!entry.installed || (source == entry.installation.source && updateURL == entry.installation.updateURL && approvalKnown))
        return;
    auto change = [&](InstalledExtension& record) {
        record.source = source;
        record.updateURL = updateURL;
        if (!approvalKnown) {
            record.permissions = permissions;
            record.origins = origins;
        }
    };
    std::string error;
    if (fCatalog.Modify(entry.installation.identifier, change, error)) change(entry.installation);
    else std::fprintf(stderr, "Summit extension %s: %s\n", entry.installation.identifier.c_str(), error.c_str());
}
bool ExtensionController::ModifyInstallation(const std::string& identifier, const std::function<void(InstalledExtension&)>& change)
{
    auto found = std::find_if(fEntries.begin(), fEntries.end(), [&](const auto& entry) { return entry.installation.identifier == identifier; });
    if (found == fEntries.end() || !found->installed) return false;
    std::string error;
    if (!fCatalog.Modify(identifier, change, error)) {
        std::fprintf(stderr, "Summit extension %s: %s\n", identifier.c_str(), error.c_str());
        return false;
    }
    change(found->installation);
    Changed();
    return true;
}
bool ExtensionController::ApplyUpdate(std::unique_ptr<Update> update, std::function<void(bool, std::string)> done)
{
    if (!IsReady() || !update || !update->package || update->token.empty()) return false;
    auto found = std::find_if(fEntries.begin(), fEntries.end(), [&](const auto& entry) { return entry.installation.identifier == update->identifier; });
    if (found == fEntries.end() || !found->installed || !found->loaded) return false;
    fIndex = found - fEntries.begin();
    fOperation = Operation::Update;
    fUpdate = std::move(update);
    fUpdateDone = std::move(done);
    found->error.clear();
    SUMMIT_EXTENSION_TIMING("updating %s from %s to %s", found->installation.identifier.c_str(), found->installation.version.c_str(),
        fUpdate->version.c_str());
    Changed();
    Next();
    return true;
}
void ExtensionController::LoadUpdate()
{
    auto& entry = fEntries[fIndex];
    BWebKitExtensionLoadOptions options;
    options.uniqueIdentifier = entry.installation.identifier;
    options.expectedFingerprint = fUpdate->fingerprint;
    // It keeps the replaced version's state, data and grants; these are the
    // new version's own, which the user approved.
    options.purpose = BWebKitExtensionLoadOptions::Purpose::ExtensionUpdate;
    options.permissions = fUpdate->permissions;
    for (const auto& origin : fUpdate->origins)
        if (entry.installation.allowFileURLs || origin.compare(0, 5, "file:") != 0) options.origins.push_back(origin);
    options.allowFileURLs = entry.installation.allowFileURLs;
    options.allowPrivateBrowsing = entry.installation.allowPrivateBrowsing;
    BWebKitExtensionInstallation installation { fUpdate->package->Path().parent_path().filename().string(), entry.installation.installationOrder };
    fPending = Pending::Load;
    auto status = fContext->LoadPreparedExtension(fUpdate->token.c_str(), options, installation, BMessenger(this), ++fRequest);
    if (status != B_OK) {
        fPending = Pending::None;
        UpdateFailed(std::strerror(status));
    }
}
void ExtensionController::DropUpdate(const std::string& reason)
{
    fContext->DiscardPreparedExtension(fUpdate->token.c_str());
    if (auto done = std::exchange(fUpdateDone, {})) done(false, reason);
    fUpdate.reset();
}
void ExtensionController::UpdateFailed(const std::string& reason)
{
    auto& entry = fEntries[fIndex];
    entry.error = "Version " + fUpdate->version + " did not start (" + reason + "). Version " + entry.installation.version + " runs again.";
    std::fprintf(stderr, "Summit extension %s: %s\n", entry.installation.identifier.c_str(), entry.error.c_str());
    DropUpdate(reason);
    // Start the installed version again.
    fOperation = Operation::Enable;
    Next();
}
void ExtensionController::CommitUpdate(const BMessage& loaded)
{
    auto& entry = fEntries[fIndex];
    auto record = entry.installation;
    const auto previous = record.version, oldPackage = record.package;
    record.name = fUpdate->name;
    record.version = fUpdate->version;
    record.fingerprint = fUpdate->fingerprint;
    record.source = fUpdate->source.empty() ? record.source : fUpdate->source;
    record.updateURL = fUpdate->updateURL;
    record.permissions = fUpdate->permissions;
    record.origins = fUpdate->origins;
    record.updated = record.checked = std::time(nullptr);
    entry.loaded = true;
    entry.baseURL = field(loaded, "base_url");
    entry.newTabURL = field(loaded, "new_tab_url");
    entry.notice = field(loaded, "recovered");
    entry.error.clear();
    std::string error;
    const bool saved = fCatalog.Replace(*fUpdate->package, record, error);
    if (saved) {
        record.package = fUpdate->package->Path().parent_path().filename().string();
        entry.installation = record;
        std::string ignored;
        if (!fCatalog.RemovePackage(oldPackage, ignored))
            std::fprintf(stderr, "Summit extension %s: the old package stays: %s\n", record.identifier.c_str(), ignored.c_str());
        SUMMIT_EXTENSION_TIMING("updated %s from %s to %s", record.identifier.c_str(), previous.c_str(), record.version.c_str());
    } else {
        entry.error = "Version " + fUpdate->version + " runs until Summit quits, but the update could not be saved: " + error;
        std::fprintf(stderr, "Summit extension %s: %s\n", record.identifier.c_str(), entry.error.c_str());
    }
    if (auto done = std::exchange(fUpdateDone, {})) done(saved, saved ? previous : error);
    fUpdate.reset();
}
void ExtensionController::Next()
{
    if (fPending != Pending::None) return;
    while (fIndex < fEntries.size()) {
        auto& entry = fEntries[fIndex];
        if (fStopping) {
            if (!entry.loaded) { ++fIndex; continue; }
            fPending = Pending::Unload;
            auto status = fContext->UnloadExtension(entry.installation.identifier.c_str(), BMessenger(this), ++fRequest);
            if (status != B_OK) Fail(std::strerror(status));
            return;
        }
        if (fOperation == Operation::Update) {
            if (!entry.loaded) { LoadUpdate(); return; }
            fPending = Pending::Unload;
            auto status = fContext->UnloadExtension(entry.installation.identifier.c_str(), BMessenger(this), ++fRequest);
            if (status != B_OK) {
                fPending = Pending::None;
                UpdateFailed(std::strerror(status));
            }
            return;
        }
        if (fOperation == Operation::Disable || fOperation == Operation::Remove) {
            if (!entry.loaded) { FinishRemovalOrDisable(); return; }
            fPending = Pending::Unload;
            auto status = fContext->UnloadExtension(entry.installation.identifier.c_str(), BMessenger(this), ++fRequest);
            if (status != B_OK) Fail(std::strerror(status));
            return;
        }
        if (!entry.installation.enabled) { ++fIndex; continue; }
        fPending = Pending::Prepare;
        auto path = fCatalog.PackagePath(entry.installation);
        SUMMIT_EXTENSION_TIMING("preparing %s", entry.installation.identifier.c_str());
        auto status = fContext->PrepareExtension(path.c_str(), BMessenger(this), ++fRequest);
        if (status != B_OK) Fail(std::strerror(status));
        return;
    }
    Changed();
}
void ExtensionController::MessageReceived(BMessage* message)
{
    uint32 expected = fPending == Pending::Prepare ? B_WEBKIT_EXTENSION_PACKAGE_PREPARED
        : fPending == Pending::Load ? B_WEBKIT_EXTENSION_LOADED
        : fPending == Pending::Unload ? B_WEBKIT_EXTENSION_UNLOADED
        : fPending == Pending::RemoveData ? uint32(B_WEBKIT_EXTENSION_DATA_REMOVED) : 0;
    if (!expected || message->what != expected) {
        BHandler::MessageReceived(message);
        return;
    }
    uint64 identifier = 0;
    int32 error = B_ERROR;
    if (message->FindUInt64("identifier", &identifier) != B_OK || identifier != fRequest
        || message->FindInt32("error", &error) != B_OK || fIndex >= fEntries.size())
        return;
    auto pending = std::exchange(fPending, Pending::None);
    auto& entry = fEntries[fIndex];
    if (pending == Pending::RemoveData) {
        FinishRemoval(error);
        return;
    }
    if (pending == Pending::Prepare && error == B_OK)
        fToken = field(*message, "token");
    if (fStopping && pending != Pending::Unload) {
        // A submitted load can succeed before shutdown's cancellation is seen.
        // Record the real outcome so the shutdown walk unloads that runtime.
        if (fOperation == Operation::Update && fUpdate) {
            if (pending == Pending::Load && error == B_OK) CommitUpdate(*message);
            else DropUpdate("Summit is quitting.");
        }
        if (pending == Pending::Load && error == B_OK) {
            entry.loaded = true;
            entry.baseURL = field(*message, "base_url");
            entry.newTabURL = field(*message, "new_tab_url");
            fToken.clear();
        }
        DiscardToken();
        fOperation = Operation::None;
        fIndex = 0;
        Next();
        return;
    }
    // Quitting while the old version stopped: the new one waits for the next check.
    if (fOperation == Operation::Update && fUpdate && fStopping) DropUpdate("Summit is quitting.");
    if (fOperation == Operation::Update && fUpdate) {
        auto reason = field(*message, "description");
        if (reason.empty()) reason = std::strerror(error);
        if (pending == Pending::Unload) {
            if (error != B_OK && error != B_NAME_NOT_FOUND) {
                // The installed version still runs.
                entry.error = "Version " + fUpdate->version + " was not installed: " + reason;
                DropUpdate(reason);
                Advance();
                return;
            }
            entry.loaded = false;
            LoadUpdate();
            return;
        }
        if (pending == Pending::Load) {
            if (error != B_OK) UpdateFailed(reason);
            else {
                CommitUpdate(*message);
                Advance();
            }
            return;
        }
    }
    if (error != B_OK && !(pending == Pending::Unload && error == B_NAME_NOT_FOUND)) {
        auto reason = field(*message, "description");
        Fail(reason.empty() ? std::strerror(error) : reason);
        return;
    }
    if (pending == Pending::Prepare) {
        auto fingerprint = field(*message, "fingerprint");
        if (!fToken.empty() && fingerprint != entry.installation.fingerprint && developerModeAcceptsChangedPackages()) {
            // Extension development: an edited package keeps its approval for this session only.
            std::fprintf(stderr, "Summit extension %s: developer mode accepted changed package %s\n",
                entry.installation.identifier.c_str(), fingerprint.c_str());
            entry.installation.fingerprint = fingerprint;
        }
        if (fToken.empty() || fingerprint != entry.installation.fingerprint) {
            Fail("The installed package has changed and requires new approval.");
            return;
        }
        RecordPackageDetails(entry, *message);
        BWebKitExtensionLoadOptions options;
        options.uniqueIdentifier = entry.installation.identifier;
        options.expectedFingerprint = entry.installation.fingerprint;
        BWebKitExtensionInstallation installation { entry.installation.package, entry.installation.installationOrder };
        options.purpose = BWebKitExtensionLoadOptions::Purpose::BrowserStartup;
        // The install consent listed every required manifest permission, including ones an
        // older engine lacked. Name the currently supported ones so an engine update can add them.
        const char* permission = nullptr;
        for (int32 i = 0; i < 256 && message->FindString("permission", i, &permission) == B_OK; ++i) {
            if (permission && *permission && std::strlen(permission) <= 4096)
                options.permissions.emplace_back(permission);
        }
        options.allowFileURLs = entry.installation.allowFileURLs;
        options.allowPrivateBrowsing = entry.installation.allowPrivateBrowsing;
        fPending = Pending::Load;
        SUMMIT_EXTENSION_TIMING("prepared %s, loading", entry.installation.identifier.c_str());
        auto status = fContext->LoadPreparedExtension(fToken.c_str(), options, installation, BMessenger(this), ++fRequest);
        if (status != B_OK) Fail(std::strerror(status));
        return;
    }
    entry.loaded = pending == Pending::Load;
    SUMMIT_EXTENSION_TIMING("%s %s", pending == Pending::Load ? "loaded" : "unloaded", entry.installation.identifier.c_str());
    entry.baseURL = entry.loaded ? field(*message, "base_url") : std::string();
    entry.newTabURL = entry.loaded ? field(*message, "new_tab_url") : std::string();
    entry.error.clear();
    if (entry.loaded) {
        entry.notice = field(*message, "recovered");
        if (!entry.notice.empty())
            std::fprintf(stderr, "Summit extension %s: %s\n", entry.installation.identifier.c_str(), entry.notice.c_str());
    }
    fToken.clear();
    if (pending == Pending::Unload && (fOperation == Operation::Disable || fOperation == Operation::Remove))
        FinishRemovalOrDisable();
    else Advance();
}
void ExtensionController::Shutdown()
{
    if (fStopping) return;
    fStopping = true;
    if (fPending == Pending::Prepare)
        fContext->CancelExtensionPreparation(fRequest);
    if (fPending != Pending::None) return;
    DiscardToken();
    fIndex = 0;
    Next();
}
bool ExtensionController::HasPendingWork() const
{
    return fPending != Pending::None || fContext->HasPendingExtensionPreparations();
}
}
#endif

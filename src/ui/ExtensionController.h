#pragma once
#if SUMMIT_MODERN_WEBKIT
#include "core/ExtensionCatalog.h"
#include <Handler.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class BWebKitContext;
namespace summit {
// Attach to BApplication. The controller serializes extension operations and
// retains its context until shutdown replies and preparation workers drain.
class ExtensionController final : public BHandler {
public:
    struct Entry {
        InstalledExtension installation;
        bool loaded = false;
        std::string baseURL;
        std::string error;
        bool installed = true;
        // chrome_url_overrides.newtab of the loaded extension, as an absolute URL.
        std::string newTabURL;
        // sidebar_action's (or side_panel's) page and title, for View › Sidebar.
        std::string sidebarURL, sidebarTitle;
        // How the engine started it again when its saved state was damaged.
        std::string notice;
    };
    // A new version of an installed extension, downloaded, staged and
    // prepared (ExtensionUpdater).
    struct Update {
        std::string identifier, token, name, version, fingerprint, source, updateURL;
        std::unique_ptr<StagedExtensionPackage> package;
        // Every required permission and site of the new version.
        std::vector<std::string> permissions, origins;
    };
    ExtensionController(std::shared_ptr<BWebKitContext>, std::filesystem::path catalogRoot,
        std::function<void()> changed = { });
    void Start();
    void Shutdown();
    bool HasPendingWork() const;
    void MessageReceived(BMessage*) override;
    const std::vector<Entry>& Entries() const { return fEntries; }
    const std::string& CatalogError() const { return fCatalogError; }
    bool IsReady() const;
    bool SetEnabled(const std::string& identifier, bool enabled);
    bool Remove(const std::string& identifier);
    // Lets the extension work in private windows (or stops it) at once, and
    // remembers the choice for the next start.
    bool SetAllowPrivateBrowsing(const std::string& identifier, bool allowed);
    // Runs the update in place of the installed version, which keeps its
    // settings and data; done hears whether it worked (with the reason).
    // False when busy or the extension is not running.
    bool ApplyUpdate(std::unique_ptr<Update>, std::function<void(bool, std::string)> done);
    // Change an installation's catalog record (update details) and the entry.
    bool ModifyInstallation(const std::string& identifier, const std::function<void(InstalledExtension&)>&);
    // What went wrong when an extension was last removed, or empty.
    const std::string& RemovalError() const { return fRemovalError; }
    void AddLoaded(InstalledExtension, std::string baseURL, std::string error = {}, bool installed = true,
        std::string newTabURL = {});
private:
    enum class Pending { None, Prepare, Load, Unload, RemoveData };
    enum class Operation { None, Enable, Disable, Remove, Update };
    void Next();
    void Fail(const std::string&);
    void Changed();
    void DiscardToken();
    void Advance();
    void FinishRemovalOrDisable();
    void FinishRemoval(status_t);
    void RecordPackageDetails(Entry&, const BMessage& prepared);
    void LoadUpdate();
    void UpdateFailed(const std::string&);
    // Give the prepared update up; the updater hears why.
    void DropUpdate(const std::string&);
    void CommitUpdate(const BMessage& loaded);
    std::shared_ptr<BWebKitContext> fContext;
    ExtensionCatalog fCatalog;
    std::function<void()> fChanged;
    std::vector<Entry> fEntries;
    std::string fCatalogError, fToken, fRemovalError;
    std::unique_ptr<Update> fUpdate;
    std::function<void(bool, std::string)> fUpdateDone;
    size_t fIndex = 0;
    uint64 fRequest = 0;
    Pending fPending = Pending::None;
    Operation fOperation = Operation::None;
    bool fStarted = false, fStopping = false;
};
}
#endif

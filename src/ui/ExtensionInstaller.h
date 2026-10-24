#pragma once
#if SUMMIT_MODERN_WEBKIT
#include "core/ExtensionCatalog.h"
#include "core/ExtensionStore.h"
#include <Handler.h>
#include <Message.h>
#include <functional>
#include <memory>
#include <string>
#include <thread>

class BWebKitContext;
class BMessageRunner;
namespace summit {
std::string ExtensionDisplayText(std::string);
class ExtensionInstaller final : public BHandler {
public:
    // entry, base URL, error, installed, new-tab override URL
    using Loaded = std::function<void(InstalledExtension, std::string, std::string, bool, std::string)>;
    ExtensionInstaller(std::shared_ptr<BWebKitContext>, std::filesystem::path, Loaded, std::function<void()> changed);
    ~ExtensionInstaller() override;
    void Start();
    bool Import(const std::filesystem::path&);
    bool ImportStoreURL(const std::string&);
    void Approve(uint64 generation, bool allowFiles, bool allowPrivate);
    void Cancel(uint64 generation);
    void Shutdown();
    bool IsBusy() const;
    bool HasPendingWork() const;
    uint64 Generation() const { return fGeneration; }
    BMessage Snapshot() const;
    void MessageReceived(BMessage*) override;
private:
    struct ImportWork;
    struct Draft;
    bool BeginImport(const std::filesystem::path&, std::optional<ExtensionStoreItem> = {});
    void Poll();
    void Changed();
    void Finish(std::string);
    void Rollback(std::string);
    void RetainUnsavedRuntime(std::string);
    // fPending is sent; Poll gives up on it after its deadline.
    void Await(uint32 operation);
    void PendingTimedOut();
    std::shared_ptr<BWebKitContext> fContext;
    ExtensionCatalog fCatalog;
    std::filesystem::path fDownloadRoot;
    Loaded fLoaded;
    std::function<void()> fChanged;
    std::shared_ptr<ImportWork> fWork;
    std::thread fThread;
    std::unique_ptr<BMessageRunner> fTimer;
    std::unique_ptr<Draft> fDraft;
    std::string fStatus;
    uint64 fGeneration = 0, fRequest = 0;
    uint32 fPending = 0;
    bigtime_t fPendingSince = 0;
    bool fStopping = false, fCancelled = false, fHadWork = false;
};
}
#endif

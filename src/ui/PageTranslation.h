#pragma once
#if SUMMIT_MODERN_WEBKIT
#include "core/TranslationModels.h"
#include <Handler.h>
#include <Messenger.h>
#include <filesystem>
#include <memory>
#include <string>

class BWindow;
class BMessageRunner;
class BWebKitView;
class BWebKitContext;

namespace summit {
constexpr uint32 kTranslationStatus = 'ptst';
constexpr uint32 kTranslatePage = 'ptgo';
constexpr uint32 kRestoreTranslation = 'ptre';
constexpr uint32 kDismissTranslation = 'ptdi';
constexpr uint32 kTranslationTick = 'pttk';

const std::string& TranslationDocumentScript();
const std::vector<TranslationModel>& AvailableTranslationModels();
std::string PreferredTranslationLanguage();
// Called by the application looper: WebKit contexts must be created there.
bool HandleTranslationContextMessage(BMessage*);
struct TranslationContextRequest;

// One local worker per browser window, in its own private WebKit context.
// All methods run on the owner window's looper. Downloads own their state
// independently and can finish cancellation after a window has closed.
class PageTranslator final : public BHandler {
public:
    explicit PageTranslator(BWindow& owner);
    ~PageTranslator() override;
    uint64 Prepare(const std::vector<TranslationModel>& route, const std::filesystem::path& cache);
    void Translate(uint64 epoch, const std::string& groups);
    void Cancel();
    bool Busy() const { return fBusy; }
    bool Ready() const { return fReady; }
    uint64 Job() const { return fJob; }
    void MessageReceived(BMessage*) override;
private:
    struct Work;
    void RequestContext();
    void StartView();
    void Report(const char* state, const std::string& text = {});
    void Resource(const BMessage&);
    BWindow& fOwner;
    BWindow* fWindow = nullptr;
    BWebKitView* fView = nullptr;
    BMessenger fViewMessenger;
    std::shared_ptr<BWebKitContext> fContext;
    std::shared_ptr<Work> fWork;
    std::shared_ptr<TranslationContextRequest> fContextRequest;
    std::unique_ptr<BMessageRunner> fTimer;
    uint64 fJob = 0;
    bigtime_t fDeadline = 0;
    bool fBusy = false, fReady = false;
    std::string fOrigin;
};
}
#endif

#include "PageTranslation.h"
#if SUMMIT_MODERN_WEBKIT
#include "ReaderMode.h"
#include "core/PageLanguage.h"
#include <WebKit/WebKitView.h>
#include <WebKit/WebKitContext.h>
#include <WebKit/WebKitEmbedding.h>
#include <Application.h>
#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <MessageRunner.h>
#include <Path.h>
#include <Resources.h>
#include <Roster.h>
#include <Window.h>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace summit {
struct TranslationContextRequest {
    std::mutex mutex;
    std::atomic<bool> cancelled {false};
    std::shared_ptr<BWebKitContext> context;
    BMessenger receiver;
    uint64 ticket = 0, job = 0;
};
namespace {
using json = nlohmann::json;
constexpr uint32 kCacheProgress = 'ptcp', kCacheReady = 'ptcr', kDeadline = 'ptdl';
constexpr uint64 kConfigure = 1, kTranslate = 2;
constexpr uint32 kCreateContext = 'ptcc', kContextReady = 'ptct';
std::mutex contextRequestsMutex;
std::map<uint64, std::weak_ptr<TranslationContextRequest>> contextRequests;
uint64 nextContextRequest = 0;
void Deliver(BMessenger receiver, BMessage message)
{
    const auto status = receiver.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
    if (status == B_WOULD_BLOCK || status == B_TIMED_OUT)
        BMessageRunner::StartSending(receiver, &message, 10000, 1);
}
std::string ResourceBytes(int32 id)
{
    app_info info;
    if (!be_app || be_app->GetAppInfo(&info) != B_OK) return {};
    BFile file(&info.ref, B_READ_ONLY);
    BResources resources(&file);
    size_t size = 0;
    const auto* bytes = static_cast<const char*>(resources.LoadResource('RAWT', id, &size));
    return bytes && size && size < 8 * 1024 * 1024 ? std::string(bytes, size) : std::string();
}

bool VerifiedFile(const std::filesystem::path& path, const TranslationModelFile& expected, const std::atomic<bool>& cancelled)
{
    std::error_code error;
    if (std::filesystem::file_size(path, error) != expected.size || error) return false;
    std::ifstream file(path, std::ios::binary);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!file || !digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) return false;
    std::array<char, 65536> buffer;
    uint64_t read = 0;
    while (file && !cancelled) {
        file.read(buffer.data(), buffer.size());
        const auto count = file.gcount(); read += count;
        if (count && EVP_DigestUpdate(digest.get(), buffer.data(), count) != 1) return false;
    }
    if (cancelled || !file.eof() || read != expected.size) return false;
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash;
    unsigned length = 0;
    if (EVP_DigestFinal_ex(digest.get(), hash.data(), &length) != 1 || length != 32) return false;
    std::string hex;
    for (unsigned i = 0; i < length; ++i) { hex += "0123456789abcdef"[hash[i] >> 4]; hex += "0123456789abcdef"[hash[i] & 15]; }
    return hex == expected.sha256;
}

struct Transfer {
    FILE* file;
    const std::atomic<bool>& cancelled;
    uint64_t count = 0, size = 0, preceding = 0, total = 0;
    BMessenger receiver;
    uint64 job;
    bigtime_t reported = 0;
    void Progress()
    {
        if (system_time() - reported < 250000) return;
        reported = system_time();
        BMessage message(kCacheProgress);
        message.AddUInt64("job", job); message.AddUInt64("bytes", preceding + count); message.AddUInt64("total", total);
        receiver.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
    }
};
size_t WriteModel(char* data, size_t size, size_t count, void* context)
{
    auto& transfer = *static_cast<Transfer*>(context);
    if (transfer.cancelled || (size && count > (transfer.size - transfer.count) / size)) return 0;
    const size_t bytes = size * count;
    if (std::fwrite(data, 1, bytes, transfer.file) != bytes) return 0;
    transfer.count += bytes; transfer.Progress();
    return bytes;
}
int TransferProgress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<Transfer*>(context)->cancelled ? 1 : 0;
}
void FetchModel(const TranslationModelFile& model, const std::filesystem::path& path, Transfer& transfer)
{
    static std::once_flag initialized;
    std::call_once(initialized, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("Could not start the language download.");
    auto* handle = curl.get();
    curl_easy_setopt(handle, CURLOPT_URL, model.url.c_str());
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "Summit language models");
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, WriteModel);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, TransferProgress);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    const auto result = curl_easy_perform(handle);
    long status = 0; curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    if (transfer.cancelled) throw std::runtime_error("Translation cancelled.");
    if (result != CURLE_OK || status != 200)
        throw std::runtime_error("Could not download language files. Check your connection and try again.");
    if (transfer.count != model.size || std::fflush(transfer.file)
        || !VerifiedFile(path, model, transfer.cancelled))
        throw std::runtime_error("The downloaded language file failed verification. Please try again.");
}
}

bool HandleTranslationContextMessage(BMessage* message)
{
    if (message->what != kCreateContext) return false;
    std::shared_ptr<TranslationContextRequest> request;
    {
        std::lock_guard lock(contextRequestsMutex);
        auto found = contextRequests.find(message->GetUInt64("ticket", 0));
        if (found == contextRequests.end()) return true;
        request = found->second.lock();
        contextRequests.erase(found);
    }
    if (!request || request->cancelled) return true;
    {
        std::lock_guard lock(request->mutex);
        request->context = std::make_shared<BWebKitContext>("", true);
    }
    BMessage ready(kContextReady); ready.AddUInt64("job", request->job);
    if (!request->cancelled) Deliver(request->receiver, ready);
    return true;
}

const std::string& TranslationDocumentScript()
{
    static const auto source = ResourceBytes(901);
    return source;
}
const std::vector<TranslationModel>& AvailableTranslationModels()
{
    static const auto models = ReadTranslationModels(ResourceBytes(907));
    return models;
}
std::string PreferredTranslationLanguage()
{
    BPath path;
    BFile file;
    BMessage settings;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK || path.Append("Locale settings") != B_OK
        || file.SetTo(path.Path(), B_READ_ONLY) != B_OK || settings.Unflatten(&file) != B_OK) return {};
    const char* language = nullptr;
    return settings.FindString("language", &language) == B_OK && language ? PageLanguageKey(language) : std::string();
}

struct PageTranslator::Work {
    std::atomic<bool> cancelled {false};
    std::vector<TranslationModel> route;
    std::map<std::string, TranslationModelFile> files;
    std::filesystem::path cache;
};
PageTranslator::PageTranslator(BWindow& owner) : BHandler("page translator"), fOwner(owner)
{
    fOwner.AddHandler(this);
    BMessage deadline(kDeadline);
    fTimer = std::make_unique<BMessageRunner>(BMessenger(this), &deadline, 1000000);
}
PageTranslator::~PageTranslator()
{
    fTimer.reset(); Cancel(); fOwner.RemoveHandler(this);
}
void PageTranslator::Cancel()
{
    if (fContextRequest) {
        fContextRequest->cancelled = true;
        std::lock_guard lock(contextRequestsMutex);
        contextRequests.erase(fContextRequest->ticket);
        fContextRequest.reset();
    }
    if (fWork) fWork->cancelled = true;
    fWork.reset(); fReady = false; fBusy = false; fDeadline = 0;
    fView = nullptr; fViewMessenger = BMessenger();
    if (fWindow && fWindow->Lock()) { auto* window = fWindow; fWindow = nullptr; window->Quit(); }
    fContext.reset();
}
void PageTranslator::Report(const char* state, const std::string& text)
{
    BMessage result(kTranslationStatus);
    result.AddUInt64("job", fJob); result.AddString("state", state); result.AddString("text", text.c_str());
    Deliver(BMessenger(&fOwner), result);
}
uint64 PageTranslator::Prepare(const std::vector<TranslationModel>& route, const std::filesystem::path& cache)
{
    Cancel(); ++fJob;
    if (route.empty() || route.size() > 2) { Report("error", "This language pair is not available for local translation."); return fJob; }
    fWork = std::make_shared<Work>(); fWork->route = route; fWork->cache = cache;
    for (const auto& model : route) for (const auto& file : model.files) fWork->files.emplace(file.sha256, file);
    fBusy = true; fDeadline = system_time() + 240000000;
    Report("downloading", "Preparing language files…");
    const auto job = fJob;
    const BMessenger receiver(this);
    std::thread([work = fWork, receiver, job] {
        BMessage result(kCacheReady); result.AddUInt64("job", job);
        try {
            std::filesystem::create_directories(work->cache);
            uint64_t total = 0, preceding = 0;
            for (const auto& [hash, model] : work->files) total += model.size;
            for (const auto& [hash, model] : work->files) {
                if (work->cancelled) return;
                const auto path = work->cache / hash;
                if (!VerifiedFile(path, model, work->cancelled)) {
                    auto temporary = (work->cache / (hash + ".part-XXXXXX")).string();
                    const int fd = mkstemp(temporary.data());
                    if (fd < 0) throw std::runtime_error("Could not create the language cache file.");
                    struct Remove { const std::string& path; ~Remove() { unlink(path.c_str()); } } remove {temporary};
                    std::unique_ptr<FILE, decltype(&std::fclose)> file(fdopen(fd, "w+b"), std::fclose);
                    if (!file) { close(fd); throw std::runtime_error("Could not write the language cache file."); }
                    Transfer transfer {file.get(), work->cancelled, 0, model.size, preceding, total, receiver, job};
                    FetchModel(model, temporary, transfer);
                    file.reset();
                    if (work->cancelled) return;
                    std::filesystem::rename(temporary, path);
                }
                preceding += model.size;
                BMessage progress(kCacheProgress); progress.AddUInt64("job", job);
                progress.AddUInt64("bytes", preceding); progress.AddUInt64("total", total);
                receiver.SendMessage(&progress, static_cast<BHandler*>(nullptr), 0);
            }
        } catch (const std::exception& error) { result.AddString("error", error.what()); }
        if (!work->cancelled) Deliver(receiver, result);
    }).detach();
    return fJob;
}

void PageTranslator::RequestContext()
{
    fContextRequest = std::make_shared<TranslationContextRequest>();
    fContextRequest->receiver = BMessenger(this); fContextRequest->job = fJob;
    {
        std::lock_guard lock(contextRequestsMutex);
        fContextRequest->ticket = ++nextContextRequest;
        contextRequests.emplace(fContextRequest->ticket, fContextRequest);
    }
    BMessage create(kCreateContext); create.AddUInt64("ticket", fContextRequest->ticket);
    Deliver(BMessenger(be_app), create);
}
void PageTranslator::StartView()
{
    // Reuse only the cryptographic random host generator, not Reader state.
    fOrigin = NewReaderURL();
    if (fOrigin.empty()) { Cancel(); Report("error", "Could not create a private translation session."); return; }
    fOrigin.replace(0, std::string("summit-reader").size(), "summit-translate");
    if (!fContext || fContext->InitCheck() != B_OK) {
        Cancel(); Report("error", "Could not create the local translator."); return;
    }
    fWindow = new BWindow(BRect(0, 0, 100, 100), "Summit local translation", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
    fView = new BWebKitView(fWindow->Bounds(), "translator", BMessenger(this), B_FOLLOW_ALL, fContext);
    if (fView->InitCheck() != B_OK) {
        delete fView; fView = nullptr; Cancel(); Report("error", "Could not open the local translator."); return;
    }
    fWindow->AddChild(fView); fView->Hide(); fViewMessenger = BMessenger(fView);
    fView->SetURLSchemeHandler("summit-translate", BMessenger(this), B_WEBKIT_SCHEME_SECURE | B_WEBKIT_SCHEME_CORS_ENABLED);
    fView->AddScriptMessageHandler("summitTranslation", BMessenger(this));
    fView->LoadURL((fOrigin + "index.html").c_str());
    fWindow->Run(); fDeadline = system_time() + 120000000;
    Report("downloading", "Loading local translator…");
}
void PageTranslator::Resource(const BMessage& message)
{
    const auto id = message.GetUInt64("identifier", 0);
    const std::string url = message.GetString("url", "");
    BMessenger view; message.FindMessenger("view", &view);
    if (!fWork || view != fViewMessenger || url.compare(0, fOrigin.size(), fOrigin)) {
        BWebKitView::FailURLSchemeRequest(id, "This translation session has ended."); return;
    }
    const auto path = url.substr(fOrigin.size());
    if (std::getenv("SUMMIT_TRACE_TRANSLATION")) std::fprintf(stderr, "Translation resource: %s\n", path.c_str());
    static const std::map<std::string, std::pair<int32, const char*>> resources {
        {"index.html", {903, "text/html; charset=utf-8"}}, {"service.js", {902, "text/javascript"}},
        {"bergamot/translator-worker.js", {904, "text/javascript"}},
        {"bergamot/bergamot-translator-worker.js", {905, "text/javascript"}},
        {"bergamot/bergamot-translator-worker.wasm", {906, "application/wasm"}}
    };
    std::string bytes;
    const char* mime = "application/octet-stream";
    if (auto it = resources.find(path); it != resources.end()) { bytes = ResourceBytes(it->second.first); mime = it->second.second; }
    else if (path.starts_with("models/") && fWork->files.contains(path.substr(7))) {
        const auto& model = fWork->files.at(path.substr(7));
        std::ifstream file(fWork->cache / model.sha256, std::ios::binary);
        bytes.resize(model.size);
        if (!file.read(bytes.data(), bytes.size()) || file.peek() != std::char_traits<char>::eof()) bytes.clear();
    }
    if (bytes.empty()) { BWebKitView::FailURLSchemeRequest(id, "The translation resource is unavailable."); return; }
    BMessage headers;
    headers.AddString("name", "Cache-Control"); headers.AddString("value", "no-store");
    headers.AddString("name", "Content-Security-Policy");
    headers.AddString("value", "default-src 'none'; script-src 'self' 'wasm-unsafe-eval'; worker-src 'self'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-src 'none'");
    BWebKitView::RespondToURLSchemeRequest(id, 200, mime, &headers, bytes.data(), bytes.size());
}
void PageTranslator::Translate(uint64 epoch, const std::string& groups)
{
    if (!fReady || fBusy || !fView || groups.size() > 500000) { Report("error", "The local translator is not ready."); return; }
    const auto data = json::parse(groups, nullptr, false);
    if (!data.is_array()) { Report("error", "Invalid page translation request."); return; }
    fBusy = true; fDeadline = system_time() + 150000000;
    const auto script = "return JSON.stringify(await summitTranslator.translate(" + std::to_string(epoch) + "," + data.dump() + "));";
    fView->EvaluateJavaScript(script.c_str(), BMessenger(this), kTranslate, true);
}
void PageTranslator::MessageReceived(BMessage* message)
{
    if (std::getenv("SUMMIT_TRACE_TRANSLATION") && message->what != kDeadline && message->what != kCacheProgress)
        std::fprintf(stderr, "Translation event: %08lx id=%llu\n", static_cast<unsigned long>(message->what),
            static_cast<unsigned long long>(message->GetUInt64("identifier", 0)));
    if (message->what == kDeadline) {
        if (fBusy && fDeadline && system_time() > fDeadline) { Cancel(); Report("error", "Local translation took too long. Please try again."); }
        return;
    }
    if (message->what == kCacheProgress || message->what == kCacheReady) {
        if (!fWork || message->GetUInt64("job", 0) != fJob) return;
        if (message->what == kCacheProgress) {
            const auto bytes = message->GetUInt64("bytes", 0), total = message->GetUInt64("total", 0);
            Report("downloading", "Preparing language files: " + std::to_string(bytes / 1000000) + " of " + std::to_string((total + 999999) / 1000000) + " MB…");
            fDeadline = system_time() + 240000000;
        } else if (message->HasString("error")) { const std::string error = message->GetString("error", ""); Cancel(); Report("error", error); }
        else RequestContext();
        return;
    }
    if (message->what == kContextReady) {
        if (!fWork || !fContextRequest || message->GetUInt64("job", 0) != fJob) return;
        {
            std::lock_guard lock(fContextRequest->mutex);
            fContext = fContextRequest->context;
        }
        fContextRequest.reset();
        StartView(); return;
    }
    if (message->what == B_WEBKIT_URL_SCHEME_REQUEST) { Resource(*message); return; }
    BMessenger view; message->FindMessenger("view", &view);
    if (std::getenv("SUMMIT_TRACE_TRANSLATION") && message->what == B_WEBKIT_SCRIPT_MESSAGE)
        std::fprintf(stderr, "Translation ready: view=%d main=%d url=%d\n", view == fViewMessenger,
            message->GetBool("main_frame", false), message->GetString("frame_url", "") == fOrigin + "index.html");
    if (view != fViewMessenger || !fWork) return;
    if (message->what == B_WEBKIT_SCRIPT_MESSAGE) {
        const auto token = message->GetUInt64("reply_token", 0);
        if (!message->GetBool("main_frame", false) || message->GetString("frame_url", "") != fOrigin + "index.html") {
            BWebKitView::ReplyToScriptMessage(token, "", "Unknown translation document."); return;
        }
        BWebKitView::ReplyToScriptMessage(token, "ok");
        json specification = json::array();
        for (const auto& model : fWork->route) {
            json files = json::array();
            for (const auto& file : model.files) files.push_back({{"part", file.part}, {"sha256", file.sha256}, {"size", file.size}});
            specification.push_back({{"from", model.from}, {"to", model.to}, {"files", files}, {"config", {{"gemm-precision", model.precision}}}});
        }
        const auto script = "return JSON.stringify(await summitTranslator.configure(" + specification.dump() + "));";
        fView->EvaluateJavaScript(script.c_str(), BMessenger(this), kConfigure, true);
    } else if (message->what == B_WEBKIT_JAVASCRIPT_RESULT) {
        if (message->HasString("error")) { Cancel(); Report("error", "The local translator could not process this page. Please try again."); return; }
        fBusy = false; fDeadline = 0;
        if (message->GetUInt64("identifier", 0) == kConfigure) { fReady = true; Report("ready"); }
        else Report("result", message->GetString("result", ""));
    } else if (message->what == B_WEBKIT_PROCESS_EXITED) { Cancel(); Report("error", "The local translation process stopped. Please try again."); }
}
}
#endif

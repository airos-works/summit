#include "ExtensionStoreDownload.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <unistd.h>

namespace summit {
namespace {
struct Transfer {
    const std::atomic<bool>& cancelled;
    FILE* file = nullptr;
    std::string text;
    size_t count = 0, limit;
    bool tooLarge = false;
    long status = 0;
};
size_t Write(char* data, size_t size, size_t count, void* context)
{
    auto& transfer = *static_cast<Transfer*>(context);
    if (transfer.cancelled) return 0;
    if (size && count > (transfer.limit - transfer.count) / size) { transfer.tooLarge = true; return 0; }
    const size_t bytes = size * count;
    // Exceptions must never unwind through libcurl's C stack.
    try {
        if (transfer.file) {
            if (std::fwrite(data, 1, bytes, transfer.file) != bytes) return 0;
        } else transfer.text.append(data, bytes);
    } catch (...) { return 0; }
    transfer.count += bytes;
    return bytes;
}
int Progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<Transfer*>(context)->cancelled ? 1 : 0;
}
void Get(const std::string& url, Transfer& transfer)
{
    static std::once_flag initialized;
    std::call_once(initialized, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("Could not start the extension download.");
    auto* handle = curl.get();
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "Summit extension installer");
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, Write);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, Progress);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    const auto result = curl_easy_perform(handle);
    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    transfer.status = status;
    if (transfer.cancelled) throw std::runtime_error("Installation cancelled.");
    if (transfer.tooLarge) throw std::runtime_error("The store response exceeds the extension size limit.");
    if (result != CURLE_OK) throw std::runtime_error(std::string("The extension could not be downloaded: ") + curl_easy_strerror(result));
    if (status != 200 || !transfer.count) throw std::runtime_error("The store did not provide an extension package.");
}
struct TemporaryDirectory {
    std::filesystem::path path;
    ~TemporaryDirectory() { if (!path.empty()) { std::error_code ignored; std::filesystem::remove_all(path, ignored); } }
};
std::string SHA256(FILE* file)
{
    std::rewind(file);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("Could not check the extension hash.");
    std::array<unsigned char, 65536> bytes;
    while (size_t count = std::fread(bytes.data(), 1, bytes.size(), file))
        if (EVP_DigestUpdate(digest.get(), bytes.data(), count) != 1) throw std::runtime_error("Could not check the extension hash.");
    if (std::ferror(file)) throw std::runtime_error("Could not read the downloaded package.");
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash;
    unsigned length = 0;
    if (EVP_DigestFinal_ex(digest.get(), hash.data(), &length) != 1 || length != 32)
        throw std::runtime_error("Could not check the extension hash.");
    std::string result = "sha256:";
    for (unsigned i = 0; i < length; ++i) {
        result += "0123456789abcdef"[hash[i] >> 4];
        result += "0123456789abcdef"[hash[i] & 15];
    }
    return result;
}
}
std::unique_ptr<StagedExtensionPackage> DownloadStoreExtension(const ExtensionStoreItem& item,
    const std::filesystem::path& temporaryRoot, const ExtensionCatalog& catalog, const std::atomic<bool>& cancelled,
    std::string& expectedIdentity, std::string& error)
{
    try {
        if (cancelled) throw std::runtime_error("Installation cancelled.");
        auto packageURL = item.requestURL;
        std::string hash;
        uint64_t expectedSize = 0;
        if (item.store == ExtensionStoreItem::Store::Firefox) {
            Transfer metadata { cancelled, nullptr, {}, 0, 2 * 1024 * 1024 };
            Get(item.requestURL, metadata);
            const auto addon = nlohmann::json::parse(metadata.text);
            if (addon.at("type") != "extension" || addon.value("is_disabled", false))
                throw std::runtime_error("This store item is not an available browser extension.");
            const auto& file = addon.at("current_version").at("file");
            if (file.at("status") != "public") throw std::runtime_error("This extension version is not publicly available.");
            packageURL = file.at("url").get<std::string>();
            constexpr std::string_view prefix = "https://addons.mozilla.org/firefox/downloads/file/";
            if (packageURL.compare(0, prefix.size(), prefix) != 0)
                throw std::runtime_error("Mozilla returned an unexpected package address.");
            expectedIdentity = addon.at("guid").get<std::string>();
            hash = file.at("hash").get<std::string>();
            expectedSize = file.at("size").get<uint64_t>();
            if (hash.size() != 71 || hash.compare(0, 7, "sha256:") != 0 || !expectedSize || expectedSize > 128 * 1024 * 1024)
                throw std::runtime_error("Mozilla returned invalid package verification data.");
        } else expectedIdentity = item.identifier;
        std::filesystem::create_directories(temporaryRoot);
        auto pattern = (temporaryRoot / "store-XXXXXX").string();
        if (!mkdtemp(pattern.data())) throw std::runtime_error(std::strerror(errno));
        TemporaryDirectory temporary { pattern };
        const auto archive = temporary.path / (item.store == ExtensionStoreItem::Store::Chrome ? "extension.crx" : "extension.xpi");
        std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(archive.c_str(), "w+b"), std::fclose);
        if (!file) throw std::runtime_error(std::strerror(errno));
        Transfer package { cancelled, file.get(), {}, 0, 128 * 1024 * 1024 };
        Get(packageURL, package);
        if (std::fflush(file.get())) throw std::runtime_error("Could not save the downloaded extension.");
        if (!hash.empty() && (package.count != expectedSize || SHA256(file.get()) != hash))
            throw std::runtime_error("The extension did not match Mozilla's published size and SHA-256 hash.");
        if (cancelled) throw std::runtime_error("Installation cancelled.");
        file.reset();
        return catalog.Stage(archive, error);
    } catch (const std::exception& exception) {
        error = exception.what();
        return nullptr;
    }
}

bool ResolveFirefoxStoreItem(const ExtensionStoreItem& item, std::string& identifier, std::string& name, bool& extension,
    std::string& error)
{
    static const std::atomic<bool> never { false };
    try {
        if (item.store != ExtensionStoreItem::Store::Firefox) throw std::runtime_error("Not a Firefox Add-ons item.");
        Transfer metadata { never, nullptr, {}, 0, 2 * 1024 * 1024 };
        Get(item.requestURL + "?lang=en-US", metadata);
        const auto addon = nlohmann::json::parse(metadata.text);
        identifier = addon.at("guid").get<std::string>();
        extension = addon.value("type", "") == "extension";
        // A translated field: one string, or one per language.
        const auto& names = addon.at("name");
        if (names.is_string()) name = names.get<std::string>();
        else if (names.is_object() && names.contains("en-US") && names["en-US"].is_string()) name = names["en-US"].get<std::string>();
        else if (names.is_object() && !names.empty() && names.begin()->is_string()) name = names.begin()->get<std::string>();
        return !identifier.empty();
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool FetchStoreText(const std::string& url, size_t limit, const std::atomic<bool>& cancelled, std::string& text,
    std::string& error, long* status)
{
    Transfer transfer { cancelled, nullptr, {}, 0, limit };
    try {
        Get(url, transfer);
        text = std::move(transfer.text);
        if (status) *status = transfer.status;
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        if (status) *status = transfer.status;
        return false;
    }
}
}

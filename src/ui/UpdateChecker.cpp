#include "UpdateChecker.h"
#include "Messages.h"
#include "core/BuildTarget.h"
#include "core/DownloadText.h"
#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <FindDirectory.h>
#include <Invoker.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <Node.h>
#include <OS.h>
#include <Path.h>
#include <ScrollView.h>
#include <StatusBar.h>
#include <StringView.h>
#include <TextView.h>
#include <Window.h>
#include <image.h>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace summit {
using json = nlohmann::json;

namespace {
constexpr uint32 kCheckDone = 'upcd', kTick = 'uptk', kOpenRelease = 'upal';
// The update window: its buttons, the install thread's reports, and the
// application's answer when it did not restart.
constexpr uint32 kInstall = 'upin', kShowChanges = 'upsc', kInstallProgress = 'uppg', kInstallDone = 'updn',
    kRestartCancelled = 'uprc', kActivate = 'upac';
constexpr const char* kRepositoryAPI = "https://api.github.com/repos/jmgasper/summit";
constexpr const char* kDownloadPrefix = "https://github.com/jmgasper/summit/releases/download/";

// --- What runs ---

std::string AppImagePath()
{
    int32 cookie = 0;
    image_info image;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK)
        if (image.type == B_APP_IMAGE) return image.name;
    return { };
}

std::string LibraryPath(const char* fileName)
{
    int32 cookie = 0;
    image_info image;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
        const char* slash = std::strrchr(image.name, '/');
        if (!std::strcmp(slash ? slash + 1 : image.name, fileName)) return image.name;
    }
    return { };
}

std::string Resolved(const std::string& path)
{
    std::error_code error;
    auto real = std::filesystem::canonical(path, error);
    return error ? path : real.string();
}

// A string attribute, as packagefs gives every file of a package
// ("SYS:PACKAGE": "summit-0.1.0~git20261019.0340-1", "SYS:PACKAGE_FILE").
std::string Attribute(const std::string& path, const char* name)
{
    BNode node(path.c_str());
    char value[B_PATH_NAME_LENGTH] = "";
    const ssize_t length = node.InitCheck() == B_OK ? node.ReadAttr(name, B_STRING_TYPE, 0, value, sizeof(value) - 1) : -1;
    return length > 0 ? std::string(value, strnlen(value, size_t(length))) : std::string();
}

std::string Tail(const std::string& text, size_t limit)
{
    if (text.size() <= limit) return text;
    const size_t start = text.find('\n', text.size() - limit);
    return "…" + text.substr(start == std::string::npos ? text.size() - limit : start);
}

// Runs a /bin/sh command line with the system's library path (a test copy's
// LIBRARY_PATH names its own libraries) and returns its output (stdout and
// stderr), its exit status in status (-1 when it did not finish in time).
// The shell is started with load_image(): posix_spawn() with redirections
// forks this large, many-threaded process, which failed on Haiku R1/beta6.
// So its output goes to a temporary file, which its exit status ends: the
// engine's process reaper may collect the shell before waitpid() here could.
std::string RunShell(const std::string& command, bigtime_t timeout, int& status)
{
    status = -1;
    constexpr const char* marker = "summit-update-status=";
    BPath temporary;
    std::string file = std::string(find_directory(B_SYSTEM_TEMP_DIRECTORY, &temporary) == B_OK ? temporary.Path() : "/tmp")
        + "/summit-run-XXXXXX";
    const int fd = mkstemp(file.data());
    if (fd < 0) return "no temporary file: " + std::string(std::strerror(errno));
    close(fd);
    struct Remove {
        const std::string& path;
        ~Remove() { unlink(path.c_str()); }
    } remove { file };
    const std::string script = "unset LIBRARY_PATH LD_PRELOAD; exec </dev/null >" + ShellQuote(file) + " 2>&1\n" + command
        + "\nstatus=$?; echo; echo \"" + marker + "$status\"";
    const char* argv[] = { "/bin/sh", "-c", script.c_str(), nullptr };
    const thread_id shell = load_image(3, argv, const_cast<const char**>(environ));
    if (shell < 0) return "could not start /bin/sh: " + std::string(std::strerror(shell));
    resume_thread(shell);
    // The team's main thread is gone once the shell has exited.
    const bigtime_t deadline = system_time() + timeout;
    thread_info info;
    while (get_thread_info(shell, &info) == B_OK && system_time() < deadline) snooze(20000);
    const bool finished = get_thread_info(shell, &info) != B_OK;
    if (!finished) kill_team(shell);
    std::ifstream in(file, std::ios::binary);
    std::string output((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!finished) return Tail(output, 256 * 1024) + "\n(stopped: it took too long)";
    if (const size_t at = output.rfind(marker); at != std::string::npos) {
        status = std::atoi(output.c_str() + at + std::strlen(marker));
        output.resize(at);
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    }
    return output;
}

InstalledBuild DetectBuild()
{
    InstalledBuild build;
    build.executable = AppImagePath();
    if (const char* simulated = std::getenv("SUMMIT_UPDATE_INSTALLED"); simulated && *simulated) {
        if (!ParseSimulatedBuild(simulated, build))
            build.problem = "SUMMIT_UPDATE_INSTALLED does not name a summit and a summit_webkit package.";
        build.packagesDirectory = "/boot/system/packages";
        return build;
    }
    const std::string executable = Resolved(build.executable);
    const std::string engine = Resolved(LibraryPath("libWebKit.so.1"));
    if (auto package = ParseVersionedPackage(Attribute(executable, "SYS:PACKAGE")); package && package->name == "summit")
        build.summit = package->version;
    if (auto package = ParseVersionedPackage(Attribute(engine, "SYS:PACKAGE")); package && package->name == "summit_webkit")
        build.webkit = package->version;
    if (!build.summit) {
        build.problem = "This copy of Summit (" + executable + ") was not installed from a package.";
        return build;
    }
    if (!build.webkit) {
        build.problem = "Summit's engine (" + engine + ") was not installed from the summit_webkit package.";
        return build;
    }
    build.packageFile = Attribute(executable, "SYS:PACKAGE_FILE");
    const std::string engineFile = Attribute(engine, "SYS:PACKAGE_FILE");
    std::error_code error;
    for (auto [which, home] : {std::pair { B_SYSTEM_PACKAGES_DIRECTORY, false }, std::pair { B_USER_PACKAGES_DIRECTORY, true }}) {
        BPath directory;
        if (find_directory(which, &directory) != B_OK || build.packageFile.empty()) continue;
        if (std::filesystem::exists(std::filesystem::path(directory.Path()) / build.packageFile, error)) {
            build.packagesDirectory = directory.Path();
            build.home = home;
            break;
        }
    }
    if (build.packagesDirectory.empty()) {
        build.problem = "Summit's package file (" + build.packageFile + ") is in neither the system's nor the home folder's packages.";
        return build;
    }
    // Both are replaced in one transaction, so they must be in one place.
    if (engineFile.empty() || !std::filesystem::exists(std::filesystem::path(build.packagesDirectory) / engineFile, error)) {
        build.problem = "Summit and its engine are installed in different places (the engine's package " + engineFile
            + " is not in " + build.packagesDirectory + ").";
        return build;
    }
    int status = 0;
    const auto output = RunShell("package list -i "
        + ShellQuote((std::filesystem::path(build.packagesDirectory) / build.packageFile).string()), 30000000, status);
    build.commit = CommitFromDescription(ParsePackageInfo(output).description);
    if (build.commit.empty())
        build.commitProblem = "package list -i (status " + std::to_string(status) + "): " + Tail(output, 400);
    return build;
}

bool DryRun()
{
    const char* value = std::getenv("SUMMIT_UPDATE_DRY_RUN");
    return (value && *value && std::strcmp(value, "0")) || CurrentBuild().simulated;
}

std::string ReleaseAPI()
{
    const char* api = std::getenv("SUMMIT_UPDATE_API");
    return api && *api ? api : kRepositoryAPI;
}

std::string LongDate(std::string_view iso)
{
    // "2026-10-19T03:50:42Z" -> "19 October 2026"
    static const char* const months[] = {"January", "February", "March", "April", "May", "June", "July", "August",
        "September", "October", "November", "December"};
    if (iso.size() < 10 || iso[4] != '-' || iso[7] != '-') return { };
    const int month = std::atoi(std::string(iso.substr(5, 2)).c_str());
    const int day = std::atoi(std::string(iso.substr(8, 2)).c_str());
    if (month < 1 || month > 12 || day < 1 || day > 31) return { };
    return std::to_string(day) + " " + months[month - 1] + " " + std::string(iso.substr(0, 4));
}

std::string BuildName(const PackageVersion& version, const std::string& commit)
{
    return version.ToString() + (commit.empty() ? "" : " (" + commit.substr(0, 7) + ")");
}

std::string SystemName()
{
    return std::string(SUMMIT_TARGET_NAME) + " (" + PackageArchitecture() + ")";
}

// --- GitHub ---

struct Response {
    long status = 0;
    std::string body;
    // Why nothing came back (no network, a timeout), or empty.
    std::string error;
    std::string remaining, retryAfter;
    int64_t reset = 0;
    bool cancelled = false;
};

struct Transfer {
    Response& response;
    size_t limit;
    const std::atomic<bool>* cancelled;
    FILE* file = nullptr;
    EVP_MD_CTX* digest = nullptr;
    uint64_t received = 0;
    std::function<void(uint64_t)> progress {};
    bigtime_t lastProgress = 0;
    bool tooLarge = false, writeFailed = false;
};

size_t WriteBody(char* data, size_t size, size_t count, void* context)
{
    auto& transfer = *static_cast<Transfer*>(context);
    const size_t bytes = size * count;
    if (transfer.cancelled && *transfer.cancelled) return 0;
    if (transfer.received + bytes > transfer.limit) { transfer.tooLarge = true; return 0; }
    // Exceptions must never unwind through libcurl's C stack.
    try {
        if (transfer.file) {
            if (std::fwrite(data, 1, bytes, transfer.file) != bytes) { transfer.writeFailed = true; return 0; }
            if (transfer.digest && EVP_DigestUpdate(transfer.digest, data, bytes) != 1) { transfer.writeFailed = true; return 0; }
        } else transfer.response.body.append(data, bytes);
    } catch (...) { return 0; }
    transfer.received += bytes;
    if (transfer.progress && system_time() - transfer.lastProgress > 150000) {
        transfer.lastProgress = system_time();
        transfer.progress(transfer.received);
    }
    return bytes;
}

size_t WriteHeader(char* data, size_t size, size_t count, void* context)
{
    auto& response = static_cast<Transfer*>(context)->response;
    const std::string_view line(data, size * count);
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) return size * count;
    std::string name(line.substr(0, colon));
    for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
    std::string value(line.substr(colon + 1));
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(0, 1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    if (name == "x-ratelimit-remaining") response.remaining = value;
    else if (name == "x-ratelimit-reset") response.reset = std::atoll(value.c_str());
    else if (name == "retry-after") response.retryAfter = value;
    return size * count;
}

int TransferProgress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto& transfer = *static_cast<Transfer*>(context);
    return transfer.cancelled && *transfer.cancelled ? 1 : 0;
}

std::string UserAgent()
{
    const auto& build = CurrentBuild();
    return "Summit/" + (build.summit ? build.summit->ToString() : std::string("development")) + " (" SUMMIT_TARGET_NAME "; "
        + PackageArchitecture() + "; update check)";
}

// One GET: the API's JSON (api true) or a file. Status and body come back
// whatever the status; error says why there was no answer at all.
void Fetch(const std::string& url, bool api, Transfer& transfer)
{
    static std::once_flag initialized;
    std::call_once(initialized, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    auto& response = transfer.response;
    if (!curl) {
        response.error = "libcurl did not start";
        return;
    }
    auto* handle = curl.get();
    const std::string agent = UserAgent();
    // A test server named by SUMMIT_UPDATE_API may be plain HTTP; GitHub never is.
    const char* protocols = std::getenv("SUMMIT_UPDATE_API") ? "http,https" : "https";
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_USERAGENT, agent.c_str());
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, protocols);
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 20L);
    if (api) curl_easy_setopt(handle, CURLOPT_TIMEOUT, 60L);
    // A download may take a while on a slow line, but not stall.
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, WriteBody);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, WriteHeader);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &transfer);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, TransferProgress);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, curl_slist_free_all);
    if (api) {
        curl_slist* list = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
        list = curl_slist_append(list, "X-GitHub-Api-Version: 2022-11-28");
        headers.reset(list);
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.get());
    }
    const CURLcode result = curl_easy_perform(handle);
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);
    if (transfer.cancelled && *transfer.cancelled) {
        response.cancelled = true;
        response.error = "cancelled";
    } else if (transfer.tooLarge) response.error = "the answer was larger than expected";
    else if (transfer.writeFailed) response.error = "the file could not be written (" + std::string(std::strerror(errno)) + ")";
    else if (result != CURLE_OK) response.error = curl_easy_strerror(result);
}

// A small answer kept in memory: the API's JSON, or a text file of a release (api false).
Response Get(const std::string& url, size_t limit, bool api = true)
{
    Response response;
    Transfer transfer { response, limit, nullptr };
    Fetch(url, api, transfer);
    return response;
}

std::string Hex(const unsigned char* bytes, unsigned length)
{
    std::string text;
    for (unsigned i = 0; i < length; ++i) {
        text += "0123456789abcdef"[bytes[i] >> 4];
        text += "0123456789abcdef"[bytes[i] & 15];
    }
    return text;
}

// Downloads a release file to path and checks its size and (when the release
// gives one) its SHA-256. Throws with what went wrong.
void Download(const ReleasePackage& package, const std::filesystem::path& path, const std::string& checksum,
    const std::atomic<bool>& cancelled, const std::function<void(uint64_t)>& progress)
{
    std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path.c_str(), "wb"), std::fclose);
    if (!file) throw std::runtime_error("Could not save " + package.fileName + ": " + std::strerror(errno));
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("Could not check the downloaded packages.");
    Response response;
    Transfer transfer { response, size_t(package.asset.size) + 1, &cancelled, file.get(), digest.get() };
    transfer.progress = progress;
    Fetch(package.asset.url, false, transfer);
    if (response.cancelled) throw std::runtime_error("cancelled");
    if (!response.error.empty()) throw std::runtime_error(package.fileName + " could not be downloaded: " + response.error + ".");
    if (response.status != 200)
        throw std::runtime_error(package.fileName + " could not be downloaded (HTTP status " + std::to_string(response.status) + ").");
    if (std::fflush(file.get()) != 0 || std::ferror(file.get()))
        throw std::runtime_error("Could not save " + package.fileName + ": " + std::strerror(errno));
    if (transfer.received != package.asset.size)
        throw std::runtime_error(package.fileName + " has " + std::to_string(transfer.received) + " bytes, but the release says "
            + std::to_string(package.asset.size) + ".");
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash;
    unsigned length = 0;
    if (EVP_DigestFinal_ex(digest.get(), hash.data(), &length) != 1 || length != 32)
        throw std::runtime_error("Could not check the downloaded packages.");
    if (!checksum.empty() && Hex(hash.data(), length) != checksum)
        throw std::runtime_error(package.fileName + " does not have the SHA-256 checksum the release gives: it is not the published file.");
    progress(transfer.received);
}

struct TemporaryDirectory {
    std::filesystem::path path;
    ~TemporaryDirectory()
    {
        if (path.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
}

// --- Checking ---

struct UpdateChecker::Job {
    enum class Outcome { Failed, NotInstalled, UpToDate, Newer, Available };
    std::atomic<bool> manual { false };
    InstalledBuild installed;
    Outcome outcome = Outcome::Failed;
    std::string error;
    // GitHub answered the release request (the check counts for the day).
    bool answered = false;
    std::optional<Release> release;
    std::optional<ReleaseBuild> build;
    std::optional<Comparison> comparison;
    std::string changesError;
    int64_t finishedAt = 0;
};

namespace {
const char* OutcomeName(UpdateChecker::Job::Outcome outcome)
{
    using Outcome = UpdateChecker::Job::Outcome;
    switch (outcome) {
        case Outcome::Failed: return "failed";
        case Outcome::NotInstalled: return "not-installed";
        case Outcome::UpToDate: return "up-to-date";
        case Outcome::Newer: return "newer";
        case Outcome::Available: return "available";
    }
    return "";
}

void RunCheck(UpdateChecker::Job& job)
{
    using Outcome = UpdateChecker::Job::Outcome;
    job.installed = CurrentBuild();
    // Copies that do not come from packages are not checked by themselves.
    if (!job.installed.FromPackages() && !job.manual) {
        job.outcome = Outcome::NotInstalled;
        return;
    }
    const std::string api = ReleaseAPI();
    const Response response = Get(api + "/releases/tags/" + ReleaseTag(SUMMIT_AIROS), 4 * 1024 * 1024);
    if (!response.error.empty() && !response.status) {
        job.error = "Summit could not reach GitHub (" + response.error + "). Check the network connection and try again.";
        return;
    }
    job.answered = true;
    if (response.status != 200 || !response.error.empty()) {
        job.error = response.status == 200 ? "GitHub's answer could not be read (" + response.error + ")."
            : DescribeGitHubFailure(response.status, response.remaining, response.reset, response.retryAfter, response.body,
                std::time(nullptr));
        return;
    }
    job.release = ParseRelease(response.body, job.error);
    if (!job.release) return;
    // A test server (SUMMIT_UPDATE_API) may serve its packages over plain HTTP.
    job.build = ChooseReleaseBuild(*job.release, PackageArchitecture(), job.error, !std::getenv("SUMMIT_UPDATE_API"));
    if (!job.build) return;
    if (!std::getenv("SUMMIT_UPDATE_API")) {
        for (const auto* package : {&job.build->summit, &job.build->webkit})
            if (package->asset.url.rfind(kDownloadPrefix, 0) != 0) {
                job.error = "GitHub gave an unexpected address for " + package->fileName + ".";
                job.build.reset();
                return;
            }
    }
    if (!job.installed.FromPackages()) {
        job.outcome = Outcome::NotInstalled;
        return;
    }
    switch (CompareBuilds(job.installed, *job.build)) {
        case UpdateStatus::UpToDate: job.outcome = Outcome::UpToDate; return;
        case UpdateStatus::Newer: job.outcome = Outcome::Newer; return;
        case UpdateStatus::Available: job.outcome = Outcome::Available; break;
    }
    // What changed: the commits between the two builds.
    if (job.installed.commit.empty() || job.build->commit.empty()) return;
    const Response changes = Get(api + "/compare/" + job.installed.commit + "..." + job.build->commit + "?per_page=100",
        24 * 1024 * 1024);
    if (!changes.error.empty()) job.changesError = changes.error;
    else if (changes.status != 200)
        job.changesError = DescribeGitHubFailure(changes.status, changes.remaining, changes.reset, changes.retryAfter, changes.body,
            std::time(nullptr));
    else job.comparison = ParseComparison(changes.body, job.changesError);
}

json BuildJSON(const InstalledBuild& build)
{
    return {{"executable", build.executable}, {"summit", build.summit ? build.summit->ToString() : ""},
        {"summitWebKit", build.webkit ? build.webkit->ToString() : ""}, {"commit", build.commit}, {"commitProblem", build.commitProblem},
        {"packageFile", build.packageFile}, {"packagesDirectory", build.packagesDirectory}, {"home", build.home},
        {"simulated", build.simulated}, {"fromPackages", build.FromPackages()}, {"problem", build.problem}};
}

// --- The update window ---

struct Offer {
    InstalledBuild installed;
    ReleaseBuild build;
    std::string publishedAt, releaseURL, changesURL, changes;
    bool dryRun = false;
};

void Report(const BMessenger& window, const std::string& text, float fraction)
{
    BMessage progress(kInstallProgress);
    progress.AddString("text", text.c_str());
    progress.AddFloat("fraction", fraction);
    window.SendMessage(&progress);
}

// Downloads, checks and installs the two packages, then tells the window
// (kInstallDone: "error", or "detail" of a test run).
void InstallBuild(Offer offer, BMessenger window, std::shared_ptr<std::atomic<bool>> cancelled)
{
    std::string error, detail;
    // pkgman finished its transaction: from here a failure leaves the system changed.
    bool changed = false;
    try {
        BPath temporaryRoot;
        if (find_directory(B_SYSTEM_TEMP_DIRECTORY, &temporaryRoot) != B_OK) throw std::runtime_error("There is no temporary folder.");
        std::string pattern = std::string(temporaryRoot.Path()) + "/summit-update-XXXXXX";
        if (!mkdtemp(pattern.data())) throw std::runtime_error("Could not make a temporary folder: " + std::string(std::strerror(errno)));
        TemporaryDirectory temporary { pattern };
        auto& build = offer.build;
        const uint64_t total = build.summit.asset.size + build.webkit.asset.size;
        // The files, then pkgman's copy of them in the packages folder.
        struct statvfs volume {};
        if (statvfs(temporary.path.c_str(), &volume) == 0) {
            const uint64_t available = uint64_t(volume.f_bavail) * volume.f_frsize, needed = 2 * total + 64 * 1024 * 1024;
            if (available < needed)
                throw std::runtime_error("There is not enough free space on the disk: the update needs " + FormatByteSize(needed)
                    + " and " + FormatByteSize(available) + " is free.");
        }
        if (build.checksumFile) {
            Report(window, "Downloading the checksums…", 0);
            const Response sums = Get(build.checksumFile->url, 1024 * 1024, false);
            if (!sums.error.empty() || sums.status != 200)
                throw std::runtime_error("The release's SHA256SUMS could not be downloaded" + (sums.error.empty() ? "." : ": " + sums.error + "."));
            AddChecksums(build, ParseChecksums(sums.body));
        }
        uint64_t done = 0;
        std::vector<std::filesystem::path> files;
        for (const auto* package : {&build.webkit, &build.summit}) {
            std::string problem;
            const std::string checksum = ExpectedChecksum(*package, problem);
            if (!problem.empty()) throw std::runtime_error(problem);
            const auto path = temporary.path / package->fileName;
            const std::string name = package->name;
            Download(*package, path, checksum, *cancelled, [&](uint64_t received) {
                Report(window, "Downloading " + name + " (" + FormatByteProgress(done + received, total) + ")…",
                    total ? float(double(done + received) / double(total)) : 0);
            });
            done += package->asset.size;
            files.push_back(path);
        }
        if (*cancelled) throw std::runtime_error("cancelled");
        // What each package says it is.
        Report(window, "Checking the packages…", 1);
        size_t index = 0;
        for (const auto* package : {&build.webkit, &build.summit}) {
            int status = 0;
            const auto output = RunShell("package list -i " + ShellQuote(files[index++].string()), 60000000, status);
            if (status != 0) throw std::runtime_error(package->fileName + " is not a package that can be read:\n" + Tail(output, 600));
            const auto problem = CheckDownloadedPackage(ParsePackageInfo(output), *package, PackageArchitecture(), SUMMIT_AIROS);
            if (!problem.empty()) throw std::runtime_error(problem);
        }
        if (*cancelled) throw std::runtime_error("cancelled");
        // Both in one transaction: never one without the other.
        const std::string install = std::string("pkgman install -y ") + (offer.installed.home ? "-H " : "")
            + ShellQuote(files[0].string()) + " " + ShellQuote(files[1].string());
        if (offer.dryRun) {
            Report(window, "Test mode: unpacking the packages into a scratch folder…", 1);
            const auto scratch = temporary.path / "scratch";
            std::filesystem::create_directories(scratch);
            for (const auto& file : files) {
                int status = 0;
                const auto output = RunShell("package extract -C " + ShellQuote(scratch.string()) + " " + ShellQuote(file.string()),
                    300000000, status);
                if (status != 0) throw std::runtime_error(file.filename().string() + " could not be unpacked:\n" + Tail(output, 600));
            }
            std::error_code ignored;
            for (const char* needed : {"apps/Summit", "lib/summit-webkit/lib/libWebKit.so.1", "lib/summit-webkit/WebProcess"})
                if (!std::filesystem::exists(scratch / needed, ignored))
                    throw std::runtime_error(std::string("The unpacked packages have no ") + needed + ".");
            detail = "Test mode: both packages were downloaded, checked and unpacked into " + scratch.string()
                + "; pkgman was not run. It would have run: " + install;
        } else {
            Report(window, "Installing the packages with pkgman…", 1);
            int status = 0;
            const auto output = RunShell(install, 20 * 60 * 1000000LL, status);
            std::fprintf(stderr, "Summit update: %s (status %d)\n%s\n", install.c_str(), status, Tail(output, 4000).c_str());
            if (status != 0)
                throw std::runtime_error("pkgman could not install the packages" + std::string(status < 0 ? " in time" : "")
                    + ":\n" + Tail(output, 900));
            changed = true;
            // The running executable's file is now the new package's.
            const auto now = ParseVersionedPackage(Attribute(Resolved(offer.installed.executable), "SYS:PACKAGE"));
            if (!now || now->name != "summit" || ComparePackageVersions(now->version, build.summit.version) != 0)
                throw std::runtime_error("pkgman finished, but Summit's files still come from "
                    + (now ? now->name + " " + now->version.ToString() : std::string("no package")) + ".");
            detail = "Installed with: " + install;
        }
        std::fprintf(stderr, "Summit update: %s\n", detail.c_str());
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    BMessage finished(kInstallDone);
    if (!error.empty()) finished.AddString("error", error.c_str());
    finished.AddString("detail", detail.c_str());
    finished.AddBool("changed", changed);
    window.SendMessage(&finished);
}
}

struct UpdateChecker::WindowState {
    mutable std::mutex mutex;
    std::string phase = "ready", status, detail, version;
    float fraction = 0;
};

namespace {
class UpdateWindow final : public BWindow {
public:
    UpdateWindow(Offer offer, std::shared_ptr<UpdateChecker::WindowState> state)
        : BWindow(BRect(0, 0, 560, 420), "Summit Update", B_TITLED_WINDOW,
            B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE | B_NOT_ZOOMABLE)
        , fOffer(std::move(offer))
        , fState(std::move(state))
    {
        auto* heading = new BStringView("update-heading", fOffer.dryRun ? "A new build of Summit is available (test mode)"
            : "A new build of Summit is available");
        heading->SetFont(be_bold_font);
        const auto& build = fOffer.build;
        const auto date = LongDate(fOffer.publishedAt);
        auto* release = new BStringView("update-new", ("Summit " + BuildName(build.summit.version, build.commit)
            + (date.empty() ? "" : ", built " + date) + ", for " + SystemName() + ".").c_str());
        const auto& installed = fOffer.installed;
        std::string yours = "You have " + (installed.summit ? BuildName(*installed.summit, installed.commit) : std::string("an unknown build"));
        if (installed.webkit && installed.summit && !SameBuild(*installed.webkit, *installed.summit))
            yours += " with Summit WebKit " + installed.webkit->ToString();
        auto* current = new BStringView("update-current", (yours + ".").c_str());
        auto* changesTitle = new BStringView("update-changes-title", "What changed:");
        changesTitle->SetFont(be_bold_font);
        fChanges = new BTextView("update-changes");
        fChanges->MakeEditable(false);
        fChanges->MakeSelectable(true);
        fChanges->SetWordWrap(true);
        fChanges->SetInsets(10, 8, 10, 8);
        fChanges->SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
        fChanges->SetLowUIColor(B_DOCUMENT_BACKGROUND_COLOR);
        auto color = ui_color(B_DOCUMENT_TEXT_COLOR);
        fChanges->SetFontAndColor(be_plain_font, B_FONT_ALL, &color);
        fChanges->SetText(fOffer.changes.c_str());
        auto* scroll = new BScrollView("update-changes-scroll", fChanges, 0, false, true);
        scroll->SetExplicitMinSize(BSize(500, 200));
        fStatus = new BStringView("update-status", "");
        // Its text changes as the update goes on; the window keeps its width.
        fStatus->SetExplicitMinSize(BSize(200, B_SIZE_UNSET));
        fStatus->SetExplicitPreferredSize(BSize(540, B_SIZE_UNSET));
        fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
        fStatus->SetTruncation(B_TRUNCATE_END);
        fProgress = new BStatusBar("update-progress");
        fProgress->SetMaxValue(1);
        fProgress->SetBarHeight(10);
        fProgress->Hide();
        fShowChanges = new BButton("update-show-changes", "Show on GitHub", new BMessage(kShowChanges));
        fLater = new BButton("update-later", "Later", new BMessage(B_QUIT_REQUESTED));
        fInstall = new BButton("update-install", "Install and Restart", new BMessage(kInstall));
        BLayoutBuilder::Group<>(this, B_VERTICAL, 8).SetInsets(16)
            .Add(heading)
            .Add(release)
            .Add(current)
            .AddStrut(4)
            .Add(changesTitle)
            .Add(scroll)
            .Add(fStatus)
            .Add(fProgress)
            .AddGroup(B_HORIZONTAL)
                .Add(fShowChanges)
                .AddGlue()
                .Add(fLater)
                .Add(fInstall)
            .End();
        SetDefaultButton(fInstall);
        Ready();
    }
    // Over the browser window in front (as Preferences opens), or in the
    // middle of the screen.
    void Place(BRect over)
    {
        UpdateSizeLimits();
        ResizeToPreferred();
        if (!over.IsValid()) {
            CenterOnScreen();
            return;
        }
        const BRect frame = Frame();
        MoveTo(std::round(over.left + (over.Width() - frame.Width()) / 2),
            std::round(over.top + std::max(0.0f, (over.Height() - frame.Height()) / 3)));
    }
    void MessageReceived(BMessage* message) override
    {
        switch (message->what) {
            case kInstall:
                if (fPhase != Phase::Ready && fPhase != Phase::Failed) break;
                fCancelled = std::make_shared<std::atomic<bool>>(false);
                SetPhase(Phase::Downloading, "Starting the download…");
                fProgress->Show();
                fProgress->SetTo(0);
                std::thread(InstallBuild, fOffer, BMessenger(this), fCancelled).detach();
                break;
            case kShowChanges: {
                BMessage open(kOpenPageInTab);
                open.AddString("url", (fOffer.changesURL.empty() ? fOffer.releaseURL : fOffer.changesURL).c_str());
                be_app->PostMessage(&open);
                break;
            }
            case kInstallProgress: {
                if (fPhase != Phase::Downloading && fPhase != Phase::Installing) break;
                const std::string text = message->GetString("text", "");
                const float fraction = message->GetFloat("fraction", 0);
                // Past the downloads nothing can be cancelled.
                const bool installing = text.rfind("Downloading", 0) != 0;
                SetPhase(installing ? Phase::Installing : Phase::Downloading, text, fraction);
                fProgress->SetTo(fraction * fProgress->MaxValue());
                break;
            }
            case kInstallDone: {
                const char* error = nullptr;
                const std::string detail = message->GetString("detail", "");
                if (message->FindString("error", &error) == B_OK) {
                    fProgress->Hide();
                    if (fCancelled && *fCancelled) {
                        if (fClosing) {
                            {
                                std::lock_guard lock(fState->mutex);
                                fState->phase = "closed";
                            }
                            Quit();
                            return;
                        }
                        Ready("The update was cancelled. Nothing was installed.");
                        break;
                    }
                    std::fprintf(stderr, "Summit update: failed: %s\n", error);
                    {
                        std::lock_guard lock(fState->mutex);
                        fState->detail = error;
                    }
                    const std::string summary = message->GetBool("changed", false)
                        ? "pkgman installed the packages, but the new build is not in place."
                        : "The update did not complete. Summit is as it was.";
                    SetPhase(Phase::Failed, summary, 0);
                    // The reason can be pkgman's output: more than the status line holds.
                    (new BAlert("Summit Update", (summary + "\n\n" + error).c_str(), "OK", nullptr, nullptr, B_WIDTH_AS_USUAL,
                        B_WARNING_ALERT))->Go(nullptr);
                    break;
                }
                {
                    std::lock_guard lock(fState->mutex);
                    fState->detail = detail;
                }
                SetPhase(Phase::Restarting, fOffer.dryRun ? "Test run finished; restarting Summit…"
                    : "The new build is installed. Restarting Summit…", 1);
                be_app->PostMessage(kUpdateRestart);
                break;
            }
            case kActivate:
                Activate();
                break;
            case kRestartCancelled:
                if (fPhase == Phase::Restarting)
                    SetPhase(Phase::Installed, "Summit did not restart: a page kept its window open. The new build starts "
                        "the next time Summit starts.", 1);
                break;
            default:
                BWindow::MessageReceived(message);
        }
    }
    bool QuitRequested() override
    {
        // pkgman cannot be stopped halfway; a download can.
        if (fPhase == Phase::Installing) return false;
        if (fPhase == Phase::Downloading && fCancelled) {
            *fCancelled = true;
            fClosing = true;
            return false;
        }
        std::lock_guard lock(fState->mutex);
        fState->phase = "closed";
        return true;
    }

private:
    enum class Phase { Ready, Downloading, Installing, Failed, Restarting, Installed };
    static const char* PhaseName(Phase phase)
    {
        switch (phase) {
            case Phase::Ready: return "ready";
            case Phase::Downloading: return "downloading";
            case Phase::Installing: return "installing";
            case Phase::Failed: return "failed";
            case Phase::Restarting: return "restarting";
            case Phase::Installed: return "installed";
        }
        return "";
    }
    void Ready(const std::string& note = std::string())
    {
        const uint64_t size = fOffer.build.summit.asset.size + fOffer.build.webkit.asset.size;
        std::string text = note;
        if (text.empty())
            text = fOffer.dryRun ? "Test mode: the packages are checked and unpacked into a scratch folder, not installed."
                : "Installs both packages (" + FormatByteSize(size) + "), then Summit restarts with its windows and tabs.";
        SetPhase(Phase::Ready, text, 0);
    }
    void SetPhase(Phase phase, const std::string& text, float fraction = 0)
    {
        fPhase = phase;
        fStatus->SetText(text.c_str());
        fStatus->SetToolTip(text.c_str());
        fStatus->SetHighUIColor(phase == Phase::Failed ? B_FAILURE_COLOR : B_PANEL_TEXT_COLOR);
        const bool busy = phase == Phase::Downloading || phase == Phase::Installing || phase == Phase::Restarting;
        fInstall->SetLabel(phase == Phase::Failed ? "Try Again" : "Install and Restart");
        fInstall->SetEnabled(!busy && phase != Phase::Installed);
        fLater->SetLabel(phase == Phase::Downloading ? "Cancel" : phase == Phase::Ready ? "Later" : "Close");
        fLater->SetEnabled(phase != Phase::Installing && phase != Phase::Restarting);
        std::lock_guard lock(fState->mutex);
        fState->phase = PhaseName(phase);
        fState->status = text;
        fState->fraction = fraction;
        fState->version = fOffer.build.summit.version.ToString();
    }
    Offer fOffer;
    std::shared_ptr<UpdateChecker::WindowState> fState;
    std::shared_ptr<std::atomic<bool>> fCancelled;
    Phase fPhase = Phase::Ready;
    bool fClosing = false;
    BTextView* fChanges;
    BStringView* fStatus;
    BStatusBar* fProgress;
    BButton* fShowChanges;
    BButton* fLater;
    BButton* fInstall;
};
}

// --- The checker ---

const InstalledBuild& CurrentBuild()
{
    static InstalledBuild build;
    static std::once_flag detected;
    std::call_once(detected, [] { build = DetectBuild(); });
    return build;
}

std::string BuildDescription()
{
    const auto& build = CurrentBuild();
    if (!build.summit) return "Not installed from a package";
    std::string text = "Summit " + BuildName(*build.summit, build.commit);
    if (build.webkit) text += "\nSummit WebKit " + build.webkit->ToString();
    if (build.simulated) text += "\n(pretended by SUMMIT_UPDATE_INSTALLED)";
    return text;
}

UpdateChecker::UpdateChecker(std::function<Settings()> settings, std::function<void(int64_t)> checked,
    std::function<BRect()> frontFrame)
    : BHandler("update checker")
    , fSettings(std::move(settings))
    , fChecked(std::move(checked))
    , fFrontFrame(std::move(frontFrame))
    , fWindowState(std::make_shared<WindowState>())
{
}

UpdateChecker::~UpdateChecker() = default;

void UpdateChecker::Start()
{
    // A while after starting, then every hour: is the day's check due?
    fFirst = std::make_unique<BMessageRunner>(BMessenger(this), BMessage(kTick), 30000000, 1);
    fHourly = std::make_unique<BMessageRunner>(BMessenger(this), BMessage(kTick), 3600000000LL);
}

void UpdateChecker::Check(bool manual)
{
    if (fJob) {
        // The check under way says what it finds.
        if (manual) fJob->manual = true;
        return;
    }
    if (fWindow.IsValid()) {
        // An update window is open: a manual check brings it forward, unless
        // it is idle and a newer build may have come out since.
        std::lock_guard lock(fWindowState->mutex);
        if (fWindowState->phase != "ready" && fWindowState->phase != "failed") {
            if (manual) fWindow.SendMessage(kActivate);
            return;
        }
    }
    auto job = std::make_shared<Job>();
    job->manual = manual;
    fJob = job;
    const BMessenger target(this);
    std::thread([job, target] {
        RunCheck(*job);
        job->finishedAt = std::time(nullptr);
        target.SendMessage(kCheckDone);
    }).detach();
}

void UpdateChecker::RestartCancelled()
{
    if (fWindow.IsValid()) fWindow.SendMessage(kRestartCancelled);
}

void UpdateChecker::Finished()
{
    using Outcome = Job::Outcome;
    auto job = std::exchange(fJob, nullptr);
    if (!job) return;
    fLast = job;
    if (job->answered) fChecked(job->finishedAt);
    const bool manual = job->manual;
    const auto& build = job->build;
    auto alert = [](const std::string& text, alert_type type = B_INFO_ALERT) {
        (new BAlert("Summit Update", text.c_str(), "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, type))->Go(nullptr);
    };
    switch (job->outcome) {
        case Outcome::Failed:
            std::fprintf(stderr, "Summit update check: %s\n", job->error.c_str());
            if (manual) alert("Summit could not check for a new build.\n\n" + job->error, B_WARNING_ALERT);
            return;
        case Outcome::NotInstalled: {
            if (!manual) return;
            std::string text = job->installed.problem + " It cannot update itself.";
            if (build) {
                const auto date = LongDate(job->release->publishedAt);
                text += "\n\nThe latest build for " + SystemName() + " is " + BuildName(build->summit.version, build->commit)
                    + (date.empty() ? "" : ", from " + date) + ". To use it, install its summit and summit_webkit packages "
                    "from the release page.";
            }
            auto* message = new BMessage(kOpenRelease);
            message->AddString("url", job->release ? job->release->htmlURL.c_str() : "https://github.com/jmgasper/summit/releases");
            auto* prompt = new BAlert("Summit Update", text.c_str(), "Open Release Page", "OK");
            prompt->SetShortcut(1, B_ESCAPE);
            prompt->Go(new BInvoker(message, this));
            return;
        }
        case Outcome::UpToDate:
            if (manual)
                alert("Summit is up to date.\n\nYou have the latest build for " + SystemName() + ":\n"
                    + BuildName(*job->installed.summit, job->installed.commit.empty() ? build->commit : job->installed.commit));
            return;
        case Outcome::Newer:
            if (manual)
                alert("This build of Summit is newer than the latest release for " + SystemName() + ".\n\nYours: "
                    + BuildName(*job->installed.summit, job->installed.commit) + "\nRelease: " + BuildName(build->summit.version, build->commit));
            return;
        case Outcome::Available:
            ShowWindow();
            return;
    }
}

void UpdateChecker::ShowWindow()
{
    const auto& job = *fLast;
    Offer offer;
    offer.installed = job.installed;
    offer.build = *job.build;
    offer.publishedAt = job.release->publishedAt;
    offer.releaseURL = job.release->htmlURL;
    offer.dryRun = DryRun();
    if (job.comparison) {
        offer.changes = FormatChanges(*job.comparison);
        offer.changesURL = job.comparison->htmlURL;
    } else if (job.installed.commit.empty())
        offer.changes = "Your build's package does not say which commit it was built from, so its changes are not known. "
            "Show on GitHub opens the release.";
    else offer.changes = "The list of changes could not be loaded: " + job.changesError;
    if (fWindow.IsValid()) {
        // Replaces an idle window, which may show an older build.
        BMessenger old = fWindow;
        old.SendMessage(B_QUIT_REQUESTED);
    }
    auto* window = new UpdateWindow(std::move(offer), fWindowState = std::make_shared<WindowState>());
    const BRect front = fFrontFrame ? fFrontFrame() : BRect();
    window->Lock();
    window->Place(front);
    window->Unlock();
    fWindow = BMessenger(window);
    window->Show();
}

void UpdateChecker::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kCheckDone:
            Finished();
            break;
        case kTick: {
            const auto settings = fSettings();
            if (settings.enabled && UpdateCheckDue(settings.lastCheck, std::time(nullptr))) Check(false);
            break;
        }
        case kOpenRelease:
            if (message->GetInt32("which", 1) == 0) {
                BMessage open(kOpenPageInTab);
                open.AddString("url", message->GetString("url", ""));
                be_app->PostMessage(&open);
            }
            break;
        default:
            BHandler::MessageReceived(message);
    }
}

json UpdateChecker::State() const
{
    const auto settings = fSettings();
    json state = {{"installed", BuildJSON(CurrentBuild())}, {"target", SUMMIT_TARGET_NAME}, {"architecture", PackageArchitecture()},
        {"release", ReleaseTag(SUMMIT_AIROS)}, {"api", ReleaseAPI()}, {"dryRun", DryRun()},
        {"dailyCheck", settings.enabled}, {"lastCheck", settings.lastCheck},
        {"checkDue", UpdateCheckDue(settings.lastCheck, std::time(nullptr))}, {"checking", bool(fJob)}};
    if (fLast) {
        json last = {{"outcome", OutcomeName(fLast->outcome)}, {"manual", bool(fLast->manual)}, {"answered", fLast->answered},
            {"finishedAt", fLast->finishedAt}};
        if (!fLast->error.empty()) last["error"] = fLast->error;
        if (fLast->build) {
            last["summit"] = fLast->build->summit.version.ToString();
            last["summitWebKit"] = fLast->build->webkit.version.ToString();
            last["commit"] = fLast->build->commit;
            last["files"] = {fLast->build->webkit.fileName, fLast->build->summit.fileName};
        }
        if (fLast->comparison) last["changes"] = FormatChanges(*fLast->comparison);
        if (!fLast->changesError.empty()) last["changesError"] = fLast->changesError;
        state["last"] = last;
    }
    {
        std::lock_guard lock(fWindowState->mutex);
        state["window"] = {{"open", fWindow.IsValid()}, {"phase", fWindowState->phase}, {"status", fWindowState->status},
            {"fraction", fWindowState->fraction}, {"detail", fWindowState->detail}, {"version", fWindowState->version}};
    }
    return state;
}

bool UpdateChecker::Answer(const std::string& button)
{
    if (!fWindow.IsValid()) return false;
    if (button == "install") return fWindow.SendMessage(kInstall) == B_OK;
    if (button == "changes") return fWindow.SendMessage(kShowChanges) == B_OK;
    if (button == "later") return fWindow.SendMessage(B_QUIT_REQUESTED) == B_OK;
    return false;
}
}

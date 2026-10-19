#include "UpdateCheck.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace summit {
namespace {
using json = nlohmann::json;

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsHex(char c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
char Lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
bool StartsWith(std::string_view text, std::string_view prefix) { return text.substr(0, prefix.size()) == prefix; }
bool EndsWith(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

bool IsHexText(std::string_view text, size_t minimum, size_t maximum)
{
    return text.size() >= minimum && text.size() <= maximum && std::all_of(text.begin(), text.end(), IsHex);
}

std::string LowerText(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), Lower);
    return result;
}

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

int CaseCompare(std::string_view a, std::string_view b)
{
    const size_t count = std::min(a.size(), b.size());
    for (size_t i = 0; i < count; ++i) {
        const char x = Lower(a[i]), y = Lower(b[i]);
        if (x != y) return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
}

// The hex word (7 to 40 characters) right after prefix, wherever prefix
// first comes with one; empty if it never does.
std::string HexAfter(std::string_view text, std::string_view prefix)
{
    for (size_t at = text.find(prefix); at != std::string_view::npos; at = text.find(prefix, at + 1)) {
        size_t end = at + prefix.size();
        while (end < text.size() && IsHex(text[end])) ++end;
        const auto word = text.substr(at + prefix.size(), end - at - prefix.size());
        const bool bounded = end == text.size() || !std::isalnum(static_cast<unsigned char>(text[end]));
        if (bounded && IsHexText(word, 7, 40)) return LowerText(word);
    }
    return { };
}

std::string String(const json& object, const char* key)
{
    auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

// "summit-0.1.0~git20261019.0340-1-x86_64.hpkg" -> name, version, architecture.
struct FileParts {
    std::string name, version, architecture;
};
std::optional<FileParts> SplitPackageFileName(std::string_view file)
{
    constexpr std::string_view suffix = ".hpkg";
    if (file.size() <= suffix.size() || !EndsWith(file, suffix)) return std::nullopt;
    file.remove_suffix(suffix.size());
    const size_t lastDash = file.rfind('-'), firstDash = file.find('-');
    if (lastDash == std::string_view::npos || firstDash == lastDash || firstDash == 0) return std::nullopt;
    return FileParts { std::string(file.substr(0, firstDash)), std::string(file.substr(firstDash + 1, lastDash - firstDash - 1)),
        std::string(file.substr(lastDash + 1)) };
}

// A release file's name without its notes: GitHub wrote '.' for the '~'
// before the pre-release, which is the first part after the major version
// that starts with a letter ("0.1.0.git20261019.0340-1").
std::string GuessPackageFileName(std::string_view asset)
{
    auto parts = SplitPackageFileName(asset);
    if (!parts) return std::string(asset);
    std::string version = parts->version, revision;
    if (const size_t dash = version.rfind('-'); dash != std::string::npos) {
        revision = version.substr(dash);
        version.resize(dash);
    }
    if (version.find('~') == std::string::npos) {
        size_t start = version.find('.');
        while (start != std::string::npos && start + 1 < version.size()) {
            if (!IsDigit(version[start + 1])) {
                version[start] = '~';
                break;
            }
            start = version.find('.', start + 1);
        }
    }
    return parts->name + "-" + version + revision + "-" + parts->architecture + ".hpkg";
}

std::string ShortDate(std::string_view iso)
{
    // "2026-10-19T03:40:44Z" -> "19 Oct"
    static const char* const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (iso.size() < 10 || iso[4] != '-' || iso[7] != '-') return { };
    const int month = std::atoi(std::string(iso.substr(5, 2)).c_str());
    const int day = std::atoi(std::string(iso.substr(8, 2)).c_str());
    if (month < 1 || month > 12 || day < 1 || day > 31) return { };
    return std::to_string(day) + " " + months[month - 1];
}
}

std::optional<PackageVersion> PackageVersion::Parse(std::string_view text)
{
    PackageVersion version;
    text = Trim(text);
    if (text.empty() || text.size() > 255) return std::nullopt;
    for (char c : text)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '~' && c != '-' && c != '+')
            return std::nullopt;
    if (const size_t dash = text.rfind('-'); dash != std::string_view::npos) {
        const auto revision = text.substr(dash + 1);
        if (revision.empty() || revision.size() > 9 || !std::all_of(revision.begin(), revision.end(), IsDigit))
            return std::nullopt;
        version.revision = static_cast<uint32_t>(std::strtoul(std::string(revision).c_str(), nullptr, 10));
        text = text.substr(0, dash);
    }
    if (const size_t tilde = text.find('~'); tilde != std::string_view::npos) {
        version.preRelease = text.substr(tilde + 1);
        if (version.preRelease.empty() || version.preRelease.find('~') != std::string::npos) return std::nullopt;
        text = text.substr(0, tilde);
    }
    if (text.find('-') != std::string_view::npos) return std::nullopt;
    const size_t first = text.find('.');
    version.major = text.substr(0, first);
    if (first != std::string_view::npos) {
        text = text.substr(first + 1);
        const size_t second = text.find('.');
        version.minor = text.substr(0, second);
        if (second != std::string_view::npos) version.micro = text.substr(second + 1);
    }
    if (version.major.empty()) return std::nullopt;
    return version;
}

std::string PackageVersion::ToString() const
{
    std::string text = major;
    if (!minor.empty() || !micro.empty()) text += "." + minor;
    if (!micro.empty()) text += "." + micro;
    if (!preRelease.empty()) text += "~" + preRelease;
    if (revision) text += "-" + std::to_string(revision);
    return text;
}

int NaturalCompare(std::string_view a, std::string_view b)
{
    size_t i = 0, j = 0;
    while (true) {
        if (i == a.size()) return j == b.size() ? 0 : -1;
        if (j == b.size()) return 1;
        const bool digitA = IsDigit(a[i]), digitB = IsDigit(b[j]);
        // Different kinds of chunk: the rest compares as text.
        if (digitA != digitB) return CaseCompare(a.substr(i), b.substr(j));
        size_t endA = i, endB = j;
        while (endA < a.size() && IsDigit(a[endA]) == digitA) ++endA;
        while (endB < b.size() && IsDigit(b[endB]) == digitB) ++endB;
        auto chunkA = a.substr(i, endA - i), chunkB = b.substr(j, endB - j);
        if (digitA) {
            while (chunkA.size() > 1 && chunkA.front() == '0') chunkA.remove_prefix(1);
            while (chunkB.size() > 1 && chunkB.front() == '0') chunkB.remove_prefix(1);
            if (chunkA.size() != chunkB.size()) return chunkA.size() < chunkB.size() ? -1 : 1;
            if (const int order = chunkA.compare(chunkB)) return order < 0 ? -1 : 1;
        } else if (const int order = CaseCompare(chunkA, chunkB)) return order;
        i = endA;
        j = endB;
    }
}

int ComparePackageVersions(const PackageVersion& a, const PackageVersion& b)
{
    if (int order = NaturalCompare(a.major, b.major)) return order;
    if (int order = NaturalCompare(a.minor, b.minor)) return order;
    if (int order = NaturalCompare(a.micro, b.micro)) return order;
    // A pre-release comes before the release itself.
    if (a.preRelease.empty() != b.preRelease.empty()) return a.preRelease.empty() ? 1 : -1;
    if (int order = NaturalCompare(a.preRelease, b.preRelease)) return order;
    return a.revision == b.revision ? 0 : a.revision < b.revision ? -1 : 1;
}

bool SameBuild(const PackageVersion& a, const PackageVersion& b)
{
    return a.preRelease == b.preRelease && a.revision == b.revision;
}

std::optional<VersionedPackage> ParseVersionedPackage(std::string_view text)
{
    text = Trim(text);
    const size_t dash = text.find('-');
    if (dash == 0 || dash == std::string_view::npos) return std::nullopt;
    const auto name = text.substr(0, dash);
    for (char c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != '+') return std::nullopt;
    auto version = PackageVersion::Parse(text.substr(dash + 1));
    if (!version) return std::nullopt;
    return VersionedPackage { std::string(name), *version };
}

bool ParseSimulatedBuild(std::string_view text, InstalledBuild& build)
{
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = text.find_first_of(" ,\t\n", at);
        const auto word = text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        at = end == std::string_view::npos ? text.size() : end + 1;
        if (word.empty()) continue;
        if (auto package = ParseVersionedPackage(word)) {
            if (package->name == "summit") build.summit = package->version;
            else if (package->name == "summit_webkit") build.webkit = package->version;
        } else if (IsHexText(word, 7, 40)) build.commit = LowerText(word);
    }
    build.simulated = true;
    return build.summit && build.webkit;
}

std::string CommitFromDescription(std::string_view description)
{
    if (auto commit = HexAfter(description, "from Summit "); !commit.empty()) return commit;
    return HexAfter(description, "of Summit ");
}

std::optional<Release> ParseRelease(const std::string& text, std::string& error)
{
    try {
        const auto document = json::parse(text);
        if (!document.is_object()) throw std::runtime_error("not an object");
        Release release;
        release.tag = String(document, "tag_name");
        release.name = String(document, "name");
        release.body = String(document, "body");
        release.htmlURL = String(document, "html_url");
        release.publishedAt = String(document, "published_at");
        if (release.tag.empty()) throw std::runtime_error("no tag");
        // CI points the release at the commit it built.
        if (const auto target = String(document, "target_commitish"); IsHexText(target, 7, 40)) release.commit = LowerText(target);
        else if (auto named = HexAfter(release.name, "("); !named.empty()) release.commit = named;
        else release.commit = HexAfter(release.body, "from ");
        if (auto assets = document.find("assets"); assets != document.end() && assets->is_array()) {
            for (const auto& item : *assets) {
                if (!item.is_object()) continue;
                ReleaseAsset asset;
                asset.name = String(item, "name");
                asset.url = String(item, "browser_download_url");
                if (auto size = item.find("size"); size != item.end() && size->is_number_unsigned()) asset.size = size->get<uint64_t>();
                if (const auto digest = String(item, "digest"); StartsWith(digest, "sha256:") && IsHexText(digest.substr(7), 64, 64))
                    asset.sha256 = LowerText(digest.substr(7));
                if (!asset.name.empty() && !asset.url.empty()) release.assets.push_back(std::move(asset));
            }
        }
        return release;
    } catch (const std::exception& exception) {
        error = std::string("GitHub's answer could not be read (") + exception.what() + ").";
        return std::nullopt;
    }
}

const char* ReleaseTag(bool airos)
{
    return airos ? "latest" : "latest-haiku";
}

const char* PackageArchitecture()
{
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__aarch64__)
    return "arm64";
#elif defined(__riscv)
    return "riscv64";
#elif defined(__i386__) && __GNUC__ <= 2
    return "x86_gcc2";
#elif defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

std::map<std::string, std::string> ParseChecksums(std::string_view text)
{
    std::map<std::string, std::string> sums;
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = text.find('\n', at);
        auto line = Trim(text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        at = end == std::string_view::npos ? text.size() : end + 1;
        if (line.empty()) continue;
        if (line.front() == '|') {
            // "| NAME | `HASH` |": a file name cell and a hash cell.
            std::string name, hash;
            size_t cell = 1;
            while (cell < line.size()) {
                const size_t next = line.find('|', cell);
                auto value = Trim(line.substr(cell, next == std::string_view::npos ? std::string_view::npos : next - cell));
                cell = next == std::string_view::npos ? line.size() : next + 1;
                while (!value.empty() && value.front() == '`') value.remove_prefix(1);
                while (!value.empty() && value.back() == '`') value.remove_suffix(1);
                if (IsHexText(value, 64, 64)) hash = LowerText(value);
                else if (value.find(' ') == std::string_view::npos && value.size() > 5 && EndsWith(value, ".hpkg")) name = value;
            }
            if (!name.empty() && !hash.empty()) sums[name] = hash;
            continue;
        }
        // "HASH  NAME" or "HASH *NAME" (binary mode), as sha256sum writes them.
        const size_t space = line.find_first_of(" \t");
        if (space == std::string_view::npos || !IsHexText(line.substr(0, space), 64, 64)) continue;
        auto name = Trim(line.substr(space));
        if (!name.empty() && name.front() == '*') name.remove_prefix(1);
        if (!name.empty() && name.find_first_of(" \t/") == std::string_view::npos)
            sums[std::string(name)] = LowerText(line.substr(0, space));
    }
    return sums;
}

std::string AssetName(std::string_view packageFileName)
{
    std::string name(packageFileName);
    std::replace(name.begin(), name.end(), '~', '.');
    return name;
}

std::optional<ReleaseBuild> ChooseReleaseBuild(const Release& release, std::string_view architecture, std::string& error,
    bool requireHTTPS)
{
    // The notes name each package as it is named itself (with '~').
    const auto notes = ParseChecksums(release.body);
    std::map<std::string, std::string> fileOfAsset;
    for (const auto& [file, hash] : notes) fileOfAsset[AssetName(file)] = file;
    std::optional<ReleasePackage> summit, webkit;
    std::optional<ReleaseAsset> checksumFile;
    for (const auto& asset : release.assets) {
        const auto lower = LowerText(asset.name);
        if (lower == "sha256sums" || lower == "sha256sums.txt") {
            checksumFile = asset;
            continue;
        }
        const auto known = fileOfAsset.find(asset.name);
        const std::string file = known != fileOfAsset.end() ? known->second : GuessPackageFileName(asset.name);
        const auto parts = SplitPackageFileName(file);
        if (!parts || parts->architecture != architecture || (parts->name != "summit" && parts->name != "summit_webkit")) continue;
        const auto version = PackageVersion::Parse(parts->version);
        if (!version) continue;
        ReleasePackage package { parts->name, *version, asset, file, { } };
        if (!asset.sha256.empty()) package.checksums.push_back(asset.sha256);
        if (auto hash = notes.find(file); hash != notes.end()) package.checksums.push_back(hash->second);
        auto& slot = parts->name == "summit" ? summit : webkit;
        if (!slot || ComparePackageVersions(slot->version, package.version) < 0) slot = std::move(package);
    }
    const std::string where = "The release \"" + release.tag + "\"";
    if (!summit || !webkit) {
        error = where + " has no " + (summit ? "summit_webkit" : "summit") + " package for " + std::string(architecture) + ".";
        return std::nullopt;
    }
    if (!SameBuild(summit->version, webkit->version)) {
        error = where + "'s packages come from different builds (summit " + summit->version.ToString() + ", summit_webkit "
            + webkit->version.ToString() + "), so Summit does not install them.";
        return std::nullopt;
    }
    for (const auto* package : {&*summit, &*webkit}) {
        const bool secure = StartsWith(package->asset.url, "https://") || (!requireHTTPS && StartsWith(package->asset.url, "http://"));
        if (!package->asset.size || !secure) {
            error = where + " lists " + package->asset.name + " without a size or a secure address.";
            return std::nullopt;
        }
    }
    ReleaseBuild build { std::move(*summit), std::move(*webkit), release.commit, std::move(checksumFile) };
    return build;
}

void AddChecksums(ReleaseBuild& build, const std::map<std::string, std::string>& sums)
{
    for (auto* package : {&build.summit, &build.webkit}) {
        if (auto found = sums.find(package->fileName); found != sums.end()) package->checksums.push_back(found->second);
        else if (auto byAsset = sums.find(package->asset.name); byAsset != sums.end()) package->checksums.push_back(byAsset->second);
    }
}

std::string ExpectedChecksum(const ReleasePackage& package, std::string& error)
{
    std::string expected;
    for (const auto& checksum : package.checksums) {
        const auto value = LowerText(checksum);
        if (expected.empty()) expected = value;
        else if (expected != value) {
            error = "The release gives two different SHA-256 checksums for " + package.fileName + ".";
            return { };
        }
    }
    return expected;
}

UpdateStatus CompareBuilds(const InstalledBuild& installed, const ReleaseBuild& release)
{
    if (!installed.summit) return UpdateStatus::Available;
    const int browser = ComparePackageVersions(*installed.summit, release.summit.version);
    if (browser < 0) return UpdateStatus::Available;
    if (browser > 0) return UpdateStatus::Newer;
    if (!installed.webkit || ComparePackageVersions(*installed.webkit, release.webkit.version) < 0) return UpdateStatus::Available;
    return UpdateStatus::UpToDate;
}

std::optional<Comparison> ParseComparison(const std::string& text, std::string& error)
{
    try {
        const auto document = json::parse(text);
        if (!document.is_object()) throw std::runtime_error("not an object");
        Comparison comparison;
        comparison.status = String(document, "status");
        comparison.htmlURL = String(document, "html_url");
        if (auto total = document.find("total_commits"); total != document.end() && total->is_number_integer())
            comparison.totalCommits = total->get<int>();
        if (auto commits = document.find("commits"); commits != document.end() && commits->is_array()) {
            for (const auto& item : *commits) {
                if (!item.is_object()) continue;
                const auto details = item.find("commit");
                if (details == item.end() || !details->is_object()) continue;
                const std::string message = String(*details, "message");
                if (message.find("[skip ci]") != std::string::npos) {
                    ++comparison.skipped;
                    continue;
                }
                CommitSummary commit;
                commit.sha = String(item, "sha").substr(0, 7);
                commit.title = message.substr(0, message.find('\n'));
                for (const char* who : {"committer", "author"})
                    if (auto person = details->find(who); commit.date.empty() && person != details->end() && person->is_object())
                        commit.date = String(*person, "date");
                comparison.commits.push_back(std::move(commit));
            }
        }
        // GitHub lists them oldest first.
        std::reverse(comparison.commits.begin(), comparison.commits.end());
        return comparison;
    } catch (const std::exception& exception) {
        error = std::string("GitHub's list of changes could not be read (") + exception.what() + ").";
        return std::nullopt;
    }
}

std::string FormatChanges(const Comparison& comparison, size_t limit)
{
    std::string text;
    size_t shown = 0;
    for (const auto& commit : comparison.commits) {
        if (shown == limit) break;
        ++shown;
        text += "• " + commit.title;
        const auto date = ShortDate(commit.date);
        text += " (" + commit.sha + (date.empty() ? "" : ", " + date) + ")\n";
    }
    // GitHub lists at most a page of commits; the rest are on its page.
    const int listed = static_cast<int>(comparison.commits.size()) + comparison.skipped;
    const int more = static_cast<int>(comparison.commits.size() - shown) + std::max(0, comparison.totalCommits - listed);
    if (more > 0) text += "… and " + std::to_string(more) + " more.\n";
    if (comparison.skipped > 0)
        text += "(" + std::to_string(comparison.skipped) + (comparison.skipped == 1 ? " update" : " updates")
            + " of the README and documentation not listed.)\n";
    if (text.empty()) text = "No changes to the browser are listed.\n";
    return text;
}

PackageFileInfo ParsePackageInfo(std::string_view text)
{
    PackageFileInfo info;
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = text.find('\n', at);
        const auto line = Trim(text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        at = end == std::string_view::npos ? text.size() : end + 1;
        const size_t colon = line.find(": ");
        if (colon == std::string_view::npos) continue;
        const auto key = line.substr(0, colon);
        const auto value = std::string(Trim(line.substr(colon + 2)));
        std::string* field = key == "name" ? &info.name : key == "version" ? &info.version
            : key == "architecture" ? &info.architecture : key == "description" ? &info.description : nullptr;
        if (field && field->empty()) *field = value;
    }
    return info;
}

std::string CheckDownloadedPackage(const PackageFileInfo& info, const ReleasePackage& expected, std::string_view architecture, bool airos)
{
    const std::string file = expected.fileName;
    if (info.name != expected.name) return file + " is the package \"" + info.name + "\", not " + expected.name + ".";
    const auto version = PackageVersion::Parse(info.version);
    if (!version || ComparePackageVersions(*version, expected.version) != 0)
        return file + " has version " + info.version + ", not " + expected.version.ToString() + ".";
    if (info.architecture != architecture && info.architecture != "any")
        return file + " is for " + info.architecture + ", not " + std::string(architecture) + ".";
    // CI's descriptions say "The air/OS build" or "The Haiku R1/beta6 build".
    const auto& description = info.description;
    for (size_t at = description.find("The "); at != std::string::npos; at = description.find("The ", at + 1)) {
        const size_t end = description.find(" build", at);
        if (end == std::string::npos || end - at > 40) continue;
        const auto system = description.substr(at + 4, end - at - 4);
        if (system == "air/OS" || StartsWith(system, "Haiku")) {
            if ((system == "air/OS") != airos)
                return file + " is the " + system + " build, not the one for " + (airos ? "air/OS." : "Haiku.");
            break;
        }
    }
    return { };
}

bool UpdateCheckDue(int64_t lastCheck, int64_t now)
{
    return lastCheck <= 0 || now < lastCheck || now - lastCheck >= 24 * 60 * 60;
}

bool IsRateLimited(long status, std::string_view remaining, std::string_view retryAfter, std::string_view body)
{
    if (status != 403 && status != 429) return false;
    return Trim(remaining) == "0" || !Trim(retryAfter).empty() || LowerText(body).find("rate limit") != std::string::npos;
}

std::string DescribeGitHubFailure(long status, std::string_view remaining, int64_t reset, std::string_view retryAfter,
    std::string_view body, int64_t now)
{
    if (IsRateLimited(status, remaining, retryAfter, body)) {
        int64_t wait = reset > now ? reset - now : 0;
        if (!wait) wait = std::atoll(std::string(Trim(retryAfter)).c_str());
        std::string text = "GitHub allows only so many requests an hour from one network, and this network has used them up.";
        if (wait > 0) {
            const int64_t minutes = (wait + 59) / 60;
            text += " Try again in " + std::to_string(minutes) + (minutes == 1 ? " minute." : " minutes.");
        } else text += " Try again later.";
        return text;
    }
    if (status == 404) return "GitHub has no such release of Summit.";
    if (status >= 500) return "GitHub is not answering properly (HTTP status " + std::to_string(status) + "). Try again later.";
    return "GitHub answered with HTTP status " + std::to_string(status) + ".";
}

std::string ShellQuote(std::string_view text)
{
    std::string quoted = "'";
    for (char c : text) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    return quoted + "'";
}
}

#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Updating Summit from its GitHub releases (File › Check for Updates…): the
// parts that need no network or Haiku, so that they are tested on the host.
// src/ui/UpdateChecker.cpp asks GitHub, downloads and installs.
namespace summit {
// A Haiku package version, "0.1.0~git20261019.0340-1": major.minor.micro,
// an optional pre-release after '~' and the revision after the last '-'.
struct PackageVersion {
    std::string major, minor, micro, preRelease;
    uint32_t revision = 0;
    // Nothing for text that is not a version (empty, no major part, a bad revision).
    static std::optional<PackageVersion> Parse(std::string_view text);
    std::string ToString() const;
};
// As the package kit compares versions (BPackageVersion::Compare): major,
// minor and micro naturally (runs of digits as numbers), then a version
// without a pre-release after any with one, then the pre-releases naturally,
// then the revision. Negative, zero or positive.
int ComparePackageVersions(const PackageVersion&, const PackageVersion&);
// Haiku's NaturalCompare: runs of digits compare as numbers, other text
// without regard to case.
int NaturalCompare(std::string_view, std::string_view);
// Two packages of one CI build: the same pre-release (git date and time) and revision.
bool SameBuild(const PackageVersion&, const PackageVersion&);

// "summit-0.1.0~git20261019.0340-1", as packagefs names a file's package in
// its SYS:PACKAGE attribute. Package names have no '-'.
struct VersionedPackage {
    std::string name;
    PackageVersion version;
};
std::optional<VersionedPackage> ParseVersionedPackage(std::string_view text);

// The Summit that runs, as its files tell (UpdateChecker.cpp fills it in).
struct InstalledBuild {
    std::string executable;
    // From the browser's and the engine's SYS:PACKAGE; nothing when the file
    // does not come from a package (a bundle or a build of one's own).
    std::optional<PackageVersion> summit, webkit;
    // The browser's package file (SYS:PACKAGE_FILE) and the folder it is in.
    std::string packageFile, packagesDirectory;
    // Installed in the home folder's packages (pkgman install -H).
    bool home = false;
    // The Summit commit it was built from (from its package description), short.
    std::string commit;
    // Why the commit is not known, when it is not.
    std::string commitProblem;
    // Described by SUMMIT_UPDATE_INSTALLED for tests: never really installed.
    bool simulated = false;
    // Why the build cannot update itself, when it cannot.
    std::string problem;
    bool FromPackages() const { return summit && webkit && problem.empty(); }
};
// SUMMIT_UPDATE_INSTALLED: "summit-VERSION summit_webkit-VERSION [COMMIT]",
// the build to pretend is installed. False when either package is missing.
bool ParseSimulatedBuild(std::string_view text, InstalledBuild&);
// "from Summit 15fce3d" in a summit or summit_webkit package's description.
std::string CommitFromDescription(std::string_view description);

// A GitHub release, as the releases API returns it.
struct ReleaseAsset {
    std::string name, url;
    uint64_t size = 0;
    // Lower-case hex, from the API's "digest" ("sha256:…"), or empty.
    std::string sha256;
};
struct Release {
    std::string tag, name, body, htmlURL, publishedAt;
    // The commit the release was built from, full or short; empty if unknown.
    std::string commit;
    std::vector<ReleaseAsset> assets;
};
std::optional<Release> ParseRelease(const std::string& json, std::string& error);

// The release a build updates from: "latest" for air/OS, "latest-haiku" for
// a stock Haiku release.
const char* ReleaseTag(bool airos);
// The architecture this program was built for, as package files name it.
const char* PackageArchitecture();

// File name -> lower-case hex SHA-256, from a SHA256SUMS file ("HASH  NAME")
// and from the table in a release's notes ("| NAME | `HASH` |"). Names are
// as written, with '~' in the version.
std::map<std::string, std::string> ParseChecksums(std::string_view text);
// GitHub names release files with '.' for the '~' of package versions.
std::string AssetName(std::string_view packageFileName);

// One package of a release, and the pair a build installs.
struct ReleasePackage {
    std::string name;
    PackageVersion version;
    ReleaseAsset asset;
    // "summit-0.1.0~git20261019.0340-1-x86_64.hpkg", as the package itself is named.
    std::string fileName;
    // Every SHA-256 the release states for it (the API's, the notes', SHA256SUMS'), which must agree.
    std::vector<std::string> checksums;
};
struct ReleaseBuild {
    ReleasePackage summit, webkit;
    std::string commit;
    // The SHA256SUMS file of the release, if it has one (checked before installing).
    std::optional<ReleaseAsset> checksumFile;
};
// The summit and summit_webkit packages of one architecture: both, from the
// same build, at https:// addresses (plain HTTP too without requireHTTPS, for
// a test server), or nothing with error.
std::optional<ReleaseBuild> ChooseReleaseBuild(const Release&, std::string_view architecture, std::string& error,
    bool requireHTTPS = true);
// Adds a SHA256SUMS file's hashes to the packages' checksums.
void AddChecksums(ReleaseBuild&, const std::map<std::string, std::string>& sums);
// The checksum the packages' sources agree on; empty with error if they differ.
std::string ExpectedChecksum(const ReleasePackage&, std::string& error);

enum class UpdateStatus {
    UpToDate,
    // The release's browser is newer, or the same browser with a newer engine
    // (installing the pair repairs an engine left behind).
    Available,
    // This build is newer than the release (installed by hand, or the release is behind).
    Newer,
};
UpdateStatus CompareBuilds(const InstalledBuild&, const ReleaseBuild&);

// The commits between two builds (GitHub's compare API).
struct CommitSummary {
    std::string sha, title, date;
};
struct Comparison {
    std::string status, htmlURL;
    int totalCommits = 0;
    // Newest first, without the [skip ci] commits (README and documentation updates).
    std::vector<CommitSummary> commits;
    int skipped = 0;
};
std::optional<Comparison> ParseComparison(const std::string& json, std::string& error);
// The list for the update window: one change per line, newest first.
std::string FormatChanges(const Comparison&, size_t limit = 60);

// What `package list -i FILE` says about a package file.
struct PackageFileInfo {
    std::string name, version, architecture, description;
};
PackageFileInfo ParsePackageInfo(std::string_view text);
// Checks a downloaded package against what the release said it is: its name,
// version, architecture and (from its description) the system it is for.
// Empty when it matches, otherwise why not.
std::string CheckDownloadedPackage(const PackageFileInfo&, const ReleasePackage&, std::string_view architecture, bool airos);

// The automatic check runs at most once a day: when the last one was a day
// ago, never happened, or is in the future (the clock went back).
bool UpdateCheckDue(int64_t lastCheck, int64_t now);

// Why a GitHub request failed, for people: rate limits (status 403 or 429
// with no requests left, or Retry-After), a missing release, other statuses.
// reset is X-RateLimit-Reset (seconds since the epoch), 0 if absent.
std::string DescribeGitHubFailure(long status, std::string_view remaining, int64_t reset, std::string_view retryAfter,
    std::string_view body, int64_t now);
bool IsRateLimited(long status, std::string_view remaining, std::string_view retryAfter, std::string_view body);
// Single-quoted for /bin/sh.
std::string ShellQuote(std::string_view);
}

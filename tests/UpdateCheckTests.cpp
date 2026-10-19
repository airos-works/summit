#include "core/Profile.h"
#include "core/UpdateCheck.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <unistd.h>

namespace {
// The air/OS release as GitHub's API returned it on 19 October 2026 (one
// architecture's packages left out, and an unrelated file added).
const char* kRelease = R"json({
  "url": "https://api.github.com/repos/jmgasper/summit/releases/1",
  "html_url": "https://github.com/jmgasper/summit/releases/tag/latest",
  "tag_name": "latest",
  "target_commitish": "15fce3d6bd83b8f0ad2d03120d2f3e2e8d87d41d",
  "name": "Latest air/OS build (15fce3d)",
  "published_at": "2026-10-19T03:50:42Z",
  "assets": [
    {"name": "summit-0.1.0.git20261019.0340-1-arm64.hpkg", "size": 4599619,
     "digest": "sha256:c1bfa00f1d798c82d4d872632c4338c225b6610aa1f58bccea0a2c94c47ddf37",
     "browser_download_url": "https://github.com/jmgasper/summit/releases/download/latest/summit-0.1.0.git20261019.0340-1-arm64.hpkg"},
    {"name": "summit-0.1.0.git20261019.0340-1-x86_64.hpkg", "size": 4535939,
     "digest": "sha256:5ef232565f8382d8774e581393b30dd187f4a756be138cf27d450cbda8b92bba",
     "browser_download_url": "https://github.com/jmgasper/summit/releases/download/latest/summit-0.1.0.git20261019.0340-1-x86_64.hpkg"},
    {"name": "summit_webkit-1.10.1.git20261019.0340-1-x86_64.hpkg", "size": 83651077, "digest": null,
     "browser_download_url": "https://github.com/jmgasper/summit/releases/download/latest/summit_webkit-1.10.1.git20261019.0340-1-x86_64.hpkg"},
    {"name": "notes.txt", "size": 10, "browser_download_url": "https://github.com/jmgasper/summit/releases/download/latest/notes.txt"}
  ],
  "body": "Built by air/OS CI from 15fce3d on 2026-10-19.\n\nHaiku SDK: x86_64 hrev60207-1701-gc28cd80a26.\n\n| Package | SHA-256 |\n|---|---|\n| summit-0.1.0~git20261019.0340-1-arm64.hpkg | `c1bfa00f1d798c82d4d872632c4338c225b6610aa1f58bccea0a2c94c47ddf37` |\n| summit-0.1.0~git20261019.0340-1-x86_64.hpkg | `5ef232565f8382d8774e581393b30dd187f4a756be138cf27d450cbda8b92bba` |\n| summit_webkit-1.10.1~git20261019.0340-1-x86_64.hpkg | `c597a690773a4b20d53629c139d80faeddacf429ebdd99de4b1b3ae3129d1a8d` |\n\nInstall on air/OS or Haiku with `pkgman install <file>`.\n"
})json";

const char* kComparison = R"json({
  "html_url": "https://github.com/jmgasper/summit/compare/2b89650...15fce3d",
  "status": "ahead", "ahead_by": 3, "behind_by": 0, "total_commits": 3,
  "commits": [
    {"sha": "e0298ee0000000000000000000000000000000000", "commit": {"message": "README: latest air/OS builds (2b89650) [skip ci]",
      "author": {"date": "2026-10-19T03:05:06Z"}, "committer": {"date": "2026-10-19T03:05:06Z"}}},
    {"sha": "9c286e60000000000000000000000000000000000", "commit": {"message": "README: latest Haiku R1/beta6 builds (2b89650) [skip ci]",
      "author": {"date": "2026-10-19T03:05:20Z"}, "committer": {"date": "2026-10-19T03:05:20Z"}}},
    {"sha": "15fce3d6bd83b8f0ad2d03120d2f3e2e8d87d41d", "commit": {"message": "On AMD's GPUs direct presents also write app_server's copy\n\nDetails.",
      "author": {"date": "2026-10-19T03:40:44Z"}, "committer": {"date": "2026-10-19T03:40:44Z"}}}
  ],
  "files": []
})json";

// `package list -i` of CI's summit package.
const char* kPackageInfo = "package-attributes:\n"
    "\tname: summit\n"
    "\tsummary: A native WebKit browser\n"
    "\tdescription: Summit is a WebKit browser for Haiku and air/OS, with tabs. The air/OS build, made by air/OS CI from "
    "Summit 15fce3d against the summit_webkit engine of the same build.\n"
    "\tvendor: Summit project\n"
    "\tarchitecture: x86_64\n"
    "\tversion: 0.1.0~git20261019.0340-1\n"
    "\tprovides: summit = 0.1.0~git20261019.0340\n"
    "\trequires: summit_webkit>=1.10.1~git20261019.0340\n";
}

int main()
{
    using namespace summit;
    unsigned checks = 0;
    auto check = [&](bool value, const std::string& label) {
        ++checks;
        if (!value) { std::cerr << "FAIL: " << label << '\n'; std::exit(1); }
    };
    auto version = [](const char* text) { return *PackageVersion::Parse(text); };
    auto compare = [&](const char* a, const char* b) { return ComparePackageVersions(version(a), version(b)); };

    // Versions, as the package kit reads and orders them.
    auto parsed = PackageVersion::Parse("0.1.0~git20261019.0340-1");
    check(parsed && parsed->major == "0" && parsed->minor == "1" && parsed->micro == "0" && parsed->preRelease == "git20261019.0340"
        && parsed->revision == 1, "parse CI version");
    check(parsed->ToString() == "0.1.0~git20261019.0340-1", "version text round trip");
    check(version("1.10.0-4").ToString() == "1.10.0-4" && version("2").ToString() == "2" && version("1.2.3.4").micro == "3.4",
        "other versions");
    for (const char* bad : {"", "-1", "1.0-", "1.0-x", "1.0~", "1 .0", "1.0~a~b", "1.0-1-2x"})
        check(!PackageVersion::Parse(bad), std::string("bad version ") + bad);
    check(compare("0.1.0~git20261019.0340-1", "0.1.0~git20261020.0100-1") < 0, "a later day is newer");
    check(compare("0.1.0~git20261019.0340-1", "0.1.0~git20261019.1200-1") < 0, "a later time is newer");
    check(compare("0.1.0~git20261019.0340-2", "0.1.0~git20261019.0340-1") > 0, "a higher revision is newer");
    check(compare("0.1.0~git20261019.0340-1", "0.1.0~git20261019.0340-1") == 0, "same version");
    check(compare("0.1.0~dev2-2", "0.1.0~git20261005.1811-1") < 0, "hand-made dev package is older");
    check(compare("1.10.0-4", "1.10.1~git20261019.0340-1") < 0, "micro version decides first");
    check(compare("0.1.0", "0.1.0~git20261019.0340-1") > 0, "a release is newer than its pre-releases");
    check(compare("0.10.0", "0.9.0") > 0 && compare("0.010", "0.10") == 0, "numbers compare as numbers");
    check(NaturalCompare("Git", "git") == 0 && NaturalCompare("a2", "a10") < 0 && NaturalCompare("a", "1") > 0, "natural compare");
    check(SameBuild(version("0.1.0~git20261019.0340-1"), version("1.10.1~git20261019.0340-1"))
        && !SameBuild(version("0.1.0~git20261019.0340-1"), version("1.10.1~git20261018.2008-1"))
        && !SameBuild(version("0.1.0~git20261019.0340-1"), version("1.10.1~git20261019.0340-2")), "same build");

    // What packagefs says a file came from.
    auto package = ParseVersionedPackage("summit_webkit-1.10.1~git20261019.0340-1");
    check(package && package->name == "summit_webkit" && package->version.ToString() == "1.10.1~git20261019.0340-1", "SYS:PACKAGE");
    check(!ParseVersionedPackage("summit") && !ParseVersionedPackage("-1.0") && !ParseVersionedPackage("sum mit-1.0"), "bad SYS:PACKAGE");
    InstalledBuild simulated;
    check(ParseSimulatedBuild("summit-0.1.0~git20261019.0305-1 summit_webkit-1.10.1~git20261019.0305-1 2B89650", simulated)
        && simulated.simulated && simulated.summit->preRelease == "git20261019.0305" && simulated.commit == "2b89650", "simulated build");
    InstalledBuild half;
    check(!ParseSimulatedBuild("summit-0.1.0~git20261019.0305-1", half), "simulated build needs the engine");
    check(CommitFromDescription("The air/OS build, made by air/OS CI from Summit 15fce3d against the engine.") == "15fce3d",
        "commit in the browser's description");
    check(CommitFromDescription("Built by air/OS CI from WebKit abc (jmgasper/WebKit) with the Haiku port of Summit 2b89650.") == "2b89650",
        "commit in the engine's description");
    check(CommitFromDescription("Summit is a WebKit browser, made by hand.").empty()
        && CommitFromDescription("from Summit deadbeefcafe1 and more").size() == 13
        && CommitFromDescription("from Summit 15fce3dx").empty(), "no commit");

    // The release and the packages for this machine.
    std::string error;
    auto release = ParseRelease(kRelease, error);
    check(release && release->tag == "latest" && release->commit == "15fce3d6bd83b8f0ad2d03120d2f3e2e8d87d41d"
        && release->assets.size() == 4 && release->assets[2].sha256.empty() && release->assets[1].size == 4535939, "parse release");
    check(!ParseRelease("{\"message\": \"Not Found\"}", error) && !error.empty(), "release without a tag");
    check(!ParseRelease("<html>", error), "release that is not JSON");
    check(ReleaseTag(true) == std::string("latest") && ReleaseTag(false) == std::string("latest-haiku"), "release tags");
    auto build = ChooseReleaseBuild(*release, "x86_64", error);
    check(build && build->summit.name == "summit" && build->summit.version.ToString() == "0.1.0~git20261019.0340-1"
        && build->webkit.version.ToString() == "1.10.1~git20261019.0340-1"
        && build->summit.fileName == "summit-0.1.0~git20261019.0340-1-x86_64.hpkg"
        && build->webkit.asset.size == 83651077 && build->commit.rfind("15fce3d", 0) == 0 && !build->checksumFile, "choose x86_64");
    check(build->summit.checksums.size() == 2 && build->webkit.checksums.size() == 1
        && build->webkit.checksums[0] == "c597a690773a4b20d53629c139d80faeddacf429ebdd99de4b1b3ae3129d1a8d", "checksums from API and notes");
    check(ExpectedChecksum(build->summit, error) == "5ef232565f8382d8774e581393b30dd187f4a756be138cf27d450cbda8b92bba", "expected checksum");
    check(!ChooseReleaseBuild(*release, "arm64", error) && error.find("summit_webkit") != std::string::npos, "arm64 lacks its engine");
    check(!ChooseReleaseBuild(*release, "riscv64", error), "no riscv64 packages");
    {
        // Without notes the versions come from the file names.
        Release bare = *release;
        bare.body.clear();
        auto guessed = ChooseReleaseBuild(bare, "x86_64", error);
        check(guessed && guessed->summit.version.ToString() == "0.1.0~git20261019.0340-1"
            && guessed->webkit.fileName == "summit_webkit-1.10.1~git20261019.0340-1-x86_64.hpkg"
            && guessed->webkit.checksums.empty(), "versions from file names");
        // A pair from two builds is never installed.
        bare.assets[2].name = "summit_webkit-1.10.1.git20261018.2008-1-x86_64.hpkg";
        check(!ChooseReleaseBuild(bare, "x86_64", error) && error.find("different builds") != std::string::npos, "mismatched pair");
        // A SHA256SUMS file is checked too, and must agree.
        Release sums = *release;
        sums.assets.push_back({"SHA256SUMS", "https://github.com/jmgasper/summit/releases/download/latest/SHA256SUMS", 300, ""});
        auto withSums = ChooseReleaseBuild(sums, "x86_64", error);
        check(withSums && withSums->checksumFile && withSums->checksumFile->name == "SHA256SUMS", "SHA256SUMS found");
        const auto parsedSums = ParseChecksums("5ef232565f8382d8774e581393b30dd187f4a756be138cf27d450cbda8b92bba  "
            "summit-0.1.0~git20261019.0340-1-x86_64.hpkg\n"
            "1111111111111111111111111111111111111111111111111111111111111111 *summit_webkit-1.10.1.git20261019.0340-1-x86_64.hpkg\n"
            "not a line\n");
        check(parsedSums.size() == 2, "SHA256SUMS lines");
        AddChecksums(*withSums, parsedSums);
        check(ExpectedChecksum(withSums->summit, error).size() == 64, "agreeing checksums");
        error.clear();
        check(ExpectedChecksum(withSums->webkit, error).empty() && !error.empty(), "checksums that differ");
        // A package without a size or a secure address is refused.
        Release insecure = *release;
        insecure.assets[1].url = "http://example.com/summit.hpkg";
        check(!ChooseReleaseBuild(insecure, "x86_64", error), "insecure address");
        check(ChooseReleaseBuild(insecure, "x86_64", error, false).has_value(), "plain HTTP for a test server");
        insecure.assets[1].url = "file:///boot/home/summit.hpkg";
        check(!ChooseReleaseBuild(insecure, "x86_64", error, false), "never a file address");
    }
    check(AssetName("summit-0.1.0~git20261019.0340-1-x86_64.hpkg") == "summit-0.1.0.git20261019.0340-1-x86_64.hpkg", "asset name");

    // Installed against released.
    InstalledBuild installed;
    installed.summit = version("0.1.0~git20261019.0305-1");
    installed.webkit = version("1.10.1~git20261019.0305-1");
    check(CompareBuilds(installed, *build) == UpdateStatus::Available, "older build");
    installed.summit = version("0.1.0~git20261019.0340-1");
    check(CompareBuilds(installed, *build) == UpdateStatus::Available, "older engine alone");
    installed.webkit = version("1.10.1~git20261019.0340-1");
    check(CompareBuilds(installed, *build) == UpdateStatus::UpToDate, "same build");
    installed.summit = version("0.1.0~git20261020.0100-1");
    check(CompareBuilds(installed, *build) == UpdateStatus::Newer, "newer build");
    check(!installed.FromPackages() || installed.problem.empty(), "from packages");
    installed.problem = "runs from a folder";
    check(!installed.FromPackages(), "a problem stops updating");

    // The changes between two builds.
    auto comparison = ParseComparison(kComparison, error);
    check(comparison && comparison->status == "ahead" && comparison->commits.size() == 1 && comparison->skipped == 2
        && comparison->commits[0].sha == "15fce3d" && comparison->commits[0].title == "On AMD's GPUs direct presents also write app_server's copy",
        "parse comparison");
    const auto changes = FormatChanges(*comparison);
    check(changes.find("• On AMD's GPUs direct presents also write app_server's copy (15fce3d, 19 Oct)\n") == 0
        && changes.find("2 updates of the README") != std::string::npos, "format changes: " + changes);
    comparison->totalCommits = 250;
    check(FormatChanges(*comparison).find("… and 247 more.") != std::string::npos, "more than a page");
    check(FormatChanges(Comparison{}).find("No changes") == 0, "no changes");
    check(!ParseComparison("[]", error), "comparison that is not an object");

    // The downloaded packages.
    const auto info = ParsePackageInfo(kPackageInfo);
    check(info.name == "summit" && info.version == "0.1.0~git20261019.0340-1" && info.architecture == "x86_64"
        && info.description.find("The air/OS build") != std::string::npos, "package info");
    check(CheckDownloadedPackage(info, build->summit, "x86_64", true).empty(), "package matches");
    check(CheckDownloadedPackage(info, build->webkit, "x86_64", true).find("not summit_webkit") != std::string::npos, "wrong package");
    check(CheckDownloadedPackage(info, build->summit, "arm64", true).find("for x86_64") != std::string::npos, "wrong architecture");
    check(CheckDownloadedPackage(info, build->summit, "x86_64", false).find("air/OS build") != std::string::npos, "wrong system");
    auto haiku = info;
    haiku.description = "Summit. The Haiku R1/beta6 build, made by air/OS CI from Summit 15fce3d.";
    check(CheckDownloadedPackage(haiku, build->summit, "x86_64", false).empty()
        && !CheckDownloadedPackage(haiku, build->summit, "x86_64", true).empty(), "Haiku build");
    auto older = info;
    older.version = "0.1.0~git20261019.0305-1";
    check(CheckDownloadedPackage(older, build->summit, "x86_64", true).find("has version") != std::string::npos, "wrong version");
    auto plain = info;
    plain.description = "A browser.";
    check(CheckDownloadedPackage(plain, build->summit, "x86_64", false).empty(), "no system named");

    // Once a day.
    const int64_t day = 24 * 60 * 60, now = 1792389386;
    check(UpdateCheckDue(0, now) && UpdateCheckDue(now - day, now) && UpdateCheckDue(now + 3600, now), "check due");
    check(!UpdateCheckDue(now - day + 60, now) && !UpdateCheckDue(now, now), "checked today");

    // GitHub's refusals.
    check(IsRateLimited(403, "0", "", "") && IsRateLimited(429, "", "60", "") && IsRateLimited(403, "", "", "API rate limit exceeded")
        && !IsRateLimited(403, "12", "", "Forbidden") && !IsRateLimited(404, "0", "", ""), "rate limits");
    check(DescribeGitHubFailure(403, "0", now + 1381, "", "", now).find("Try again in 24 minutes.") != std::string::npos, "rate limit wait");
    check(DescribeGitHubFailure(429, "", 0, "60", "", now).find("Try again in 1 minute.") != std::string::npos, "retry after");
    check(DescribeGitHubFailure(404, "50", 0, "", "", now).find("no such release") != std::string::npos, "missing release");
    check(DescribeGitHubFailure(502, "50", 0, "", "", now).find("502") != std::string::npos, "server error");
    check(ShellQuote("/tmp/a b/it's.hpkg") == "'/tmp/a b/it'\\''s.hpkg'", "shell quoting");

    // The profile keeps the setting and the time of the last check.
    {
        const auto directory = std::filesystem::temp_directory_path() / ("summit-update-test-" + std::to_string(getpid()));
        std::filesystem::create_directories(directory);
        Profile profile;
        check(profile.checkForUpdates && profile.lastUpdateCheck == 0, "update defaults");
        profile.checkForUpdates = false;
        profile.lastUpdateCheck = now;
        check(profile.Save(directory / "profile.json", error), "save profile: " + error);
        auto loaded = Profile::Load(directory / "profile.json", error);
        check(!loaded.checkForUpdates && loaded.lastUpdateCheck == now, "update settings round trip");
        std::filesystem::remove_all(directory);
    }
    std::cout << checks << " update checks passed\n";
    return 0;
}

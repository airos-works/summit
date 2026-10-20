#include "core/ExtensionCatalog.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
static unsigned checks, failures;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::cerr << __LINE__ << ": " #value "\n"; } } while (0)
static void put(const fs::path& path, const std::string& value)
{
    std::ofstream output(path, std::ios::binary);
    output << value;
}
static std::string read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}
int main()
{
    using namespace summit;
    char temporary[] = "/tmp/summit-extension-catalog-XXXXXX";
    if (!mkdtemp(temporary)) return 2;
    const fs::path root(temporary), storage = root / "Extensions", source = root / "source.xpi";
    ExtensionCatalog catalog(storage);
    std::string error;
    std::vector<InstalledExtension> entries;
    CHECK(catalog.Load(entries, error) && entries.empty() && error.empty());
    CHECK(!fs::exists(storage));
    put(source, "owned archive bytes");
    fs::path abandoned;
    {
        auto staged = catalog.Stage(source, error);
        CHECK(staged && error.empty());
        if (!staged) return 2;
        abandoned = staged->Path().parent_path();
        CHECK(read(staged->Path()) == "owned archive bytes");
        CHECK(catalog.Load(entries, error) && entries.empty());
    }
    CHECK(!fs::exists(abandoned));
    auto staged = catalog.Stage(source, error);
    CHECK(staged && error.empty());
    if (!staged) return 2;
    put(source, "changed external bytes");
    CHECK(read(staged->Path()) == "owned archive bytes");
    fs::remove(source);
    CHECK(fs::exists(staged->Path()));
    InstalledExtension entry { "fixture@example.test", "Native extension — 测试", "1.2.3", std::string(64, 'a'), "", true, true, false };
    auto malformed = entry;
    malformed.fingerprint = "unapproved";
    CHECK(!catalog.Install(*staged, malformed, error) && !error.empty());
    malformed = entry;
    malformed.identifier = "../escape";
    CHECK(!catalog.Install(*staged, malformed, error));
    CHECK(!fs::exists(storage / "catalog.json") && fs::exists(staged->Path()));
    CHECK(catalog.Install(*staged, entry, error) && error.empty());
    CHECK(!catalog.Install(*staged, entry, error));
    auto installedPath = staged->Path();
    staged.reset();
    CHECK(fs::exists(installedPath));
    CHECK(catalog.Load(entries, error) && entries.size() == 1);
    if (entries.size() != 1) return 2;
    CHECK(entries[0].identifier == entry.identifier && entries[0].name == entry.name && entries[0].version == entry.version);
    CHECK(entries[0].enabled && entries[0].allowFileURLs && !entries[0].allowPrivateBrowsing);
    CHECK(entries[0].fingerprint == entry.fingerprint && catalog.PackagePath(entries[0]) == installedPath);
    struct stat status { };
    CHECK(!::stat((storage / "catalog.json").c_str(), &status) && (status.st_mode & 0777) == 0600);
    CHECK(!::stat(installedPath.c_str(), &status) && (status.st_mode & 0777) == 0600);
    CHECK(!::stat(installedPath.parent_path().c_str(), &status) && (status.st_mode & 0777) == 0700);
    CHECK(catalog.SetEnabled(entry.identifier, false, error));
    CHECK(catalog.Load(entries, error) && !entries[0].enabled);
    auto saved = read(storage / "catalog.json");
    CHECK(!catalog.SetEnabled("missing", true, error) && read(storage / "catalog.json") == saved);
    CHECK(!entries[0].allowPrivateBrowsing && catalog.SetAllowPrivateBrowsing(entry.identifier, true, error));
    CHECK(catalog.Load(entries, error) && entries[0].allowPrivateBrowsing && !entries[0].enabled);
    CHECK(catalog.SetAllowPrivateBrowsing(entry.identifier, false, error) && catalog.Load(entries, error) && !entries[0].allowPrivateBrowsing);
    saved = read(storage / "catalog.json");
    CHECK(!catalog.SetAllowPrivateBrowsing("missing", true, error) && read(storage / "catalog.json") == saved);
    put(source, "second archive");
    {
        auto duplicate = catalog.Stage(source, error);
        CHECK(duplicate && !catalog.Install(*duplicate, entry, error));
        CHECK(read(storage / "catalog.json") == saved && fs::exists(installedPath));
        ExtensionCatalog other(root / "Other");
        CHECK(duplicate && !other.Install(*duplicate, entry, error));
        CHECK(!fs::exists(root / "Other/catalog.json"));
        auto invalidText = entry;
        invalidText.identifier = "another@example.test";
        invalidText.name = std::string(1, static_cast<char>(0xff));
        CHECK(duplicate && !catalog.Install(*duplicate, invalidText, error));
        CHECK(read(storage / "catalog.json") == saved && fs::exists(duplicate->Path()));
    }
    {
        ExtensionCatalog uppercaseCatalog(root / "Uppercase");
        auto uppercaseStage = uppercaseCatalog.Stage(source, error);
        auto uppercase = entry;
        uppercase.fingerprint = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
        auto invalidDigest = uppercase;
        invalidDigest.fingerprint[0] = 'G';
        CHECK(uppercaseStage && !uppercaseCatalog.Install(*uppercaseStage, invalidDigest, error));
        CHECK(uppercaseStage && uppercaseCatalog.Install(*uppercaseStage, uppercase, error));
        std::vector<InstalledExtension> restored;
        CHECK(uppercaseCatalog.Load(restored, error) && restored.size() == 1);
        CHECK(restored.size() == 1 && restored[0].fingerprint == uppercase.fingerprint);
    }
    {
        const auto orderRoot = root / "Order";
        ExtensionCatalog ordered(orderRoot);
        std::vector<InstalledExtension> restored;
        auto install = [&](const char* identity, uint64_t order = 0) {
            auto package = ordered.Stage(source, error);
            auto item = entry;
            item.identifier = identity;
            item.installationOrder = order;
            CHECK(package && ordered.Install(*package, item, error));
        };
        install("older", 1);
        install("newer", 2);
        CHECK(ordered.Load(restored, error) && restored.size() == 2);
        CHECK(restored[0].installationOrder == 1 && restored[1].installationOrder == 2);
        CHECK(ordered.SetEnabled("older", false, error) && ordered.SetEnabled("older", true, error));
        CHECK(ordered.Load(restored, error) && restored[0].installationOrder == 1);
        auto data = nlohmann::json::parse(read(orderRoot / "catalog.json"));
        std::swap(data["extensions"][0], data["extensions"][1]);
        put(orderRoot / "catalog.json", data.dump());
        CHECK(ordered.Load(restored, error) && restored[0].identifier == "newer");
        CHECK(restored[0].installationOrder == 2 && restored[1].installationOrder == 1);
        CHECK(ExtensionCatalog::NextInstallationOrder(restored) == 3);
        CHECK(ordered.Forget("older", error));
        install("third", 3);
        CHECK(ordered.Load(restored, error) && restored[0].installationOrder == 2 && restored[1].installationOrder == 3);
        // A consent review cannot silently acquire a different precedence if
        // the catalog changes before the native runtime is committed.
        auto stale = ordered.Stage(source, error);
        auto staleEntry = entry; staleEntry.identifier = "stale"; staleEntry.installationOrder = 3;
        const auto approved = read(orderRoot / "catalog.json");
        CHECK(stale && !ordered.Install(*stale, staleEntry, error));
        CHECK(read(orderRoot / "catalog.json") == approved && fs::exists(stale->Path()));
        // Legacy reads assign the existing append order without rewriting data.
        data = nlohmann::json::parse(approved);
        for (auto& item : data["extensions"]) item.erase("installation_order");
        const auto legacy = data.dump(); put(orderRoot / "catalog.json", legacy);
        CHECK(ordered.Load(restored, error) && restored[0].installationOrder == 1 && restored[1].installationOrder == 2);
        CHECK(read(orderRoot / "catalog.json") == legacy);
        CHECK(ordered.SetEnabled("newer", false, error));
        data = nlohmann::json::parse(read(orderRoot / "catalog.json"));
        CHECK(data["extensions"][0]["installation_order"] == 1 && data["extensions"][1]["installation_order"] == 2);
        const auto migrated = data;
        for (const auto& invalidOrder : {nlohmann::json(0), nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json("2"), nlohmann::json(true)}) {
            data = migrated; data["extensions"][0]["installation_order"] = invalidOrder;
            put(orderRoot / "catalog.json", data.dump());
            CHECK(!ordered.Load(restored, error));
            CHECK(restored.size() == 2 && restored[0].installationOrder == 1);
        }
        data = migrated; data["extensions"][1]["installation_order"] = 1;
        put(orderRoot / "catalog.json", data.dump());
        CHECK(!ordered.Load(restored, error));
        data = migrated; data["extensions"][1].erase("installation_order");
        put(orderRoot / "catalog.json", data.dump());
        CHECK(!ordered.Load(restored, error));
        std::swap(data["extensions"][0], data["extensions"][1]);
        put(orderRoot / "catalog.json", data.dump());
        CHECK(!ordered.Load(restored, error));
        data = migrated; data["extensions"][1]["installation_order"] = std::numeric_limits<uint64_t>::max();
        put(orderRoot / "catalog.json", data.dump());
        CHECK(ordered.Load(restored, error) && restored[1].installationOrder == std::numeric_limits<uint64_t>::max());
        bool exhausted = false;
        try { ExtensionCatalog::NextInstallationOrder(restored); } catch (const std::exception&) { exhausted = true; }
        CHECK(exhausted);
        staleEntry.installationOrder = 0;
        CHECK(!ordered.Install(*stale, staleEntry, error));
        CHECK(read(orderRoot / "catalog.json") == data.dump());
        put(orderRoot / "catalog.json", migrated.dump());
        CHECK(ordered.Load(restored, error));
        const auto oldPackage = restored[1].package;
        CHECK(ordered.Forget("third", error));
        install("third");
        CHECK(ordered.Load(restored, error) && restored[1].package != oldPackage);
        CHECK(restored[1].installationOrder > restored[0].installationOrder);
    }
    auto directory = root / "directory";
    fs::create_directories(directory / "nested");
    put(directory / "manifest.json", "{\"manifest_version\":2}");
    put(directory / "nested/code.js", "const retained = true;");
    fs::create_symlink("nested/code.js", directory / "alias.js");
    {
        auto unpacked = catalog.Stage(directory, error);
        CHECK(unpacked && read(unpacked->Path() / "nested/code.js") == "const retained = true;");
        CHECK(unpacked && fs::read_symlink(unpacked->Path() / "alias.js") == "nested/code.js");
        CHECK(unpacked && read(unpacked->Path() / "manifest.json") == "{\"manifest_version\":2}");
    }
    fs::create_symlink(source, root / "linked.xpi");
    CHECK(!catalog.Stage(root / "linked.xpi", error));
    CHECK(!catalog.Stage(root / "missing", error));
    CHECK(!catalog.Stage(root, error));
    CHECK(::mkfifo((directory / "pipe").c_str(), 0600) == 0);
    CHECK(!catalog.Stage(directory, error));
    size_t packages = 0;
    for (const auto& ignored : fs::directory_iterator(storage / "packages")) { (void)ignored; ++packages; }
    CHECK(packages == 1); // Failed and abandoned imports all cleaned up.
    { std::ofstream sparse(source); sparse.seekp(128 * 1024 * 1024); sparse.put('x'); }
    CHECK(!catalog.Stage(source, error));
    put(storage / "catalog.json", "{broken");
    CHECK(!catalog.Load(entries, error) && !error.empty() && entries.size() == 1);
    CHECK(!catalog.SetEnabled(entry.identifier, true, error) && read(storage / "catalog.json") == "{broken");
    auto invalid = nlohmann::json::parse(saved);
    invalid["extensions"][0]["package"] = "../../external";
    put(storage / "catalog.json", invalid.dump());
    CHECK(!catalog.Load(entries, error));
    InstalledExtension unsafe = entry;
    unsafe.package = "../../external";
    CHECK(catalog.PackagePath(unsafe).empty());
    invalid = nlohmann::json::parse(saved);
    invalid["extensions"].push_back(invalid["extensions"][0]);
    put(storage / "catalog.json", invalid.dump());
    CHECK(!catalog.Load(entries, error));
    put(storage / "catalog.json", saved);
    // Older catalogs have no update details: they load with automatic updates on.
    CHECK(catalog.Load(entries, error) && entries.size() == 1 && entries[0].source.empty() && entries[0].autoUpdate
        && !entries[0].updated && !entries[0].checked);
    CHECK(catalog.Modify(entry.identifier, [](InstalledExtension& item) {
        item.source = "firefox";
        item.autoUpdate = false;
        item.checked = 1760000000;
    }, error));
    CHECK(catalog.Load(entries, error) && entries[0].source == "firefox" && !entries[0].autoUpdate && entries[0].checked == 1760000000);
    CHECK(!catalog.Modify(entry.identifier, [](InstalledExtension& item) { item.source = "elsewhere"; }, error));
    CHECK(!catalog.Modify(entry.identifier, [](InstalledExtension& item) { item.package = "pkg-000000"; }, error));
    CHECK(!catalog.Modify("missing@example.test", [](InstalledExtension&) { }, error));
    // An update takes a new package; the old one goes once no record names it.
    {
        put(source, "updated archive bytes");
        auto update = catalog.Stage(source, error);
        CHECK(update);
        if (!update) return 2;
        auto replacement = entries[0];
        replacement.version = "1.2.4";
        replacement.fingerprint = std::string(64, 'b');
        replacement.updated = 1760000100;
        const auto oldPackage = entries[0].package;
        CHECK(!catalog.RemovePackage(oldPackage, error));
        CHECK(catalog.Replace(*update, replacement, error));
        CHECK(!catalog.Replace(*update, replacement, error));
        CHECK(catalog.Load(entries, error) && entries.size() == 1 && entries[0].version == "1.2.4" && entries[0].package != oldPackage
            && entries[0].updated == 1760000100 && entries[0].installationOrder == replacement.installationOrder && !entries[0].autoUpdate);
        CHECK(read(catalog.PackagePath(entries[0])) == "updated archive bytes");
        CHECK(fs::exists(storage / "packages" / oldPackage) && catalog.RemovePackage(oldPackage, error) && !fs::exists(storage / "packages" / oldPackage));
        CHECK(!catalog.RemovePackage(entries[0].package, error) && !catalog.RemovePackage("../escape", error));
        saved = read(storage / "catalog.json");
        installedPath = catalog.PackagePath(entries[0]);
    }
    CHECK(catalog.Forget(entry.identifier, error));
    CHECK(catalog.Load(entries, error) && entries.empty());
    CHECK(fs::exists(installedPath)); // Metadata removal does not delete package or runtime data.
    CHECK(!catalog.Forget(entry.identifier, error));
    fs::remove(storage / "catalog.json");
    fs::create_symlink(root / "missing-catalog", storage / "catalog.json");
    CHECK(!catalog.Load(entries, error));
    fs::remove(storage / "catalog.json");
    { std::ofstream sparse(storage / "catalog.json"); sparse.seekp(4 * 1024 * 1024); sparse.put('x'); }
    CHECK(!catalog.Load(entries, error));
    fs::remove_all(root);
    CHECK(!fs::exists(root));
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}

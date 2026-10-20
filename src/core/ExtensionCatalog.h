#pragma once
#include <filesystem>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace summit {
struct InstalledExtension {
    std::string identifier;
    std::string name;
    std::string version;
    std::string fingerprint;
    std::string package;
    bool enabled = true;
    bool allowFileURLs = false;
    bool allowPrivateBrowsing = false;
    uint64_t installationOrder = 0;
    // Where updates come from: "chrome" (the Chrome Web Store, by the identity)
    // or "firefox" (Firefox Add-ons, by the add-on ID); empty for none.
    std::string source;
    // The manifest's update_url, for Chrome packages.
    std::string updateURL;
    bool autoUpdate = true;
    // When an update was installed and when updates were last looked for,
    // in seconds since the epoch (0: never).
    int64_t updated = 0;
    int64_t checked = 0;
    // The required permissions and sites the user approved for this version
    // (an update asking for more needs approval again); empty when unknown.
    std::vector<std::string> permissions, origins;
};

// An import owns a private copy until Install commits it to the catalog. Pass
// Path() to WebKit's preparation API, then obtain consent for that fingerprint.
// Imports do not parse manifests, authenticate packages, or grant permissions.
class StagedExtensionPackage final {
public:
    ~StagedExtensionPackage();
    StagedExtensionPackage(const StagedExtensionPackage&) = delete;
    StagedExtensionPackage& operator=(const StagedExtensionPackage&) = delete;
    const std::filesystem::path& Path() const { return fPath; }
private:
    friend class ExtensionCatalog;
    explicit StagedExtensionPackage(std::filesystem::path directory);
    std::filesystem::path fDirectory, fPath;
    bool fInstalled = false;
};

// Filesystem operations are synchronous; run imports on a worker and serialize
// catalog mutations. The root is the browser profile's dedicated Extensions
// directory. Runtime grants remain in WebKit's fingerprint-bound saved state.
class ExtensionCatalog final {
public:
    explicit ExtensionCatalog(std::filesystem::path root);
    bool Load(std::vector<InstalledExtension>&, std::string& error) const;
    static uint64_t NextInstallationOrder(const std::vector<InstalledExtension>&);
    std::unique_ptr<StagedExtensionPackage> Stage(const std::filesystem::path& source, std::string& error) const;
    bool Install(StagedExtensionPackage&, InstalledExtension, std::string& error) const;
    bool SetEnabled(const std::string& identifier, bool enabled, std::string& error) const;
    // Whether the extension also works in private windows.
    bool SetAllowPrivateBrowsing(const std::string& identifier, bool allowed, std::string& error) const;
    // Forget only the installation record. Unload first; package/data removal
    // is a separate operation so a failed metadata write cannot lose resources.
    bool Forget(const std::string& identifier, std::string& error) const;
    // Change an installation's record (not its identity, package or order).
    bool Modify(const std::string& identifier, const std::function<void(InstalledExtension&)>&, std::string& error) const;
    // Commit a new version of an installed extension: its record takes the
    // staged package, name, version, fingerprint and update details of entry.
    // The replaced package stays until RemovePackage.
    bool Replace(StagedExtensionPackage&, const InstalledExtension& entry, std::string& error) const;
    // Delete a package that no record names any more.
    bool RemovePackage(const std::string& package, std::string& error) const;
    std::filesystem::path PackagePath(const InstalledExtension&) const;
private:
    bool Save(const std::vector<InstalledExtension>&, std::string& error) const;
    std::filesystem::path fRoot;
};
}

#pragma once
#include "core/ExtensionCatalog.h"
#include "core/ExtensionStore.h"
#include <atomic>

namespace summit {
// Downloads into a private temporary directory beside the profile, checks the
// store metadata, stages an immutable copy, and removes all temporary files.
// Runs on the installer's worker; cancellation also aborts network transfers.
std::unique_ptr<StagedExtensionPackage> DownloadStoreExtension(const ExtensionStoreItem&,
    const std::filesystem::path& temporaryRoot, const ExtensionCatalog&, const std::atomic<bool>& cancelled,
    std::string& expectedIdentity, std::string& error);
// The add-on ID and name of a Firefox Add-ons item, from Mozilla's API: its
// pages name it by a slug, an installation by its ID. extension is false for
// themes, dictionaries and the like. Blocks for the network; run it on a
// thread of its own. False with error on failure.
bool ResolveFirefoxStoreItem(const ExtensionStoreItem&, std::string& identifier, std::string& name, bool& extension,
    std::string& error);
}

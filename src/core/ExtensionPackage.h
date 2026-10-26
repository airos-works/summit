#pragma once
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace summit {
// A file of an installed extension package, as the profile keeps it: a CRX3,
// ZIP or XPI archive (stored or deflated entries, checked against their CRC),
// or an unpacked folder. name is the path inside the package
// ("icons/48.png"). Nothing when the file is missing, damaged or larger than
// limit. Only reads; nothing in the package is run.
std::optional<std::string> ReadExtensionPackageFile(const std::filesystem::path& package, std::string name,
    size_t limit = 4 * 1024 * 1024);

// The raster image (PNG, usually) of the extension's own icon that suits size
// pixels best: the smallest of the manifest's "icons" at least that large,
// else the largest, else its toolbar action's default icon. Empty when the
// package has none Summit can read.
std::string ExtensionIconData(const std::filesystem::path& package, int size);

// A Safari web extension, as macOS ships it: an app (Name.app) whose
// Contents/PlugIns has an app extension (Name Extension.appex) with the web
// extension in its Contents/Resources (manifest.json and the rest), or that
// .appex itself. The Resources folder of the first such extension, to import
// unpacked; nothing for anything else.
std::optional<std::filesystem::path> SafariWebExtensionResources(const std::filesystem::path& bundle);
}

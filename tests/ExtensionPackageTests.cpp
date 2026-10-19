#include "core/ExtensionPackage.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

// ExtensionPackageTests FIXTURES [PACKAGE...]: FIXTURES is
// tests/fixtures/extensions/package-icons. Extra packages (real CRX, ZIP or
// XPI downloads) only need a readable manifest and icon.
int main(int argc, char** argv)
{
    using namespace summit;
    if (argc < 2) { std::cerr << "usage: " << argv[0] << " FIXTURES [PACKAGE...]\n"; return 2; }
    const std::filesystem::path fixtures = argv[1];
    unsigned checks = 0;
    auto check = [&](bool value, const std::string& label) {
        ++checks;
        if (!value) { std::cerr << "FAIL: " << label << '\n'; std::exit(1); }
    };
    auto file = [&](const std::string& name) {
        std::ifstream input(fixtures / "unpacked" / name, std::ios::binary);
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    };
    for (const char* package : { "unpacked", "package.zip", "package.crx", "stored.xpi" }) {
        const auto path = fixtures / package;
        const std::string label = package;
        check(ReadExtensionPackageFile(path, "manifest.json") == file("manifest.json"), label + " manifest");
        check(ReadExtensionPackageFile(path, "/icons/48.png") == file("icons/48.png"), label + " leading slash");
        check(!ReadExtensionPackageFile(path, "icons/../manifest.json"), label + " no parent paths");
        check(!ReadExtensionPackageFile(path, "icons/missing.png"), label + " missing file");
        check(!ReadExtensionPackageFile(path, "icons/128.png", 100), label + " size limit");
        // Exact sizes, the next larger one, the largest for a larger screen;
        // SVG icons are skipped.
        check(ExtensionIconData(path, 16) == file("icons/16.png"), label + " 16");
        check(ExtensionIconData(path, 32) == file("icons/48.png"), label + " 32");
        check(ExtensionIconData(path, 64) == file("icons/128.png"), label + " 64");
        check(ExtensionIconData(path, 256) == file("icons/128.png"), label + " 256");
    }
    // Dynamic Huffman blocks and long matches.
    for (const char* package : { "unpacked", "package.zip", "package.crx" })
        check(ReadExtensionPackageFile(fixtures / package, "background.js") == file("background.js"),
            std::string(package) + " deflated text");
    // A corrupted member fails its CRC instead of returning damaged bytes.
    {
        std::ifstream input(fixtures / "stored.xpi", std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto png = bytes.find("\x89PNG");
        check(png != std::string::npos, "stored icon found");
        bytes[png + 20] ^= 0x55;
        const auto damaged = std::filesystem::temp_directory_path() / "summit-damaged-package.xpi";
        std::ofstream(damaged, std::ios::binary) << bytes;
        check(!ReadExtensionPackageFile(damaged, "icons/16.png"), "damaged member rejected");
        std::filesystem::remove(damaged);
    }
    check(ExtensionIconData(fixtures / "action-only", 16) == [&] {
        std::ifstream input(fixtures / "action-only/images/32.png", std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    }(), "action icon by size, skipping a missing one");
    check(ExtensionIconData(fixtures / "missing-package", 32).empty(), "no package");
    for (int i = 2; i < argc; ++i) {
        const auto icon = ExtensionIconData(argv[i], 32);
        check(icon.size() > 8 && icon.compare(0, 4, "\x89PNG") == 0, std::string("icon of ") + argv[i]);
    }
    std::cout << checks << " extension package checks passed\n";
}

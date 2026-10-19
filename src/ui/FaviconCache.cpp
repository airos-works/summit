#include "FaviconCache.h"
#include "core/Favicon.h"
#include <Bitmap.h>
#include <DataIO.h>
#include <TranslationUtils.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unistd.h>

namespace summit {
static Image ToImage(const BBitmap& source)
{
    Image image;
    BBitmap converted(source.Bounds(), B_RGBA32);
    if (converted.InitCheck() != B_OK || converted.ImportBits(&source) != B_OK) return image;
    const color_space space = source.ColorSpace();
    const bool alpha = space == B_RGBA32 || space == B_RGBA32_BIG || space == B_RGBA15 || space == B_RGBA15_BIG
        || space == B_CMAP8;
    image.width = converted.Bounds().IntegerWidth() + 1;
    image.height = converted.Bounds().IntegerHeight() + 1;
    image.rgba.resize(size_t(image.width) * image.height * 4);
    for (int y = 0; y < image.height; ++y) {
        const auto* row = static_cast<const uint8*>(converted.Bits()) + size_t(y) * converted.BytesPerRow();
        for (int x = 0; x < image.width; ++x) {
            uint8_t* pixel = &image.rgba[(size_t(y) * image.width + x) * 4];
            pixel[0] = row[x * 4 + 2];
            pixel[1] = row[x * 4 + 1];
            pixel[2] = row[x * 4];
            pixel[3] = alpha ? row[x * 4 + 3] : 255;
        }
    }
    return image;
}

static std::unique_ptr<BBitmap> ToBitmap(const Image& image)
{
    if (image.Empty()) return nullptr;
    auto bitmap = std::make_unique<BBitmap>(BRect(0, 0, image.width - 1, image.height - 1), B_RGBA32);
    if (bitmap->InitCheck() != B_OK) return nullptr;
    for (int y = 0; y < image.height; ++y) {
        auto* row = static_cast<uint8*>(bitmap->Bits()) + size_t(y) * bitmap->BytesPerRow();
        for (int x = 0; x < image.width; ++x) {
            const uint8_t* pixel = &image.rgba[(size_t(y) * image.width + x) * 4];
            row[x * 4] = pixel[2];
            row[x * 4 + 1] = pixel[1];
            row[x * 4 + 2] = pixel[0];
            row[x * 4 + 3] = pixel[3];
        }
    }
    return bitmap;
}

// Any format the Translation Kit knows.
static Image Translate(const void* data, size_t size)
{
    BMemoryIO stream(data, size);
    std::unique_ptr<BBitmap> bitmap(BTranslationUtils::GetBitmap(&stream));
    return bitmap ? ToImage(*bitmap) : Image();
}

static Image Decode(const void* data, size_t size)
{
    const std::string_view bytes(static_cast<const char*>(data), size);
    if (IsIco(bytes)) {
        // Choose the entry ourselves: the translator takes whichever comes first.
        const auto entry = SelectIcoEntry(bytes, 32);
        Image image;
        if (entry.kind == IcoEntryKind::DIB && DecodeDIB(entry.bytes, image)) return image;
        if (entry.kind == IcoEntryKind::PNG) {
            image = Translate(entry.bytes.data(), entry.bytes.size());
            if (!image.Empty()) return image;
        }
    }
    return Translate(data, size);
}

std::unique_ptr<BBitmap> FaviconCache::DecodeIcon(const void* data, size_t size, int pixels)
{
    if (!data || !size) return nullptr;
    return ToBitmap(ScaleImage(Decode(data, size), pixels));
}

FaviconCache::FaviconCache(std::filesystem::path directory, bool persistent)
    : fDirectory(std::move(directory)), fPersistent(persistent) { }
FaviconCache::~FaviconCache() = default;

FaviconCache::Entry* FaviconCache::Load(const std::string& key)
{
    if (key.empty()) return nullptr;
    auto [found, inserted] = fEntries.try_emplace(key);
    if (!inserted) return found->second.get();
    std::ifstream file(fDirectory / (key + ".png"), std::ios::binary);
    if (!file) return nullptr;
    std::string png((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (png.empty() || png.size() > 256 * 1024) return nullptr;
    auto bitmap = ToBitmap(Translate(png.data(), png.size()));
    if (!bitmap) return nullptr;
    found->second = std::make_unique<Entry>(Entry{std::move(bitmap), std::move(png)});
    return found->second.get();
}

FaviconCache::Entry* FaviconCache::Find(const std::string& pageURL)
{
    const auto key = FaviconKey(pageURL);
    if (auto* entry = Load(key)) return entry;
    // www.example.com and example.com are nearly always the same site.
    return Load(key.rfind("www.", 0) == 0 ? key.substr(4) : key.empty() ? key : "www." + key);
}

bool FaviconCache::Store(const std::string& pageURL, const void* data, size_t size)
{
    const auto key = FaviconKey(pageURL);
    if (key.empty() || !data || !size) return false;
    // Twice the 16 point size shown, for screens drawn at 200%.
    auto scaled = ScaleImage(Decode(data, size), 32);
    auto png = EncodePNG(scaled);
    auto bitmap = ToBitmap(scaled);
    if (png.empty() || !bitmap) return false;
    if (auto* existing = Load(key); existing && existing->png == png) return false;
    if (!fPersistent) {
        fEntries[key] = std::make_unique<Entry>(Entry{std::move(bitmap), std::move(png)});
        return true;
    }
    std::error_code error;
    std::filesystem::create_directories(fDirectory, error);
    const auto path = fDirectory / (key + ".png");
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(png.data(), png.size());
        if (!file.flush()) error = std::make_error_code(std::errc::io_error);
    }
    if (error || std::rename(temporary.c_str(), path.c_str()) != 0) unlink(temporary.c_str());
    fEntries[key] = std::make_unique<Entry>(Entry{std::move(bitmap), std::move(png)});
    return true;
}

const BBitmap* FaviconCache::Icon(const std::string& pageURL)
{
    auto* entry = Find(pageURL);
    return entry ? entry->bitmap.get() : nullptr;
}

std::string FaviconCache::DataURL(const std::string& pageURL)
{
    auto* entry = Find(pageURL);
    return entry ? "data:image/png;base64," + Base64(entry->png) : std::string();
}

void FaviconCache::Forget(const std::string& pageURL)
{
    const auto key = FaviconKey(pageURL);
    if (key.empty()) return;
    fEntries.erase(key);
    fEntries.erase(key.rfind("www.", 0) == 0 ? key.substr(4) : "www." + key);
}
}

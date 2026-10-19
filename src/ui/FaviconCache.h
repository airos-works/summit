#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <string>

class BBitmap;

namespace summit {
// Site icons, 16x16, keyed by host and kept as PNG files in the profile so
// tabs, the bookmarks bar and the built-in pages can show them after a restart.
// Each browser window has its own cache, used only from its thread.
class FaviconCache {
public:
    // A cache that does not persist (a private window's) reads icons from the
    // directory but keeps the ones it is given in memory only.
    explicit FaviconCache(std::filesystem::path directory, bool persistent = true);
    ~FaviconCache();
    // Decodes an icon file (ICO, PNG, GIF, JPEG, WebP…) delivered for a page.
    // Returns true when that site's icon changed. Previously returned bitmaps
    // for the site are then released.
    bool Store(const std::string& pageURL, const void* data, size_t size);
    // The site's icon, or null. Owned by the cache.
    const BBitmap* Icon(const std::string& pageURL);
    // The site's icon as a data: URL for the built-in pages, or empty.
    std::string DataURL(const std::string& pageURL);
    // Drops the site's cached icon so it is read from disk again (another
    // window stored a new one). Previously returned bitmaps are released.
    void Forget(const std::string& pageURL);
    // Decodes an icon file as Store() does, scaled to pixels x pixels
    // (B_RGBA32), or null. Any thread.
    static std::unique_ptr<BBitmap> DecodeIcon(const void* data, size_t size, int pixels);

private:
    struct Entry {
        std::unique_ptr<BBitmap> bitmap;
        std::string png;
    };
    Entry* Find(const std::string& pageURL);
    Entry* Load(const std::string& key);
    std::filesystem::path fDirectory;
    bool fPersistent = true;
    // A null entry records a site known to have no icon on disk.
    std::map<std::string, std::unique_ptr<Entry>> fEntries;
};
}

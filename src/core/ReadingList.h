#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace summit {
// Reading List (#156): pages kept to read later, as in Safari. Each can have
// an offline copy, a web archive (MHTML) in the profile's ReadingList folder
// named after its id, shown when the page itself cannot be loaded.
struct ReadingListItem {
    // Lower-case hex digits, unique in the list; also the offline copy's name.
    std::string id;
    std::string url, title;
    // When it was added, in seconds since the epoch.
    int64_t added = 0;
    // When it was last opened from the Reading List; 0 while unread.
    int64_t read = 0;
    // When its offline copy was saved; 0 when it has none.
    int64_t archived = 0;
    bool operator==(const ReadingListItem& other) const
    {
        return id == other.id && url == other.url && title == other.title && added == other.added && read == other.read
            && archived == other.archived;
    }
};
constexpr size_t kMaxReadingListItems = 2000;
// Pages the Reading List keeps: http and https addresses.
bool IsReadingListURL(const std::string& url);
// Whether an id is one the Reading List makes (and so safe as a file name).
bool IsReadingListID(std::string_view id);
// The same page: addresses equal without their fragments.
bool SameReadingListPage(std::string_view a, std::string_view b);
// The file name of an item's offline copy ("<id>.mhtml").
std::string ReadingListArchiveName(const std::string& id);
// The item of an address (fragments ignored), or nullptr.
const ReadingListItem* FindReadingListURL(const std::vector<ReadingListItem>& list, std::string_view url);
ReadingListItem* FindReadingListItem(std::vector<ReadingListItem>& list, std::string_view id);
// Adds a page at the top (unread), or moves the existing item of its address
// there, unread again, with the newer title. Returns the item's id; empty
// when the address is not kept. Past the limit the oldest read items go
// (their ids in dropped, for their offline copies).
std::string AddToReadingList(std::vector<ReadingListItem>& list, const std::string& url, const std::string& title,
    int64_t now, std::vector<std::string>* dropped = nullptr);
size_t CountUnread(const std::vector<ReadingListItem>& list);
}

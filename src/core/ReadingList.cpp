#include "ReadingList.h"
#include <algorithm>
#include <random>

namespace summit {
bool IsReadingListURL(const std::string& url)
{
    return (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0) && url.size() <= 65536
        && url.find_first_of(std::string("\0\r\n", 3)) == std::string::npos;
}

bool IsReadingListID(std::string_view id)
{
    return !id.empty() && id.size() <= 32 && id.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

static std::string_view WithoutFragment(std::string_view url)
{
    return url.substr(0, url.find('#'));
}

bool SameReadingListPage(std::string_view a, std::string_view b)
{
    return WithoutFragment(a) == WithoutFragment(b);
}

std::string ReadingListArchiveName(const std::string& id)
{
    return id + ".mhtml";
}

const ReadingListItem* FindReadingListURL(const std::vector<ReadingListItem>& list, std::string_view url)
{
    for (const auto& item : list) if (SameReadingListPage(item.url, url)) return &item;
    return nullptr;
}

ReadingListItem* FindReadingListItem(std::vector<ReadingListItem>& list, std::string_view id)
{
    for (auto& item : list) if (item.id == id) return &item;
    return nullptr;
}

std::string AddToReadingList(std::vector<ReadingListItem>& list, const std::string& url, const std::string& title,
    int64_t now, std::vector<std::string>* dropped)
{
    if (!IsReadingListURL(url)) return {};
    auto found = std::find_if(list.begin(), list.end(), [&](const ReadingListItem& item) { return SameReadingListPage(item.url, url); });
    ReadingListItem item;
    if (found != list.end()) {
        item = std::move(*found);
        list.erase(found);
    } else {
        static std::mt19937_64 random { std::random_device()() };
        do {
            const uint64_t value = random();
            item.id.clear();
            for (int shift = 60; shift >= 0; shift -= 4) item.id += "0123456789abcdef"[(value >> shift) & 15];
        } while (std::any_of(list.begin(), list.end(), [&](const ReadingListItem& other) { return other.id == item.id; }));
    }
    item.url = url;
    if (!title.empty() || item.title.empty()) item.title = title.size() > 1024 ? title.substr(0, 1024) : title;
    item.added = now;
    item.read = 0;
    list.insert(list.begin(), item);
    while (list.size() > kMaxReadingListItems) {
        // The oldest read item, else the oldest.
        auto last = std::find_if(list.rbegin(), list.rend(), [](const ReadingListItem& old) { return old.read != 0; });
        auto drop = last != list.rend() ? std::prev(last.base()) : std::prev(list.end());
        if (dropped) dropped->push_back(drop->id);
        list.erase(drop);
    }
    return item.id;
}

size_t CountUnread(const std::vector<ReadingListItem>& list)
{
    return std::count_if(list.begin(), list.end(), [](const ReadingListItem& item) { return item.read == 0; });
}
}

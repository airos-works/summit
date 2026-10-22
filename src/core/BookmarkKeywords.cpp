#include "Bookmarks.h"
#include "Address.h"
#include <algorithm>
#include <cerrno>
#include <dlfcn.h>
#include <functional>

namespace summit {
namespace {
void UTF8(std::string& into, uint32_t value)
{
    if (value >= 0xd800 && value <= 0xdfff) value = 0xfffd;
    if (value < 0x80) into += char(value);
    else if (value < 0x800) { into += char(0xc0 | (value >> 6)); into += char(0x80 | (value & 63)); }
    else if (value < 0x10000) {
        into += char(0xe0 | (value >> 12)); into += char(0x80 | ((value >> 6) & 63)); into += char(0x80 | (value & 63));
    } else {
        into += char(0xf0 | (value >> 18)); into += char(0x80 | ((value >> 12) & 63));
        into += char(0x80 | ((value >> 6) & 63)); into += char(0x80 | (value & 63));
    }
}

// Firefox stores escaped POST templates and applies JavaScript unescape()
// before binding them. %XX denotes a Latin-1 code point, %uXXXX UTF-16.
std::string Unescape(std::string_view text)
{
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    auto code = [&](size_t start, size_t count) -> int {
        if (start + count > text.size()) return -1;
        int value = 0;
        for (size_t i = start; i < start + count; ++i) {
            if (hex(text[i]) < 0) return -1;
            value = value * 16 + hex(text[i]);
        }
        return value;
    };
    std::string result;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '%') {
            const bool unicode = i + 1 < text.size() && text[i + 1] == 'u';
            int value = code(i + (unicode ? 2 : 1), unicode ? 4 : 2);
            if (value >= 0) {
                i += unicode ? 6 : 3;
                if (unicode && value >= 0xd800 && value <= 0xdbff && text.substr(i, 2) == "%u") {
                    const int low = code(i + 2, 4);
                    if (low >= 0xdc00 && low <= 0xdfff) { value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00; i += 6; }
                }
                UTF8(result, value);
                continue;
            }
        }
        result += text[i++];
    }
    return result;
}

std::string InCharset(const std::string& input, const std::string& charset)
{
    if (charset.empty() || charset == "UTF-8" || charset == "utf-8") return input;
    struct Converter {
#ifdef __HAIKU__
        void* library = dlopen("libiconv.so.2", RTLD_LAZY | RTLD_LOCAL);
        const char* prefix = "libiconv";
#else
        void* library = dlopen(nullptr, RTLD_LAZY | RTLD_LOCAL);
        const char* prefix = "iconv";
#endif
        using Open = void* (*)(const char*, const char*);
        using Convert = size_t (*)(void*, char**, size_t*, char**, size_t*);
        using Close = int (*)(void*);
        Open open = library ? reinterpret_cast<Open>(dlsym(library, (std::string(prefix) + "_open").c_str())) : nullptr;
        Convert convert = library ? reinterpret_cast<Convert>(dlsym(library, prefix)) : nullptr;
        Close close = library ? reinterpret_cast<Close>(dlsym(library, (std::string(prefix) + "_close").c_str())) : nullptr;
        ~Converter() { if (library) dlclose(library); }
    };
    static const Converter converter;
    if (!converter.open || !converter.convert || !converter.close) return input;
    void* state = converter.open(charset.c_str(), "UTF-8");
    if (state == reinterpret_cast<void*>(-1)) return input;
    std::string output(input.size() * 8 + 64, '\0');
    char* from = const_cast<char*>(input.data());
    char* to = output.data();
    size_t fromSize = input.size(), toSize = output.size();
    const bool converted = converter.convert(state, &from, &fromSize, &to, &toSize) != size_t(-1)
        && converter.convert(state, nullptr, nullptr, &to, &toSize) != size_t(-1);
    converter.close(state);
    // Firefox also falls back to UTF-8 for unavailable/failed conversions.
    if (!converted) return input;
    output.resize(output.size() - toSize);
    return output;
}

std::string EncodedParameter(const std::string& input, const std::string& charset)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : InCharset(input, charset)) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || std::string_view("-_.!~*'()").find(c) != std::string_view::npos) result += char(c);
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}

bool Parameterized(const std::string& text) { return text.find("%s") != std::string::npos || text.find("%S") != std::string::npos; }
std::string Bind(const std::string& text, const std::string& encoded, const std::string& raw)
{
    std::string result;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 1 < text.size() && (text[i + 1] == 's' || text[i + 1] == 'S')) {
            const auto& value = text[++i] == 's' ? encoded : raw;
            if (result.size() + value.size() > 1024 * 1024) return std::string(1024 * 1024 + 1, 'x');
            result += value;
        } else result += text[i];
    }
    return result;
}
}

std::optional<BookmarkNavigation> Bookmarks::ResolveKeyword(std::string_view input) const
{
    const auto text = Trim(input);
    if (text.empty() || text.size() > 65536
        || std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; })) return std::nullopt;
    const auto end = text.find(' ');
    auto name = text.substr(0, end);
    if (name.find_first_of(":/") != std::string::npos) return std::nullopt;
    for (char& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    const auto parameter = end == std::string::npos ? std::string() : Trim(text.substr(end + 1));
    const BookmarkNode* bookmark = nullptr;
    const BookmarkKeyword* keyword = nullptr;
    bool ambiguous = false;
    std::function<void(const BookmarkNode&)> visit = [&](const BookmarkNode& folder) {
        for (const auto& child : folder.children) {
            if (child.folder) { visit(child); continue; }
            if (child.separator) continue;
            for (const auto& candidate : child.metadata.keywords) if (candidate.name == name) {
                if (keyword && (bookmark->url != child.url || !(*keyword == candidate))) ambiguous = true;
                else { bookmark = &child; keyword = &candidate; }
            }
        }
    };
    visit(Bar()); visit(Other());
    if (!keyword) return std::nullopt;
    BookmarkNavigation result;
    if (ambiguous) {
        result.error = "More than one bookmark uses the shortcut “" + name + "”. Edit their keywords in Bookmarks.";
        return result;
    }
    result.url = bookmark->url;
    result.title = bookmark->title;
    result.post = !keyword->postData.empty();
    const auto decoded = Unescape(keyword->postData);
    const bool urlParameter = Parameterized(result.url), postParameter = Parameterized(decoded);
    if (!urlParameter && !postParameter && !parameter.empty()) return std::nullopt;
    auto charset = keyword->charset;
    if (urlParameter || postParameter) {
        const auto suffix = result.url.rfind("&mozcharset=");
        if (suffix != std::string::npos) {
            const auto value = result.url.substr(suffix + 12);
            if (!value.empty() && value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos) {
                charset = value;
                result.url.resize(suffix);
            }
        }
        const auto encoded = EncodedParameter(parameter, charset);
        result.url = Bind(result.url, encoded, parameter);
        result.postData = postParameter ? Bind(decoded, encoded, parameter) : keyword->postData;
    } else result.postData = keyword->postData;
    // Only a resolved, saved keyword can enter the bookmarklet route; raw
    // javascript typed/pasted into the address field is still rejected.
    const auto address = IsBookmarkletURL(result.url) ? Address{result.url, {}, false} : ResolveAddress(result.url);
    if (!address.error.empty() || !IsBookmarkableURL(result.url)
        || result.postData.size() > 1024 * 1024 || result.postData.find('\0') != std::string::npos)
        result.error = "The bookmark shortcut produces an invalid or oversized request.";
    else if (result.post && address.url.rfind("http://", 0) != 0 && address.url.rfind("https://", 0) != 0)
        result.error = "A bookmark with POST search data needs an HTTP or HTTPS address.";
    else result.url = address.url;
    return result;
}
}

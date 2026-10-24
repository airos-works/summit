#include "PageLanguage.h"
#include "cld2/public/compact_lang_det.h"
#include <algorithm>

namespace summit {
namespace {
bool Letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool Digit(char c) { return c >= '0' && c <= '9'; }
char Lower(char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
}

std::string PageLanguageKey(std::string_view locale)
{
    if (locale.empty() || locale.size() > 100) return {};
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= locale.size(); ++i) {
        if (i != locale.size() && locale[i] != '-' && locale[i] != '_') continue;
        auto part = locale.substr(start, i - start);
        if (part.empty() || part.size() > 8
            || !std::all_of(part.begin(), part.end(), [](char c) { return Letter(c) || Digit(c); })) return {};
        parts.emplace_back(part);
        std::transform(parts.back().begin(), parts.back().end(), parts.back().begin(), Lower);
        start = i + 1;
    }
    std::string language = parts.front();
    if (language.size() < 2 || language.size() > 3
        || !std::all_of(language.begin(), language.end(), Letter)
        || language == "und" || language == "mul" || language == "zxx"
        || language == "un" || language == "xxx") return {};
    if (language == "iw") language = "he";
    if (language == "ji") language = "yi";
    if (language == "in") language = "id";

    std::string script, region;
    size_t i = 1;
    if (i < parts.size() && parts[i].size() == 4 && std::all_of(parts[i].begin(), parts[i].end(), Letter))
        script = parts[i++];
    if (i < parts.size() && ((parts[i].size() == 2 && std::all_of(parts[i].begin(), parts[i].end(), Letter))
        || (parts[i].size() == 3 && std::all_of(parts[i].begin(), parts[i].end(), Digit))))
        region = parts[i++];
    // Skip variants, followed by extensions/private use. We do not need a
    // registry of every variant, but reject incomplete or malformed subtags.
    while (i < parts.size() && (parts[i].size() >= 5
        || (parts[i].size() == 4 && Digit(parts[i][0])))) ++i;
    while (i < parts.size()) {
        if (parts[i].size() != 1 || i + 1 == parts.size()) return {};
        if (parts[i++] == "x") { i = parts.size(); break; }
        const auto first = i;
        while (i < parts.size() && parts[i].size() >= 2) ++i;
        if (i == first) return {};
    }
    if (language == "zh" && script.empty())
        script = region == "tw" || region == "hk" || region == "mo" ? "hant" : "hans";
    if (language == "zh" && !script.empty()) {
        script[0] -= 'a' - 'A';
        language += '-' + script;
    }
    return language;
}

PageLanguage DetectPageLanguage(std::string_view text)
{
    PageLanguage result;
    if (text.empty() || text.size() > kPageLanguageSampleLimit) return result;
    CLD2::Language languages[3];
    int percentages[3];
    double scores[3];
    bool reliable = false;
    int validBytes = 0;
    auto language = CLD2::ExtDetectLanguageSummaryCheckUTF8(text.data(), static_cast<int>(text.size()),
        true, nullptr, 0, languages, percentages, scores, nullptr, &result.textBytes, &reliable, &validBytes);
    if (validBytes != static_cast<int>(text.size()) || language == CLD2::UNKNOWN_LANGUAGE)
        return {};
    result.code = PageLanguageKey(CLD2::LanguageCode(language));
    for (size_t i = 0; i < 3; ++i)
        if (languages[i] == language) result.percentage = percentages[i];
    // CLD2 can be confident about a single familiar word. A translation
    // prompt needs enough text to represent the page, with a clear majority.
    result.reliable = reliable && !result.code.empty() && result.textBytes >= 200 && result.percentage >= 80;
    return result;
}

TextLanguages DetectTextLanguages(std::string_view text)
{
    TextLanguages result;
    if (text.size() > kPageLanguageSampleLimit) {
        // Cut at a character boundary, not inside a UTF-8 sequence.
        size_t end = kPageLanguageSampleLimit;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
        text = text.substr(0, end);
    }
    if (text.empty()) return result;
    CLD2::Language languages[3];
    int percentages[3];
    double scores[3];
    bool reliable = false;
    int textBytes = 0, validBytes = 0;
    auto language = CLD2::ExtDetectLanguageSummaryCheckUTF8(text.data(), static_cast<int>(text.size()),
        true, nullptr, 0, languages, percentages, scores, nullptr, &textBytes, &reliable, &validBytes);
    if (validBytes != static_cast<int>(text.size()) || language == CLD2::UNKNOWN_LANGUAGE) return result;
    for (size_t i = 0; i < 3; ++i)
        if (languages[i] != CLD2::UNKNOWN_LANGUAGE && percentages[i] > 0)
            result.languages.emplace_back(CLD2::LanguageCode(languages[i]), percentages[i]);
    result.reliable = reliable && !result.languages.empty();
    return result;
}

std::string PreferredPageLanguage(const std::vector<std::string>& preferred)
{
    return preferred.empty() ? std::string() : PageLanguageKey(preferred.front());
}

bool PageNeedsTranslation(const PageLanguage& page, std::string_view target)
{
    const auto sourceKey = PageLanguageKey(page.code), targetKey = PageLanguageKey(target);
    return page.reliable && !sourceKey.empty() && !targetKey.empty() && sourceKey != targetKey;
}
}

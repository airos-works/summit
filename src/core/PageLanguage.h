#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace summit {
struct PageLanguage {
    std::string code;
    int percentage = 0;
    int textBytes = 0;
    bool reliable = false;
};

// Samples are plain UTF-8 text, without markup, form values or hidden text.
// Oversized and malformed input is rejected; callers should bound extraction.
inline constexpr size_t kPageLanguageSampleLimit = 65536;
PageLanguage DetectPageLanguage(std::string_view text);

// A language key for comparing detected text to a system locale. Regions are
// ignored, except for Chinese's separately detected writing systems. Accepts
// Haiku's underscore-separated locales as well as BCP 47 language tags.
// Empty/invalid identifiers, C/POSIX and undetermined languages return empty.
std::string PageLanguageKey(std::string_view locale);

// Use the first system preference, not the first language supported by a
// translation service. Never silently choose a different target language.
std::string PreferredPageLanguage(const std::vector<std::string>& preferred);
bool PageNeedsTranslation(const PageLanguage& page, std::string_view target);
}

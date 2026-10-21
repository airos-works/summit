#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace summit {
struct TranslationModelFile {
    std::string part, url, sha256;
    uint64_t size = 0;
};
struct TranslationModel {
    std::string from, to, precision;
    std::vector<TranslationModelFile> files;
};
// Reads Summit's embedded, pinned registry. Invalid input returns no models;
// downloads are never allowed to supply or override this registry.
std::vector<TranslationModel> ReadTranslationModels(const std::string& registry);
// Direct model, or exactly two models via English. Empty means unavailable
// (or the languages are the same). The requested target is never substituted.
std::vector<TranslationModel> TranslationRoute(const std::vector<TranslationModel>&,
    const std::string& from, const std::string& to);
std::string TranslationLanguageName(const std::string& code);
}

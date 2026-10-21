#include "TranslationModels.h"
#include "PageLanguage.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>
#include <set>

namespace summit {
std::vector<TranslationModel> ReadTranslationModels(const std::string& registry)
{
    using json = nlohmann::json;
    if (registry.size() > 256000) return {};
    const auto data = json::parse(registry, nullptr, false);
    if (!data.is_object() || data.empty() || data.size() > 128) return {};
    std::vector<TranslationModel> result;
    try {
        for (const auto& [pair, record] : data.items()) {
            if (pair.size() != 4 || !std::all_of(pair.begin(), pair.end(), [](char c) { return c >= 'a' && c <= 'z'; })
                || !record.is_object()) return {};
            TranslationModel model {pair.substr(0, 2), pair.substr(2), "int8shiftAlphaAll", {}};
            if (model.from == model.to) return {};
            if (record.contains("config")) {
                const auto& config = record.at("config");
                if (!config.is_object() || config.size() != 1 || config.value("gemm-precision", "") != "int8shiftAll") return {};
                model.precision = "int8shiftAll";
            }
            std::set<std::string> parts;
            for (const auto& [part, file] : record.items()) {
                if (part == "config") continue;
                if (part != "model" && part != "lex" && part != "vocab" && part != "srcvocab" && part != "trgvocab") return {};
                if (!file.is_object() || !file.at("size").is_number_unsigned()) return {};
                TranslationModelFile entry {part, file.at("name").get<std::string>(), file.at("expectedSha256Hash").get<std::string>(), file.at("size").get<uint64_t>()};
                constexpr std::string_view prefix = "https://bergamot.s3.amazonaws.com/models/";
                if (entry.url.compare(0, prefix.size(), prefix) || entry.url.size() > 300 || entry.url.find("..") != std::string::npos
                    || !std::all_of(entry.url.begin() + prefix.size(), entry.url.end(), [](char c) {
                        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                            || c == '/' || c == '.' || c == '_' || c == '-';
                    }) || entry.sha256.size() != 64
                    || !std::all_of(entry.sha256.begin(), entry.sha256.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })
                    || !entry.size || entry.size > 64 * 1024 * 1024) return {};
                parts.insert(part);
                model.files.push_back(std::move(entry));
            }
            if (parts != std::set<std::string>{"model", "lex", "vocab"}
                && parts != std::set<std::string>{"model", "lex", "srcvocab", "trgvocab"}) return {};
            result.push_back(std::move(model));
        }
    } catch (...) { return {}; }
    return result;
}

std::vector<TranslationModel> TranslationRoute(const std::vector<TranslationModel>& models,
    const std::string& from, const std::string& to)
{
    const auto source = PageLanguageKey(from), target = PageLanguageKey(to);
    if (source.empty() || target.empty() || source == target) return {};
    auto find = [&](const std::string& a, const std::string& b) -> const TranslationModel* {
        for (const auto& model : models) if (model.from == a && model.to == b) return &model;
        return nullptr;
    };
    if (const auto* direct = find(source, target)) return {*direct};
    if (source == "en" || target == "en") return {};
    const auto* first = find(source, "en");
    const auto* second = find("en", target);
    return first && second ? std::vector<TranslationModel>{*first, *second} : std::vector<TranslationModel>{};
}

std::string TranslationLanguageName(const std::string& code)
{
    static const std::map<std::string, std::string> names {
        {"bg", "Bulgarian"}, {"cs", "Czech"}, {"de", "German"}, {"en", "English"},
        {"es", "Spanish"}, {"et", "Estonian"}, {"fr", "French"}, {"it", "Italian"},
        {"pt", "Portuguese"}, {"ru", "Russian"}, {"uk", "Ukrainian"}
    };
    const auto key = PageLanguageKey(code);
    const auto it = names.find(key);
    return it == names.end() ? key : it->second;
}
}

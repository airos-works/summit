#include "Spelling.h"
#include <algorithm>
#include <filesystem>
#include <set>

namespace summit {
std::vector<std::string> SpellingDictionaries(const std::vector<std::string>& directories)
{
    std::set<std::string> names;
    for (const auto& directory : directories) {
        std::error_code error;
        for (auto it = std::filesystem::directory_iterator(directory, error); !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
            const auto path = it->path();
            if (path.extension() != ".aff") continue;
            auto dictionary = path;
            dictionary.replace_extension(".dic");
            const auto name = path.stem().string();
            if (!name.empty() && name.find("..") == std::string::npos && std::filesystem::is_regular_file(dictionary, error))
                names.insert(name);
        }
    }
    return {names.begin(), names.end()};
}

std::string SpellingLanguages(const std::string& chosen, const std::vector<std::string>& preferred,
    const std::vector<std::string>& available)
{
    std::vector<std::string> result;
    auto add = [&](const std::string& name) {
        if (std::find(result.begin(), result.end(), name) == result.end()) result.push_back(name);
    };
    auto installed = [&](const std::string& name) {
        return std::find(available.begin(), available.end(), name) != available.end();
    };
    if (!chosen.empty() && installed(chosen)) add(chosen);
    else {
        for (auto language : preferred) {
            std::replace(language.begin(), language.end(), '-', '_');
            if (language.empty()) continue;
            if (installed(language)) { add(language); continue; }
            // "de" or "de_AT": the language's dictionaries, its main region's first.
            const auto base = language.substr(0, language.find('_'));
            std::string upper = base;
            std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
            if (installed(base + "_" + upper)) add(base + "_" + upper);
            for (const auto& name : available)
                if (name == base || name.rfind(base + "_", 0) == 0) add(name);
        }
    }
    add("en_US");
    std::string text;
    for (const auto& name : result) text += (text.empty() ? "" : ",") + name;
    return text;
}
}

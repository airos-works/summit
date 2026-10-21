#include "core/TranslationModels.h"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>

int main(int argc, char** argv)
{
    using namespace summit;
    unsigned checks = 0;
    auto check = [&](bool value, const char* label) { ++checks; if (!value) { std::cerr << "FAIL " << label << '\n'; std::exit(1); } };
    check(argc == 2, "model registry argument");
    std::ifstream file(argv[1]);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto models = ReadTranslationModels(text);
    check(models.size() == 20, "all pinned directed models");
    const auto german = TranslationRoute(models, "de-DE", "en_AU");
    check(german.size() == 1 && german.front().from == "de" && german.front().to == "en", "direct and regional route");
    uint64_t bytes = 0;
    for (const auto& part : german.front().files) bytes += part.size;
    check(bytes == 22972674, "verified German model size");
    const auto french = TranslationRoute(models, "de", "fr_CA");
    check(french.size() == 2 && french[0].from == "de" && french[0].to == "en"
        && french[1].from == "en" && french[1].to == "fr", "pivot uses requested target");
    check(TranslationRoute(models, "de", "ja").empty(), "unsupported target never replaced by English");
    check(TranslationRoute(models, "ja", "de").empty(), "unsupported source");
    check(TranslationRoute(models, "de", "de_AT").empty(), "no same-language route");
    check(TranslationRoute(models, "", "en").empty(), "missing source");
    check(TranslationRoute(models, "de", "").empty(), "missing target");
    for (const auto& model : models) {
        check(TranslationRoute(models, model.from, model.to).size() == 1, "every advertised model has a direct route");
        check(model.precision == ((model.from == "uk" || model.to == "uk") ? "int8shiftAll" : "int8shiftAlphaAll"), "model precision");
    }
    const auto registry = nlohmann::json::parse(text);
    for (auto value : {nlohmann::json(-1), nlohmann::json(0), nlohmann::json(1.5), nlohmann::json(1000000000), nlohmann::json("23")}) {
        auto corrupt = registry; corrupt["deen"]["model"]["size"] = value;
        check(ReadTranslationModels(corrupt.dump()).empty(), "reject invalid model size");
    }
    for (const auto* url : {"http://bergamot.s3.amazonaws.com/models/deen/model.bin", "https://evil.example/model", "https://bergamot.s3.amazonaws.com/models/../file", "https://bergamot.s3.amazonaws.com/models/deen/a?secret", "https://bergamot.s3.amazonaws.com/models/deen/%2f"}) {
        auto corrupt = registry; corrupt["deen"]["model"]["name"] = url;
        check(ReadTranslationModels(corrupt.dump()).empty(), "reject unpinned download origin or malformed URL");
    }
    for (const auto* hash : {"", "abcd", "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF", "gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg"}) {
        auto corrupt = registry; corrupt["deen"]["model"]["expectedSha256Hash"] = hash;
        check(ReadTranslationModels(corrupt.dump()).empty(), "require complete lowercase SHA-256");
    }
    auto corrupt = registry; corrupt["deen"].erase("vocab");
    check(ReadTranslationModels(corrupt.dump()).empty(), "incomplete model");
    corrupt = registry; corrupt["deen"]["config"] = {{"workspace", "9999999"}};
    check(ReadTranslationModels(corrupt.dump()).empty(), "unknown model configuration");
    check(ReadTranslationModels("not JSON").empty(), "invalid registry");
    check(ReadTranslationModels("{}").empty(), "empty registry");
    check(ReadTranslationModels(std::string(256001, ' ')).empty(), "bounded registry");
    std::cout << checks << " translation model checks passed\n";
}

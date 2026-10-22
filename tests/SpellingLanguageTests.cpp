// Dictionary-name lookup used by the native Hunspell checker.
#include "SpellingLanguageHaiku.h"
#include <cstdio>

int main()
{
    using WebKit::SpellingLanguageHaiku::select;
    const std::vector<std::string> available { "de_DE", "en_AU", "en_US", "fr_CA", "fr_FR", "sr_Cyrl_RS", "sr_Latn_RS", "zh_Hant_TW" };
    unsigned checks = 0, failed = 0;
    auto check = [&](std::string_view tag, const std::string& expected, const std::vector<std::string>& preferred = { "en_US" }) {
        ++checks;
        const auto result = select(tag, available, preferred);
        if (result != expected) {
            ++failed;
            std::fprintf(stderr, "FAIL tag=%.*s result=%s expected=%s\n", int(tag.size()), tag.data(), result.c_str(), expected.c_str());
        }
    };
    check("de", "de_DE");
    check("DE-de", "de_DE");
    check("de_AT", "de_DE");
    check("fr-CA", "fr_CA");
    check("fr", "fr_FR");
    check("fr", "fr_CA", { "fr_CA", "en_US" });
    check("en", "en_US");
    check("en", "en_AU", { "en_AU", "en_US" });
    check("en-AU", "en_AU");
    check("en-NZ", "en_US");
    check("sr-Latn", "sr_Latn_RS");
    check("sr-Cyrl", "sr_Cyrl_RS");
    check("zh-Hant-HK", "zh_Hant_TW");
    check("fr-CA-x-private", "fr_CA");
    check("de-DE-u-co-phonebk", "de_DE");
    check("it", "");
    check("", "");
    check("../../fr_FR", "");
    check("fr_FR,en_US", "");
    check("fr--FR", "");
    check("fr-", "");
    check(" fr ", "");
    check("fr/FR", "");
    check(std::string("fr\0FR", 5), "");
    check(std::string(129, 'a'), "");
    check("fr-123456789", "");
    check("x-private", "");
    check("fr", "fr_FR", { "missing", "it_IT" });
    ++checks;
    if (!select("de", { }, { "de_DE" }).empty()) ++failed;
    ++checks;
    if (select("pt-BR", { "pt_BR", "pt_PT" }, { "pt_PT" }) != "pt_BR") ++failed;
    std::printf("%u spelling language checks, %u failures\n", checks, failed);
    return failed ? 1 : 0;
}

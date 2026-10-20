#pragma once
#include <string>
#include <vector>

namespace summit {
// The Hunspell dictionaries (name.aff with name.dic, as "de_DE") in the
// directories, sorted, each once.
std::vector<std::string> SpellingDictionaries(const std::vector<std::string>& directories);
// The languages to give the engine, best first (BWebKitSetSpellChecking):
// the one chosen in Preferences when it is installed, else the dictionaries
// that fit the system's preferred languages ("de" takes de_DE or another
// de_ one, "en_AU" takes en_AU first), then en_US.
std::string SpellingLanguages(const std::string& chosen, const std::vector<std::string>& preferred,
    const std::vector<std::string>& available);
}

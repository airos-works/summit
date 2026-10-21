#pragma once
#include <array>
#include <string_view>

namespace summit {
struct UserAgentPreset {
    const char* id;
    const char* name;
    // Empty restores the engine's normal, site-aware user agent.
    const char* agent;
};
const std::array<UserAgentPreset, 9>& UserAgentPresets();
// Unknown or obsolete profile values safely return the Summit default.
const UserAgentPreset& FindUserAgentPreset(std::string_view id);
}

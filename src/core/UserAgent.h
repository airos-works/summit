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
// The agent of touch layouts (air/OS Mobile, Android): Chrome on Android, so
// sites send their phone layouts and media that plays through Media Source
// (an iPhone's agent gets HLS, which the engine lacks).
const char* MobileUserAgent();
}

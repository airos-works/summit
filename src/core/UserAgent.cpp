#include "UserAgent.h"

namespace summit {
const std::array<UserAgentPreset, 9>& UserAgentPresets()
{
    // Fixed compatibility identities, not a claim about the installed engine.
    // Chromium's reduced desktop UA freezes the OS and minor version tokens.
    // Firefox uses the fixed desktop Gecko date; Edge uses the Edg token.
    // Safari for Windows last shipped as 5.1.7, so label that choice explicitly.
    static constexpr std::array<UserAgentPreset, 9> presets {{
        {"default", "Summit (default)", ""},
        {"chrome-windows", "Chrome — Windows", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36"},
        {"chrome-macos", "Chrome — macOS", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36"},
        {"firefox-windows", "Firefox — Windows", "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:140.0) Gecko/20100101 Firefox/140.0"},
        {"firefox-macos", "Firefox — macOS", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:140.0) Gecko/20100101 Firefox/140.0"},
        {"safari-macos", "Safari — macOS", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15"},
        {"safari-windows", "Safari 5.1.7 — Windows (legacy)", "Mozilla/5.0 (Windows NT 6.1; WOW64) AppleWebKit/534.57.2 (KHTML, like Gecko) Version/5.1.7 Safari/534.57.2"},
        {"edge-windows", "Edge — Windows", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36 Edg/140.0.0.0"},
        {"edge-macos", "Edge — macOS", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36 Edg/140.0.0.0"},
    }};
    return presets;
}

const UserAgentPreset& FindUserAgentPreset(std::string_view id)
{
    for (const auto& preset : UserAgentPresets())
        if (preset.id == id) return preset;
    return UserAgentPresets().front();
}
}

#pragma once
#include <string>
#include <string_view>

namespace summit {
struct ProtocolHandler {
    // "app": absolute application path. "web": an approved HTTP(S) URL
    // template containing %s, registered by origin.
    std::string kind, target, origin;
    bool operator==(const ProtocolHandler& other) const { return kind == other.kind && target == other.target && origin == other.origin; }
};
std::string URLScheme(std::string_view);
bool IsExternalScheme(std::string_view);
bool IsWebHandlerScheme(std::string_view);
// An extension page: webkit-extension: or, for extensions with a Chrome ID
// installed since 23 Oct 2026, chrome-extension:.
bool IsExtensionPageURL(std::string_view);
std::string ProtocolOrigin(std::string_view);
bool ValidProtocolHandler(std::string_view scheme, const ProtocolHandler&);
// Replaces the first %s only; credentials in the incoming URL are removed.
std::string ProtocolHandlerURL(const ProtocolHandler&, std::string_view url);
}

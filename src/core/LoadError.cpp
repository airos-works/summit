#include "LoadError.h"

namespace summit {
namespace {
// libcurl's codes (curl.h), which the engine's network process reports in
// CurlErrorDomain.
enum Curl {
    ResolveProxy = 5, ResolveHost = 6, Connect = 7, WeirdReply = 8, HTTP2 = 16, PartialFile = 18,
    TimedOut = 28, SSLConnect = 35, GotNothing = 52, SSLEngineNotFound = 53, SSLEngineSet = 54,
    Send = 55, Receive = 56, SSLCertificateProblem = 58, SSLCipher = 59, PeerVerification = 60,
    UseSSL = 64, SSLEngineInit = 66, SSLCACertificate = 77, SSLShutdown = 80, SSLCRL = 82,
    SSLIssuer = 83, SSLPinnedKey = 90, SSLCertificateStatus = 91, HTTP2Stream = 92,
    SSLClientCertificate = 98, ECHRequired = 101,
};
}

LoadFailure ClassifyLoadFailure(const std::string& domain, int code)
{
    if (domain == "CurlErrorDomain") {
        switch (code) {
        case ResolveHost: case ResolveProxy: return LoadFailure::HostNotFound;
        case Connect: return LoadFailure::Refused;
        case GotNothing: case Send: case Receive: case HTTP2: case HTTP2Stream: case PartialFile: case WeirdReply:
            return LoadFailure::Reset;
        case TimedOut: return LoadFailure::TimedOut;
        case SSLConnect: case SSLEngineNotFound: case SSLEngineSet: case SSLCertificateProblem: case SSLCipher:
        case PeerVerification: case UseSSL: case SSLEngineInit: case SSLCACertificate: case SSLShutdown:
        case SSLCRL: case SSLIssuer: case SSLPinnedKey: case SSLCertificateStatus: case SSLClientCertificate:
        case ECHRequired:
            return LoadFailure::Secure;
        default: return LoadFailure::Other;
        }
    }
    if (IsMediaTakeover(domain, code)) return LoadFailure::None;
    // WebKit's own: 100 a type it hands to a download, 102 a policy change
    // (a download or another application took the address over).
    if (domain == "WebKitErrorDomain" && (code == 100 || code == 102 || code == 203)) return LoadFailure::None;
    // Stopped.
    if (domain == "WebKitNetworkError" && code == 302) return LoadFailure::None;
    // An extension's view asked to show a page that is not the extension's.
    if (domain == "WebExtensionNavigationError") return LoadFailure::None;
    return LoadFailure::Other;
}

bool IsMediaTakeover(const std::string& domain, int code)
{
    // The port's plug-in error domain is WebKit's own (API::Error).
    return (domain == "WebKitErrorDomain" || domain == "WebKitPluginError" || domain == "WebKitMediaError") && code == 204;
}

std::string LoadFailureHost(const std::string& url)
{
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return url;
    auto start = scheme + 3;
    const auto end = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (const auto at = authority.rfind('@'); at != std::string::npos) authority.erase(0, at + 1);
    if (!authority.empty() && authority[0] == '[') {
        const auto close = authority.find(']');
        return close == std::string::npos ? authority : authority.substr(1, close - 1);
    }
    if (const auto colon = authority.find(':'); colon != std::string::npos) authority.erase(colon);
    return authority.empty() ? url : authority;
}

LoadFailureText DescribeLoadFailure(LoadFailure failure, const std::string& url, const std::string& description)
{
    const std::string host = LoadFailureHost(url);
    switch (failure) {
    case LoadFailure::HostNotFound:
        return {"Can't find " + host, "Summit couldn't find the server at " + host + ". Check the address for "
            "typing mistakes. If it is right, the site may be down, or the network's name server isn't answering."};
    case LoadFailure::Refused:
        return {"Can't connect to " + host, host + " refused the connection. The site may be down, or it may "
            "not take connections on this port."};
    case LoadFailure::Reset:
        return {"Can't connect to " + host, "The connection to " + host + " was broken off before the page "
            "arrived. The site may be busy; try again in a moment."};
    case LoadFailure::TimedOut:
        return {"The server took too long", host + " didn't answer in time. The site may be busy or down, or "
            "the connection may be very slow."};
    case LoadFailure::Offline:
        return {"You're offline", "This computer isn't connected to a network. Summit loads the page again by "
            "itself when the connection comes back."};
    case LoadFailure::Secure:
        return {"Secure connection failed", "Summit couldn't set up a secure connection to " + host + ". The "
            "site's security settings may be out of date or wrong, or something on the network may be "
            "interfering." + (description.empty() ? std::string() : " (" + description + ")")};
    case LoadFailure::None:
    case LoadFailure::Other:
        break;
    }
    return {"Summit can't open this page", description.empty() ? std::string("The page could not be loaded.") : description};
}
}

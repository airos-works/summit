#include "SafeBrowsing.h"
#include <openssl/evp.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <system_error>

namespace summit::safebrowsing {
namespace {
constexpr char kMagic[8] = {'S', 'U', 'M', 'M', 'I', 'T', 'S', 'B'};
constexpr uint32_t kVersion = 1;
// A list larger than this is not one of Summit's: refuse to load it.
constexpr uint64_t kMaxEntries = 64u * 1024 * 1024;

int HexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// One pass of percent-decoding; true when something was decoded.
bool UnescapeOnce(std::string& text)
{
    std::string out;
    out.reserve(text.size());
    bool changed = false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = HexValue(text[i + 1]), low = HexValue(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out += char(high * 16 + low);
                i += 2;
                changed = true;
                continue;
            }
        }
        out += text[i];
    }
    text = std::move(out);
    return changed;
}

// Safe Browsing: "repeatedly percent-unescape the URL until it has no more
// percent-escapes" (bounded: "%25252525…" must not take long).
void Unescape(std::string& text)
{
    for (int pass = 0; pass < 16 && UnescapeOnce(text); ++pass) { }
}

// Escapes what Safe Browsing escapes: control characters, space, non-ASCII,
// '#' and '%', as %XX in upper case.
std::string Escape(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        if (c <= 32 || c >= 127 || c == '#' || c == '%') {
            char escaped[4];
            std::snprintf(escaped, sizeof(escaped), "%%%02X", c);
            out += escaped;
        } else out += char(c);
    }
    return out;
}

std::string Lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

// One part of an IPv4 address as inet_aton reads it: decimal, octal (a
// leading 0) or hexadecimal (0x).
std::optional<uint64_t> NumberPart(std::string_view part)
{
    if (part.empty()) return std::nullopt;
    int base = 10;
    if (part.size() > 1 && part[0] == '0' && (part[1] == 'x' || part[1] == 'X')) {
        base = 16;
        part.remove_prefix(2);
        if (part.empty()) return 0;
    } else if (part.size() > 1 && part[0] == '0') {
        base = 8;
        part.remove_prefix(1);
    }
    uint64_t value = 0;
    for (char c : part) {
        const int digit = HexValue(c);
        if (digit < 0 || digit >= base) return std::nullopt;
        value = value * base + digit;
        if (value > 0xffffffffull) return std::nullopt;
    }
    return value;
}

// "3279880203", "0xc3.0x8.0x9.0xb" and other spellings of an IPv4 address,
// as four decimal numbers; nothing for host names.
std::optional<std::string> NormalizeIPv4(std::string_view host)
{
    std::vector<uint64_t> parts;
    size_t start = 0;
    while (true) {
        const size_t dot = host.find('.', start);
        const auto part = NumberPart(host.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
        if (!part) return std::nullopt;
        parts.push_back(*part);
        if (dot == std::string_view::npos) break;
        start = dot + 1;
        if (parts.size() == 4) return std::nullopt;
    }
    uint32_t address = 0;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        if (parts[i] > 255) return std::nullopt;
        address |= uint32_t(parts[i]) << (8 * (3 - i));
    }
    const size_t remaining = 4 - (parts.size() - 1);
    if (remaining < 4 && parts.back() >= (1ull << (8 * remaining))) return std::nullopt;
    address |= uint32_t(parts.back());
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", address >> 24, (address >> 16) & 255, (address >> 8) & 255, address & 255);
    return std::string(text);
}

// The path with "." and ".." segments resolved and runs of slashes made one;
// it keeps a trailing slash.
std::string NormalizePath(const std::string& path)
{
    std::vector<std::string> segments;
    size_t start = 1;
    bool trailing = path.size() > 1 && path.back() == '/';
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        if (slash == std::string::npos) slash = path.size();
        const std::string segment = path.substr(start, slash - start);
        if (segment == "..") {
            if (!segments.empty()) segments.pop_back();
            trailing = true;
        } else if (segment == ".") trailing = true;
        else if (!segment.empty()) {
            segments.push_back(segment);
            trailing = slash < path.size();
        }
        start = slash + 1;
    }
    std::string out = "/";
    for (size_t i = 0; i < segments.size(); ++i) {
        out += segments[i];
        if (i + 1 < segments.size() || trailing) out += '/';
    }
    return out;
}

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1);
    return text;
}

bool IsHostCharacter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
}
}

const char* ThreatName(Threat threat)
{
    switch (threat) {
    case Threat::Phishing: return "phishing";
    case Threat::Malware: return "malware";
    case Threat::None: break;
    }
    return nullptr;
}

std::optional<Threat> ParseThreat(std::string_view name)
{
    if (name == "phishing") return Threat::Phishing;
    if (name == "malware") return Threat::Malware;
    return std::nullopt;
}

std::optional<CanonicalURL> Canonicalize(std::string_view input)
{
    std::string url;
    for (char c : Trim(input)) if (c != '\t' && c != '\r' && c != '\n') url += c;
    if (const auto hash = url.find('#'); hash != std::string::npos) url.erase(hash);
    std::string_view rest(url);
    if (const auto colon = rest.find(':'); colon != std::string_view::npos && colon < rest.find_first_of("/?")) {
        if (rest.substr(colon).starts_with("://")) {
            const std::string scheme = Lower(rest.substr(0, colon));
            if (scheme != "http" && scheme != "https") return std::nullopt;
            rest.remove_prefix(colon + 3);
        } else {
            // "example.com:8080/a" has a port; "mailto:x" and
            // "javascript:…" a scheme without an authority.
            const auto end = rest.find_first_of("/?");
            const auto port = rest.substr(colon + 1, end == std::string_view::npos ? end : end - colon - 1);
            if (!std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
        }
    }
    const size_t authorityEnd = std::min(rest.size(), rest.find_first_of("/?"));
    std::string authority(rest.substr(0, authorityEnd));
    std::string_view tail = rest.substr(authorityEnd);
    if (const auto at = authority.rfind('@'); at != std::string::npos) authority.erase(0, at + 1);
    CanonicalURL result;
    std::string host;
    if (!authority.empty() && authority[0] == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) return std::nullopt;
        host = Lower(authority.substr(0, close + 1));
        result.ip = true;
    } else {
        host = authority;
        if (const auto colon = host.rfind(':'); colon != std::string::npos) host.erase(colon);
        Unescape(host);
        host = Lower(host);
        // Leading and trailing dots go; runs of dots become one.
        std::string dotted;
        for (char c : host) if (c != '.' || (!dotted.empty() && dotted.back() != '.')) dotted += c;
        while (!dotted.empty() && dotted.back() == '.') dotted.pop_back();
        host = dotted;
        if (const auto ip = NormalizeIPv4(host)) {
            host = *ip;
            result.ip = true;
        }
    }
    if (host.empty()) return std::nullopt;
    result.host = Escape(host);
    std::string path, query;
    if (const auto question = tail.find('?'); question != std::string_view::npos) {
        path = std::string(tail.substr(0, question));
        query = std::string(tail.substr(question + 1));
        result.hasQuery = true;
    } else path = std::string(tail);
    Unescape(path);
    if (path.empty() || path[0] != '/') path.insert(0, 1, '/');
    result.path = Escape(NormalizePath(path));
    if (result.hasQuery) {
        Unescape(query);
        result.query = Escape(query);
    }
    return result;
}

std::vector<std::string> Expressions(const CanonicalURL& url)
{
    std::vector<std::string> hosts { url.host };
    if (!url.ip) {
        std::vector<size_t> dots;
        for (size_t i = 0; i < url.host.size(); ++i) if (url.host[i] == '.') dots.push_back(i);
        // The last 5 names down to the last 2: dots.size() + 1 names in all.
        const size_t names = dots.size() + 1;
        for (size_t keep = std::min<size_t>(5, names - 1); keep >= 2; --keep) {
            const std::string suffix = url.host.substr(dots[names - 1 - keep] + 1);
            if (suffix != url.host) hosts.push_back(suffix);
        }
    }
    std::vector<std::string> paths;
    if (url.hasQuery) paths.push_back(url.path + "?" + url.query);
    paths.push_back(url.path);
    // "/", then the first three folders.
    std::string prefix = "/";
    for (size_t start = 1, count = 0; count < 4; ++count) {
        if (std::find(paths.begin(), paths.end(), prefix) == paths.end()) paths.push_back(prefix);
        const size_t slash = url.path.find('/', start);
        if (slash == std::string::npos) break;
        prefix = url.path.substr(0, slash + 1);
        start = slash + 1;
    }
    std::vector<std::string> expressions;
    for (const auto& host : hosts)
        for (const auto& path : paths) expressions.push_back(host + path);
    return expressions;
}

uint64_t ExpressionHash(std::string_view expression)
{
    const std::string lower = Lower(expression);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(lower.data(), lower.size(), digest, &length, EVP_sha256(), nullptr) != 1 || length < 8) return 0;
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | digest[i];
    return value;
}

std::optional<std::string> LineExpression(std::string_view line)
{
    line = Trim(line);
    if (line.empty() || line[0] == '#' || line[0] == '!' || line[0] == '[' || line.starts_with("@@")) return std::nullopt;
    std::string entry;
    if (line.starts_with("||")) {
        // An Adblock rule for a host or an address prefix: only plain ones,
        // and only those that apply to pages ($all, $document).
        line.remove_prefix(2);
        if (const auto dollar = line.find('$'); dollar != std::string_view::npos) {
            const auto options = line.substr(dollar + 1);
            if (options != "all" && options != "document" && options != "doc") return std::nullopt;
            line = line.substr(0, dollar);
        }
        if (line.ends_with('^')) line.remove_suffix(1);
        if (line.empty() || line.find_first_of("*^|") != std::string_view::npos) return std::nullopt;
        entry = std::string(line);
    } else if (line.find_first_of(" \t") != std::string_view::npos) {
        // A hosts file: "0.0.0.0 evil.example # comment".
        const auto space = line.find_first_of(" \t");
        const auto address = line.substr(0, space);
        if (!std::all_of(address.begin(), address.end(), [](char c) { return HexValue(c) >= 0 || c == '.' || c == ':'; }))
            return std::nullopt;
        auto name = Trim(line.substr(space + 1));
        name = name.substr(0, name.find_first_of(" \t#"));
        if (name.empty() || name == "localhost") return std::nullopt;
        entry = std::string(name);
    } else entry = std::string(line);
    if (entry.find("://") == std::string::npos) {
        // A host alone, or a scheme-less address: its host must look like one.
        const auto end = entry.find_first_of("/?");
        const std::string_view host(entry.data(), end == std::string::npos ? entry.size() : end);
        if (host.empty() || !std::all_of(host.begin(), host.end(), [](char c) { return IsHostCharacter(c) || c == ':'; }))
            return std::nullopt;
    }
    const auto canonical = Canonicalize(entry);
    if (!canonical) return std::nullopt;
    return canonical->host + canonical->path + (canonical->hasQuery ? "?" + canonical->query : std::string());
}

std::vector<uint64_t> ParseList(std::string_view text)
{
    std::vector<uint64_t> hashes;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        if (const auto expression = LineExpression(text.substr(start, end - start))) hashes.push_back(ExpressionHash(*expression));
        start = end + 1;
    }
    return hashes;
}

HashList::HashList(std::vector<uint64_t> hashes)
    : fHashes(std::move(hashes))
{
    std::sort(fHashes.begin(), fHashes.end());
    fHashes.erase(std::unique(fHashes.begin(), fHashes.end()), fHashes.end());
}

bool HashList::Contains(uint64_t hash) const
{
    return std::binary_search(fHashes.begin(), fHashes.end(), hash);
}

bool HashList::Save(const std::filesystem::path& path, std::string& error) const
{
    const auto temporary = path.string() + ".new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "could not write " + temporary;
            return false;
        }
        unsigned char header[24] {};
        std::memcpy(header, kMagic, 8);
        const uint64_t count = fHashes.size();
        for (int i = 0; i < 4; ++i) header[8 + i] = (kVersion >> (8 * i)) & 255;
        for (int i = 0; i < 8; ++i) header[16 + i] = (count >> (8 * i)) & 255;
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        std::vector<unsigned char> bytes(fHashes.size() * 8);
        for (size_t n = 0; n < fHashes.size(); ++n)
            for (int i = 0; i < 8; ++i) bytes[n * 8 + i] = (fHashes[n] >> (8 * i)) & 255;
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        out.flush();
        if (!out) {
            error = "could not write " + temporary;
            return false;
        }
    }
    std::error_code code;
    std::filesystem::rename(temporary, path, code);
    if (code) {
        error = "could not replace " + path.string() + ": " + code.message();
        return false;
    }
    return true;
}

std::optional<HashList> HashList::Load(const std::filesystem::path& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "no list at " + path.string();
        return std::nullopt;
    }
    unsigned char header[24];
    if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) || std::memcmp(header, kMagic, 8)) {
        error = path.string() + " is not a Summit block list";
        return std::nullopt;
    }
    uint32_t version = 0;
    uint64_t count = 0;
    for (int i = 0; i < 4; ++i) version |= uint32_t(header[8 + i]) << (8 * i);
    for (int i = 0; i < 8; ++i) count |= uint64_t(header[16 + i]) << (8 * i);
    if (version != kVersion || count > kMaxEntries) {
        error = path.string() + " is of another version";
        return std::nullopt;
    }
    std::vector<unsigned char> bytes(count * 8);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) {
        error = path.string() + " is cut short";
        return std::nullopt;
    }
    std::vector<uint64_t> hashes(count);
    for (size_t n = 0; n < count; ++n)
        for (int i = 0; i < 8; ++i) hashes[n] |= uint64_t(bytes[n * 8 + i]) << (8 * i);
    // Sorted already when Summit wrote it; HashList sorts anyway.
    return HashList(std::move(hashes));
}

void Database::SetList(Threat threat, HashList list)
{
    std::unique_lock lock(fMutex);
    (threat == Threat::Malware ? fMalware : fPhishing) = std::move(list);
}

size_t Database::Count(Threat threat) const
{
    std::shared_lock lock(fMutex);
    return (threat == Threat::Malware ? fMalware : fPhishing).Size();
}

bool Database::AddTestEntry(Threat threat, std::string_view urlOrHost)
{
    const auto expression = LineExpression(urlOrHost);
    if (!expression || threat == Threat::None) return false;
    std::unique_lock lock(fMutex);
    fTests.emplace_back(ExpressionHash(*expression), threat);
    return true;
}

void Database::AllowHost(std::string_view url)
{
    const auto canonical = Canonicalize(url);
    if (!canonical) return;
    std::unique_lock lock(fMutex);
    fAllowed.insert(canonical->host);
}

bool Database::HostAllowed(std::string_view url) const
{
    const auto canonical = Canonicalize(url);
    if (!canonical) return false;
    std::shared_lock lock(fMutex);
    return fAllowed.count(canonical->host) > 0;
}

Threat Database::Check(std::string_view url) const
{
    const auto canonical = Canonicalize(url);
    if (!canonical) return Threat::None;
    std::vector<uint64_t> hashes;
    for (const auto& expression : Expressions(*canonical)) hashes.push_back(ExpressionHash(expression));
    std::shared_lock lock(fMutex);
    if (fAllowed.count(canonical->host)) return Threat::None;
    bool phishing = false;
    for (const uint64_t hash : hashes) {
        if (fMalware.Contains(hash)) return Threat::Malware;
        phishing = phishing || fPhishing.Contains(hash);
        for (const auto& [test, threat] : fTests) {
            if (test != hash) continue;
            if (threat == Threat::Malware) return Threat::Malware;
            phishing = true;
        }
    }
    return phishing ? Threat::Phishing : Threat::None;
}
}

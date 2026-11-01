#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

// Fraudulent and malicious website warnings (#150): the block lists Summit
// keeps in the profile and the question "is this address on one?". The parts
// that need no network or Haiku, so that they are tested on the host;
// src/ui/SafeBrowsingService.cpp downloads the lists and asks.
//
// Addresses are compared as Google Safe Browsing compares them: canonicalised
// (lower-case host without port, user or fragment, percent-escapes resolved,
// dot segments and repeated slashes gone), then each of the host's last few
// names combined with each of the path's first few folders ("expressions"),
// so that a listed host covers its subdomains and pages, and a listed folder
// the pages in it. The lists keep the first 8 bytes of each expression's
// SHA-256, sorted: about 2 MB for a quarter of a million entries.
namespace summit::safebrowsing {
enum class Threat : uint8_t { None = 0, Phishing = 1, Malware = 2 };
// "phishing", "malware", or null for None.
const char* ThreatName(Threat);
std::optional<Threat> ParseThreat(std::string_view name);

struct CanonicalURL {
    std::string host;  // lower case, IPv4 addresses as four decimal numbers
    std::string path;  // starts with '/'
    std::string query; // without its '?'
    bool hasQuery = false;
    bool ip = false;   // the host is an IP address (no suffixes are looked up)
};
// http:, https: or no scheme ("example.com/a"); nothing for addresses of
// other schemes or without a host.
std::optional<CanonicalURL> Canonicalize(std::string_view url);
// Safe Browsing's host suffix / path prefix combinations, most specific
// first: at most 5 hosts times 6 paths.
std::vector<std::string> Expressions(const CanonicalURL&);
// The list key of an expression: the first 8 bytes of the SHA-256 of its
// lower-case form (the URLhaus list Summit uses is all lower case).
uint64_t ExpressionHash(std::string_view expression);
// The expression a list line names: a host ("evil.example" or a hosts-file
// line), an address ("http://evil.example/login/"), or an Adblock rule
// ("||evil.example/x.exe^$all"). Nothing for comments, exceptions, rules
// with wildcards or options other than $all/$document, and anything else.
std::optional<std::string> LineExpression(std::string_view line);
// The hashes of every usable line of a list file, in its order.
std::vector<uint64_t> ParseList(std::string_view text);

// A sorted set of expression hashes.
class HashList {
public:
    HashList() = default;
    explicit HashList(std::vector<uint64_t> hashes);
    bool Contains(uint64_t hash) const;
    size_t Size() const { return fHashes.size(); }
    bool Empty() const { return fHashes.empty(); }
    // A small binary file: "SUMMITSB", version, count, then the hashes
    // (little-endian). Save writes a new file and renames it into place.
    bool Save(const std::filesystem::path&, std::string& error) const;
    static std::optional<HashList> Load(const std::filesystem::path&, std::string& error);
private:
    std::vector<uint64_t> fHashes;
};

// The lists in use, thread-safe: the engine asks from its own thread.
class Database {
public:
    void SetList(Threat, HashList);
    size_t Count(Threat) const;
    // An address (or host) treated as listed, for tests: never visit a real
    // malicious site to see the warning. Kept apart from the lists, so a
    // list update leaves it.
    bool AddTestEntry(Threat, std::string_view urlOrHost);
    // "Visit This Site Anyway": the host is not checked again until Summit quits.
    void AllowHost(std::string_view url);
    bool HostAllowed(std::string_view url) const;
    // The list that has url (http or https), malware before phishing; None
    // for addresses on no list, allowed hosts, and other schemes.
    Threat Check(std::string_view url) const;
private:
    mutable std::shared_mutex fMutex;
    HashList fPhishing, fMalware;
    std::vector<std::pair<uint64_t, Threat>> fTests;
    std::set<std::string> fAllowed;
};
}

#include "core/ExtensionIdentity.h"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>

using nlohmann::json;
static int checks, failures;
static void check(bool okay, const char* label)
{
    ++checks;
    if (!okay) ++failures;
    std::printf("%s %s\n", okay ? "PASS" : "FAIL", label);
}
static void rejects(const json& manifest, const char* label)
{
    try { summit::ExtensionIdentity(manifest, "summit-pkg-abc123"); check(false, label); }
    catch (const std::exception&) { check(true, label); }
}
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    std::ifstream input(argv[1]);
    std::string publicKey;
    std::getline(input, publicKey);
    if (!input || publicKey.empty()) return 2;
    const auto id = [](const json& manifest) { return summit::ExtensionIdentity(manifest, "summit-pkg-abc123"); };
    // Expected IDs and public key are Chromium's independent IDUtilTest vectors;
    // see fixtures/extensions/chrome-key/README.md for their source.
    check(id({{"key", publicKey}}) == "melddjfinppjdikinhbgehiennejpfhp", "Chromium public-key vector has the same extension ID");
    check(id({{"key", "dGVzdA=="}}) == "jpignaibiiemhngfjkcpokkamffknabf", "Chromium test-bytes vector matches");
    check(id({{"key", "Xw=="}}) == "ncocknphbhhlhkikpnnlmbcnbgdempcd", "Chromium underscore vector matches");
    check(id({{"key", "-----BEGIN PUBLIC KEY-----\n" + publicKey.substr(0, 64) + "\r\n" + publicKey.substr(64) + "\n-----END PUBLIC KEY-----\n"}})
        == "melddjfinppjdikinhbgehiennejpfhp", "PEM wrapping preserves the public-key identity");
    check(id(json::object()) == "summit-pkg-abc123", "unkeyed package retains its installation identity");
    const std::string signedID = "fkgkibajhfbepljeaefdnfnegdcjomkh";
    check(summit::ExtensionIdentity(json::object(), "local", signedID) == signedID, "verified CRX without manifest key retains signer identity");
    check(summit::ExtensionIdentity({{"key", publicKey}, {"browser_specific_settings", {{"gecko", {{"id", "other@example.test"}}}}}}, "local", signedID) == signedID,
        "verified CRX signer takes precedence over mixed manifest identities");
    for (const auto& invalid : { "short", "Fkgkibajhfbepljeaefdnfnegdcjomkh", "zkgkibajhfbepljeaefdnfnegdcjomkh" }) {
        try { summit::ExtensionIdentity(json::object(), "local", invalid); check(false, "malformed signed ID is rejected"); }
        catch (const std::exception&) { check(true, "malformed signed ID is rejected"); }
    }
    check(id({{"applications", {{"gecko", {{"id", "legacy@example.test"}}}}}}) == "legacy@example.test", "legacy Gecko identity is retained");
    check(id({{"browser_specific_settings", {{"gecko", {{"id", "modern@example.test"}}}}}}) == "modern@example.test", "current Gecko identity is retained");
    check(id({{"key", publicKey}, {"applications", {{"gecko", {{"id", "legacy@example.test"}}}}},
        {"browser_specific_settings", {{"gecko", {{"id", "modern@example.test"}}}}}}) == "modern@example.test", "current Gecko declaration keeps precedence in mixed manifests");
    check(id({{"key", publicKey}, {"applications", json::object()}}) == "melddjfinppjdikinhbgehiennejpfhp", "unrelated browser metadata does not hide Chrome key");
    for (const auto& key : {"", "=", "====", "A===", "AAAA=", "AAAA====", "AA=A", "AA!A", "AAA", " AAAA", "AA_A", "AA-A", "-----BEGIN PUBLIC KEY-----AAAA",
        "-----BEGIN PUBLIC KEY-----AAAA-----END PUBLIC KEY-----garbage", "-----BEGIN PRIVATE KEY-----AAAA-----END PRIVATE KEY-----"})
        rejects({{"key", key}}, "malformed or unsupported key cannot silently get a local identity");
    rejects({{"key", 42}}, "non-string key is rejected");
    rejects({{"key", std::string(1024 * 1024 + 1, 'A')}}, "oversized key is bounded");
    rejects({{"browser_specific_settings", {{"gecko", {{"id", 42}}}}}}, "non-string Gecko identity is rejected");
    rejects({{"applications", {{"gecko", {{"id", "../escape"}}}}}}, "unsafe declared identity is rejected");
    rejects(json::array(), "non-object manifest is rejected");
    // CRX3 headers with and without a proof by the Chrome Web Store's
    // publisher key (its public ECDSA key, from a store package's header).
    {
        const std::string publisherHex = "3059301306072a8648ce3d020106082a8648ce3d03010703420004b402f9a91caee10c84f8dc9b177298a5e032d5db"
            "2b59d849c0efbee86bf9f86252b86a35536c078b38d4ff9addbcddf9bc482269cc2d6ca10160b451e37bd1cf";
        std::string publisher;
        for (size_t i = 0; i + 1 < publisherHex.size(); i += 2) publisher += static_cast<char>(std::stoi(publisherHex.substr(i, 2), nullptr, 16));
        auto lengthDelimited = [](int field, const std::string& bytes) {
            std::string out(1, static_cast<char>(field << 3 | 2));
            for (size_t length = bytes.size(); ; length >>= 7) {
                out += static_cast<char>((length & 0x7f) | (length > 0x7f ? 0x80 : 0));
                if (length <= 0x7f) break;
            }
            return out + bytes;
        };
        auto crx = [&](int version, const std::string& header) {
            std::string out = "Cr24";
            for (uint32_t value : { uint32_t(version), uint32_t(header.size()) })
                for (int shift = 0; shift < 32; shift += 8) out += static_cast<char>(value >> shift & 0xff);
            return out + header + "PK\x05\x06";
        };
        auto proof = [&](const std::string& key) { return lengthDelimited(1, key) + lengthDelimited(2, std::string(64, 's')); };
        const std::string developer(294, 'k');
        check(summit::CRXHasWebStorePublisherProof(crx(3, lengthDelimited(2, proof(developer)) + lengthDelimited(3, proof(publisher)))),
            "a CRX3 with the store's publisher proof is the store's");
        check(!summit::CRXHasWebStorePublisherProof(crx(3, lengthDelimited(2, proof(developer)))), "a developer-signed CRX3 is not the store's");
        check(!summit::CRXHasWebStorePublisherProof(crx(2, lengthDelimited(3, proof(publisher)))), "a CRX2 is never the store's");
        check(!summit::CRXHasWebStorePublisherProof(crx(3, lengthDelimited(3, proof(publisher)).substr(0, 40))), "a malformed header has no publisher");
        check(!summit::CRXHasWebStorePublisherProof("PK\x03\x04"), "a ZIP is not a CRX");
    }
    std::printf("EXTENSION_IDENTITY_RESULT checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}

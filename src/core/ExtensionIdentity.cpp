#include "ExtensionIdentity.h"
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace summit {
namespace {
bool asciiSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}
std::string chromeIdentity(std::string key)
{
    if (key.empty() || key.size() > 1024 * 1024)
        throw std::runtime_error("The extension declares an empty or oversized Chrome key.");
    // The documented manifest form is one line of base64. Also accept the
    // conventional public-key PEM wrappers when importing developer packages.
    if (key.compare(0, 10, "-----BEGIN") == 0) {
        const std::array<std::string, 2> labels { "PUBLIC KEY", "RSA PUBLIC KEY" };
        bool found = false;
        for (const auto& label : labels) {
            const auto header = "-----BEGIN " + label + "-----";
            const auto footer = "-----END " + label + "-----";
            if (key.compare(0, header.size(), header) != 0) continue;
            const auto end = key.find(footer, header.size());
            if (end == std::string::npos || !std::all_of(key.begin() + end + footer.size(), key.end(), asciiSpace))
                throw std::runtime_error("The extension declares an invalid Chrome public-key envelope.");
            key = key.substr(header.size(), end - header.size());
            key.erase(std::remove_if(key.begin(), key.end(), asciiSpace), key.end());
            found = true;
            break;
        }
        if (!found) throw std::runtime_error("The extension declares an unsupported Chrome key envelope.");
    }
    if (key.empty() || key.size() % 4)
        throw std::runtime_error("The extension declares an invalid base64 Chrome key.");
    size_t padding = key.back() == '=' ? 1 : 0;
    if (padding && key[key.size() - 2] == '=') ++padding;
    const auto payload = key.size() - padding;
    if (!std::all_of(key.begin(), key.begin() + payload, [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
    })) throw std::runtime_error("The extension declares an invalid base64 Chrome key.");
    std::vector<unsigned char> bytes(key.size() / 4 * 3);
    const int decoded = EVP_DecodeBlock(bytes.data(), reinterpret_cast<const unsigned char*>(key.data()), static_cast<int>(key.size()));
    if (decoded <= static_cast<int>(padding))
        throw std::runtime_error("The extension declares an invalid base64 Chrome key.");
    bytes.resize(decoded - padding);
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash;
    unsigned hashSize = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), hash.data(), &hashSize, EVP_sha256(), nullptr) != 1 || hashSize != 32)
        throw std::runtime_error("Could not calculate the Chrome extension identity.");
    std::string identity;
    identity.reserve(32);
    // Chromium uses the first 128 bits of SHA-256, with hexadecimal digits
    // represented by a..p. This identifies key bytes; it verifies no signature.
    for (size_t i = 0; i < 16; ++i) {
        identity += static_cast<char>('a' + (hash[i] >> 4));
        identity += static_cast<char>('a' + (hash[i] & 15));
    }
    return identity;
}
// A protobuf length-delimited field reader for the CRX3 header
// (Chromium's crx3.proto): field number and bytes; false when malformed.
bool readVarint(std::string_view& bytes, uint64_t& value)
{
    value = 0;
    for (int shift = 0; shift < 64 && !bytes.empty(); shift += 7) {
        const auto byte = static_cast<unsigned char>(bytes.front());
        bytes.remove_prefix(1);
        value |= uint64_t(byte & 0x7f) << shift;
        if (!(byte & 0x80)) return true;
    }
    return false;
}
template<typename Function>
bool eachField(std::string_view bytes, const Function& function)
{
    while (!bytes.empty()) {
        uint64_t key = 0, length = 0;
        if (!readVarint(bytes, key)) return false;
        switch (key & 7) {
        case 0: if (!readVarint(bytes, length)) return false; break;
        case 1: if (bytes.size() < 8) return false; bytes.remove_prefix(8); break;
        case 5: if (bytes.size() < 4) return false; bytes.remove_prefix(4); break;
        case 2:
            if (!readVarint(bytes, length) || length > bytes.size()) return false;
            function(key >> 3, bytes.substr(0, length));
            bytes.remove_prefix(length);
            break;
        default: return false;
        }
    }
    return true;
}
bool validIdentity(std::string_view value)
{
    return !value.empty() && value.size() <= 255 && value != "." && value != ".."
        && std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || c == '.' || c == '_' || c == '-' || c == '@' || c == '{' || c == '}';
        });
}
}
std::string ExtensionIdentity(const nlohmann::json& manifest, std::string_view localIdentity, std::string_view verifiedCRXIdentity)
{
    if (!manifest.is_object()) throw std::runtime_error("Invalid extension manifest.");
    if (!verifiedCRXIdentity.empty()) {
        if (verifiedCRXIdentity.size() != 32 || !std::all_of(verifiedCRXIdentity.begin(), verifiedCRXIdentity.end(), [](char c) { return c >= 'a' && c <= 'p'; }))
            throw std::runtime_error("The engine returned an invalid signed Chrome identity.");
        return std::string(verifiedCRXIdentity);
    }
    const nlohmann::json* geckoIdentity = nullptr;
    for (const char* key : { "applications", "browser_specific_settings" }) {
        auto settings = manifest.find(key);
        if (settings == manifest.end() || !settings->is_object()) continue;
        auto gecko = settings->find("gecko");
        if (gecko != settings->end() && gecko->is_object() && gecko->contains("id"))
            geckoIdentity = &gecko->at("id");
    }
    std::string identity(localIdentity);
    if (geckoIdentity) {
        if (!geckoIdentity->is_string()) throw std::runtime_error("The extension declares an invalid Gecko identity.");
        identity = geckoIdentity->get<std::string>();
    } else if (manifest.contains("key")) {
        if (!manifest["key"].is_string()) throw std::runtime_error("The extension declares an invalid Chrome key.");
        identity = chromeIdentity(manifest["key"].get<std::string>());
    }
    if (!validIdentity(identity)) throw std::runtime_error("The extension declares an invalid identity.");
    return identity;
}

bool CRXHasWebStorePublisherProof(std::string_view crx)
{
    // The SHA-256 of the store's publisher key (Chromium's kPublisherKeyHash,
    // components/crx_file/crx_verifier.cc).
    static constexpr std::array<unsigned char, 32> publisherKeyHash {
        0x61, 0xf7, 0xf2, 0xa6, 0xbf, 0xcf, 0x74, 0xcd, 0x0b, 0xc1, 0xfe,
        0x24, 0x97, 0xcc, 0x9b, 0x04, 0x25, 0x4c, 0x65, 0x8f, 0x79, 0xf2,
        0x14, 0x53, 0x92, 0x86, 0x7e, 0xa8, 0x36, 0x63, 0x67, 0xcf };
    if (crx.size() < 12 || crx.substr(0, 4) != "Cr24") return false;
    auto little = [&](size_t offset) {
        return uint32_t(static_cast<unsigned char>(crx[offset])) | uint32_t(static_cast<unsigned char>(crx[offset + 1])) << 8
            | uint32_t(static_cast<unsigned char>(crx[offset + 2])) << 16 | uint32_t(static_cast<unsigned char>(crx[offset + 3])) << 24;
    };
    if (little(4) != 3) return false;
    const size_t headerSize = little(8);
    if (headerSize > crx.size() - 12) return false;
    bool found = false;
    // Proofs: field 2 (RSA) and 3 (ECDSA), each {1: public key, 2: signature}.
    const bool parsed = eachField(crx.substr(12, headerSize), [&](uint64_t number, std::string_view proof) {
        if (number != 2 && number != 3) return;
        eachField(proof, [&](uint64_t field, std::string_view key) {
            if (field != 1) return;
            std::array<unsigned char, EVP_MAX_MD_SIZE> hash;
            unsigned size = 0;
            if (EVP_Digest(key.data(), key.size(), hash.data(), &size, EVP_sha256(), nullptr) == 1 && size == publisherKeyHash.size()
                && std::equal(publisherKeyHash.begin(), publisherKeyHash.end(), hash.begin()))
                found = true;
        });
    });
    return parsed && found;
}
}

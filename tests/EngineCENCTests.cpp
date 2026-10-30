// Portable CENC validation, known-answer cryptography, and Shaka interoperability.
#include "FragmentedMP4ParserHaiku.h"
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
using namespace SummitMP4;
using Bytes = std::vector<uint8_t>;
static Bytes hex(const char* text)
{
    Bytes bytes;
    for (; *text; text += 2) bytes.push_back(uint8_t(std::stoul(std::string(text, 2), nullptr, 16)));
    return bytes;
}
static Bytes read(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary); assert(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
static size_t box(const Bytes& bytes, const char* name)
{
    auto at = std::search(bytes.begin(), bytes.end(), name, name + 4);
    assert(at != bytes.end()); return at - bytes.begin();
}
static uint32_t u32(const Bytes& bytes, size_t at)
{
    assert(at + 4 <= bytes.size()); return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at+1]) << 16 | uint32_t(bytes[at+2]) << 8 | bytes[at+3];
}
static void put32(Bytes& bytes, size_t at, uint32_t value)
{
    assert(at + 4 <= bytes.size()); for (unsigned i = 0; i < 4; ++i) bytes[at+i] = uint8_t(value >> (24-8*i));
}
static void renameBox(Bytes& bytes, const char* from, const char* to)
{
    auto at = box(bytes, from); std::copy(to, to + 4, bytes.begin() + at);
}
static std::vector<Sample> parse(const Bytes& init, const Bytes& media, size_t chunk, bool encrypted)
{
    Parser parser; std::vector<Sample> samples; unsigned initializations = 0;
    parser.onInitSegment = [&](InitSegment&& segment) {
        ++initializations; assert(segment.tracks.size() == 1 && segment.tracks[0].encrypted == encrypted);
        if (encrypted) assert(segment.protectionSystemHeaders.size() >= 52);
    };
    parser.onSample = [&](Sample&& sample) { assert(bool(sample.encryption) == encrypted); samples.push_back(std::move(sample)); };
    for (const auto* bytes : {&init, &media}) {
        for (size_t at = 0; at < bytes->size(); at += chunk)
            assert(parser.append(bytes->data() + at, std::min(chunk, bytes->size() - at)));
    }
    assert(initializations == 1 && !samples.empty());
    return samples;
}
static void reject(const Bytes& init, const Bytes& media = {})
{
    Parser parser;
    bool ok = parser.append(init.data(), init.size());
    if (ok && !media.empty()) ok = parser.append(media.data(), media.size());
    assert(!ok);
    const uint8_t empty[] = {0}; assert(!parser.append(empty, 1));
    parser.reset();
}
int main(int argc, char** argv)
{
    assert(argc == 2);
    // NIST SP 800-38A F.5.1 AES-128 CTR, including carry across the counter.
    auto secret = hex("2b7e151628aed2a6abf7158809cf4f3c");
    std::array<uint8_t, 16> key; std::copy(secret.begin(), secret.end(), key.begin());
    CencSample info; info.iv = hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    auto cipher = hex("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee");
    auto plain = hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    Bytes output; assert(decryptCenc(cipher, info, key, output) && output == plain);
    Bytes subsamples {1,2,3}; subsamples.insert(subsamples.end(), cipher.begin(), cipher.begin() + 7);
    subsamples.insert(subsamples.end(), {4,5,6,7,8}); subsamples.insert(subsamples.end(), cipher.begin() + 7, cipher.end());
    Bytes expected {1,2,3}; expected.insert(expected.end(), plain.begin(), plain.begin() + 7);
    expected.insert(expected.end(), {4,5,6,7,8}); expected.insert(expected.end(), plain.begin() + 7, plain.end());
    info.subsamples = {{3,7},{5,57}}; assert(decryptCenc(subsamples, info, key, output) && output == expected);
    auto unchanged = output;
    info.subsamples[1].encryptedBytes++; assert(!decryptCenc(subsamples, info, key, output) && output == unchanged);
    info.subsamples = {{UINT32_MAX,UINT32_MAX}}; assert(!decryptCenc(subsamples, info, key, output));
    info.subsamples.clear(); info.iv.resize(7); assert(!decryptCenc(cipher, info, key, output));
    info.iv.resize(17); assert(!decryptCenc(cipher, info, key, output));
    info.iv.resize(16); info.subsamples.resize(65536); assert(!decryptCenc(cipher, info, key, output));

    std::filesystem::path root(argv[1]); unsigned total = 0;
    for (std::string track : {"video", "audio"}) {
        auto init = read(root / (track + "-init.mp4"));
        auto clearInit = read(root / ("clear-" + track + "-init.mp4"));
        for (unsigned i = 0; i < 16; ++i) key[i] = track == "video" ? i : 15 - i;
        for (unsigned number = 1; number <= 9; ++number) {
            auto media = read(root / (track + "-" + std::to_string(number) + ".m4s"));
            auto clearMedia = read(root / ("clear-" + track + "-" + std::to_string(number) + ".m4s"));
            auto clearSamples = parse(clearInit, clearMedia, 4093, false);
            auto check = [&](const Bytes& input, size_t chunk) {
                auto samples = parse(init, input, chunk, true);
                assert(samples.size() == clearSamples.size());
                for (size_t i = 0; i < samples.size(); ++i) {
                    assert(decryptCenc(samples[i].data, *samples[i].encryption, key, output));
                    assert(output == clearSamples[i].data);
                    assert(samples[i].decodeTime == clearSamples[i].decodeTime && samples[i].presentationTime == clearSamples[i].presentationTime);
                    ++total;
                }
            };
            check(media, 4093);
            if (number != 1) continue;
            check(media, 1); check(media, 13); check(media, media.size());
            auto auxiliaryOnly = media; renameBox(auxiliaryOnly, "senc", "free"); check(auxiliaryOnly, 113);
            auto inlineOnly = media; renameBox(inlineOnly, "saiz", "free"); renameBox(inlineOnly, "saio", "free"); check(inlineOnly, 131);
            auto typedAuxiliary = media;
            // Add optional aux_info_type/parameter fields without changing sample bytes.
            for (const char* type : {"saio", "saiz"}) {
                auto at = box(typedAuxiliary, type);
                put32(typedAuxiliary, at - 4, u32(typedAuxiliary, at - 4) + 8);
                typedAuxiliary[at + 7] = 1;
                typedAuxiliary.insert(typedAuxiliary.begin() + at + 8, {'c','e','n','c',0,0,0,0});
            }
            for (const char* type : {"moof", "traf"}) { auto at = box(typedAuxiliary, type) - 4; put32(typedAuxiliary, at, u32(typedAuxiliary, at) + 16); }
            for (auto at : {box(typedAuxiliary, "saio") + 20, box(typedAuxiliary, "trun") + 12}) put32(typedAuxiliary, at, u32(typedAuxiliary, at) + 16);
            check(typedAuxiliary, 113);
            auto bad = init; auto tenc = box(bad, "tenc"); bad[tenc + 11] = 7; reject(bad);
            bad = init; auto schm = box(bad, "schm"); std::copy_n("cbc2", 4, bad.begin() + schm + 8); reject(bad); // unknown scheme (cbcs is EngineCENCSchemeTests.cpp's)
            bad = init; bad[tenc + 9] = 0x19; reject(bad); // pattern encryption
            bad = init; renameBox(bad, "sinf", "free"); reject(bad);
            bad = media; auto senc = box(bad, "senc"); bad[senc + 7] |= 1; reject(init, bad); // unsupported override
            bad = media; bad[senc + 11]++; reject(init, bad); // wrong sample count
            bad = media; auto saiz = box(bad, "saiz"); bad[saiz + 8]++; reject(init, bad); // auxiliary sizes disagree
            bad = media; auto saio = box(bad, "saio"); bad[saio + 15]++; reject(init, bad); // auxiliary offset disagrees
            bad = media; renameBox(bad, "senc", "free"); renameBox(bad, "saiz", "free"); renameBox(bad, "saio", "free"); reject(init, bad);
            if (track == "video") { bad = media; bad[senc + 22] = 0xff; bad[senc + 23] = 0xff; reject(init, bad); }
            bad = init; auto pssh = box(bad, "pssh"); bad[pssh + 7] = 1; reject(bad);
            // Corrupt metadata, including box lengths/counts, under ASan/UBSan.
            std::mt19937 random(0xCE4C);
            for (unsigned iteration = 0; iteration < 2000; ++iteration) {
                auto mutatedInit = init, mutatedMedia = media;
                auto& mutated = iteration % 2 ? mutatedInit : mutatedMedia;
                size_t bound = iteration % 2 ? mutated.size() : box(mutated, "mdat") - 4;
                mutated[random() % bound] ^= uint8_t(1 + random() % 255);
                Parser parser; if (parser.append(mutatedInit.data(), mutatedInit.size())) parser.append(mutatedMedia.data(), mutatedMedia.size());
            }
        }
    }
    auto init16 = read(root / "iv16-video-init.mp4"), media16 = read(root / "iv16-video-1.m4s");
    auto reference = parse(read(root / "clear-video-init.mp4"), read(root / "clear-video-1.m4s"), 257, false);
    auto iv16 = parse(init16, media16, 17, true);
    assert(iv16.size() == reference.size());
    for (unsigned i = 0; i < 16; ++i) key[i] = i;
    for (size_t i = 0; i < iv16.size(); ++i) {
        assert(iv16[i].encryption->iv.size() == 16);
        assert(decryptCenc(iv16[i].data, *iv16[i].encryption, key, output) && output == reference[i].data);
        ++total;
    }
    // An empty mdhd/tkhd must fail before dereferencing its absent version.
    reject(hex("000000186d6f6f76000000107472616b00000008746b6864"));
    reject(hex("000000206d6f6f76000000187472616b000000106d646961000000086d646864"));
    std::cout << "CENC_PASS compared=" << total << " mutated=4000" << std::endl;
}

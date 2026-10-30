// Portable checks for the 'cbc1', 'cens' and 'cbcs' protection schemes, clear
// leads, 'seig' sample groups and WebM encryption, against independently
// packaged media: Chromium's media/test/data vectors (Shaka Packager) and
// fixtures from tools/bench/eme-fixture.py --schemes.
// test-cenc-schemes CHROMIUM_VECTORS_DIR EME_FIXTURE_DIR
#include "FragmentedMP4ParserHaiku.h"
#include "WebMParserHaiku.h"
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
using namespace SummitMP4;
using Bytes = std::vector<uint8_t>;
using Key = std::array<uint8_t, 16>;
static Bytes hex(const char* text)
{
    Bytes bytes;
    for (; *text; text += 2) bytes.push_back(uint8_t(std::stoul(std::string(text, 2), nullptr, 16)));
    return bytes;
}
static Key key(const char* text)
{
    auto bytes = hex(text); Key result; std::copy(bytes.begin(), bytes.end(), result.begin()); return result;
}
static Bytes read(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in.good()) { std::cerr << "missing " << file << std::endl; assert(false); }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
struct Parsed {
    InitSegment init;
    std::map<uint32_t, std::vector<Sample>> samples;
    size_t encrypted { 0 };
};
template<typename P> static Parsed parseWith(const std::vector<Bytes>& pieces, size_t chunk)
{
    P parser; Parsed result; unsigned initializations = 0;
    parser.onInitSegment = [&](InitSegment&& segment) { ++initializations; result.init = std::move(segment); };
    parser.onSample = [&](Sample&& sample) { result.encrypted += bool(sample.encryption); result.samples[sample.trackID].push_back(std::move(sample)); };
    for (size_t piece = 0; piece < pieces.size(); ++piece) {
        const auto& bytes = pieces[piece];
        for (size_t at = 0; at < bytes.size(); at += chunk) {
            if (!parser.append(bytes.data() + at, std::min(chunk, bytes.size() - at))) {
                std::cerr << "piece " << piece << " rejected near byte " << at << std::endl; assert(false);
            }
        }
        if constexpr (std::is_same_v<P, SummitWebM::Parser>) parser.flushPendingSamples();
    }
    assert(initializations >= 1);
    return result;
}
static Parsed parseMP4(const std::vector<Bytes>& pieces, size_t chunk = 4093) { return parseWith<Parser>(pieces, chunk); }
static Parsed parseWebM(const std::vector<Bytes>& pieces, size_t chunk = 4093) { return parseWith<SummitWebM::Parser>(pieces, chunk); }
static bool rejectsMP4(const Bytes& bytes)
{
    Parser parser; return !parser.append(bytes.data(), bytes.size());
}
static size_t box(const Bytes& bytes, const char* name, size_t from = 0)
{
    auto at = std::search(bytes.begin() + from, bytes.end(), name, name + 4);
    assert(at != bytes.end()); return at - bytes.begin();
}
// Decrypts every sample and compares it with the clear stream's. Returns
// the number of encrypted samples.
static unsigned compare(const Parsed& encrypted, const Parsed& clear, const Key& secret, const char* name,
    std::optional<CencScheme> scheme = std::nullopt)
{
    assert(encrypted.samples.size() == clear.samples.size());
    unsigned decrypted = 0;
    for (auto& [track, samples] : encrypted.samples) {
        auto& reference = clear.samples.at(track);
        if (samples.size() != reference.size()) { std::cerr << name << ": " << samples.size() << " vs " << reference.size() << std::endl; assert(false); }
        for (size_t i = 0; i < samples.size(); ++i) {
            Bytes output = samples[i].data;
            if (samples[i].encryption) {
                if (scheme) assert(samples[i].encryption->scheme == *scheme);
                assert(decryptCenc(samples[i].data, *samples[i].encryption, secret, output));
                ++decrypted;
            }
            if (output != reference[i].data) { std::cerr << name << ": track " << track << " sample " << i << " differs" << std::endl; assert(false); }
            // Encrypted WebM audio keeps the container's millisecond times:
            // its packets can't be parsed for their exact durations.
            int64_t tolerance = 0;
            for (auto& info : encrypted.init.tracks) {
                if (info.id == track && info.packetTimedAudio && samples[i].encryption) tolerance = info.timescale / 1000 + 1;
            }
            if (std::abs(samples[i].presentationTime - reference[i].presentationTime) > tolerance) { std::cerr << name << ": sample " << i << " time " << samples[i].presentationTime << " vs " << reference[i].presentationTime << std::endl; assert(false); }
        }
    }
    return decrypted;
}
static void mutate(const Bytes& bytes, uint32_t seed, bool webm)
{
    std::mt19937 random(seed);
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
        auto mutated = bytes;
        size_t bound = std::min<size_t>(mutated.size(), 4096);
        for (unsigned flips = 1 + random() % 3; flips; --flips)
            mutated[random() % bound] ^= uint8_t(1 + random() % 255);
        if (webm) { SummitWebM::Parser parser; parser.append(mutated.data(), mutated.size()); parser.flushPendingSamples(); }
        else { Parser parser; parser.append(mutated.data(), mutated.size()); }
    }
}
int main(int argc, char** argv)
{
    assert(argc == 3);
    std::filesystem::path vectors(argv[1]), fixtures(argv[2]);
    unsigned total = 0;

    // Known answers: AES-128-CBC (NIST SP 800-38A F.2.1), as 'cbc1' and as
    // 'cbcs' with every block encrypted (0:0), and the pattern itself.
    auto nist = key("2b7e151628aed2a6abf7158809cf4f3c");
    auto plain = hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    auto cbc = hex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b273bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7");
    Bytes output;
    for (auto scheme : {CencScheme::CBC1, CencScheme::CBCS}) {
        CencSample sample; sample.scheme = scheme; sample.iv = hex("000102030405060708090a0b0c0d0e0f");
        assert(decryptCenc(cbc, sample, nist, output) && output == plain);
        // A trailing partial block stays clear.
        auto tail = cbc; tail.insert(tail.end(), {1, 2, 3}); auto expected = plain; expected.insert(expected.end(), {1, 2, 3});
        assert(decryptCenc(tail, sample, nist, output) && output == expected);
        // 'cbcs' restarts the chain at every subsample; 'cbc1' continues it.
        Bytes split {9}; split.insert(split.end(), cbc.begin(), cbc.begin() + 32); split.push_back(8); split.insert(split.end(), cbc.begin() + 32, cbc.end());
        sample.subsamples = {{1, 32}, {1, 32}};
        assert(decryptCenc(split, sample, nist, output));
        assert(std::equal(plain.begin(), plain.begin() + 32, output.begin() + 1));
        assert(std::equal(plain.begin() + 32, plain.end(), output.begin() + 34) == (scheme == CencScheme::CBC1));
    }
    {
        // 1:1 pattern: blocks 0 and 2 are one CBC chain, 1 and 3 are clear.
        CencSample sample; sample.scheme = CencScheme::CBCS; sample.cryptByteBlock = 1; sample.skipByteBlock = 1;
        sample.iv = hex("000102030405060708090a0b0c0d0e0f");
        Bytes mixed(64); std::copy(cbc.begin(), cbc.begin() + 16, mixed.begin()); std::copy(plain.begin() + 16, plain.begin() + 32, mixed.begin() + 16);
        std::copy(cbc.begin() + 16, cbc.begin() + 32, mixed.begin() + 32); std::copy(plain.begin() + 48, plain.end(), mixed.begin() + 48);
        Bytes expected(plain.begin(), plain.begin() + 16); expected.insert(expected.end(), plain.begin() + 16, plain.begin() + 32);
        expected.insert(expected.end(), plain.begin() + 16, plain.begin() + 32); expected.insert(expected.end(), plain.begin() + 48, plain.end());
        assert(decryptCenc(mixed, sample, nist, output) && output == expected);
        sample.cryptByteBlock = 0; assert(!decryptCenc(mixed, sample, nist, output)); // 0:1 encrypts nothing
        sample.cryptByteBlock = 16; assert(!decryptCenc(mixed, sample, nist, output));
    }

    // Chromium's scheme vectors: one H.264 stream packaged four ways, with a
    // 0.5 s clear lead (a second, clear sample description).
    auto chromium = key("ebdd62f16814d27b68ef122afce4ae3c");
    auto clearVideo = parseMP4({read(vectors / "bear-640x360-v_frag.mp4")});
    for (auto [file, scheme] : {std::pair {"bear-640x360-v_frag-cenc.mp4", CencScheme::CENC}, {"bear-640x360-v_frag-cbc1.mp4", CencScheme::CBC1},
             {"bear-640x360-v_frag-cens.mp4", CencScheme::CENS}, {"bear-640x360-v_frag-cbcs.mp4", CencScheme::CBCS}}) {
        auto bytes = read(vectors / file);
        auto parsed = parseMP4({bytes}, 997);
        assert(parsed.init.tracks.size() == 1 && parsed.init.tracks[0].encrypted && parsed.init.tracks[0].scheme == scheme);
        assert(parsed.init.tracks[0].clearDescriptions == 2 && parsed.init.tracks[0].codec == "avc1");
        unsigned count = compare(parsed, clearVideo, chromium, file, scheme);
        assert(count > 0 && count < parsed.samples.begin()->second.size());
        total += count;
        mutate(bytes, count, false);
    }
    auto audio = read(vectors / "bear-640x360-a_frag-cbcs.mp4");
    auto audioParsed = parseMP4({audio}, 61);
    assert(audioParsed.init.tracks[0].scheme == CencScheme::CBCS && !audioParsed.init.tracks[0].defaultIVSize && audioParsed.init.tracks[0].constantIV.size() == 16);
    total += compare(audioParsed, parseMP4({read(vectors / "bear-640x360-a_frag.mp4")}), chromium, "a_frag-cbcs", CencScheme::CBCS);
    // A constant IV only belongs to 'cbcs'.
    {
        auto bad = audio; auto schm = box(bad, "schm"); std::copy_n("cenc", 4, bad.begin() + schm + 8); assert(rejectsMP4(bad));
        bad = audio; auto tenc = box(bad, "tenc"); bad[tenc + 28] = 7; assert(rejectsMP4(bad)); // constant IV size
        bad = audio; bad[tenc + 9] = 0x01; assert(rejectsMP4(bad)); // 0:1 pattern
    }

    // Chromium's key rotation vectors: each fragment names its key in a
    // 'seig' sample group (the track's tenc key ID is zero), and the key IDs
    // and keys rotate left by one byte per crypto period.
    for (auto [file, clearName] : {std::pair {"bear-640x360-v_frag-cenc-key_rotation.mp4", "bear-640x360-v_frag.mp4"},
             {"bear-640x360-a_frag-cenc-key_rotation.mp4", "bear-640x360-a_frag.mp4"}}) {
        auto bytes = read(vectors / file);
        auto parsed = parseMP4({bytes}, 333);
        auto clear = parseMP4({read(vectors / clearName)});
        auto baseID = key("30313233343536373839303132333435");
        std::set<Key> keyIDs;
        for (auto& [trackID, samples] : parsed.samples) {
            auto& reference = clear.samples.at(trackID);
            assert(samples.size() == reference.size());
            for (size_t i = 0; i < samples.size(); ++i) {
                assert(samples[i].encryption);
                auto& id = samples[i].encryption->keyID;
                keyIDs.insert(id);
                std::optional<Key> secret;
                for (unsigned n = 0; n < 16 && !secret; ++n) {
                    auto rotatedID = baseID; std::rotate(rotatedID.begin(), rotatedID.begin() + n, rotatedID.end());
                    if (rotatedID != id) continue;
                    secret = chromium; std::rotate(secret->begin(), secret->begin() + n, secret->end());
                }
                assert(secret);
                Bytes result;
                assert(decryptCenc(samples[i].data, *samples[i].encryption, *secret, result) && result == reference[i].data);
                ++total;
            }
        }
        // The files are shorter than a crypto period: one key, named only
        // by the 'seig' groups.
        assert(keyIDs.size() == 1 && *keyIDs.begin() != Key {} && parsed.init.tracks[0].defaultKeyID == Key {});
        mutate(bytes, keyIDs.size(), false);
    }

    // eme-fixture.py --schemes: H.264/AAC segments packaged by Shaka Packager
    // as 'cbcs' with a 2 s clear lead, and as 'cens'.
    for (std::string prefix : {"cbcs-", "cens-"}) {
        for (std::string track : {"video", "audio"}) {
            Key secret; for (unsigned i = 0; i < 16; ++i) secret[i] = track == "video" ? i : 15 - i;
            std::vector<Bytes> pieces {read(fixtures / (prefix + track + "-init.mp4"))}, clearPieces {read(fixtures / ("clear-" + track + "-init.mp4"))};
            for (unsigned number = 1; number <= 9; ++number) {
                pieces.push_back(read(fixtures / (prefix + track + "-" + std::to_string(number) + ".m4s")));
                clearPieces.push_back(read(fixtures / ("clear-" + track + "-" + std::to_string(number) + ".m4s")));
            }
            auto parsed = parseMP4(pieces, 509);
            total += compare(parsed, parseMP4(clearPieces), secret, (prefix + track).c_str());
            if (prefix == "cbcs-") mutate(pieces[1], track.size(), false);
        }
    }

    // WebM encryption (signal byte, 8-byte IV, partitions): Chromium's
    // whole-frame VP8/Vorbis file, its clear-lead and all-clear variants, VP9
    // subsamples and Opus, AV1; then Shaka-packaged VP9/Opus.
    for (auto [encryptedName, clearName] : {std::pair {"bear-320x240-av_enc-av.webm", "bear-320x240.webm"},
             {"bear-320x240-av_enc-av_clear-1s.webm", "bear-320x240.webm"}, {"bear-320x240-av_enc-av_clear-all.webm", "bear-320x240.webm"},
             {"bear-640x360-av_enc-av.webm", "bear-640x360.webm"}, {"bear-320x240-opus-av_enc-av.webm", "bear-vp9-opus.webm"}}) {
        if (std::string(clearName).find("vp9") == std::string::npos && !SummitWebM::Parser::supportsVorbis()) continue; // VP8 + Vorbis
        auto bytes = read(vectors / encryptedName);
        auto parsed = parseWebM({bytes}, 1021);
        auto clear = parseWebM({read(vectors / clearName)});
        for (auto& track : parsed.init.tracks) {
            if (std::string(encryptedName).find("clear-all") == std::string::npos)
                assert(track.encrypted && track.defaultKeyID == key("30313233343536373839303132333435"));
        }
        unsigned count = compare(parsed, clear, chromium, encryptedName, CencScheme::CENC);
        assert((count == 0) == (std::string(encryptedName).find("clear-all") != std::string::npos));
        total += count;
        mutate(bytes, count, true);
    }
    // Partitioned (subsample) VP9 and AV1. Their clear sources in Chromium's
    // tree were re-encoded since, so check the decrypted bitstreams instead:
    // VP9 frame markers and keyframe sync codes; AV1 OBU headers and sizes
    // that add up to each temporal unit. (Written to IVF, all 82 frames of
    // each decode without errors in FFmpeg.)
    for (auto name : {"bear-320x240-v-vp9_subsample_enc-v.webm", "bear-av1-cenc.webm"}) {
        auto bytes = read(vectors / name);
        auto parsed = parseWebM({bytes}, 1021);
        auto& samples = parsed.samples.begin()->second;
        assert(samples.size() == 82);
        size_t partitioned = 0;
        bool av1 = parsed.init.tracks[0].codec == "av01";
        for (auto& sample : samples) {
            assert(sample.encryption);
            partitioned += sample.encryption->subsamples.size() > 1;
            assert(decryptCenc(sample.data, *sample.encryption, chromium, output));
            if (!av1) {
                assert((output[0] & 0xc0) == 0x80);
                if (sample.isSync) assert(output.size() > 4 && output[1] == 0x49 && output[2] == 0x83 && output[3] == 0x42);
            } else {
                for (size_t at = 0; at < output.size();) {
                    uint8_t header = output[at++];
                    assert(!(header & 0x80) && (header & 2)); // forbidden bit, has_size_field
                    if (header & 4) ++at; // extension
                    uint64_t size = 0;
                    for (unsigned shift = 0;; shift += 7) { assert(at < output.size() && shift < 56); size |= uint64_t(output[at] & 0x7f) << shift; if (!(output[at++] & 0x80)) break; }
                    assert(size <= output.size() - at); at += size;
                }
            }
            ++total;
        }
        assert(partitioned > 0);
        mutate(bytes, partitioned, true);
    }
    {
        Key secret; for (unsigned i = 0; i < 16; ++i) secret[i] = i;
        auto parsed = parseWebM({read(fixtures / "webm-video.webm")}, 777);
        total += compare(parsed, parseWebM({read(fixtures / "clear-webm-video.webm")}), secret, "shaka webm video", CencScheme::CENC);
        for (unsigned i = 0; i < 16; ++i) secret[i] = 15 - i;
        parsed = parseWebM({read(fixtures / "webm-audio.webm")}, 777);
        total += compare(parsed, parseWebM({read(fixtures / "clear-webm-audio.webm")}), secret, "shaka webm audio", CencScheme::CENC);
    }
    std::cout << "CENC_SCHEMES_PASS decrypted=" << total << std::endl;
}

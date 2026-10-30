// HLS through the DASH player (DashPlaybackHaiku + HLSPlaylistHaiku), and
// DASH SegmentBase, with the fMP4 fixtures of tools/bench/native-streaming-fixture.py.
// MPEG-TS and WebM need the engine's FFmpeg and libwebm demuxers and are
// checked in the browser instead.
#include "DashPlaybackHaiku.h"
#include "HLSPlaylistHaiku.h"
#include <cassert>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
using namespace SummitDASH;

static std::filesystem::path root;

static std::string readFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    assert(stream.good());
    return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
}

// The fixture server's live window (tools/bench/native-streaming-fixture.py), at `now`.
static std::string livePlaylist(const std::string& text, double now, unsigned window)
{
    if (text.find("#EXT-X-STREAM-INF") != std::string::npos) return text;
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> segments;
    std::vector<std::string> pending;
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);) {
        if (line.empty() || line.starts_with("#EXT-X-MEDIA-SEQUENCE") || line.starts_with("#EXT-X-PLAYLIST-TYPE") || line.starts_with("#EXT-X-ENDLIST")) continue;
        if (segments.empty() && pending.empty() && (line.starts_with("#EXTM3U") || line.starts_with("#EXT-X-VERSION") || line.starts_with("#EXT-X-TARGETDURATION") || line.starts_with("#EXT-X-MAP") || line.starts_with("#EXT-X-KEY")))
            header.push_back(line);
        else if (line[0] != '#') { pending.push_back(line); segments.push_back(std::move(pending)); pending.clear(); }
        else pending.push_back(line);
    }
    std::vector<double> durations;
    for (auto& segment : segments) for (auto& line : segment) if (line.starts_with("#EXTINF:")) durations.push_back(std::stod(line.substr(8)));
    // A short tail segment would give the audio rendition one segment more
    // per loop than the video, and the loops' discontinuities would differ.
    if (durations.size() > 1 && durations.back() < 0.5) { durations.pop_back(); segments.pop_back(); }
    size_t count = segments.size(), produced = window;
    double elapsed = now;
    while (elapsed >= durations[produced % count]) { elapsed -= durations[produced % count]; ++produced; }
    size_t first = produced - window;
    std::string out;
    for (auto& line : header) out += line + "\n";
    out += "#EXT-X-MEDIA-SEQUENCE:" + std::to_string(first) + "\n#EXT-X-DISCONTINUITY-SEQUENCE:" + std::to_string(first / count) + "\n";
    for (size_t g = first; g < produced; ++g) {
        if (g % count == 0 && g > first) out += "#EXT-X-DISCONTINUITY\n";
        for (auto& line : segments[g % count]) out += line + "\n";
    }
    return out;
}

struct Run {
    std::array<unsigned, 2> samples {};
    std::array<double, 2> minimum { 1e9, 1e9 }, maximum { -1e9, -1e9 };
    std::array<double, 2> lastDecode { -1e9, -1e9 };
    std::set<unsigned> widths;
    unsigned resets = 0, changes = 0, finished = 0, playlistRequests = 0, keyRequests = 0, backwards = 0;
    bool failed = false;
    std::string error;
    double now = 0, media = 0;
    // The renderers' queues (90 video and 120 audio samples, as in
    // MediaPlayerPrivateHaikuMSE): they drain only while the clock runs.
    std::array<std::deque<double>, 2> queued;
};

int main(int argc, char** argv)
{
    assert(argc == 2);
    root = argv[1];
    struct Pending { uint64_t id; Resource resource; size_t limit; };
    std::deque<Pending> requests;
    std::set<uint64_t> cancelled;
    Run run;
    Player player({
        [&](uint64_t id, const Resource& resource, size_t limit) { requests.push_back({id, resource, limit}); },
        [&](uint64_t id) { cancelled.insert(id); },
        [](const Representation& rep) { return rep.container == Container::MP4 && !rep.encrypted; },
        [&](unsigned track) { return run.queued[track].size() < (track ? 120u : 90u); },
        [&](unsigned track, const SummitMP4::TrackInfo& configuration, SummitMP4::Sample&& sample, double offset) {
            assert(!sample.data.empty() && configuration.timescale && std::isfinite(offset));
            ++run.samples[track];
            double time = double(sample.presentationTime) / configuration.timescale + offset;
            double decode = double(sample.decodeTime) / configuration.timescale + offset;
            run.minimum[track] = std::min(run.minimum[track], time);
            run.maximum[track] = std::max(run.maximum[track], time);
            if (decode + 0.05 < run.lastDecode[track]) ++run.backwards;
            run.lastDecode[track] = decode;
            run.queued[track].push_back(time + double(sample.duration) / configuration.timescale);
            if (!track) run.widths.insert(configuration.width);
        },
        [&](unsigned) { ++run.finished; },
        [&](double position) { ++run.resets; run.lastDecode = { -1e9, -1e9 }; run.queued = { }; run.media = position; },
        [&] { ++run.changes; },
        [&](const std::string& message, bool) { run.failed = true; run.error = message; }
    });
    auto deliver = [&] {
        if (requests.empty()) return false;
        auto request = std::move(requests.front()); requests.pop_front();
        if (cancelled.contains(request.id)) return true;
        auto path = request.resource.url.substr(std::string("https://example.test/").size());
        bool live = path.starts_with("live/");
        if (live) path = path.substr(5);
        auto data = readFile(root / path);
        if (path.ends_with(".m3u8")) {
            ++run.playlistRequests;
            if (live) data = livePlaylist(data, run.now, 4);
        }
        if (path.ends_with(".bin")) ++run.keyRequests;
        if (request.resource.range) data = data.substr(request.resource.range->first, request.resource.range->last - request.resource.range->first + 1);
        assert(data.size() <= request.limit);
        player.complete(request.id, { data.begin(), data.end() }, request.resource.url, data.size() * 8 / 5000000.0);
        return true;
    };
    auto play = [&](const std::string& url, double seconds, double start = 0) {
        run = Run {}; requests.clear(); cancelled.clear();
        run.now = start;
        player.start("https://example.test/" + url, 1767225600, start);
        for (unsigned i = 0; i < 40 && !player.hasMetadata() && deliver(); ++i) { }
        run.media = player.initialTime();
        for (double t = 0; t < seconds && !player.hasEnded() && !run.failed; t += 0.05) {
            run.now = start + t;
            // The clock runs as the media player's does: with data at and
            // just after the playback position (HaveFutureData).
            if (player.hasCurrentData() && player.bufferedEnd() - run.media >= 0.1) {
                run.media += 0.05;
                for (auto& queue : run.queued) while (!queue.empty() && queue.front() <= run.media + 0.5) queue.pop_front();
            }
            player.tick(run.media, run.now);
            deliver(); deliver();
        }
        return run.media;
    };
    auto report = [&](const char* name) {
        std::cerr << name << ": samples=" << run.samples[0] << '/' << run.samples[1] << " video=" << run.minimum[0] << ".." << run.maximum[0]
            << " audio=" << run.minimum[1] << ".." << run.maximum[1] << " widths=" << run.widths.size() << " playlists=" << run.playlistRequests
            << " keys=" << run.keyRequests << " backwards=" << run.backwards << (run.failed ? " ERROR " + run.error : std::string()) << '\n';
    };

    // Multivariant fMP4, separate audio rendition, adaptive switches.
    play("vod/master.m3u8", 60); report("vod");
    assert(!run.failed && player.hasEnded() && player.hasVideo() && player.hasAudio() && std::abs(player.duration() - 24) < 0.05);
    assert(run.samples[0] == 720 && run.samples[1] > 1100 && run.finished == 2 && !run.backwards);
    assert(run.minimum[0] >= -0.01 && run.maximum[0] < 24 && run.maximum[1] < 24.1);
    // Seek, then a stale completion.
    player.seek(10);
    for (unsigned i = 0; i < 100 && player.bufferedEnd() < 12; ++i) { player.tick(10, 100 + i * .05); deliver(); }
    assert(!run.failed && player.bufferedEnd() >= 12 && run.resets == 2);

    // Byte ranges in one file per rendition.
    play("single/master.m3u8", 60); report("single");
    assert(!run.failed && player.hasEnded() && run.samples[0] == 720 && run.samples[1] > 1100 && !run.backwards);

    // A media playlist on its own: tracks are found in the first segment.
    player.start("https://example.test/mux/a.m3u8", 1767225600, 0); run = Run {};
    assert(deliver() && !player.hasMetadata());
    for (unsigned i = 0; i < 10 && !player.hasMetadata(); ++i) deliver();
    assert(player.hasMetadata() && player.hasVideo() && player.hasAudio());
    play("mux/a.m3u8", 60); report("muxed");
    assert(!run.failed && player.hasEnded() && run.samples[0] == 720 && run.samples[1] > 1100 && run.finished == 2);

    // A discontinuity: new initialization, timestamps from 500 s.
    play("mux/disc.m3u8", 80); report("discontinuity");
    assert(!run.failed && player.hasEnded() && run.samples[0] == 720 + 300 && run.widths.size() == 2 && !run.backwards);
    assert(std::abs(player.duration() - 36) < 0.5 && run.maximum[0] > 35 && run.maximum[0] < 36.5 && run.minimum[0] > -0.1);

    // AES-128, IV from the media sequence number.
    play("mux/enc.m3u8", 60); report("aes-128");
    assert(!run.failed && player.hasEnded() && run.samples[0] == 720 && run.keyRequests == 1);

    // Live: a 4-segment window that loops with a discontinuity every 24 s.
    double media = play("live/vod/master.m3u8", 70, 30); report("live");
    std::cerr << "live: initial=" << player.initialTime() << " media=" << media << " seekable=" << player.seekableStart() << ".." << player.seekableEnd() << '\n';
    assert(!run.failed && player.isLive() && std::isinf(player.duration()) && !player.hasEnded());
    assert(run.playlistRequests > 3 * 10 && player.seekableStart() > 60 && player.seekableEnd() - player.seekableStart() <= 16.01);
    assert(media > player.seekableEnd() - 14 && run.maximum[0] > media && !run.backwards);
    assert(std::abs(run.minimum[0] - player.initialTime()) < 4.1 && std::abs(run.minimum[1] - run.minimum[0]) < 0.2);

    // DASH SegmentBase: segments from the files' sidx boxes.
    play("dashsb/manifest.mpd", 60); report("dash-segmentbase");
    assert(!run.failed && player.hasEnded() && run.samples[0] == 720 && run.samples[1] > 1100 && !run.backwards);
    player.seek(13);
    for (unsigned i = 0; i < 100 && player.bufferedEnd() < 16; ++i) { player.tick(13, 100 + i * .05); deliver(); }
    assert(!run.failed && player.bufferedEnd() >= 16 && run.minimum[0] >= 0);

    // Refused forms.
    for (auto [text, expected] : { std::pair { "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"k\"\n#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n", "SAMPLE-AES" },
             std::pair { "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4,\na.aac\n#EXT-X-ENDLIST\n", "packed audio" },
             std::pair { "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"skd://x\",KEYFORMAT=\"com.apple.streamingkeydelivery\"\n#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n", "DRM" } }) {
        run = Run {}; requests.clear();
        player.start("https://example.test/x.m3u8", 0, 0);
        auto id = requests.front().id; requests.pop_front();
        std::string body(text);
        player.complete(id, { body.begin(), body.end() }, "https://example.test/x.m3u8", .01);
        assert(run.failed && run.error.find(expected) != std::string::npos);
    }
    // Attribute lists, decimal AVC codecs, relative and absolute URIs.
    SummitHLS::Presentation presentation;
    std::string error;
    assert(presentation.start("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=500000,CODECS=\"avc1.66.30,mp4a.40.5\",RESOLUTION=320x240\nlow/p.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=900000,CODECS=\"avc1.77.31,mp4a.40.2\"\nhttps://cdn.example.test/hi/p.m3u8?token=a,b\n", "https://example.test/m/master.m3u8", error));
    auto load = presentation.playlistsToLoad();
    assert(load.size() == 2 && load[0].second == "https://example.test/m/low/p.m3u8" && load[1].second == "https://cdn.example.test/hi/p.m3u8?token=a,b");
    for (auto& [index, url] : load)
        assert(presentation.update(index, "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MAP:URI=\"i.mp4\"\n#EXTINF:2,\ns.m4s\n#EXT-X-BYTERANGE:100@0\n#EXTINF:2,\nr.m4s\n#EXT-X-BYTERANGE:50\n#EXTINF:2,\nr.m4s\n#EXT-X-ENDLIST\n", url, error));
    auto manifest = presentation.manifest(error);
    assert(manifest && manifest->periods.size() == 1 && manifest->duration == 6);
    auto& low = manifest->periods[0].representations[0];
    assert(low.codecs == "avc1.42001e,mp4a.40.2" && low.muxedAudio && low.width == 320 && low.segments.size() == 3);
    assert(low.segments[2].resource.range && low.segments[2].resource.range->first == 100 && low.segments[2].resource.range->last == 149);
    std::array<uint8_t, 16> iv = SummitHLS::sequenceIV(258);
    assert(iv[15] == 2 && iv[14] == 1 && !iv[0]);
    std::cout << "HLS_PLAYBACK_PASS\n";
}

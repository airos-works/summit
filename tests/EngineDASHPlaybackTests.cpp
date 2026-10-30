#include "DashPlaybackHaiku.h"
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
using namespace SummitDASH;
int main(int argc, char** argv)
{
    assert(argc == 2);
    struct Pending { uint64_t id; Resource resource; size_t limit; };
    std::deque<Pending> requests;
    std::array<unsigned, 2> samples {};
    std::vector<unsigned> widths;
    std::set<uint64_t> cancelled;
    unsigned resets = 0, changes = 0, finished = 0;
    bool failed = false, ready = true;
    bool live = false, multiplePeriods = false, sawSecondPeriod = false; unsigned manifestRequests = 0;
    // Periods that change track sets: 0-18 s video and audio, 18-36 s video
    // only (an ad), 36-54 s both again; or "audio then video" (one kind per
    // period).
    enum class TrackSets { Same, AdWithoutAudio, AudioThenVideo } trackSets = TrackSets::Same;
    std::array<std::set<double>, 2> offsets;
    double time = 0, bandwidth = 5000000;
    Player player({
        [&](uint64_t id, const Resource& resource, size_t limit) { requests.push_back({id, resource, limit}); },
        [&](uint64_t id) { cancelled.insert(id); },
        [](const Representation& rep) { return !rep.encrypted && (rep.codecs.starts_with("avc1.") || rep.codecs == "mp4a.40.2"); },
        [&](unsigned) { return ready; },
        [&](unsigned track, const SummitMP4::TrackInfo& configuration, SummitMP4::Sample&& sample, double offset) {
            assert(!sample.data.empty() && configuration.timescale && std::isfinite(offset));
            ++samples[track];
            sawSecondPeriod |= offset == 18;
            offsets[track].insert(offset);
            if (!track && (widths.empty() || widths.back() != configuration.width)) widths.push_back(configuration.width);
        },
        [&](unsigned) { ++finished; },
        [&](double position) { ++resets; time = position; },
        [&] { ++changes; },
        [&](const std::string& message, bool) { failed = true; std::cerr << "DASH_ERROR " << message << '\n'; }
    });
    auto deliver = [&] {
        if (requests.empty()) return false;
        auto request = std::move(requests.front()); requests.pop_front();
        if (cancelled.contains(request.id)) return true;
        auto leaf = request.resource.url.substr(request.resource.url.rfind('/') + 1);
        std::ifstream stream(std::filesystem::path(argv[1]) / leaf, std::ios::binary);
        assert(stream.good()); std::vector<uint8_t> data {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        if (leaf == "manifest.mpd") {
            ++manifestRequests;
            if (multiplePeriods) {
                std::string text(data.begin(), data.end());
                auto begin = text.find("<Period "), end = text.find("</Period>") + 9;
                auto first = text.substr(begin, end - begin), second = first;
                first.replace(first.find("start=\"PT0.0S\""), 14, "start=\"PT0S\" duration=\"PT18S\"");
                second.replace(second.find("start=\"PT0.0S\""), 14, "start=\"PT18S\" duration=\"PT18S\"");
                second.replace(second.find("id=\"0\""), 6, "id=\"1\"");
                text.replace(begin, end - begin, first + second);
                text.replace(text.find("mediaPresentationDuration=\"PT18.0S\""), std::string("mediaPresentationDuration=\"PT18.0S\"").size(), "mediaPresentationDuration=\"PT36S\"");
                data.assign(text.begin(), text.end());
            }
            if (trackSets != TrackSets::Same) {
                std::string text(data.begin(), data.end());
                auto begin = text.find("<Period "), end = text.find("</Period>") + 9;
                auto period = text.substr(begin, end - begin);
                auto audioBegin = period.find("<AdaptationSet id=\"1\""), audioEnd = period.find("</AdaptationSet>", audioBegin) + 16;
                auto videoBegin = period.find("<AdaptationSet id=\"0\""), videoEnd = period.find("</AdaptationSet>", videoBegin) + 16;
                auto make = [&](unsigned index, bool video, bool audio) {
                    auto copy = period;
                    if (!audio) copy.erase(audioBegin, audioEnd - audioBegin);
                    if (!video) copy.erase(videoBegin, videoEnd - videoBegin);
                    auto start = "start=\"PT" + std::to_string(index * 18) + "S\" duration=\"PT18S\"";
                    copy.replace(copy.find("start=\"PT0.0S\""), 14, start);
                    copy.replace(copy.find("id=\"0\""), 6, "id=\"" + std::to_string(index) + "\"");
                    return copy;
                };
                auto periods = trackSets == TrackSets::AdWithoutAudio ? make(0, true, true) + make(1, true, false) + make(2, true, true)
                    : make(0, false, true) + make(1, true, false);
                text.replace(begin, end - begin, periods);
                auto duration = trackSets == TrackSets::AdWithoutAudio ? "PT54S" : "PT36S";
                text.replace(text.find("mediaPresentationDuration=\"PT18.0S\""), std::string("mediaPresentationDuration=\"PT18.0S\"").size(),
                    std::string("mediaPresentationDuration=\"") + duration + "\"");
                data.assign(text.begin(), text.end());
            }
            if (live) {
                std::string text(data.begin(), data.end());
                auto original = std::string("mediaPresentationDuration=\"PT18.0S\"");
                text.replace(text.find(original), original.size(), "availabilityStartTime=\"2026-01-01T00:00:00Z\" timeShiftBufferDepth=\"PT8S\" minimumUpdatePeriod=\"PT1S\"");
                text.replace(text.find("type=\"static\""), 13, "type=\"dynamic\"");
                data.assign(text.begin(), text.end());
            }
        }
        assert(data.size() <= request.limit);
        double elapsed = data.size() * 8 / bandwidth;
        player.complete(request.id, std::move(data), request.resource.url, elapsed);
        return true;
    };
    player.start("https://example.test/dash/manifest.mpd", 1767225600, 0);
    // Deliberate renderer backpressure bounds fetching to initialization and
    // one media segment per track until the decoder accepts queued samples.
    ready = false;
    for (unsigned i = 0; i < 100 && deliver(); ++i) { }
    assert(player.hasMetadata() && player.hasAudio() && player.hasVideo() && !failed);
    assert(samples[0] == 0 && samples[1] == 0 && requests.empty());
    ready = true;
    for (unsigned tick = 0; tick < 500 && !player.hasEnded(); ++tick) {
        time = tick * 0.05;
        if (time > 5) bandwidth = 300000;
        player.tick(time, time);
        deliver();
        assert(!failed);
    }
    assert(player.hasEnded() && finished == 2 && samples[0] == 540 && samples[1] > 800);
    assert(widths.size() >= 3 && widths.front() == 320 && widths[1] == 640 && widths.back() == 320);
    assert(player.duration() == 18 && changes > 10 && resets == 1);
    const auto beforeSeek = samples;
    player.seek(8.5);
    assert(resets == 2 && !player.hasEnded() && player.bufferedEnd() == 8.5);
    for (unsigned tick = 0; tick < 100 && player.bufferedEnd() < 10; ++tick) { player.tick(8.5, 30 + tick * .05); deliver(); }
    assert(!failed && samples[0] > beforeSeek[0] && samples[1] > beforeSeek[1] && player.bufferedEnd() >= 10);
    player.seek(2); assert(!requests.empty());
    auto stale = requests.front(); player.seek(12); assert(cancelled.contains(stale.id));
    player.complete(stale.id, {1, 2, 3}, stale.resource.url, .1); assert(!failed);
    player.seek(18); assert(!player.hasCurrentData());
    for (unsigned tick = 0; tick < 100 && !player.hasEnded(); ++tick) { player.tick(18, 50 + tick * .05); deliver(); }
    assert(!failed && player.hasEnded() && player.hasCurrentData());
    player.stop(); assert(!player.hasAudio() && !player.hasVideo());
    requests.clear();
    live = true; bandwidth = 5000000; manifestRequests = 0;
    player.start("https://example.test/dash/manifest.mpd", 1767225608, 0);
    assert(deliver() && player.isLive() && player.initialTime() >= 1.8 && player.initialTime() <= 2);
    for (unsigned tick = 0; tick < 100; ++tick) {
        player.tick(2 + tick * .1, tick * .1); deliver(); assert(!failed);
    }
    assert(manifestRequests >= 5 && !player.hasEnded() && std::isinf(player.duration()));
    std::cerr << "LIVE start=" << player.seekableStart() << " end=" << player.seekableEnd() << " buffered=" << player.bufferedEnd() << " requests=" << manifestRequests << "\n";
    assert(player.seekableStart() >= 8 && player.seekableEnd() >= 15.9 && player.bufferedEnd() > 11.9);
    auto minimum = player.seekableStart(); player.seek(0); assert(time >= minimum);
    player.stop(); requests.clear(); live = false; multiplePeriods = true;
    auto beforePeriods = samples; bandwidth = 5000000;
    player.start("https://example.test/dash/manifest.mpd", 1767225600, 0);
    for (unsigned tick = 0; tick < 900 && !player.hasEnded(); ++tick) { player.tick(tick * .05, tick * .05); deliver(); assert(!failed); }
    assert(player.hasEnded() && player.duration() == 36 && sawSecondPeriod);
    assert(samples[0] - beforePeriods[0] == 1080 && samples[1] - beforePeriods[1] > 1600);
    player.stop(); requests.clear(); multiplePeriods = false;
    // An ad period without audio: audio moves on to the third period (its
    // samples are offset by 36 s, none by 18 s) while video plays all three,
    // and neither stalls the other: buffered data stays current throughout.
    trackSets = TrackSets::AdWithoutAudio; offsets = { };
    auto beforeAd = samples; bandwidth = 5000000;
    player.start("https://example.test/dash/manifest.mpd", 1767225600, 0);
    for (unsigned tick = 0; tick < 1300 && !player.hasEnded(); ++tick) {
        player.tick(tick * .05, tick * .05); deliver(); assert(!failed);
        if (tick > 40) assert(player.hasCurrentData());
    }
    assert(player.hasEnded() && player.duration() == 54 && player.hasAudio() && player.hasVideo());
    assert(samples[0] - beforeAd[0] == 1620 && samples[1] - beforeAd[1] > 1600 && samples[1] - beforeAd[1] < 2 * 900);
    assert((offsets[0] == std::set<double> {0, 18, 36}) && (offsets[1] == std::set<double> {0, 36}));
    // Seeking into the ad: audio's next data is the third period's.
    player.seek(20); offsets = { };
    for (unsigned tick = 0; tick < 40; ++tick) { player.tick(20 + tick * .05, 100 + tick * .05); deliver(); assert(!failed); }
    assert(player.hasCurrentData() && offsets[1] == std::set<double> {36} && offsets[0].contains(18));
    // One kind per period: audio alone, then video alone. Both kinds are
    // enabled; each track ends after its own period.
    player.stop(); requests.clear(); trackSets = TrackSets::AudioThenVideo; offsets = { };
    auto beforeSplit = samples; unsigned finishedBefore = finished;
    player.start("https://example.test/dash/manifest.mpd", 1767225600, 0);
    for (unsigned tick = 0; tick < 900 && !player.hasEnded(); ++tick) {
        player.tick(tick * .05, tick * .05); deliver(); assert(!failed);
        if (tick > 40) assert(player.hasCurrentData() && player.bufferedEnd() > tick * .05);
    }
    assert(player.hasEnded() && player.hasAudio() && player.hasVideo() && finished == finishedBefore + 2);
    assert(samples[0] - beforeSplit[0] == 540 && samples[1] - beforeSplit[1] > 800 && samples[1] - beforeSplit[1] < 900);
    assert((offsets[0] == std::set<double> {18}) && (offsets[1] == std::set<double> {0}));
    player.stop(); requests.clear(); trackSets = TrackSets::Same;
    player.start("https://example.test/dash/manifest.mpd", 1767225600, 100);
    auto failedID = requests.front().id; player.failed(failedID); assert(failed);
    std::cout << "DASH_PLAYBACK_PASS samples=" << samples[0] << '/' << samples[1] << " switches=" << widths.size() << " resets=" << resets << '\n';
}

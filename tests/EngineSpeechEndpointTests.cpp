// Copyright (C) 2026 Summit contributors. SPDX-License-Identifier: BSD-2-Clause
//
// The plain arithmetic of speech recognition on Haiku (Summit #159): the
// engine's own SpeechEndpointerHaiku.h (down-mix to one channel, the
// end-of-speech detector, the pre-roll buffer, the WAV reader of the testing
// aid), compiled as it is and fed made-up audio. No recogniser, no microphone
// and no Haiku are needed: this runs on the build host (ctest, or
// tools/test-engine-speech-endpoint.py).
#include "SpeechEndpointerHaiku.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <utility>

using namespace WebCore;

static int checks = 0, failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; std::cerr << __FILE__ << ":" << __LINE__ << ": " #condition "\n"; } } while (0)
#define CHECK_NEAR(value, expected, tolerance) do { ++checks; double v_ = (value), e_ = (expected); if (std::fabs(v_ - e_) > (tolerance)) { ++failures; std::cerr << __FILE__ << ":" << __LINE__ << ": " #value " = " << v_ << ", expected " << e_ << " +- " << (tolerance) << "\n"; } } while (0)

// Audio made of stretches of a tone ("speech") and of noise ("quiet"), the
// same every run.
class Signal {
public:
    explicit Signal(double rate)
        : m_rate(rate)
    {
    }

    Signal& noise(double seconds, float level)
    {
        size_t count = static_cast<size_t>(seconds * m_rate);
        for (size_t i = 0; i < count; ++i) {
            m_seed = m_seed * 1664525u + 1013904223u;
            // Uniform between -1 and 1 has a root mean square of 0.577.
            float uniform = static_cast<float>(m_seed >> 8) / 8388608.f - 1.f;
            m_samples.push_back(uniform * level / 0.577f);
        }
        return *this;
    }

    Signal& silence(double seconds) { return noise(seconds, 0); }

    // A 220 Hz tone with the given root mean square level.
    Signal& speech(double seconds, float level)
    {
        size_t count = static_cast<size_t>(seconds * m_rate);
        for (size_t i = 0; i < count; ++i)
            m_samples.push_back(static_cast<float>(std::sin(2 * M_PI * 220 * (m_phase++) / m_rate)) * level * 1.41421f);
        return *this;
    }

    double rate() const { return m_rate; }
    const std::vector<float>& samples() const { return m_samples; }

private:
    double m_rate;
    uint32_t m_seed { 12345 };
    uint64_t m_phase { 0 };
    std::vector<float> m_samples;
};

struct Events {
    // Seconds from the beginning of the signal.
    std::vector<double> starts;
    std::vector<double> ends;
    bool offsetsInRange { true };
};

static Events run(SpeechEndpointer& endpointer, const Signal& signal, size_t chunk)
{
    Events events;
    auto& samples = signal.samples();
    for (size_t position = 0; position < samples.size(); position += chunk) {
        size_t count = std::min(chunk, samples.size() - position);
        endpointer.process(samples.data() + position, count, signal.rate(), [&](SpeechEndpointer::Event event, size_t offset) {
            if (!offset || offset > count)
                events.offsetsInRange = false;
            double time = (position + offset) / signal.rate();
            if (event == SpeechEndpointer::Event::SpeechStart)
                events.starts.push_back(time);
            else
                events.ends.push_back(time);
        });
    }
    return events;
}

static Events run(const Signal& signal, size_t chunk = 480, const SpeechEndpointerSettings& settings = { })
{
    SpeechEndpointer endpointer(settings);
    return run(endpointer, signal, chunk);
}

static void testDownmix()
{
    std::vector<float> mono;
    const float one[] = { 0.25f, -0.5f, 1.f };
    speechDownmixToMono(one, 3, 1, mono);
    CHECK(mono.size() == 3 && mono[0] == 0.25f && mono[1] == -0.5f && mono[2] == 1.f);

    const float two[] = { 1.f, 0.f, -1.f, -0.5f, 0.25f, 0.25f };
    speechDownmixToMono(two, 3, 2, mono);
    CHECK(mono.size() == 3);
    CHECK_NEAR(mono[0], 0.5, 1e-6);
    CHECK_NEAR(mono[1], -0.75, 1e-6);
    CHECK_NEAR(mono[2], 0.25, 1e-6);

    const float three[] = { 0.3f, 0.6f, 0.9f, -0.3f, -0.3f, -0.3f };
    speechDownmixToMono(three, 2, 3, mono);
    CHECK(mono.size() == 2);
    CHECK_NEAR(mono[0], 0.6, 1e-6);
    CHECK_NEAR(mono[1], -0.3, 1e-6);

    // A source that says it has no channels gives quiet, not a crash.
    speechDownmixToMono(one, 3, 0, mono);
    CHECK(mono.size() == 3 && mono[0] == 0 && mono[2] == 0);

    speechDownmixToMono(one, 0, 2, mono);
    CHECK(mono.empty());
}

static void testQuiet()
{
    // Nothing at all, then the hiss of a microphone: no speech.
    for (float level : { 0.f, 0.0005f, 0.002f }) {
        Signal signal(16000);
        signal.noise(10, level);
        SpeechEndpointer endpointer;
        auto events = run(endpointer, signal, 160);
        CHECK(events.starts.empty() && events.ends.empty());
        CHECK(!endpointer.isInSpeech() && !endpointer.everSpoke());
        CHECK_NEAR(endpointer.secondsProcessed(), 10, 0.001);
        CHECK_NEAR(endpointer.secondsSinceSpeech(), 10, 0.001);
        CHECK(endpointer.secondsInSpeech() == 0);
    }
}

static void testOneUtterance()
{
    for (double rate : { 16000., 44100., 48000. }) {
        Signal signal(rate);
        signal.noise(1, 0.001f).speech(1.5, 0.07f).noise(2, 0.001f);
        SpeechEndpointer endpointer;
        auto events = run(endpointer, signal, static_cast<size_t>(rate / 100));
        CHECK(events.offsetsInRange);
        CHECK(events.starts.size() == 1 && events.ends.size() == 1);
        if (events.starts.size() == 1 && events.ends.size() == 1) {
            // Noticed after the 0.12 s that tell speech from a click, over
            // 0.9 s after it stopped.
            CHECK_NEAR(events.starts[0], 1.12, 0.03);
            CHECK_NEAR(events.ends[0], 3.4, 0.03);
        }
        CHECK(!endpointer.isInSpeech() && endpointer.everSpoke());
        // Since the last sound that was speech, not since the end was called.
        CHECK_NEAR(endpointer.secondsSinceSpeech(), 2, 0.03);
    }
}

static void testClicksAreNotSpeech()
{
    Signal signal(16000);
    // A key press, a door: loud, and over before speech would be.
    signal.noise(1, 0.001f).speech(0.04, 0.3f).noise(1, 0.001f).speech(0.08, 0.3f).noise(2, 0.001f);
    auto events = run(signal);
    CHECK(events.starts.empty() && events.ends.empty());
}

static void testPauses()
{
    Signal signal(16000);
    // A breath between words goes on; a full stop ends the utterance; the
    // next sentence is another one.
    signal.noise(0.5, 0.001f).speech(1, 0.05f).noise(0.4, 0.001f).speech(1, 0.05f).noise(1.5, 0.001f).speech(0.8, 0.05f).noise(1.5, 0.001f);
    SpeechEndpointer endpointer;
    auto events = run(endpointer, signal, 320);
    CHECK(events.starts.size() == 2 && events.ends.size() == 2);
    if (events.starts.size() == 2 && events.ends.size() == 2) {
        CHECK_NEAR(events.starts[0], 0.62, 0.03);
        CHECK_NEAR(events.ends[0], 2.9 + 0.9, 0.03);
        CHECK_NEAR(events.starts[1], 4.4 + 0.12, 0.03);
        CHECK_NEAR(events.ends[1], 5.2 + 0.9, 0.03);
    }
    CHECK(endpointer.everSpoke());
}

static void testSpeechFromTheFirstSample()
{
    // Nobody waited for quiet: what is loud from the start is not taken for
    // the background.
    Signal signal(16000);
    signal.speech(2, 0.06f).noise(2, 0.001f);
    auto events = run(signal);
    CHECK(events.starts.size() == 1 && events.ends.size() == 1);
    if (!events.starts.empty())
        CHECK_NEAR(events.starts[0], 0.12, 0.03);
    if (!events.ends.empty())
        CHECK_NEAR(events.ends[0], 2.9, 0.03);
}

static void testNoisyRoom()
{
    // With a fan running, what is only a little louder than the fan is not
    // speech; what is clearly louder is, and the fan alone ends it.
    Signal quietVoice(16000);
    quietVoice.noise(3, 0.006f).speech(1.5, 0.012f).noise(2, 0.006f);
    auto events = run(quietVoice);
    CHECK(events.starts.empty());

    Signal voice(16000);
    voice.noise(3, 0.006f).speech(1.5, 0.05f).noise(2, 0.006f);
    SpeechEndpointer endpointer;
    events = run(endpointer, voice, 480);
    CHECK(events.starts.size() == 1 && events.ends.size() == 1);
    if (!events.starts.empty())
        CHECK_NEAR(events.starts[0], 3.12, 0.03);
    if (!events.ends.empty())
        CHECK_NEAR(events.ends[0], 5.4, 0.03);
    // The background it learned is the fan's.
    CHECK_NEAR(endpointer.noiseLevel(), 0.006, 0.0015);
    CHECK(endpointer.onsetThreshold() > 0.012f && endpointer.onsetThreshold() < 0.05f);

    // The background becoming louder is followed (within a few seconds),
    // not taken for speech for ever.
    Signal louder(16000);
    louder.noise(2, 0.001f).noise(6, 0.004f);
    SpeechEndpointer following;
    events = run(following, louder, 480);
    CHECK(events.starts.size() == events.ends.size());
    CHECK(!following.isInSpeech());
    CHECK_NEAR(following.noiseLevel(), 0.004, 0.001);
}

static void testChunkingDoesNotMatter()
{
    Signal signal(48000);
    signal.noise(0.7, 0.002f).speech(1.1, 0.04f).noise(1.3, 0.002f).speech(0.5, 0.08f).noise(1.2, 0.002f);
    auto reference = run(signal, 480);
    CHECK(reference.starts.size() == 2 && reference.ends.size() == 2);
    // One sample at a time, a microphone's buffers, odd sizes, all at once.
    for (size_t chunk : { size_t(1), size_t(441), size_t(1024), size_t(4096), size_t(77777), signal.samples().size() }) {
        auto events = run(signal, chunk);
        CHECK(events.offsetsInRange);
        CHECK(events.starts == reference.starts);
        CHECK(events.ends == reference.ends);
    }
}

static void testSettings()
{
    Signal signal(16000);
    signal.noise(1, 0.001f).speech(1, 0.05f).noise(3, 0.001f);

    SpeechEndpointerSettings quick;
    quick.trailingSilence = 0.3;
    auto events = run(signal, 160, quick);
    CHECK(events.ends.size() == 1);
    if (!events.ends.empty())
        CHECK_NEAR(events.ends[0], 2.3, 0.03);

    SpeechEndpointerSettings patient;
    patient.trailingSilence = 2.5;
    events = run(signal, 160, patient);
    CHECK(events.ends.size() == 1);
    if (!events.ends.empty())
        CHECK_NEAR(events.ends[0], 4.5, 0.03);

    // A floor above the voice: nothing is speech.
    SpeechEndpointerSettings deaf;
    deaf.floorLevel = 0.2f;
    events = run(signal, 160, deaf);
    CHECK(events.starts.empty());
}

static void testResetAndRateChange()
{
    Signal first(16000);
    first.noise(0.5, 0.001f).speech(1, 0.05f);
    SpeechEndpointer endpointer;
    auto events = run(endpointer, first, 160);
    CHECK(events.starts.size() == 1 && events.ends.empty());
    CHECK(endpointer.isInSpeech());
    CHECK_NEAR(endpointer.secondsInSpeech(), 0.88, 0.03);

    endpointer.reset();
    CHECK(!endpointer.isInSpeech() && !endpointer.everSpoke());
    CHECK(endpointer.secondsProcessed() == 0 && endpointer.noiseLevel() == 0);

    // The same detector goes on at another rate (another microphone).
    Signal second(48000);
    second.noise(0.5, 0.001f).speech(1, 0.05f).noise(1.5, 0.001f);
    events = run(endpointer, second, 480);
    CHECK(events.starts.size() == 1 && events.ends.size() == 1);
    Signal third(8000);
    third.noise(0.5, 0.001f).speech(1, 0.05f).noise(1.5, 0.001f);
    events = run(endpointer, third, 80);
    CHECK(events.starts.size() == 1 && events.ends.size() == 1);

    // A rate that makes no sense is ignored.
    float sample = 1;
    endpointer.process(&sample, 1, 0, [&](SpeechEndpointer::Event, size_t) { CHECK(false); });
}

static void testPreRoll()
{
    SpeechPreRoll preRoll;
    preRoll.setCapacity(4);
    const float a[] = { 1, 2, 3 };
    const float b[] = { 4, 5, 6 };
    preRoll.append(a, 3);
    CHECK(preRoll.samples() == std::vector<float>({ 1, 2, 3 }));
    preRoll.append(b, 3);
    CHECK(preRoll.samples() == std::vector<float>({ 3, 4, 5, 6 }));
    const float c[] = { 7, 8, 9, 10, 11, 12 };
    preRoll.append(c, 6);
    CHECK(preRoll.samples() == std::vector<float>({ 9, 10, 11, 12 }));
    preRoll.setCapacity(2);
    CHECK(preRoll.samples() == std::vector<float>({ 11, 12 }));
    preRoll.clear();
    CHECK(preRoll.samples().empty());
    preRoll.setCapacity(0);
    preRoll.append(a, 3);
    CHECK(preRoll.samples().empty());
}

// WAV files in memory.
static void put16(std::string& out, uint16_t value)
{
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>(value >> 8));
}

static void put32(std::string& out, uint32_t value)
{
    put16(out, static_cast<uint16_t>(value & 0xFFFF));
    put16(out, static_cast<uint16_t>(value >> 16));
}

static std::string chunk(const char* name, const std::string& body)
{
    std::string out(name, 4);
    put32(out, static_cast<uint32_t>(body.size()));
    out += body;
    if (body.size() & 1)
        out.push_back('\0');
    return out;
}

static std::string format(uint16_t encoding, uint16_t channels, uint32_t rate, uint16_t bits, bool extensible = false)
{
    std::string body;
    put16(body, extensible ? 0xFFFE : encoding);
    put16(body, channels);
    put32(body, rate);
    put32(body, rate * channels * bits / 8);
    put16(body, static_cast<uint16_t>(channels * bits / 8));
    put16(body, bits);
    if (extensible) {
        put16(body, 22);
        put16(body, bits);
        put32(body, 0);
        put16(body, encoding);
        body += std::string("\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71", 14);
    }
    return body;
}

static std::string wav(const std::string& chunks)
{
    std::string out("RIFF");
    put32(out, static_cast<uint32_t>(4 + chunks.size()));
    out += "WAVE";
    return out + chunks;
}

static bool parse(const std::string& file, SpeechWavInfo& info, const char*& error)
{
    return speechParseWav(reinterpret_cast<const uint8_t*>(file.data()), file.size(), info, error);
}

static std::vector<float> convert(const std::string& file, const SpeechWavInfo& info, size_t first, size_t frames, size_t* converted = nullptr)
{
    std::vector<float> out;
    size_t count = speechConvertWavFrames(reinterpret_cast<const uint8_t*>(file.data()), info, first, frames, out);
    if (converted)
        *converted = count;
    return out;
}

static void testWav()
{
    SpeechWavInfo info;
    const char* error = nullptr;

    // 16 bit, one channel, as recordings for a recogniser usually are.
    std::string data;
    for (int16_t value : { int16_t(0), int16_t(16384), int16_t(-16384), int16_t(32767), int16_t(-32768) })
        put16(data, static_cast<uint16_t>(value));
    auto file = wav(chunk("fmt ", format(1, 1, 16000, 16)) + chunk("data", data));
    CHECK(parse(file, info, error) && !error);
    CHECK(info.encoding == 1 && info.channels == 1 && info.bitsPerSample == 16 && info.sampleRate == 16000);
    CHECK(info.frameCount() == 5 && info.dataOffset == 44);
    size_t count = 0;
    auto samples = convert(file, info, 0, 100, &count);
    CHECK(count == 5 && samples.size() == 5);
    if (samples.size() == 5) {
        CHECK(samples[0] == 0);
        CHECK_NEAR(samples[1], 0.5, 1e-6);
        CHECK_NEAR(samples[2], -0.5, 1e-6);
        CHECK_NEAR(samples[3], 1, 1e-4);
        CHECK(samples[4] == -1);
    }
    // From the middle, and past the end.
    samples = convert(file, info, 3, 1, &count);
    CHECK(count == 1 && samples.size() == 1 && samples[0] > 0.99f);
    samples = convert(file, info, 4, 10, &count);
    CHECK(count == 1 && samples.size() == 1 && samples[0] == -1);
    samples = convert(file, info, 5, 10, &count);
    CHECK(!count && samples.empty());

    // Other chunks before the samples, one of an odd length.
    file = wav(chunk("LIST", "abc") + chunk("fmt ", format(1, 1, 8000, 16)) + chunk("fact", "1234") + chunk("data", data));
    CHECK(parse(file, info, error));
    CHECK(info.sampleRate == 8000 && info.frameCount() == 5);
    samples = convert(file, info, 1, 1);
    CHECK(samples.size() == 1 && std::fabs(samples[0] - 0.5f) < 1e-6);

    // 8 bit (unsigned), two channels.
    data = std::string("\x80\xC0\x40\x00\xFF\x80", 6);
    file = wav(chunk("fmt ", format(1, 2, 22050, 8)) + chunk("data", data));
    CHECK(parse(file, info, error));
    CHECK(info.channels == 2 && info.frameCount() == 3 && info.bytesPerFrame() == 2);
    samples = convert(file, info, 0, 3);
    CHECK(samples.size() == 6);
    if (samples.size() == 6) {
        CHECK(samples[0] == 0);
        CHECK_NEAR(samples[1], 0.5, 1e-6);
        CHECK_NEAR(samples[2], -0.5, 1e-6);
        CHECK(samples[3] == -1);
        CHECK_NEAR(samples[4], 127 / 128., 1e-6);
    }

    // 24 bit.
    data = std::string("\x00\x00\x40\x00\x00\xC0\x00\x00\x80", 9);
    file = wav(chunk("fmt ", format(1, 1, 48000, 24)) + chunk("data", data));
    CHECK(parse(file, info, error));
    CHECK(info.frameCount() == 3);
    samples = convert(file, info, 0, 3);
    CHECK(samples.size() == 3);
    if (samples.size() == 3) {
        CHECK_NEAR(samples[0], 0.5, 1e-6);
        CHECK_NEAR(samples[1], -0.5, 1e-6);
        CHECK(samples[2] == -1);
    }

    // 32 bit integers, and floating point in the extensible form.
    data.clear();
    put32(data, 0x40000000u);
    put32(data, 0xC0000000u);
    file = wav(chunk("fmt ", format(1, 1, 48000, 32)) + chunk("data", data));
    CHECK(parse(file, info, error));
    samples = convert(file, info, 0, 2);
    CHECK(samples.size() == 2 && samples[0] == 0.5f && samples[1] == -0.5f);

    data.clear();
    for (float value : { 0.25f, -0.75f, 1.f, 0.f }) {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        put32(data, bits);
    }
    file = wav(chunk("fmt ", format(3, 2, 44100, 32, true)) + chunk("data", data));
    CHECK(parse(file, info, error));
    CHECK(info.encoding == 3 && info.channels == 2 && info.frameCount() == 2);
    samples = convert(file, info, 0, 2);
    CHECK(samples == std::vector<float>({ 0.25f, -0.75f, 1.f, 0.f }));

    // A header that promises more than the file has (a recording cut off):
    // what is there is read, whole frames only.
    data = std::string(10, '\x10');
    file = wav(chunk("fmt ", format(1, 2, 16000, 16)) + chunk("data", data));
    file[file.size() - 10 - 4] = '\xFF';
    file.resize(file.size() - 3);
    CHECK(parse(file, info, error));
    CHECK(info.frameCount() == 1 && info.dataSize == 4);

    // What is not such a file says so.
    CHECK(!parse("", info, error) && error);
    CHECK(!parse("RIFFxxxxWAVX", info, error) && error);
    CHECK(!parse(std::string("OggS") + std::string(100, 'x'), info, error) && error);
    CHECK(!parse(wav(chunk("fmt ", format(1, 1, 16000, 16))), info, error) && std::string(error).find("no samples") != std::string::npos);
    CHECK(!parse(wav(chunk("data", "abcd")), info, error) && error);
    CHECK(!parse(wav(chunk("fmt ", "short") + chunk("data", "abcd")), info, error) && error);
    // ADPCM, 64 bit floating point, 12 bit: not read.
    CHECK(!parse(wav(chunk("fmt ", format(2, 1, 16000, 4)) + chunk("data", "abcd")), info, error) && error);
    CHECK(!parse(wav(chunk("fmt ", format(3, 1, 16000, 64)) + chunk("data", "abcdefgh")), info, error) && error);
    CHECK(!parse(wav(chunk("fmt ", format(1, 1, 16000, 12)) + chunk("data", "abcd")), info, error) && error);
    // No channels, too many, a rate that is none.
    CHECK(!parse(wav(chunk("fmt ", format(1, 0, 16000, 16)) + chunk("data", "abcd")), info, error) && error);
    CHECK(!parse(wav(chunk("fmt ", format(1, 9, 16000, 16)) + chunk("data", std::string(36, 'a'))), info, error) && error);
    CHECK(!parse(wav(chunk("fmt ", format(1, 1, 0, 16)) + chunk("data", "abcd")), info, error) && error);
}

// A recording through the reader and the detector together, as the testing
// aid plays it: two channels at 44.1 kHz, 16 bit.
static void testWavThroughEndpointer()
{
    Signal signal(44100);
    signal.noise(0.8, 0.001f).speech(1.2, 0.06f).noise(1.6, 0.001f);
    std::string data;
    for (float sample : signal.samples()) {
        auto value = static_cast<int16_t>(std::lround(std::max(-1.f, std::min(1.f, sample)) * 32767));
        // The voice on the left channel only.
        put16(data, static_cast<uint16_t>(value));
        put16(data, 0);
    }
    auto file = wav(chunk("fmt ", format(1, 2, 44100, 16)) + chunk("data", data));
    SpeechWavInfo info;
    const char* error = nullptr;
    CHECK(parse(file, info, error));
    CHECK(info.frameCount() == signal.samples().size());

    SpeechEndpointer endpointer;
    std::vector<float> interleaved, mono;
    std::vector<double> starts, ends;
    size_t position = 0;
    while (true) {
        size_t frames = speechConvertWavFrames(reinterpret_cast<const uint8_t*>(file.data()), info, position, 441, interleaved);
        if (!frames)
            break;
        speechDownmixToMono(interleaved.data(), frames, info.channels, mono);
        endpointer.process(mono.data(), frames, info.sampleRate, [&](SpeechEndpointer::Event event, size_t offset) {
            (event == SpeechEndpointer::Event::SpeechStart ? starts : ends).push_back((position + offset) / 44100.);
        });
        position += frames;
    }
    CHECK(starts.size() == 1 && ends.size() == 1);
    if (!starts.empty())
        CHECK_NEAR(starts[0], 0.92, 0.03);
    if (!ends.empty())
        CHECK_NEAR(ends[0], 2.9, 0.03);
}

int main()
{
    testDownmix();
    testQuiet();
    testOneUtterance();
    testClicksAreNotSpeech();
    testPauses();
    testSpeechFromTheFirstSample();
    testNoisyRoom();
    testChunkingDoesNotMatter();
    testSettings();
    testResetAndRateChange();
    testPreRoll();
    testWav();
    testWavThroughEndpointer();
    std::printf("SPEECH_ENDPOINT_RESULT %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}

// Copyright (C) 2026 Summit contributors. SPDX-License-Identifier: BSD-2-Clause
//
// Speech recognition without the browser (Summit #159): opens the engine's
// libsummitspeech.so as the engine does, plays a WAV file into it the way
// SpeechRecognitionTaskHaiku does with the microphone (one channel, the
// end-of-speech detector, utterances), and prints what the system's
// recogniser returns. It tells a problem of the system (no libneural, no
// model, a model that does not load) from one of the browser.
//
// Runs on Haiku. Built with the engine's headers, without WebKit:
//
//   g++ -std=c++17 -O2 -I<engine>/Source/WebCore/Modules/speech/haiku
//       tests/SpeechShimProbe.cpp -o speech_shim_probe
//
//   speech_shim_probe [options] recording.wav
//     --shim FILE       libsummitspeech.so (default: beside this program, then
//                       /boot/system/lib/summit-webkit/lib, then the library path)
//     --lang TAG        language (default en-US; "" for the system's)
//     --model NAME      a model's name or directory
//     --interim MS      ask for results that are not final, this often
//     --continuous      go on after the first utterance, as a continuous
//                       recognition does
//     --no-vad          the whole file as one utterance
//     --silence MS      the pause that ends an utterance (default 1000, and
//                       1600 with --continuous, as in the engine)
//     --fast            as fast as the file reads, not in real time
//     --expect TEXT     succeed only when the final text contains TEXT
//                       (letters and digits compared, case ignored)
//
// The last line is "SPEECH PROBE RESULT PASS" or "... FAIL: reason"; the exit
// status says the same.
#include "SpeechEndpointerHaiku.h"
#include "SpeechRecognitionShimHaiku.h"

#include <OS.h>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <image.h>
#include <iterator>
#include <mutex>
#include <string>

using namespace WebCore;

static std::mutex lock;
static std::condition_variable changed;
static bigtime_t startTime;
static unsigned utterancesDone = 0;
static bool failed = false;
static bool ready = false;
static std::string failure;
static std::string finalText;

static double now()
{
    return (system_time() - startTime) / 1e6;
}

static void callback(void*, const summit_speech_event* event)
{
    std::lock_guard<std::mutex> guard(lock);
    switch (event->type) {
    case SUMMIT_SPEECH_EVENT_READY:
        ready = true;
        printf("%7.2f  ready: model %s, language %s\n", now(), event->text, event->language);
        break;
    case SUMMIT_SPEECH_EVENT_RESULT:
        printf("%7.2f  utterance %u %s [%.2f-%.2f s]: \"%s\"\n", now(), event->utterance, event->final ? "final  " : "interim",
            event->start / 1e6, event->end / 1e6, event->text);
        if (event->final) {
            if (!finalText.empty())
                finalText += ' ';
            finalText += event->text;
        }
        break;
    case SUMMIT_SPEECH_EVENT_UTTERANCE_DONE:
        printf("%7.2f  utterance %u done (%s)\n", now(), event->utterance, event->status ? strerror(event->status) : "ok");
        ++utterancesDone;
        break;
    case SUMMIT_SPEECH_EVENT_ERROR:
        printf("%7.2f  error %d: %s\n", now(), event->error, event->text);
        failed = true;
        failure = event->text ? event->text : "error";
        break;
    }
    fflush(stdout);
    changed.notify_all();
}

static std::string directoryOfProgram()
{
    int32 cookie = 0;
    image_info image;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
        if (image.type != B_APP_IMAGE)
            continue;
        std::string path = image.name;
        auto slash = path.rfind('/');
        return slash == std::string::npos ? "." : path.substr(0, slash);
    }
    return ".";
}

static std::string lettersOnly(const std::string& text)
{
    std::string out;
    for (unsigned char c : text) {
        if (isalnum(c))
            out += static_cast<char>(tolower(c));
        else if (c >= 0x80)
            out += static_cast<char>(c);
    }
    return out;
}

static int finish(bool pass, const std::string& reason)
{
    if (pass)
        printf("SPEECH PROBE RESULT PASS\n");
    else
        printf("SPEECH PROBE RESULT FAIL: %s\n", reason.c_str());
    fflush(stdout);
    // The library's threads are not waited for.
    _exit(pass ? 0 : 1);
    return 0;
}

int main(int argc, char** argv)
{
    std::string shim, language = "en-US", model, expect, file;
    long interim = 0, silence = 0;
    bool continuous = false, vad = true, fast = false;
    for (int i = 1; i < argc; ++i) {
        std::string argument = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (argument == "--shim")
            shim = value();
        else if (argument == "--lang")
            language = value();
        else if (argument == "--model")
            model = value();
        else if (argument == "--interim")
            interim = atol(value().c_str());
        else if (argument == "--silence")
            silence = atol(value().c_str());
        else if (argument == "--expect")
            expect = value();
        else if (argument == "--continuous")
            continuous = true;
        else if (argument == "--no-vad")
            vad = false;
        else if (argument == "--fast")
            fast = true;
        else
            file = argument;
    }
    if (file.empty()) {
        fprintf(stderr, "usage: speech_shim_probe [--shim FILE] [--lang TAG] [--model NAME] [--interim MS] [--continuous] [--no-vad] [--silence MS] [--fast] [--expect TEXT] recording.wav\n");
        return 2;
    }
    if (silence <= 0)
        silence = continuous ? 1600 : 1000;
    startTime = system_time();

    std::ifstream input(file, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    SpeechWavInfo info;
    const char* problem = nullptr;
    if (!input || !speechParseWav(bytes.data(), bytes.size(), info, problem))
        return finish(false, file + ": " + (problem ? problem : "cannot be read"));
    printf("%s: %.2f s, %u Hz, %u channels, %u bit\n", file.c_str(), double(info.frameCount()) / info.sampleRate, unsigned(info.sampleRate), unsigned(info.channels), unsigned(info.bitsPerSample));

    void* library = nullptr;
    std::string tried;
    std::vector<std::string> candidates;
    if (!shim.empty())
        candidates.push_back(shim);
    else {
        candidates.push_back(directoryOfProgram() + "/libsummitspeech.so");
        candidates.push_back("/boot/system/lib/summit-webkit/lib/libsummitspeech.so");
        candidates.push_back("libsummitspeech.so");
    }
    for (auto& candidate : candidates) {
        library = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (library) {
            printf("library: %s\n", candidate.c_str());
            break;
        }
        tried += "\n  " + candidate + ": " + dlerror();
    }
    if (!library)
        return finish(false, "libsummitspeech.so does not load (is libneural.so with BSpeechRecognizer on this system?)" + tried);

    auto version = reinterpret_cast<decltype(&summit_speech_api_version)>(dlsym(library, "summit_speech_api_version"));
    auto create = reinterpret_cast<decltype(&summit_speech_create)>(dlsym(library, "summit_speech_create"));
    auto feed = reinterpret_cast<decltype(&summit_speech_feed)>(dlsym(library, "summit_speech_feed"));
    auto endUtterance = reinterpret_cast<decltype(&summit_speech_end_utterance)>(dlsym(library, "summit_speech_end_utterance"));
    auto destroy = reinterpret_cast<decltype(&summit_speech_destroy)>(dlsym(library, "summit_speech_destroy"));
    if (!version || !create || !feed || !endUtterance || !destroy || version() != SUMMIT_SPEECH_API_VERSION)
        return finish(false, "the library has another interface than this program");

    summit_speech_config config;
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.language = language.c_str();
    config.model = model.empty() ? nullptr : model.c_str();
    config.interim_interval = interim * 1000;
    int error = 0;
    char message[512] = "";
    summit_speech_session* session = create(&config, &callback, nullptr, &error, message, sizeof(message));
    if (!session)
        return finish(false, std::string("no session (error ") + std::to_string(error) + "): " + message);

    // The file, then silence, 10 ms at a time, as the engine gets a
    // microphone's audio.
    SpeechEndpointerSettings settings;
    settings.trailingSilence = silence / 1000.;
    SpeechEndpointer endpointer(settings);
    SpeechPreRoll preRoll;
    preRoll.setCapacity(static_cast<size_t>(info.sampleRate * 0.4));
    std::vector<float> interleaved, mono;
    size_t chunk = std::max<size_t>(1, info.sampleRate / 100);
    size_t position = 0;
    size_t total = info.frameCount() + static_cast<size_t>(info.sampleRate) * 3;
    unsigned utterancesEnded = 0;
    bool open = false;
    bool listening = true;
    double rate = info.sampleRate;
    bigtime_t playStart = system_time();

    auto endOpenUtterance = [&] {
        if (open && endUtterance(session))
            ++utterancesEnded;
        open = false;
    };

    while (position < total && listening) {
        {
            std::lock_guard<std::mutex> guard(lock);
            if (failed)
                break;
        }
        size_t frames = speechConvertWavFrames(bytes.data(), info, position, chunk, interleaved);
        if (frames)
            speechDownmixToMono(interleaved.data(), frames, info.channels, mono);
        else {
            frames = chunk;
            mono.assign(chunk, 0.f);
        }

        if (!vad) {
            // Everything, as one utterance.
            if (position < info.frameCount()) {
                open = true;
                feed(session, mono.data(), frames, rate);
            }
        } else {
            bool wasInSpeech = endpointer.isInSpeech();
            bool started = false, ended = false;
            endpointer.process(mono.data(), frames, rate, [&](SpeechEndpointer::Event event, size_t) {
                (event == SpeechEndpointer::Event::SpeechStart ? started : ended) = true;
            });
            if (started) {
                std::lock_guard<std::mutex> guard(lock);
                printf("%7.2f  speech begins at %.2f s of the file\n", now(), endpointer.secondsProcessed());
            }
            if (started) {
                open = true;
                feed(session, preRoll.samples().data(), preRoll.samples().size(), rate);
                preRoll.clear();
                feed(session, mono.data(), frames, rate);
            } else if (wasInSpeech)
                feed(session, mono.data(), frames, rate);
            else
                preRoll.append(mono.data(), frames);
            if (ended) {
                {
                    std::lock_guard<std::mutex> guard(lock);
                    printf("%7.2f  speech ends at %.2f s of the file\n", now(), endpointer.secondsProcessed());
                }
                endOpenUtterance();
                if (!continuous)
                    listening = false;
            }
        }
        position += frames;
        if (!fast) {
            bigtime_t due = playStart + static_cast<bigtime_t>(position / rate * 1e6);
            if (due > system_time())
                snooze_until(due, B_SYSTEM_TIMEBASE);
        }
    }
    endOpenUtterance();
    printf("%7.2f  audio is over: %u utterance%s to wait for\n", now(), utterancesEnded, utterancesEnded == 1 ? "" : "s");
    fflush(stdout);

    {
        std::unique_lock<std::mutex> guard(lock);
        // A model on a slow machine, loading first: minutes, not hours.
        bool done = changed.wait_for(guard, std::chrono::seconds(300), [&] { return failed || utterancesDone >= utterancesEnded; });
        if (!done)
            return finish(false, "the recogniser did not finish in 5 minutes");
    }
    destroy(session);

    std::lock_guard<std::mutex> guard(lock);
    printf("final text: \"%s\"\n", finalText.c_str());
    if (failed)
        return finish(false, failure);
    if (!utterancesEnded)
        return finish(false, "no speech was found in the recording");
    if (finalText.empty())
        return finish(false, "the recogniser returned no text");
    if (!expect.empty() && lettersOnly(finalText).find(lettersOnly(expect)) == std::string::npos)
        return finish(false, "the text does not contain \"" + expect + "\"");
    return finish(true, "");
}

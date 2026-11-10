# Speech recognition (webkitSpeechRecognition)

The speech-recognition half of [issue #159](https://github.com/airos-works/summit/issues/159):
`webkitSpeechRecognition` on the system's own recogniser. Nothing leaves the
machine: the audio goes to a Whisper model that air/OS's Neural Kit runs
locally (`BSpeechRecognizer` in `libneural.so`, haiku `docs/airos/NEURAL-KIT.md`).

Status, 10 November 2026: on `main`, built for x86_64, and run in a QEMU VM
with a recording in place of the microphone
([what was checked](#what-was-checked)). Not yet tried on a real machine
with a real microphone, and CI packages do not carry `libsummitspeech.so`
yet ([Packaging](#packaging)), so installed builds do not have it.

## What a page gets

```js
const recognition = new webkitSpeechRecognition();   // the only name, as in Safari and Chrome
recognition.lang = 'en-US';
recognition.interimResults = true;
recognition.continuous = false;
recognition.onresult = event => { /* event.results[i][0].transcript, .isFinal */ };
recognition.start();
```

- The first `start()` of a site asks for the **microphone**, with the same
  question and the same remembered answer as `getUserMedia()` (Preferences ›
  Site Permissions). A page a WebDriver session controls is not asked, as for
  `getUserMedia()` there.
- Events come in the specification's order: `start`, `audiostart`,
  `soundstart`, `speechstart`, `result`…, `speechend`, `soundend`, `audioend`,
  `end`.
- **Not continuous** (the default): one utterance. The recognition ends by
  itself when the person stops speaking (a pause of 1 s), with one final
  result holding everything that was said. While they speak, with
  `interimResults`, a result that is not final is replaced about every 1.5 s.
  `nomatch` when there was sound and the model made no text of it.
- **Continuous**: every phrase the model completes is a final result of its
  own, added to `event.results`; a pause of 1.6 s ends an utterance and
  brings its last phrases. It goes on until `stop()` or `abort()`, or ends
  after a minute in which nobody spoke.
- `stop()` returns what was said so far, then ends; `abort()` ends at once.
- Results have **one alternative** whatever `maxAlternatives` asks. The model
  gives no measure of how sure it is: `confidence` is 1 for a final result
  and 0 for one that is not.
- `lang`: the language part of the tag ("en" of `en-US`) has to be one an
  installed model knows. Without `lang`, the system's first language when a
  model knows it, English otherwise.

Errors (`event.error`, with a sentence in `event.message`), each followed by
`end`:

| Error | When |
| --- | --- |
| `not-allowed` | the microphone was refused, or the page is not visible |
| `service-not-allowed` | the system has no recogniser (no `libneural.so` with `BSpeechRecognizer`, or no `libsummitspeech.so`), **no model is installed**, or a model did not load |
| `language-not-supported` | no installed model knows `lang` (`de-DE` with only the English model) |
| `audio-capture` | no microphone, or it stopped |
| `no-speech` | nobody spoke for 8 s (a minute when continuous) |
| `aborted` | another recognition started in the page, or the recogniser failed |

The interface exists only where the system has the recogniser, so that pages
do not offer dictation that cannot work: on stock Haiku, and on air/OS older
than the kit's recogniser, `'webkitSpeechRecognition' in window` is false, as
before. It also needs a secure context (`https:` or `http://localhost`).

## What the machine needs

1. **The Neural Kit with the recogniser**: air/OS built from haiku `master`
   d7ca70ae67 (hrev60207-2548) or later. `ls /boot/system/lib/libneural.so`,
   and `neural transcribe recording.wav` works there.
2. **A Summit engine with `lib/libsummitspeech.so`** next to `libWebKit.so.1`
   (`/boot/system/lib/summit-webkit/lib/`). CI builds it when its SDK has
   `neural/SpeechRecognizer.h`; see [Packaging](#packaging).
3. **A speech model.** None ships with air/OS yet. A model is a directory
   with a `speech.conf` under `neural/models` of a data directory:

   ```sh
   # on a Linux or air/OS machine with the haiku tree:
   tools/airos/neural/fetch-models.sh /tmp/models speech          # whisper-tiny.en, English, 150 MB
   tools/airos/neural/fetch-models.sh /tmp/models speech-multilingual   # whisper-tiny, 99 languages
   # then copy /tmp/models/neural/models/whisper-tiny.en to the machine as
   /boot/home/config/non-packaged/data/neural/models/whisper-tiny.en/
   # (or /boot/system/non-packaged/data/neural/models/ for every user)
   ```

   For English the engine takes a model whose name ends in `.en` when there
   is one, then any other; for another language, the models that are not
   English-only, in name order, until one knows the language. A model stays
   loaded for five minutes after its last recognition, so a page that starts
   one recognition after another does not read 150 MB each time.

Without the model `start()` fails with `service-not-allowed` ("no speech
model is installed (a directory with a speech.conf under neural/models …)"),
and the same sentence is on Summit's standard error output, with the prefix
`Summit speech recognition:`.

## How it works

```
web process                      browser process (Summit)
SpeechRecognition ──IPC──▶ SpeechRecognitionServer ─▶ SpeechRecognizer
                                 │ microphone permission (SitePermissionsHaiku)
                                 ▼
        HaikuAudioCaptureSource (Media Kit consumer node, as getUserMedia)
                                 │ interleaved float, on the node's thread
                                 ▼ copied to the main thread
        SpeechRecognitionTaskHaiku: one channel ▶ end-of-speech detector
                                 │ utterances (speech only, with 0.4 s before it)
                                 ▼ dlopen()
        libsummitspeech.so ──▶ BSpeechRecognizer (libneural.so): Start/Feed/Finish
                                 │ B_SPEECH_RESULT / B_SPEECH_FINISHED messages
                                 ▼ a BLooper of the library, then the main thread
        SpeechRecognitionUpdate ──IPC──▶ result / error / end events
```

Recognition runs in the browser process, as WebKit has it on the Mac (the
web process never sees the audio). Engine files, all in
`engine/patches/0001-haiku-port.patch`:

| File | Is |
| --- | --- |
| `Source/WebCore/Modules/speech/haiku/SpeechRecognizerHaiku.cpp` | the platform half of WebCore's `SpeechRecognizer` |
| `…/haiku/SpeechRecognitionTaskHaiku.{h,cpp}` | one `start()`: loads the library, runs the detector, turns the recogniser's results into updates, ends the recognition |
| `…/haiku/SpeechEndpointerHaiku.h` | plain C++: down-mix to one channel, the end-of-speech detector, the pre-roll buffer, the WAV reader |
| `…/haiku/SpeechRecognitionShimHaiku.h` | the C interface of `libsummitspeech.so` |
| `…/haiku/SpeechRecognitionShimHaiku.cpp` | `libsummitspeech.so` itself: a session with a `BLooper`, model choice, the recogniser kept loaded |
| `Source/WebCore/Modules/speech/SpeechRecognizer.{h,cpp}`, `SpeechRecognitionCaptureSource*.cpp` | the hooks: the task, the audio hand-off from the capture thread, the error and `end` when recognition cannot start |
| `Source/WebCore/platform/mediastream/haiku/HaikuCaptureSources.{h,cpp}` | `HaikuSpeechTestAudioSource`, the WAV file in place of the microphone |
| `Source/WebKit/UIProcess/haiku/WebView.cpp` | `SpeechRecognitionEnabled` |
| `Source/WebKit/UIProcess/UserMediaPermissionRequestManagerProxy.cpp` | no microphone question in a WebDriver session |
| `Source/WebCore/PlatformHaiku.cmake` | the sources, and the `summitspeech` target |

**No link to the kit.** The engine has to start on systems without
`libneural.so` (stock Haiku, older air/OS), so `libWebKit.so` does not link
it. `libsummitspeech.so` does, and exposes six C functions
(`summit_speech_create`, `_feed`, `_end_utterance`, `_cancel`, `_destroy`,
`_api_version`) and a callback. The engine opens it with
`dlopen(RTLD_NOW)` the first time it needs it: from the directory of
`libWebKit.so`, then by name on the library path. Where the library or the
kit is missing, or the kit is older than the recogniser, that fails cleanly.
The library itself is built only where the SDK has the kit's
`neural/SpeechRecognizer.h`.

**End of speech.** The kit's recogniser has no voice activity detection, and
a Whisper model reads 30 s windows whether or not anyone speaks. So the
engine decides what speech is (`SpeechEndpointer`): the level of 20 ms frames
against the level of the background, which it learns as the level the sound
keeps coming back to (down quickly, up within about a second, never above
−42 dBFS). Speech begins when the level stays 3.5 times above the
background (and above −50 dBFS) for 120 ms, and ends after the pause above
at less than twice the background. Only speech reaches the model, with the
0.4 s before it; quiet costs no processor time. Each stretch is an
*utterance*, one recording of the kit's recogniser
(`Start()`…`Finish()`); the next utterance can begin while the last one is
still being recognised (its audio waits in the library, at most two minutes
of it).

**Threads and teardown.** The task lives on the browser's main thread. The
capture node's thread only copies samples; the library's thread only passes
events on. `summit_speech_destroy()` returns once the callback can no longer
run and gives up the recogniser on the library's thread, so closing a page
in the middle of a recognition, a second `start()`, revoking the microphone
(the source stops: `audio-capture`) and quitting do not wait for the model.
The recogniser's results carry a token per recording; what arrives for a
recording that was dropped is ignored.

## Switches

`SpeechRecognitionEnabled` is the engine preference; Summit sets it when the
library loads. Environment variables, for testing:

| Variable | Does |
| --- | --- |
| `SUMMIT_RECOGNITION=0` / `=1` | never / always expose the interface (with `1` on a system without the recogniser, `start()` fails with `service-not-allowed`) |
| `SUMMIT_PREF_OFF=recognition` | the same as `SUMMIT_RECOGNITION=0` |
| `SUMMIT_RECOGNITION_WAV=<file>` | **a recording in place of the microphone**, see below |
| `SUMMIT_RECOGNITION_TRACE=1` | logs the detector, the model and every result to standard error |
| `SUMMIT_RECOGNITION_MODEL=<name or directory>` | this model instead of one chosen by language |
| `SUMMIT_RECOGNITION_SILENCE_MS=<n>` | the pause that ends an utterance (default 1000; 1600 when continuous) |
| `SUMMIT_RECOGNITION_INTERIM_MS=<n>` | how often results that are not final are made (default 1500) |
| `SUMMIT_RECOGNITION_VAD=0` | no detector: everything captured goes to the model, and only `stop()` ends an utterance |
| `SUMMIT_RECOGNITION_DEVICE=any` | let the kit pick the device (default: the CPU, the only one the models were measured on) |
| `SUMMIT_RECOGNITION_KEEP_MODEL=<seconds>` | how long a model stays loaded (default 300; 0: not at all) |
| `SUMMIT_RECOGNITION_SHIM=<file>` | another `libsummitspeech.so` |

### A recording in place of the microphone

With `SUMMIT_RECOGNITION_WAV=/path/recording.wav` in Summit's environment,
speech recognition hears that file instead of the microphone, on machines
with or without one: played once, in real time, from the moment a
recognition starts, then silence (which is what ends an utterance). 8, 16,
24 or 32 bit PCM or 32 bit float, any rate, up to eight channels. Only
speech recognition is affected (`getUserMedia()` and the device list never
see it), the microphone permission is still asked, and Summit says so on
standard error: `Summit speech recognition: the microphone is the recording …`.
It is a testing aid; leave the variable unset otherwise.

## Testing

**Unit tests** (any machine): the detector, the down-mix, the pre-roll and
the WAV reader, 152 checks on made-up audio.

```sh
python3 tools/test-engine-speech-endpoint.py      # or ctest -R summit_engine_speech_endpoint
```

**Without the browser** (on the machine): `tests/SpeechShimProbe.cpp` opens
`libsummitspeech.so` as the engine does and plays a WAV file into it with the
same detector. It tells a problem of the system (no kit, no model) from one
of the browser.

```sh
g++ -std=c++17 -O2 -I<engine>/Source/WebCore/Modules/speech/haiku tests/SpeechShimProbe.cpp -o speech_shim_probe
./speech_shim_probe --continuous --interim 1000 jfk.wav
```

**The manual page**: `tests/fixtures/speech-recognition.html`, served from
localhost (`python3.12 -m http.server 8000 -d tests/fixtures` on air/OS, then
`http://localhost:8000/speech-recognition.html`). Start, speak, and read the
events; `?auto=1&continuous=1` starts by itself. `window.speechTest` holds
everything for scripts.

**Scripted, through WebDriver**: on the machine,

```sh
python3.12 tools/test-speech-recognition.py --wav /boot/home/jfk.wav --expect "fellow americans"
python3.12 tools/test-speech-recognition.py --wav /boot/home/jfk.wav --continuous --stop-after 17 \
    --expect "ask what you can do for your country"
python3.12 tools/test-speech-recognition.py --expect-error service-not-allowed    # no model installed
```

serves the page, starts `summitdriver` (which starts Summit), runs one
recognition and prints its events and `SPEECH RECOGNITION TEST PASS` or
`FAIL`. `--repeat N` runs several in the same page, `--abort-after S` calls
`abort()`, `--close-after S` ends the session in the middle of the
recognition, `--trace` turns the engine's log on.

## What was checked

On 9 and 10 November 2026, on the build server; nothing on a lab machine.

- **Built**: the x86_64 engine with these changes (cross build as CI's, own
  tree), `libsummitspeech.so` against the kit in CI's x86_64 SDK
  (hrev60207-2551). The changed and new engine files also pass the arm64
  compiler (`-fsyntax-only` with the commands of CI's arm64 engine build),
  and an arm64 configure as CI's finds the kit in the arm64 SDK and builds
  and links `libsummitspeech.so`. No arm64 engine was linked.
- **Unit-tested** on the build host: 152 checks pass.
- **Run in a VM** (QEMU/KVM, the x86_64 air/OS CI image of d7ca70ae67,
  `whisper-tiny.en`): see the record below.

The image was booted as a CD under KVM (8 CPUs, 6 GB, no sound card, no
network) from a private copy with the test files added: the model, the
recordings (`jfk.wav`, 11 s, a speaker who pauses for a second between
phrases; `libri1.wav`, 10 s read without pauses), this build's
`libWebKit.so.1` and `libsummitspeech.so`. The image's own Summit (the same
commit, 19b1851) ran with that `libWebKit.so.1` preloaded in place of the
packaged one. `tools/test-speech-recognition.py` ran inside the VM, so every
line went summitdriver → Summit → the page → the engine → the kit, with
`SUMMIT_RECOGNITION_WAV` as the microphone.

| Run | What the page saw |
| --- | --- |
| packaged engine (before this change) | no `webkitSpeechRecognition` |
| no model installed | `error` `service-not-allowed` ("no speech model is installed …") and `end`, 0.3 s after `start()`; no `start` event |
| `jfk.wav`, not continuous | `start`, `audiostart`, `soundstart`, `speechstart`, `speechend`, `soundend`, `audioend`, one final `result` "And so am I fellow Americans.", `end`: ended by itself at the speaker's first pause (the recording says "And so, my fellow Americans") |
| `jfk.wav`, continuous, `stop()` at 17 s | four results that were not final, growing every 2.5 s, then the finals "And so my fellow Americans ask not what your country can do for you" and "ask what you can do for your country." about a second after the end of the utterance was noticed; `speechend`, `soundend`, `audioend`, `end` at `stop()` |
| `lang = "de-DE"`, English model only | `error` `language-not-supported` and `end` at once |
| a silent recording | `start`, `audiostart`, then `error` `no-speech` after 8.0 s, `audioend`, `end` |
| `libri1.wav`, `stop()` after 6 s | the final result of what was said until then ("He hoped there would be stew for dinner, turnips and carrots and bruised potatoes, and fat mutt-") 3.2 s later, then `end` |
| three recognitions in a row in one page | three final results; the second and third found the model still loaded and ended 0.85 s after the speech |
| `abort()` 2.5 s into speech (23 times) | `speechend`, `soundend`, `audioend`, `end` at once, no result |
| the page closed 2 to 6 s into a recognition (5 times) | Summit closed the window and went on to quit; no crash |
| the whole of `libri1.wav`, not continuous | "He hoped there would be stew for dinner, turnips and carrots and bruised potatoes, and fat-mutten pieces to be ladled out, and thick, peppered, flower-fatened sauce." |
| `SUMMIT_RECOGNITION=0`, `SUMMIT_PREF_OFF=recognition` | no `webkitSpeechRecognition` |

The same model through `libsummitspeech.so` alone
(`tests/SpeechShimProbe.cpp`, before the browser was tried): no model, a
model that does not know the language, and a continuous run with interim
results all behaved as above.

Times in the VM: the model loaded in 3 to 10 s the first time after a boot
and in 0.3 to 0.7 s afterwards; the very first utterance after a boot took
8 to 20 s to come back (the model's 150 MB were read from the emulated CD as
the model first ran), later ones about a second. The kit's own tool
(`neural transcribe`) took 2.3 s for the 11 s recording there.

After the engine was rebuilt on top of main 5d561e5 (10 November) the
cases without a model, `jfk.wav` continuous, `abort()`, the page closed in
the middle (twice), two recognitions in a row and `de-DE` were run again
and behaved the same; this time the engine used the package's Mesa (GL
compositing on llvmpipe) as an installed Summit does, where the runs above
had no EGL and painted in software. The start page and a page with a view
transition looked the same in a WebDriver screenshot as with the packaged
engine. With `libsummitspeech.so` taken away the page had no
`webkitSpeechRecognition`.

One thing in these runs is not about speech: about one WebDriver session
in ten did not make Summit quit when it ended. Summit stayed, with a Start
Page window marked "(Automated)", and the next session then went to that
copy. It happened with the image's own packaged engine as well, which has
none of this code (3 of 41 sessions that only loaded the page; 6 of 52
with this build, 5 of them among the 24 short sessions that ended with
`abort()`), so it belongs to WebDriver session teardown, not to
speech recognition. The test scripts in the VM killed such a Summit between
runs.

Not checked: a real microphone (levels, the voice processing in front of
the recogniser, noise), the microphone question outside WebDriver (a
session's page is not asked), revoking the permission during a recognition,
two pages recognising at once, a machine's real speed, arm64 at run time,
models other than `whisper-tiny.en`, and languages other than English.

## Limits

- One alternative, no real confidence, no grammars or phrase hints
  (`SpeechGrammar` is not in WebKit).
- The detector is a level detector. In a loud room (background above about
  −30 dBFS) everything counts as speech: a recognition of one utterance then
  ends only after 30 s or at `stop()`, and a continuous one keeps the model
  busy until it is stopped.
- A speaker who pauses for more than the pause above in the middle of a
  sentence is cut there; a recognition of one utterance returns the first
  part only, and the model reads halves less well than wholes.
- Whisper tiny is small: names and rare words go wrong. Larger models work
  the same way once they are converted for the kit.
- The first recognition after a start loads the model (0.25 s from the page
  cache, about 10 s from a cold disk in the VM) while audio waits; the
  first result is later by that much.
- A recognition in progress when Summit quits is dropped; the model's thread
  is not waited for.
- Under WebDriver the recogniser is the real one (there is no mock
  recogniser), and without `SUMMIT_RECOGNITION_WAV` it hears the mock
  microphone's beeps. A page a WebDriver session controls always gets
  WebKit's mock microphone (#188), so the WebDriver test never exercises the
  real capture path (HaikuCaptureNode). Check a real microphone by hand: a
  sound card that an earlier recorder left disabled sent no buffers at all
  until the capture node turned its output on again (#204).

## Packaging

`libsummitspeech.so` is a target of the engine build
(`Source/WebCore/PlatformHaiku.cmake`): built when CMake finds
`neural/SpeechRecognizer.h` and `libneural.so` in the SDK
(`HAIKU_NEURAL_INCLUDE_DIR`, `HAIKU_NEURAL_LIBRARY` name them elsewhere), into
the build's `lib/`. Configure prints `Speech recognition: libsummitspeech.so
with …` or says that it is not built. The engine builds and runs without it.
On 10 November CI's arm64 SDK and the x86_64 SDK its next build takes
(hrev60207-2551) have the header, so CI builds the library on both; the
R1/beta6 SDK has no kit.

The `summit_webkit` package has to carry it beside `libWebKit.so.1`:

- **air/OS CI** (`airos-ci`, `apps/build-app-packages.sh`,
  `build_summit_webkit`) copies the engine's files one by one, so it needs
  the few lines for this library that it has for `WebKitWebDriver`: copy
  `$SUMMIT_ENGINE_BUILD/lib/libsummitspeech.so` to `$STAGE/$engine/lib/` when
  it exists, strip it, give it the libraries' rpath. `libneural.so` is the
  system's (it is in the SDK's sysroot), so nothing is bundled for it. This
  change is **not made yet**; until it is, CI packages have no speech
  recognition even when the engine build made the library:

  ```sh
  # after: local pending=("$engine/WebProcess" "$engine/NetworkProcess") item needed source
  if [[ -e $SUMMIT_ENGINE_BUILD/lib/libsummitspeech.so ]]; then
      cp "$SUMMIT_ENGINE_BUILD/lib/libsummitspeech.so" "$STAGE/$engine/lib/libsummitspeech.so"
      chmod 644 "$STAGE/$engine/lib/libsummitspeech.so"
      "$STRIP" --strip-unneeded "$STAGE/$engine/lib/libsummitspeech.so"
      pending+=("$engine/lib/libsummitspeech.so")
  fi
  # before: ln -s libWebKit.so.1 "$STAGE/$engine/lib/libWebKit.so"
  [[ ! -e $STAGE/$engine/lib/libsummitspeech.so ]] \
      || set_rpath "$STAGE/$engine/lib/libsummitspeech.so" "$lib_rpath"
  ```
- `tools/summit-webkit-package/make-package.sh` (packages made on a machine)
  copies the bundle's whole `lib/`, and now treats `libneural.so` as part of
  the system.
- The Haiku R1/beta6 build has no kit in its SDK: no library, no interface.
- arm64 builds it the same way once the arm64 SDK has the kit.

## Later: downloading the model on first use

Not implemented. What it would have to do:

- **What to fetch.** The files of one model directory, as
  `fetch-models.sh` makes them: for `whisper-tiny.en`, `speech.conf` (3 kB),
  `encoder.tflite` (37.6 MB), `decoder.tflite` (114.3 MB), `vocabulary.bin`
  (357 kB), `mel_filters.bin` (64 kB) and `LICENSE` (the weights are OpenAI's,
  MIT). OpenAI publishes PyTorch checkpoints only, and the conversion
  (`tools/airos/neural/whisper_convert.py`) is not something to run in a
  browser, so the converted files have to be published somewhere first, for
  instance as assets of an airos-works release, with their sizes and SHA-256
  in a manifest in Summit like `resources/translation/models.json`
  (`fetch-models.sh` already takes such an archive: `SPEECH_ARCHIVE_URL`).
  `whisper-tiny` (99 languages, the same size) is the second set, fetched
  when a page asks for a language the English model does not know.
- **Where it goes.** `~/config/non-packaged/data/neural/models/<name>/`
  (`B_USER_NONPACKAGED_DATA_DIRECTORY`): the kit looks in `neural/models` of
  every data directory, the user's first. Written to a temporary directory
  beside it and renamed when every file's hash matches, so that a model
  directory never exists half.
- **When.** Today the missing model is an error from
  `summit_speech_create()` (`SUMMIT_SPEECH_NO_MODEL`, or
  `SUMMIT_SPEECH_NO_LANGUAGE` with the language), which becomes
  `service-not-allowed` at once. The download would hook in there: the
  engine tells the browser (a message to the `BWebKitContext`'s listener, as
  permission requests are), the browser asks the user once ("Speech
  recognition needs a 150 MB model"), downloads with the translation models'
  downloader (`src/core/TranslationModels.cpp`: sizes, hashes, cancel, no
  partial files), and the page's `start()` either waits for it or fails with
  `service-not-allowed` and works the next time. Nothing needs restarting:
  models are looked up at every `start()`.
- A `neural_models` package in the image would make all of this unnecessary
  for English.

# Encrypted Media Extensions

Summit implements `navigator.requestMediaKeySystemAccess()`, `MediaKeys`,
`MediaKeySession`, encrypted initialization-data events, key status changes,
and `waitingforkey` through WebKit's Encrypted Media Extensions API.
The native CDM supports **`org.w3.clearkey`** and temporary sessions, for
fragmented MP4 protected with any of the four ISO/IEC 23001-7 schemes
(`cenc`, `cbc1`, `cens`, `cbcs`) and for encrypted WebM. Applications can use
MediaSource or a direct MPEG-DASH media URL. EME is available in secure
contexts, including HTTPS and trusted localhost. The `encrypted-media`
permissions policy defaults to the same origin. Iframe `allow` attributes and
`Permissions-Policy` response headers can further restrict access;
cross-origin frames need delegation. Header allowlists support `self`, `*`,
an empty list, exact quoted origins and, since 30 October, subdomain
wildcards (below).

**Not in the shipped engine (30 October).** CI configures the engine with
`engine/arm64/rpi4-gl/init-cache-rtc.cmake` for both architectures, and that
file sets `ENABLE_ENCRYPTED_MEDIA` to `OFF` (it has since 5 October; the port's
own default is on). CI's x86_64 `CMakeCache.txt` has
`ENABLE_ENCRYPTED_MEDIA:BOOL=OFF`, and `navigator.requestMediaKeySystemAccess`
is undefined in a CI build. Everything on this page is compiled out of
Summit's packages until that line is changed, which rebuilds the whole engine.
The parsers and the decrypter are portable and tested on the host (below);
the code under `ENABLE(ENCRYPTED_MEDIA)` was compiled with the option on, but
the 30 October changes were not run in Summit.

Direct DASH tracks every media response's origin and CORS result. Cross-origin
media without CORS permission exposes an empty encrypted event and taints
canvas, even when the manifest itself is same-origin. Set the media element's
`crossOrigin` attribute and serve appropriate CORS headers to permit access.

Clear Key is the interoperable key system defined by the
[EME specification](https://www.w3.org/TR/encrypted-media/#clear-key).
It does not provide Widevine, PlayReady, FairPlay, hardware robustness,
persistent licenses, or commercial streaming-service certification. Those
key systems and unsupported configurations are rejected. Keys remain in
memory and are removed when their temporary session closes or is removed.

## Supported encryption

The fragmented MP4 reader accepts `encv`/`enca` entries whose original format
(`frma`) is H.264 (`avc1`, `avc3`), HEVC (`hvc1`, `hev1`), VP9, AV1, AAC or
Opus, with `tenc` defaults. It reads inline `senc` metadata and `saiz`/`saio`
auxiliary data (with or without an `aux_info_type` naming the scheme), with
8- or 16-byte IVs and clear / encrypted subsample ranges. OpenSSL's EVP
implementation decrypts:

- `cenc`: AES-128 CTR over every encrypted byte of the sample, the
  keystream continuing across subsamples.
- `cens`: AES-128 CTR with a pattern (`tenc` version 1: of every
  crypt + skip 16-byte blocks the first crypt are encrypted); only
  encrypted blocks use the keystream. `0:0` (Shaka's audio) encrypts every
  whole block.
- `cbc1`: AES-128 CBC with 16-byte per-sample IVs, the chain continuing
  across subsamples.
- `cbcs`: AES-128 CBC with a pattern (usually 1:9 for video, 0:0 for
  audio) and, usually, a constant IV from `tenc`; the chain restarts from the
  IV at every subsample.

The CBC and pattern schemes leave a trailing partial 16-byte block clear, as
ISO/IEC 23001-7 requires. 8-byte IVs are the counter's high half (CTR) or
zero-padded (CBC).

A **clear lead** (Shaka Packager's default) works: such a track describes
the stream twice in `stsd`, as `encv`/`enca` and as the clear entry, and its
first fragments select the clear one with the `tfhd` (or `trex`) sample
description index. Up to eight entries are accepted, all describing one stream
(same original format, codec configuration and dimensions); protected
entries must agree on their `tenc`. **`seig` sample groups** in a fragment
(`sbgp` + `sgpd`, as in key rotation) override the key ID, per-sample IV size,
pattern and constant IV per sample, and can mark samples clear; group
descriptions in the movie's sample table are not supported.

**Encrypted WebM** ([WebM Encryption](https://www.webmproject.org/docs/webm-encryption/))
is read by the WebM parser (Media Source and DASH WebM representations): a
track with one `ContentEncoding` of type encryption, AES in CTR mode and a
16-byte `ContentEncKeyID`. Each frame starts with a signal byte: a clear frame
loses that byte; an encrypted one carries an 8-byte IV (the counter's high
half) and, when partitioned, offsets that alternate clear and encrypted
ranges (VP9 and AV1 subsample encryption). Content compression (header
stripping) is still refused. An encrypted audio packet's duration can't be
read from its (encrypted) payload, so it is taken from the next packet's
timestamp: encrypted WebM audio keeps the container's millisecond timestamps
rather than the exact sample clock of clear audio, and Opus pre-skip is
applied once that duration is known. Each encrypted track's key ID raises an
`encrypted` event with init data type `webm`, which the CDM now accepts.

Malformed metadata fails before ciphertext reaches a decoder.

`requestMediaKeySystemAccess` accepts `video/mp4`, `audio/mp4`, `video/webm`
and `audio/webm` capabilities with encryption scheme `cenc` or (MP4 only)
`cbcs`, MP4 codecs from the list above, and codecs the Media Source player
accepts.

Common-SystemID version-1 PSSH data in movie and fragment boxes becomes a
`cenc` encrypted event. `keyids` initialization data is also supported.
License requests include the requested IDs and the `temporary` session
type, and license updates accept a JSON Web Key Set. The
[W3C CENC initialization-data format](https://www.w3.org/TR/eme-initdata-cenc/)
defines the Common SystemID and PSSH representation.

Key lookup and decryption run on the existing decoder threads. Missing keys
leave encrypted samples in bounded queues, trigger `waitingforkey`, and
stop the playback clock. Delivering keys or attaching the correct MediaKeys
object resumes decoding. Seeking, unloading and shutdown wake those queues.
Samples retained by MediaSource remain encrypted; a decoder receives a
separate plaintext copy only when a usable key is available.

Limitations: progressive encrypted MP4 outside DASH/MSE (below), legacy
`senc` overrides (flag 1), `seig` groups described in the movie box,
sample descriptions that change the codec within a track, WebM key IDs other
than 16 bytes, and noncontiguous auxiliary-data offset tables are not
supported. Support for these schemes does not imply support for every
feature in the ISO BMFF container.

**Progressive encrypted MP4** (a plain `src`, no Media Source or DASH) is not
decrypted: such a file goes to the Media Kit file player, which has no CDM.
On the X399 (30 October) Chromium's `bear-640x360-v_frag-cenc.mp4` as a
`src` showed its 16 clear-lead frames, then reached `ended` at 2.74 s with no
error and no `encrypted` event. Chrome and Firefox only support EME with
Media Source as well. Playing it would need the DASH player to treat a
single fragmented MP4 (its `moov` and `sidx`) as a one-representation
presentation.

## Permissions-Policy wildcards and reporting (30 October)

An allowlist origin may name every subdomain of a host,
`"https://*.example.com"` (the specification's origin wildcards, as in
Chrome): in the `Permissions-Policy` header (for `encrypted-media` and
`bluetooth`, the features Summit reads from it) and in an iframe `allow`
attribute (the latter not checked on a device). It matches origins with the same scheme and port whose host is a
subdomain at any depth, not `example.com` itself, and never anything under a
public suffix (`"https://*.co.uk"` matches nothing). A `*` anywhere else, or a
port wildcard, still grants nothing.

A directive may name a reporting endpoint, `encrypted-media=();report-to=main`
with `Reporting-Endpoints: main="https://…"`. A blocked use then sends a
`permissions-policy-violation` report (body `featureId`, `disposition`
`enforce`, `message`) to that endpoint as `application/reports+json`
(cross-origin endpoints get a CORS preflight). `ReportingObserver` does not
receive these reports; that needs a report body interface WebKit doesn't
have.

Checked on the X399 with `bluetooth` (EME is compiled out, see above):
`Permissions-Policy: bluetooth=("http://*.localhost:8773")` allowed
`navigator.bluetooth.getDevices()` on `a.localhost` and `b.a.localhost` and
refused it (`SecurityError`) on `localhost`; `self` and `()` behaved as
before; with `;report-to=eme` the fixture server received the report.

## Verification

Verified on X399 with frozen bundle `bundle-5a2zwdja`: all **207 native
checks** passed, with no remaining helpers or new crash reports. The engine
patch SHA-256 was
`b1d1c7dbc87106f8eddd67cdad557aa1bfe45e18e0117ae3437bbe66439ae401`.
The captured screen shows decrypted video; native traces confirm NVDEC video
decoding and 48 kHz AAC audio output. Tests cover secure-context exposure,
iframe and response-header policies, negotiation, malformed licenses, key
sharing and revocation, delayed key delivery, media-element attachment,
pause/seek/end, direct encrypted DASH, cross-origin metadata and canvas
protection, clear MSE, and shutdown while waiting for a key. All 82 native
clear-DASH regression checks also pass in this bundle.

`tests/EngineCENCTests.cpp` tests the [NIST SP 800-38A F.5.1 AES-128 CTR vector](https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-38a.pdf), subsample
keystream continuity, malformed IVs and ranges, and byte-for-byte recovery
of Shaka Packager's encrypted H.264/AAC samples. It exercises fragmented
input, inline and auxiliary-only metadata, 8/16-byte IVs, and 4,000 mutated
metadata inputs under ASan/UBSan. The test recovers 2,375 samples. The clear DASH playback regression also
passes after the shared parser changes, delivering 2,280 video and 3,444 audio
samples across seek, live-window and multi-period scenarios.

`tests/EngineCENCSchemeTests.cpp` (30 October) checks the other schemes and
WebM against independently packaged media:

- NIST SP 800-38A F.2.1 AES-128 CBC as `cbc1` and `cbcs`, a 1:1 pattern,
  partial blocks, the chain reset per subsample (`cbcs`) or not (`cbc1`).
- Chromium's `media/test/data` vectors (Shaka Packager; key ID
  `30313233343536373839303132333435`, key
  `ebdd62f16814d27b68ef122afce4ae3c`): `bear-640x360-v_frag-{cenc,cbc1,cens,cbcs}.mp4`
  (0.5 s clear lead, two sample descriptions) and `bear-640x360-a_frag-cbcs.mp4`
  (constant IV, 0:0) decrypt to exactly the samples of `bear-640x360-v_frag.mp4`
  / `-a_frag.mp4`; the key rotation vectors (key IDs only in `seig` groups)
  likewise; `bear-320x240-av_enc-av.webm` (VP8 + Vorbis, whole frames), its
  clear-first-second and all-clear variants, `bear-640x360-av_enc-av.webm`
  and `bear-320x240-opus-av_enc-av.webm` (VP9 + Opus) decrypt to the samples of
  their clear sources. `bear-320x240-v-vp9_subsample_enc-v.webm` and
  `bear-av1-cenc.webm` (partitioned) have no matching clear file any more;
  their decrypted frames are checked for VP9 headers and AV1 OBU structure,
  and written to IVF all 82 frames of each decode without errors in FFmpeg.
- `eme-fixture.py --schemes` output: the H.264/AAC segments as `cbcs` with a
  2 s clear lead and as `cens` (including 0:0 audio), and Shaka-packaged
  encrypted WebM (VP9 subsamples, Opus) against the same streams packaged in
  the clear.
- 1,000 mutated inputs per file under ASan/UBSan.

On the host it decrypts 5,200 samples (4,550 without a `libavcodec.so` for
Vorbis). `EngineCENCTests.cpp` still passes (2,375 samples); its check that
`cbcs` is refused now uses an unknown scheme.

Generate the interoperability fixtures with FFmpeg and Shaka Packager
(`--schemes` adds the `cbcs`, `cens` and WebM sets; v3.4.2 was used):

```sh
python3 tools/bench/eme-fixture.py --directory .vm/eme-fixture \
  --packager /path/to/packager [--schemes --ffmpeg /path/to/ffmpeg]
```

Chromium's vectors are fetched from
`https://chromium.googlesource.com/chromium/src/+/HEAD/media/test/data/NAME?format=TEXT`
(base64). Build and run:

```sh
g++ -std=c++20 -fsanitize=address,undefined -DWEBRTC_WEBKIT_BUILD=1 \
  -I$WEBKIT/Source/WebCore/platform/graphics/haiku -I$LIBWEBM/include -I$OPUS/include \
  tests/EngineCENCSchemeTests.cpp FragmentedMP4ParserHaiku.cpp CencCryptoHaiku.cpp \
  WebMParserHaiku.cpp $LIBWEBM/src/*.cc -lcrypto -lopus -ldl -o test-cenc-schemes  # not *_test.cc
test-cenc-schemes CHROMIUM_VECTORS_DIR .vm/eme-fixture
```

The server (`--serve`) also answers byte ranges (Shaka's WebM manifests use
`SegmentBase`), records report deliveries (`POST`, read back from
`/reports`), and adds `?policy=wildcard` (`"http://*.localhost:PORT"`),
`?feature=bluetooth` and `?report=1` to its policy headers.

Run the fixture server on the native test machine:

```sh
python3 tools/bench/eme-fixture.py --directory /path/to/eme-fixture --serve
```

Open `http://127.0.0.1:8773/test.html`. The automated native harness is
`tests/ModernEMETests.cpp`; it uses a private context, real Media Kit decoders,
native input, and the Web Inspector protocol to verify page-visible results.

For the ordinary-HTTP negative test, bind the server with `--host 0.0.0.0` and
set `SUMMIT_EME_INSECURE_ORIGIN` to its LAN HTTP origin. The harness also uses
`localhost` as a second origin for iframe and media CORS tests.

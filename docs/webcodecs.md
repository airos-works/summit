# WebCodecs (#112)

`VideoEncoder`, `VideoDecoder`, `VideoFrame` and `EncodedVideoChunk` work on
the codecs Summit's WebRTC already uses:

| Codec | Decode | Encode |
| --- | --- | --- |
| VP8 (`vp8`) | libvpx | libvpx |
| VP9 profile 0/2 (`vp09.00…`, `vp09.02…`) | libvpx | libvpx |
| H.264 (`avc1…`, `avc3…`) | FFmpeg, when the system has it | OpenH264, when the system has it (Constrained Baseline/Main) |

`isConfigSupported()` says what this computer can do: a codec whose library
is missing fails to be created. H.264 comes in both WebCodecs formats:
`avc` (length-prefixed units, with the parameter sets in the avcC
`description`, which the encoder's first output carries) and `annexb`. Each
encoder and decoder runs on its own work queue in the web process.

Audio: `AudioData` (kept in the page's sample format, converted on
`copyTo`); Opus through libopus (`AudioEncoder` mono or stereo, 2.5–60 ms
packets, the OpusHead as `description`; `AudioDecoder` giving `f32`
samples), at libopus's rates (8, 12, 16, 24, 48 kHz) and at other rates
that are whole hundreds of hertz (44.1 kHz above all): encoding resamples
to 48 kHz in 10 ms pieces (libwebrtc's `PushResampler`), decoding gives
48 kHz audio. AAC (`mp4a.40.2`, `.5`, `.29`) and MP3 are decoded through
FFmpeg when the system has it, loaded as WebRTC's H.264 decoder loads it:
AAC chunks with an `AudioSpecificConfig` description get ADTS headers made
from it (FFmpeg's AAC decoder then needs no extradata), and without one they
are taken as ADTS. AAC LC (`mp4a.40.2`) and MP3 are encoded through
FFmpeg too (its own AAC encoder and LAME), with FFmpeg 6 or 8 (libavcodec
60 or 62): the codec context is set up with AVOptions, except its sample
format (no AVOption sets it), which like the two `AVFrame` fields past the
prefix the decoders use (`ch_layout`, `sample_rate`) is written at those
versions' offsets, so other versions report unsupported. The offsets came
from FFmpeg 6.1's and 8.0's headers and were checked against the X399's
libraries (both encoders open with 1024/1152-sample frames and encode). AAC gives raw
frames with an `AudioSpecificConfig` as `description`, or ADTS frames with
`aac: { format: "adts" }` (table rates, up to 7 channels); after `flush()`
the next frame starts a new encoder. HE-AAC (`mp4a.40.5`) and HE-AAC v2
(`mp4a.40.29`, stereo) are encoded by Fraunhofer's FDK (`libfdk_aac`) when
FFmpeg has it, from 16-bit samples: raw frames with FFmpeg's global header
flag, the encoder's AudioSpecificConfig (explicit SBR signalling) as the
description, or FDK's own ADTS frames.

PCM (`pcm-u8`, `-s16`, `-s24`, `-s32`, `-f32`, `alaw`, `ulaw`) is decoded and
encoded in the engine (G.711 as ITU-T's reference expands and compresses
it; 24-bit decodes to 32-bit AudioData). FLAC and Vorbis are decoded
through FFmpeg with the description (FLAC's `fLaC` and metadata blocks,
Vorbis's three Xiph-laced headers) as the codec context's extradata, at
FFmpeg 6's and 8's offsets. They are encoded through FFmpeg too (its FLAC
encoder, from 16-bit samples, and libvorbis): the description is read from
the encoder's extradata (FLAC's STREAMINFO given a `fLaC` start and block
header; Vorbis's headers as they are), and `flac: { compressLevel }` sets
FFmpeg's compression level.

AV1 (`av01.*`, through dav1d) and HEVC (`hev1.*`/`hvc1.*`, FFmpeg's
decoder, `hvcC` descriptions turned into Annex B as `avcC` ones are) are
decoded through FFmpeg, by the same decoder H.264 uses (named for each;
neither is offered to WebRTC). WebKit's `WebCodecsAV1Enabled` and
`WebCodecsHEVCEnabled` are on for Haiku. They are not encoded.

Not yet: AV1 and HEVC encoding (the system FFmpeg has no encoder for
either), and hardware decoders (NVDEC, Cedar).

H.264 with B-frames: libavcodec gives pictures in presentation order,
holding B-frames back until their references are decoded. The decoder used
to label each picture with the chunk just sent in, so a B-frame stream's
pictures carried decode-order timestamps (the W3C audio/video player then
stuck on one frame); each picture now keeps its own chunk's label (the pts
libavcodec carries through), and `flush()` drains the pictures still held
back (an empty image to the WebRTC decoder).

## How

`WebCore/platform/mediastream/libwebrtc/haiku/WebCodecsHaiku.cpp`:
`createLibWebRTCVideoDecoderHaiku`/`createLibWebRTCVideoEncoderHaiku` (from
`VideoDecoder::createLocalDecoder`/`VideoEncoder::createLocalEncoder`) map
the codec string to an `SdpVideoFormat` and create libwebrtc's codec from
`createVideoDecoderFactoryHaiku`/`createVideoEncoderFactoryHaiku`; audio is
`WebCore/platform/audio/haiku/WebCodecsAudioHaiku.cpp`
(`PlatformRawAudioDataHaiku`, Opus). Pictures
cross with `videoFrameFromWebRTCBuffer`/`webRTCBufferFromVideoFrame`
(`LibWebRTCHaiku.cpp`, no copy for I420). Decoded pictures keep their
chunk's timestamp through the RTP timestamp. `ENABLE_WEB_CODECS` is on in
`OptionsHaiku.cmake` and in the released engines' cache
(`engine/arm64/rpi4-gl/init-cache-rtc.cmake`, which forces every option);
`WebCodecsVideoEnabled` defaults to true on Haiku.

## Fixture

`tests/fixtures/webcodecs.html`: encodes six coloured canvas frames with
each codec, decodes them and checks the middle pixel, and sends a second of
tone through Opus at 48 and 44.1 kHz; the JSON result is in `#result`.
`tests/fixtures/webcodecs-audio-files.html` decodes AAC (ADTS, and raw with
a description) and MP3 files made with ffmpeg (the page says how).

Verified on the X399 (24 Oct, wc112 build): VP8, VP9, H.264 `avc` (with
its avcC description) and H.264 `annexb` each encoded six canvas frames
(one key frame), decoded six pictures with the chunks' timestamps, and
`AudioEncoder` stayed hidden. That build tagged decoded pictures BT.709,
so colours came back shifted (red as 255/26/1); with BT.601, as the follow-up
makes the default, every colour came back within 3 of the source on both
VP8 and H.264. The main build (90a6581) matched all six colours with every
codec.

Audio verified on the X399 (25 Oct, wa112 build): a second of 440 Hz stereo
tone went through Opus as 50 packets with an OpusHead description and came
back as 48 000 `f32` frames with an RMS of 0.352 (0.354 expected). `AudioData`
copies in its own format are exact; interleaved `s16` converted correctly
to `f32` and `s16` planes and to `u8`, with frame offsets. AAC reports
unsupported, and so does Opus at 44.1 kHz (libopus runs at 8–48 kHz rates
only; there is no resampling yet).

AAC, MP3 and 44.1 kHz Opus verified on the X399 (25 Oct, wf112 build):
ffmpeg-made 0.25 s tone files decoded to 13 312 AAC frames (both as ADTS
and as raw frames with an AudioSpecificConfig) and 13 824 MP3 frames, with
RMS 0.059 and 0.055, the same sample counts and levels ffmpeg's own decoder
gives; a second of 44.1 kHz tone went through Opus as 50 packets and came
back as 48 000 frames at 48 kHz with RMS 0.352.

AAC and MP3 encoding verified on the X399 (25 Oct, ac112 build, FFmpeg 8
libraries): a second of 440 Hz tone went through `AudioEncoder` and back
through `AudioDecoder` as AAC at 48 kHz with a description (48 chunks, RMS
0.349), AAC ADTS at 44.1 kHz (45 chunks, 0.345) and MP3 at 44.1 kHz (40
chunks, 0.329: LAME pads with silence); the tone's RMS is 0.354.

## W3C samples

The [WebCodecs samples](https://w3c.github.io/webcodecs/samples/) on the
X399 (25 Oct, ac112 build): *Video Decoding and Display* decoded all frames
of its 1280×720 MP4 as H.264 (295 fps), VP8 (349 fps) and VP9 (453 fps) and
drew them to the canvas. *Audio Video Player* needs cross-origin isolation:
from github.io (no COOP/COEP headers) it cannot run in any browser, and
served from `localhost` with the headers (`node server.js`, or a small
Python server) it stopped at "Loading…" because `SharedArrayBuffer` was
missing although the page was `crossOriginIsolated`. The engine turned on
JSC's `useSharedArrayBuffer` for cross-origin isolated web processes only
on GTK and WPE; Haiku now does too (`WebProcess.cpp`).

*Encode and Decode in a Worker* needs Chrome's `MediaStreamTrackProcessor`
and `MediaStreamTrackGenerator` in the window, which WebKit (Safari too)
does not have; WebKit's mediacapture-transform is worker-only
(`MediaStreamTrackProcessor` and `VideoTrackGenerator` in dedicated workers).
That was off outside Cocoa (`MediaStreamTrackProcessingEnabled`); it is on
for Haiku now, for video-effect pipelines that read camera frames in a
worker (backgrounds, #126). Test page: `tests/fixtures/track-processor.html`.

Verified on the X399 (25 Oct, sab112 build): the *Audio Video Player*
sample, served from `localhost` with COOP/COEP, plays Big Buck Bunny's H.264
video in step with its AAC audio (the picture advanced between screenshots
2 s apart); `tests/fixtures/track-processor.html` read 90 frames (320×240
BGRA, 28 fps) from a canvas track in a worker and played them back through
a `VideoTrackGenerator` (the X399 had no camera then); the fixture's
VP8/VP9/H.264 round trips still decoded exactly 6 frames each after
`flush()`.

PCM, FLAC and Vorbis verified on the X399 (25 Oct, pcm112 build): `pcm-u8`,
`-s16`, `-s24`, `-s32`, `-f32` (48 kHz) and `alaw`/`ulaw` (8 kHz) each took a
second of tone through `AudioEncoder` and back (100 chunks, RMS 0.353–0.355
against 0.354); a quarter second of FLAC (3 frames, 12000 samples, `s16`)
and Ogg Vorbis (14 packets, `f32-planar`) decoded without errors at the
same loudness as the AAC and MP3 files.

FLAC and Vorbis encoding verified on the X399 (25 Oct, fv112 build): a
second of tone at 48 kHz through `AudioEncoder` and back came out as 11 FLAC
chunks (48000 frames, `s16`, RMS 0.354: lossless at 16 bits) and 49 Vorbis
chunks (RMS 0.354), each decoder configured with the encoder's description.

AV1 and HEVC verified on the X399 (25 Oct, av112 build): the *Video
Decoding and Display* sample decoded its whole 1280×720 video as AV1
(`av01.0.05M.08`, 314 fps) and HEVC (`hvc1.1.6.L93.90`, 312 fps), the last
pictures drawn as H.264's were (about 390 colours in a 64×36 copy).

HE-AAC encoding verified on the X399 (25 Oct, he112 build, FFmpeg 8 with
FDK): a second of 48 kHz tone round-tripped as HE-AAC (26 chunks of 2048
samples, RMS 0.333), HE-AAC v2 (27 chunks, 0.326) and HE-AAC in ADTS (26
chunks, 0.333); the lower RMS is the encoder's priming delay.

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
60 or 62): the codec context is set up with AVOptions, and the two `AVFrame`
fields past the prefix the decoders use (`ch_layout`, `sample_rate`) are at
those versions' offsets, so other versions report unsupported. AAC gives raw
frames with an `AudioSpecificConfig` as `description`, or ADTS frames with
`aac: { format: "adts" }` (table rates, up to 7 channels); after `flush()`
the next frame starts a new encoder. HE-AAC encoding, FLAC, Vorbis and PCM
report unsupported.

Not yet: HE-AAC encoding, FLAC and Vorbis, AV1 and HEVC, hardware
decoders (NVDEC, Cedar) and flushing pictures a decoder holds back (WebRTC's
decoders return each picture with its chunk, so B-frame streams may lose
their last pictures).

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

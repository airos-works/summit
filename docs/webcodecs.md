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
`copyTo`) and Opus through libopus (`AudioEncoder` at 8–48 kHz, mono or
stereo, 2.5–60 ms packets, the OpusHead as `description`; `AudioDecoder`
giving `f32` samples). Other audio codecs (AAC, MP3, FLAC, Vorbis, PCM)
report unsupported.

Not yet: AAC and the other audio codecs, AV1 and HEVC, hardware
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
each codec, decodes them and checks the middle pixel; the JSON result is in
`#result`.

Verified on the X399 (24 Oct, wc112 build): VP8, VP9, H.264 `avc` (with
its avcC description) and H.264 `annexb` each encoded six canvas frames
(one key frame), decoded six pictures with the chunks' timestamps, and
`AudioEncoder` stayed hidden. That build tagged decoded pictures BT.709,
so colours came back shifted (red as 255/26/1); with BT.601, as the follow-up
makes the default, every colour came back within 3 of the source on both
VP8 and H.264. The main build (90a6581) matched all six colours with every
codec.

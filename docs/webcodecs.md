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

Not yet: the audio side (`AudioEncoder`, `AudioDecoder`, `AudioData` stay
off: `WebCodecsAudioEnabled` is false on Haiku), AV1 and HEVC, hardware
decoders (NVDEC, Cedar) and flushing pictures a decoder holds back (WebRTC's
decoders return each picture with its chunk, so B-frame streams may lose
their last pictures).

## How

`WebCore/platform/mediastream/libwebrtc/haiku/WebCodecsHaiku.cpp`:
`createLibWebRTCVideoDecoderHaiku`/`createLibWebRTCVideoEncoderHaiku` (from
`VideoDecoder::createLocalDecoder`/`VideoEncoder::createLocalEncoder`) map
the codec string to an `SdpVideoFormat` and create libwebrtc's codec from
`createVideoDecoderFactoryHaiku`/`createVideoEncoderFactoryHaiku`. Pictures
cross with `videoFrameFromWebRTCBuffer`/`webRTCBufferFromVideoFrame`
(`LibWebRTCHaiku.cpp`, no copy for I420). Decoded pictures keep their
chunk's timestamp through the RTP timestamp. `ENABLE_WEB_CODECS` is on in
`OptionsHaiku.cmake`; `WebCodecsVideoEnabled` defaults to true on Haiku.

## Fixture

`tests/fixtures/webcodecs.html`: encodes six coloured canvas frames with
each codec, decodes them and checks the middle pixel; the JSON result is in
`#result`.

# Permission prompt icons

The camera and microphone questions (#128) show these instead of the alert's
light bulb. A question for both the camera and the microphone shows the camera.

- `camera.hvif`: the webcam of CodyCam from the Haiku source tree
  (`src/apps/codycam/CodyCam.rdef`, MIT licence, Copyright Haiku, Inc.),
  copied byte for byte from its `vector_icon` resource.
- `microphone.hvif`: drawn in the same style by
  `python3 tools/make-prompt-icons.py` (Haiku's Sound Recorder icon is a tape
  recorder, not a microphone).

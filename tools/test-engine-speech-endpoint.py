#!/usr/bin/env python3
"""Run the engine's speech end-point tests (Summit #159) on this machine.

tests/EngineSpeechEndpointTests.cpp compiles the engine's own
Modules/speech/haiku/SpeechEndpointerHaiku.h (down-mix, end-of-speech detector,
pre-roll, WAV reader) with the host compiler and feeds it made-up audio. No
recogniser, microphone or Haiku is needed; ctest runs the same program as
summit_engine_speech_endpoint.
"""
import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=pathlib.Path, default=ROOT / '.cache/WebKit')
    parser.add_argument('--compiler', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    include = args.source / 'Source/WebCore/Modules/speech/haiku'
    if not (include / 'SpeechEndpointerHaiku.h').is_file():
        sys.exit(f'{include}/SpeechEndpointerHaiku.h is missing: run tools/prepare-webkit.py')
    with tempfile.TemporaryDirectory(prefix='summit-speech-') as scratch:
        program = pathlib.Path(scratch) / 'speech-endpoint-tests'
        subprocess.run([args.compiler, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Wpedantic', '-Werror', f'-I{include}',
                        str(ROOT / 'tests/EngineSpeechEndpointTests.cpp'), '-o', str(program)], check=True)
        result = subprocess.run([str(program)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(result.stdout, end='')
    return 0 if result.returncode == 0 and 'SPEECH_ENDPOINT_RESULT PASS' in result.stdout else 1


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Drive tests/fixtures/speech-recognition.html through WebDriver (summitdriver).

Runs on the machine that runs Summit (air/OS has python3), because the page has
to come from http://localhost: webkitSpeechRecognition exists in secure
contexts only. The script serves the page itself, starts summitdriver (which
starts Summit), starts a recognition, waits for its end and prints every event.

A WebDriver session's page gets the microphone without a prompt. With --wav the
"microphone" is a recording (SUMMIT_RECOGNITION_WAV, docs/speech-recognition.md),
so no microphone and nobody speaking are needed:

  python3 tools/test-speech-recognition.py --wav /boot/home/jfk.wav \\
      --expect "my fellow americans"
  python3 tools/test-speech-recognition.py --wav /boot/home/jfk.wav --continuous \\
      --stop-after 16 --expect "ask what you can do for your country"

Without a model installed: --expect-error service-not-allowed.
Exit status 0 when the expectations hold (without any: when the recognition
ended without an error).
"""
import argparse
import functools
import http.server
import json
import os
import pathlib
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


class Driver:
    def __init__(self, base):
        self.base = base.rstrip('/')

    def call(self, method, path, body=None, timeout=60):
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(self.base + path, data=data, method=method,
                                         headers={'Content-Type': 'application/json'})
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return json.loads(response.read().decode() or 'null')['value']
        except urllib.error.HTTPError as error:
            detail = error.read().decode(errors='replace')
            raise RuntimeError(f'{method} {path}: HTTP {error.code}: {detail[:400]}') from None

    def wait_until_ready(self, seconds):
        deadline = time.monotonic() + seconds
        while True:
            try:
                return self.call('GET', '/status', timeout=5)
            except (OSError, RuntimeError):
                if time.monotonic() > deadline:
                    raise RuntimeError('the driver does not answer at ' + self.base) from None
                time.sleep(0.5)


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def finish(result):
    print('SPEECH RECOGNITION TEST ' + ('PASS' if result['passed'] else 'FAIL') + ' ' + json.dumps(result))
    return 0 if result['passed'] else 1


def letters(text):
    return ''.join(c for c in text.lower() if c.isalnum())


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--driver', help='a running driver, such as http://127.0.0.1:4444 (default: start summitdriver)')
    parser.add_argument('--driver-program', default='summitdriver')
    parser.add_argument('--binary', help="the Summit to start (the session's summit:browserOptions binary)")
    parser.add_argument('--profile', help='its profile folder (--profile)')
    parser.add_argument('--page', help='the test page, when something else serves it at http://localhost')
    parser.add_argument('--wav', help='a WAV file in place of the microphone (when this script starts the driver)')
    parser.add_argument('--lang', default='en-US')
    parser.add_argument('--continuous', action='store_true')
    parser.add_argument('--no-interim', action='store_true')
    parser.add_argument('--stop-after', type=float, default=0, help='call stop() after this many seconds')
    parser.add_argument('--abort-after', type=float, default=0, help='call abort() after this many seconds')
    parser.add_argument('--close-after', type=float, default=0,
                        help='end the session (close the page) after this many seconds, in the middle of the recognition')
    parser.add_argument('--repeat', type=int, default=1, help='this many recognitions, one after the other, in the same page')
    parser.add_argument('--hold', type=float, default=0, help='keep the session open until this many seconds after the page was opened')
    parser.add_argument('--timeout', type=float, default=120, help='seconds to wait for the end of the recognition')
    parser.add_argument('--expect', action='append', default=[], help='text the final results must contain (letters and digits, case ignored)')
    parser.add_argument('--expect-error', help='the error the recognition must end with, such as service-not-allowed')
    parser.add_argument('--trace', action='store_true', help="SUMMIT_RECOGNITION_TRACE=1 for the browser (when this script starts the driver)")
    args = parser.parse_args()

    server = None
    driver_process = None
    session = None
    result = {'passed': False}
    try:
        if args.page:
            page = args.page
        else:
            handler = functools.partial(QuietHandler, directory=str(ROOT / 'tests/fixtures'))
            server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
            threading.Thread(target=server.serve_forever, daemon=True).start()
            page = f'http://localhost:{server.server_address[1]}/speech-recognition.html'

        if args.driver:
            driver = Driver(args.driver)
        else:
            port = free_port()
            environment = dict(os.environ)
            if args.wav:
                environment['SUMMIT_RECOGNITION_WAV'] = args.wav
            if args.trace:
                environment['SUMMIT_RECOGNITION_TRACE'] = '1'
            driver_process = subprocess.Popen([args.driver_program, f'--port={port}', '--host=127.0.0.1'], env=environment)
            driver = Driver(f'http://127.0.0.1:{port}')
        driver.wait_until_ready(30)

        options = {}
        if args.binary:
            options['binary'] = args.binary
        if args.profile:
            options['args'] = ['--profile', args.profile]
        capabilities = {'capabilities': {'alwaysMatch': {'summit:browserOptions': options} if options else {}}}
        session = driver.call('POST', '/session', capabilities, timeout=120)['sessionId']

        query = urllib.parse.urlencode({
            'auto': 1, 'lang': args.lang, 'continuous': int(args.continuous), 'interim': int(not args.no_interim),
            'stopAfter': args.stop_after})
        driver.call('POST', f'/session/{session}/url', {'url': page + '?' + query})

        def script(source):
            return driver.call('POST', f'/session/{session}/execute/sync', {'script': source, 'args': []})

        started = time.monotonic()
        deadline = started + args.timeout
        status = None
        aborted = False
        closed = False
        while time.monotonic() < deadline:
            status = script('return window.speechTest ? speechTest.status() : null;')
            if status and not status['supported']:
                break
            if status and status['ended'] >= args.repeat:
                break
            if status and 0 < status['ended'] < args.repeat and status['state'] == 'ended':
                # The next one, as a page that dictates starts them.
                script(f'speechTest.start({{ stopAfter: {args.stop_after} }});')
            if args.abort_after and not aborted and time.monotonic() - started >= args.abort_after:
                aborted = True
                script('speechTest.abort();')
            if args.close_after and time.monotonic() - started >= args.close_after:
                closed = True
                break
            time.sleep(0.5)
        if closed:
            # The page goes away while the recogniser works: the browser has
            # to take that (the session's end closes the window and quits it).
            print(f'closing the session {time.monotonic() - started:.1f} s into the recognition: {status}')
            driver.call('DELETE', f'/session/{session}', timeout=60)
            session = None
            result.update(passed=True, problems=[], closed=True, status=status)
            return finish(result)
        events = script('return window.speechTest ? speechTest.events : [];') or []
        if args.hold:
            time.sleep(max(0, args.hold - (time.monotonic() - started)))
        for event in events:
            details = {key: value for key, value in event.items() if key not in ('time', 'type')}
            print(f"{event['time']:8.2f}  {event['type']}" + (' ' + json.dumps(details) if details else ''))

        problems = []
        if not status:
            problems.append('the page did not load')
        elif not status['supported']:
            problems.append('the page has no webkitSpeechRecognition')
        elif status['ended'] < args.repeat:
            problems.append(f'{status["ended"]} of {args.repeat} recognitions ended in {args.timeout:.0f} s')
        if status and status.get('supported'):
            finals = ' '.join(status['finals'])
            errors = [error['error'] for error in status['errors']]
            for text in args.expect:
                if letters(text) not in letters(finals):
                    problems.append(f'the final text does not contain "{text}"')
            if args.expect_error:
                if args.expect_error not in errors:
                    problems.append(f'no {args.expect_error} error (errors: {errors})')
            elif errors:
                problems.append(f'errors: {status["errors"]}')
            result.update(finals=status['finals'], errors=status['errors'], types=status['types'])
        result.update(passed=not problems, problems=problems)
    except Exception as error:  # the driver is gone, the browser did not start, ...
        result.update(passed=False, problems=[str(error)])
    finally:
        if session:
            try:
                driver.call('DELETE', f'/session/{session}', timeout=30)
            except Exception:
                pass
        if driver_process:
            driver_process.terminate()
            try:
                driver_process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                driver_process.kill()
        if server:
            server.shutdown()
    return finish(result)


if __name__ == '__main__':
    sys.exit(main())

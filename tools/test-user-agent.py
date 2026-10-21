#!/usr/bin/env python3
"""Exercise #123 in a real native browser against an HTTP echo server.

Build tools/bench/summitctl.cpp on the target first. This creates a unique
profile, addresses only the team it launches, and leaves its log for diagnosis.
The host must be reachable from the guest (10.0.2.2 for tools/haiku.sh).
"""
import argparse
import http.server
import json
import pathlib
import re
import shlex
import subprocess
import threading
import time
import uuid
from urllib.parse import parse_qs, urlparse

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shell', default='tools/haiku.sh')
    parser.add_argument('--host', default='10.0.2.2')
    parser.add_argument('--browser', required=True)
    parser.add_argument('--control', required=True)
    parser.add_argument('--directory', default='/SummitExtensions/summit')
    args = parser.parse_args()
    received = {}
    condition = threading.Condition()

    class Echo(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            key = parse_qs(urlparse(self.path).query).get('key', [''])[0]
            if self.path.startswith('/page'):
                with condition:
                    received.setdefault(key, {})['document'] = self.headers.get('User-Agent')
                body = b'''<!doctype html><title>User agent test</title><h1>User agent test</h1>
<script>fetch('/report'+location.search,{method:'POST',body:JSON.stringify({agent:navigator.userAgent})})
.then(()=>document.title='Reported');</script>'''
            else:
                body = b''
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            key = parse_qs(urlparse(self.path).query)['key'][0]
            report = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            with condition:
                received.setdefault(key, {}).update(script=report['agent'], fetch=self.headers.get('User-Agent'))
                condition.notify_all()
            self.send_response(204)
            self.end_headers()

    server = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Echo)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://{args.host}:{server.server_port}/page?key='
    profile = f'{args.directory}/user-agent-test-{uuid.uuid4().hex[:12]}'
    team = None
    results = []

    def remote(command):
        return subprocess.check_output(['bash', args.shell, command], cwd=ROOT, text=True, timeout=60).strip()

    def control(*arguments):
        return remote(shlex.join([args.control, '--team', str(team), *arguments]))

    def launch(key):
        nonlocal team
        command = shlex.join([args.browser, '--profile', profile, base + key])
        team = int(remote(f'mkdir -p {shlex.quote(profile)}; {command} > {shlex.quote(profile + "/browser.log")} 2>&1 < /dev/null & echo $!'))

    def stop():
        nonlocal team
        control('quit')
        for _ in range(30):
            if remote(f'kill -0 {team} 2>/dev/null && echo alive || echo exited') == 'exited':
                team = None
                return
            time.sleep(0.3)
        raise RuntimeError(f'Test browser {team} did not quit')

    def check(key, expected=None):
        with condition:
            if not condition.wait_for(lambda: 'script' in received.get(key, {}), timeout=45):
                raise RuntimeError(f'No report for {key}; inspect {profile}/browser.log (team {team})')
            report = received[key].copy()
        assert report['document'] == report['script'] == report['fetch'], report
        if expected is not None:
            assert report['document'] == expected, (key, report, expected)
        results.append({'case': key, **report})
        print(f'PASS {key}', flush=True)
        return report['document']

    # The exact shipped strings are the expectations for the runtime boundary:
    # setting -> profile -> all live views -> HTTP headers and navigator.
    presets = re.findall(r'\{"([^"\n]+)", "[^"\n]+", "([^"\n]*)"\}',
                         (ROOT / 'src/core/UserAgent.cpp').read_text())
    assert len(presets) == 9
    try:
        launch('initial')
        default = check('initial')
        for preset, agent in presets:
            control('useragent', preset)
            control('navigate', base + preset)
            check(preset, agent or default)
        expected = dict(presets)['edge-macos']
        for action in ('newtab', 'newwindow', 'newprivatewindow'):
            control(action, base + action)
            check(action, expected)
        stop()
        launch('restart')
        check('restart', expected)
        control('useragent', 'not-a-preset')
        control('navigate', base + 'invalid')
        check('invalid', default)
        control('useragent', 'firefox-windows')
        control('navigate', base + 'changed')
        check('changed', dict(presets)['firefox-windows'])
        control('useragent', 'default')
        control('navigate', base + 'reset')
        check('reset', default)
        stop()
    finally:
        if team is not None:
            try:
                control('quit')
            except (subprocess.SubprocessError, OSError):
                pass
        server.shutdown()
        print(json.dumps({'profile': profile, 'results': results}, indent=2))


if __name__ == '__main__':
    main()

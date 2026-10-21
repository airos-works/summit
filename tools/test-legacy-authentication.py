#!/usr/bin/env python3
"""Exercise installed WebPositive's HTTP authentication UI in an owned Haiku VM.

Uses normal WebPositive settings, an isolated shim and loopback-only test
credentials. Keep the desktop unobstructed. No installed packages are changed.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse
from urllib.request import parse_http_list, parse_keqv_list

from native_crash_log import NativeCrashLog

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--browser', type=Path, default=Path('/boot/system/apps/WebPositive'))
parser.add_argument('--library', type=Path, required=True)
parser.add_argument('--ui-helper', type=Path, required=True)
parser.add_argument('--images-helper', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--expect-unavailable', action='store_true')
parser.add_argument('--lifecycle-probe', type=Path,
                    help='Native BWebView listener probe, instead of the WebPositive UI cases')
a = parser.parse_args()
if not sys.platform.startswith('haiku'):
    parser.error('Requires native Haiku')
for name in ('browser', 'library', 'ui_helper', 'images_helper'):
    setattr(a, name, getattr(a, name).resolve(strict=True))
if a.lifecycle_probe:
    a.lifecycle_probe = a.lifecycle_probe.resolve(strict=True)
    assert not a.expect_unavailable
a.output.mkdir(mode=0o700)
assert subprocess.run(['hey', 'WebPositive', 'get', 'Name'],
                      capture_output=True).returncode != 0, 'WebPositive already running'


def sha(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def members(group):
    lines = subprocess.check_output(['ps', '-o', 'Id'], text=True).splitlines()
    assert lines[0].strip() == 'Id'
    result = []
    for line in lines[1:]:
        pid = int(line.strip())
        try:
            if os.getpgid(pid) == group:
                result.append(pid)
        except ProcessLookupError:
            pass
    return result


# Fixed, public fixture credentials. Never log Authorization headers.
USER, PASSWORD = 'legacy-test-user', 'legacy-test-password'
events = []
lock = threading.Lock()
nonce = os.urandom(16).hex()


def record(value):
    with lock:
        events.append(value)


def count(value):
    with lock:
        return events.count(value)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == '/loaded':
            record(parse_qs(parsed.query)['case'][0] + ':loaded')
            self.send_response(200)
            self.end_headers()
            return
        kind = parsed.path.strip('/')
        if kind not in ('basic', 'digest', 'cancel', 'empty', 'hold', 'fresh', 'destroy', 'after'):
            self.send_response(204)
            self.end_headers()
            return
        realm = 'legacy-' + kind + '-' + str(self.server.server_port)
        password = PASSWORD + '-' + kind
        authorization = self.headers.get('Authorization', '')
        accepted = kind == 'after'
        if kind == 'digest' and authorization.startswith('Digest '):
            fields = parse_keqv_list(parse_http_list(authorization[7:]))
            md5 = lambda value: hashlib.md5(value.encode()).hexdigest()
            ha1 = md5(f'{USER}:{realm}:{password}')
            ha2 = md5('GET:' + self.path)
            expected = md5(f"{ha1}:{nonce}:{fields.get('nc')}:{fields.get('cnonce')}:auth:{ha2}")
            accepted = (fields.get('username') == USER and fields.get('realm') == realm
                        and fields.get('nonce') == nonce and fields.get('uri') == self.path
                        and fields.get('qop') == 'auth' and fields.get('response') == expected)
        elif kind not in ('digest', 'after'):
            expected = 'Basic ' + base64.b64encode(f'{USER}:{password}'.encode()).decode()
            accepted = authorization == expected
        record(kind + (':accepted' if accepted else ':wrong' if authorization else ':challenge'))
        outcome = kind if accepted else kind + '-denied'
        body = (f'<!doctype html><title>Legacy child auth {outcome}</title>'
                '<style>html,body{margin:0;width:100%;height:100%;background:rgb(66,140,222)}</style>'
                f'<script>fetch("/loaded?case={outcome}")</script>').encode()
        self.send_response(200 if accepted else 401)
        if not accepted:
            challenge = (f'Digest realm="{realm}", nonce="{nonce}", algorithm=MD5, qop="auth"'
                         if kind == 'digest' else f'Basic realm="{realm}"')
            self.send_header('WWW-Authenticate', challenge)
        self.send_header('Content-Type', 'text/html')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
server.daemon_threads = True
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = f'http://127.0.0.1:{server.server_port}'
report = {'platform': subprocess.check_output(['uname', '-a'], text=True).strip(),
          'passed': False, 'checks': [], 'events': events, 'forced_cleanup': False,
          'loaded_images': {}, 'expect_unavailable': a.expect_unavailable,
          'lifecycle_probe': str(a.lifecycle_probe) if a.lifecycle_probe else None}
inputs = [a.browser, a.library, a.ui_helper, a.images_helper, Path(__file__),
          ROOT / 'tools/native_crash_log.py', ROOT / 'tests/LegacyWebPositiveUITests.cpp']
if a.lifecycle_probe:
    inputs += [a.lifecycle_probe, ROOT / 'tests/LegacyAuthenticationTests.cpp',
               ROOT / 'tests/ModernPDFTests.cpp', ROOT / 'vendor/nlohmann/json.hpp']
report['sha256'] = {str(path): sha(path) for path in inputs}
monitor = NativeCrashLog()
proc = None


def check(condition, label):
    report['checks'].append({'label': label, 'passed': bool(condition)})
    if not condition:
        raise AssertionError(label)


def wait(predicate, label):
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline:
        if predicate():
            check(True, label)
            return
        if proc.poll() is not None:
            raise RuntimeError('WebPositive exited early')
        time.sleep(0.15)
    check(False, label)


def hey(*args, required=True):
    result = subprocess.run(['hey', str(proc.pid), *args], capture_output=True,
                            text=True, timeout=5)
    if required and result.returncode:
        raise RuntimeError('Native scripting failed: ' + result.stdout + result.stderr)
    return result


def panel():
    result = hey('get', 'Messenger', 'of', 'Window', 'Authentication required', required=False)
    return result.stdout if result.returncode == 0 else None


def submit(user, password):
    for name, value in (('user', user), ('pass', password)):
        hey('set', 'Value', 'of', 'View', name, 'of', 'Window', 'Authentication required',
            'to', '"' + value + '"')
    # The installed panel's OK button sends 'pnok' to its own window.
    hey('pnok', 'Window', 'Authentication required')


def navigate(kind):
    hey('_ARG', 'with', 'argc=int32(2)', 'and', 'argv=' + str(a.browser),
        'and', 'argv=' + base + '/' + kind)


def capture_runtime():
    images = {}
    for team in members(proc.pid):
        result = subprocess.run([str(a.images_helper), str(team)], capture_output=True, text=True)
        if result.returncode == 0:
            images[str(team)] = result.stdout.splitlines()
            for path in images[str(team)]:
                if path != 'commpage' and path not in report['sha256']:
                    report['sha256'][path] = sha(path)
    return images


try:
    bundle = '/boot/system/lib/summit-webkit'
    env = dict(os.environ, WEBKIT_EXEC_PATH=bundle,
               LIBRARY_PATH=f'{a.library.parent}:{bundle}/lib:{bundle}/mesa/lib:/boot/system/lib',
               __EGL_VENDOR_LIBRARY_FILENAMES=bundle + '/mesa/egl_vendor.d/50_mesa.json')
    env.pop('LD_PRELOAD', None)
    with (a.output / 'native.log').open('w') as log:
        command = ([str(a.lifecycle_probe), base, str(a.output / 'profile'), bundle,
                    str(a.output / 'begin')] if a.lifecycle_probe
                   else [str(a.browser), base + '/basic'])
        proc = subprocess.Popen(command, env=env, stdout=log,
                                stderr=subprocess.STDOUT, start_new_session=True)
        report['team'] = proc.pid
        if a.lifecycle_probe:
            wait(lambda: 'LEGACY_AUTH_READY' in (a.output / 'native.log').read_text(),
                 'native lifecycle probe reaches first held challenge')
            report['initial_loaded_images'] = capture_runtime()
            (a.output / 'begin').touch()
            report['exit'] = proc.wait(timeout=90)
            check(report['exit'] == 0 and 'LEGACY_AUTH_RESULT PASS' in (a.output / 'native.log').read_text(),
                  'native authentication lifecycle checks pass')
            check(not count('hold:accepted') and not count('destroy:accepted'),
                  'abandoned and destroyed requests never authenticate')
            check(count('fresh:loaded'), 'new request accepts its own credential reply')
        elif a.expect_unavailable:
            wait(lambda: count('basic-denied:loaded'), 'baseline loads the unauthenticated 401 page')
            check(not panel(), 'baseline has no authentication prompt')
        else:
            wait(panel, 'installed WebPositive presents its authentication panel')
            report['initial_loaded_images'] = capture_runtime()
            check(hey('count', 'Window').returncode == 0,
                  'application looper responds while the authentication panel waits')
            text = hey('get', 'Text', '[0', 'to', '4096]', 'of', 'View', 'text',
                       'of', 'Window', 'Authentication required').stdout
            check(base in text and 'legacy-basic-' in text and 'basic' in text,
                  'prompt identifies origin including port, realm and scheme')
            previous = panel()
            submit(USER, 'wrong-fixture-password')
            wait(lambda: count('basic:wrong'), 'server rejects the wrong Basic password')
            wait(lambda: panel() and panel() != previous, 'wrong password produces a fresh panel')
            submit(USER, PASSWORD + '-basic')
            wait(lambda: count('basic:loaded'), 'correct Basic credentials load the page')
            wait(lambda: not panel(), 'successful Basic prompt closes')
            navigate('digest')
            wait(panel, 'Digest challenge opens the existing WebPositive panel')
            submit(USER, PASSWORD + '-digest')
            wait(lambda: count('digest:loaded'), 'Digest credentials load the page')
            wait(lambda: not panel(), 'successful Digest prompt closes')
            navigate('cancel')
            wait(panel, 'new realm opens another prompt')
            hey('quit', 'Window', 'Authentication required')
            wait(lambda: not panel(), 'Cancel closes the authentication panel')
            check(not count('cancel:accepted'), 'Cancel sends no accepted credentials')
            navigate('empty')
            wait(panel, 'empty-credential case opens a prompt')
            submit('', '')
            wait(lambda: count('empty-denied:loaded'), 'empty credentials continue to the 401 page')
            before = count('basic:loaded')
            navigate('basic')
            wait(lambda: count('basic:loaded') > before, 'same realm signs in again within the session')
            check(not panel(), 'session reuse needs no new panel')
        report['loaded_images'] = (report['initial_loaded_images'] if a.lifecycle_probe
                                   else capture_runtime())
        check(any(path.endswith(('/libWebKitLegacy.so.1', '/libWebKitLegacy.so.1.10.0'))
                  and report['sha256'].get(path) == report['sha256'][str(a.library)]
                  for paths in report['loaded_images'].values() for path in paths),
              'installed WebPositive loads the selected shim')
        if not a.lifecycle_probe:
            subprocess.run([str(a.ui_helper), str(proc.pid), 'quit'], check=True, timeout=10)
            report['exit'] = proc.wait(timeout=25)
    deadline = time.monotonic() + 15
    while members(proc.pid) and time.monotonic() < deadline:
        time.sleep(0.1)
    report['remaining_processes'] = members(proc.pid)
    check(report['exit'] == 0 and not report['remaining_processes'], 'browser and helper teams exit normally')
    report['passed'] = True
except Exception as error:
    report['error'] = repr(error)
    if proc and proc.poll() is None:
        subprocess.run([str(a.ui_helper), str(proc.pid), 'capture', str(a.output / 'failure.ppm')],
                       capture_output=True, timeout=15)
finally:
    if proc and (proc.poll() is None or members(proc.pid)):
        report['forced_cleanup'] = True
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait(timeout=10)
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)
    report['native_crash_log'] = monitor.finish()
    report['inputs_unchanged'] = all(sha(path) == digest for path, digest in report['sha256'].items())
    report['passed'] = (report['passed'] and report['inputs_unchanged']
                        and report['native_crash_log']['passed'])
    (a.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: report.get(key) for key in ('passed', 'exit', 'forced_cleanup', 'error')}))
sys.exit(0 if report['passed'] else 1)

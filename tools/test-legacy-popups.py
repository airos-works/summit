#!/usr/bin/env python3
"""Check real WebPositive popup behavior on an owned native test VM.

Uses WebPositive's normal settings directory. No shim package is installed:
--library is selected only for the owned process through LIBRARY_PATH.
Compile tests/LegacyWebPositiveUITests.cpp against the target Haiku SDK.
--expect-fallback verifies the former default's missing opener before a fix.
Keep the test desktop unobstructed: the rendering check samples screen pixels.
"""
import argparse
import hashlib
import json
import os
import pathlib
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tools'))
from native_crash_log import NativeCrashLog

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--browser', type=pathlib.Path,
               default=pathlib.Path('/boot/system/apps/WebPositive'))
p.add_argument('--library', type=pathlib.Path, required=True)
p.add_argument('--ui-helper', type=pathlib.Path, required=True)
p.add_argument('--images-helper', type=pathlib.Path, required=True)
p.add_argument('--output', type=pathlib.Path, required=True)
p.add_argument('--expect-fallback', action='store_true')
a = p.parse_args()
if not sys.platform.startswith('haiku'):
    p.error('Requires native Haiku')
a.output.mkdir(mode=0o700)
for name in ('browser', 'library', 'ui_helper', 'images_helper'):
    setattr(a, name, getattr(a, name).resolve(strict=True))
assert subprocess.run(['hey', 'WebPositive', 'get', 'Name'],
                      capture_output=True).returncode != 0, 'WebPositive is already running'

def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()


def members(group):
    result = []
    lines = subprocess.check_output(['ps', '-o', 'Id'], text=True).splitlines()
    if not lines or lines[0].strip() != 'Id':
        raise RuntimeError('Cannot enumerate native process groups')
    for line in lines[1:]:
        pid = int(line.strip())
        try:
            if os.getpgid(pid) == group:
                result.append(pid)
        except ProcessLookupError:
            pass
    return result


fixture = root / 'tests/fixtures/legacy-popups.html'
cases = ('popup', 'tab', 'blank')
events = []
lock = threading.Lock()
allow_close = {kind: threading.Event() for kind in cases}


class Handler(BaseHTTPRequestHandler):

    def log_message(self, *_):
        pass

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == '/event':
            with lock:
                events.append(parse_qs(parsed.query).get('value', [''])[0])
            data = b'ok'
            mime = 'text/plain'
        elif parsed.path == '/allow-close':
            kind = parse_qs(parsed.query)['case'][0]
            with lock:
                events.append(kind + ':server-await-close')
            released = allow_close[kind].wait(45)
            with lock:
                events.append(kind + ':server-close-released-' + str(released))
            data = b'ok'
            mime = 'text/plain'
        else:
            data = fixture.read_bytes()
            mime = 'text/html'
        self.send_response(200)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
server.daemon_threads = True
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
report = {
    'platform': subprocess.check_output(['uname', '-a'], text=True).strip(),
    'passed': False, 'checks': [], 'events': events, 'forced_cleanup': False,
    'loaded_images': {}, 'expect_fallback': a.expect_fallback, 'cases': {},
}
paths = [a.browser, a.library, a.ui_helper, a.images_helper, pathlib.Path(__file__),
         root / 'tools/native_crash_log.py', root / 'tests/LegacyWebPositiveUITests.cpp',
         fixture]
report['sha256'] = {str(p): sha(p) for p in paths}
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
        time.sleep(0.1)
    check(False, label)


def event(value):
    with lock:
        return value in events


def ui(action):
    result = subprocess.run([str(a.ui_helper), str(proc.pid), action],
                            capture_output=True, text=True, timeout=20)
    check(result.returncode == 0, 'native WebPositive UI ' + action)
    return json.loads(result.stdout) if action != 'quit' else None


def capture_runtime():
    loaded = {}
    for team in members(proc.pid):
        images = subprocess.run([str(a.images_helper), str(team)],
                                capture_output=True, text=True)
        if images.returncode == 0:
            loaded[str(team)] = images.stdout.splitlines()
            for path in images.stdout.splitlines():
                if path != 'commpage' and path not in report['sha256']:
                    report['sha256'][path] = sha(path)
    return loaded


try:
    bundle = '/boot/system/lib/summit-webkit'
    env = dict(os.environ, WEBKIT_EXEC_PATH=bundle,
               LIBRARY_PATH=f'{a.library.parent}:{bundle}/lib:{bundle}/mesa/lib:/boot/system/lib',
               __EGL_VENDOR_LIBRARY_FILENAMES=bundle + '/mesa/egl_vendor.d/50_mesa.json',
               SUMMIT_ENABLE_INPUT_SYNTHESIS='1', SUMMIT_LEGACY_TRACE='1')
    env.pop('LD_PRELOAD', None)
    env.pop('SUMMIT_LEGACY_OPENED_PAGES', None)
    with (a.output / 'native.log').open('w') as log:
        proc = subprocess.Popen([str(a.browser), f'http://127.0.0.1:{server.server_port}/'],
                                env=env, stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=True)
        report['team'] = proc.pid
        wait(lambda: event('popup:ready-to-open'), 'WebPositive loads controlled fixture')
        report['initial_loaded_images'] = capture_runtime()
        report['loaded_images'] = report['initial_loaded_images']
        check(any(path.endswith(('/libWebKitLegacy.so.1', '/libWebKitLegacy.so.1.10.0'))
                  for paths in report['loaded_images'].values() for path in paths
                  if report['sha256'].get(path) == report['sha256'][str(a.library)]),
              'WebPositive loads the selected shim library')
        for kind in cases[:1] if a.expect_fallback else cases:
            item = report['cases'][kind] = {}
            wait(lambda: event(kind + ':ready-to-open'), kind + ': main page ready for native click')
            item['before'] = ui('click')
            wait(lambda: event(kind + (':returned-null' if a.expect_fallback else ':returned-window')),
                 kind + ': window.open return value')
            wait(lambda: event(kind + (':opener-false' if a.expect_fallback else ':opener-true')),
                 kind + ': child opener relationship')
            if not a.expect_fallback:
                wait(lambda: event(kind + ':child-ready') and event(kind + ':pong'),
                     kind + ': opener and child exchange messages')

            def rendered():
                windows = ui('inspect')
                item['opened'] = windows
                return any(w.get('centerRGB') == [66, 140, 222] for w in windows)
            wait(rendered, kind + ': actual WebPositive content renders on screen')
            if a.expect_fallback:
                continue
            allow_close[kind].set()
            wait(lambda: event(kind + ':closed'), kind + ': opener observes script-closed popup')

            def child_gone():
                windows = ui('inspect')
                item['after'] = windows
                return not any('Legacy child' in w['title'] for w in windows)
            wait(child_gone, kind + ': WebPositive removes closed tab or window')
        report['loaded_images'] = capture_runtime()
        ui('quit')
        report['exit'] = proc.wait(timeout=25)
    deadline = time.monotonic() + 15
    while members(proc.pid) and time.monotonic() < deadline:
        time.sleep(0.1)
    report['remaining_processes'] = members(proc.pid)
    check(report['exit'] == 0 and not report['remaining_processes'],
          'WebPositive and helpers exit normally')
    report['passed'] = True
except Exception as e:
    report['error'] = repr(e)
    if proc and proc.poll() is None:
        capture = subprocess.run([str(a.ui_helper), str(proc.pid), 'capture',
                                  str(a.output / 'failure.ppm')],
                                 capture_output=True, timeout=15)
        report['screenshot_exit'] = capture.returncode
finally:
    for ready in allow_close.values():
        ready.set()
    if proc and (proc.poll() is None or members(proc.pid)):
        report['forced_cleanup'] = True
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait(timeout=10)
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)
    report['native_crash_log'] = monitor.finish()
    report['inputs_unchanged'] = all(sha(p) == s for p, s in report['sha256'].items())
    report['passed'] = (report['passed'] and report['inputs_unchanged']
                        and report['native_crash_log']['passed'])
    (a.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({k: report.get(k) for k in ('passed', 'exit', 'forced_cleanup', 'error')}))
sys.exit(0 if report['passed'] else 1)

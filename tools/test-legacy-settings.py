#!/usr/bin/env python3
"""Exercise legacy settings in an isolated native host with loopback HTTP."""
import argparse
import hashlib
import html
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
from native_crash_log import NativeCrashLog

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
for name in ('probe', 'library', 'bundle', 'images-helper', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
parser.add_argument('--expect-unavailable', action='store_true')
a = parser.parse_args()
if not sys.platform.startswith('haiku'):
    parser.error('Requires native Haiku')
for name in ('probe', 'library', 'bundle', 'images_helper'):
    setattr(a, name, getattr(a, name).resolve(strict=True))
a.output.mkdir(mode=0o700)


def sha(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def members(group):
    result = []
    for line in subprocess.check_output(['ps', '-o', 'Id'], text=True).splitlines()[1:]:
        pid = int(line.strip())
        try:
            if os.getpgid(pid) == group:
                result.append(pid)
        except ProcessLookupError:
            pass
    return result


events, lock = [], threading.Lock()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        url = urlparse(self.path)
        with lock:
            events.append({'path': url.path, 'query': parse_qs(url.query),
                           'user_agent': self.headers.get('User-Agent')})
        if url.path in ('/ran', '/clicked', '/favicon.ico'):
            self.send_response(204)
            self.end_headers()
            return
        body = ('''<!doctype html><meta charset="utf-8"><title>Settings fixture '''
                + html.escape(url.path) + '''</title>
<p><span id="standard">iiiiiiiiWWWWWWWW</span></p>
<p><span id="serif" style="font-family:serif">iiiiiiiiWWWWWWWW</span></p>
<p><span id="sans" style="font-family:sans-serif">iiiiiiiiWWWWWWWW</span></p>
<p><span id="fixed" style="font-family:monospace">iiiiiiiiWWWWWWWW</span></p>
<button id="click" onclick="document.documentElement.dataset.clicks=String(Number(document.documentElement.dataset.clicks||0)+1);fetch('/clicked?case='+encodeURIComponent(location.pathname))">Click</button>
<button id="popup" onclick="window.open('/popup','settings_popup','width=420,height=300')">Popup</button>
<script>document.documentElement.dataset.ran='yes';fetch('/ran?case='+encodeURIComponent(location.pathname));</script>''').encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)


server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = f'http://127.0.0.1:{server.server_port}'
report = {'passed': False, 'forced_cleanup': False, 'expect_unavailable': a.expect_unavailable,
          'checks': [], 'events': events, 'bundle': str(a.bundle)}
inputs = [a.probe, a.library, a.images_helper, Path(__file__), ROOT / 'tools/native_crash_log.py',
          ROOT / 'tests/LegacySettingsTests.cpp', ROOT / 'tests/ModernPDFTests.cpp',
          ROOT / 'vendor/nlohmann/json.hpp']
inputs += [a.bundle / name for name in ('WebProcess', 'NetworkProcess',
                                       'lib/libWebKit.so.1', 'lib/libJavaScriptCore.so.18')]
report['sha256'] = {str(path): sha(path) for path in inputs}
monitor = NativeCrashLog()
proc = None


def check(value, label):
    report['checks'].append({'passed': bool(value), 'label': label})
    if not value:
        raise AssertionError(label)


def inventory():
    images = {}
    for pid in members(proc.pid):
        result = subprocess.run([str(a.images_helper), str(pid)], capture_output=True, text=True)
        if result.returncode == 0:
            images[str(pid)] = result.stdout.splitlines()
            for path in images[str(pid)]:
                if path != 'commpage' and path not in report['sha256']:
                    report['sha256'][path] = sha(path)
    return images


try:
    env = dict(os.environ, WEBKIT_EXEC_PATH=str(a.bundle),
               LIBRARY_PATH=f'{a.library.parent}:{a.bundle}/lib:{a.bundle}/mesa/lib:/boot/system/lib',
               __EGL_VENDOR_LIBRARY_FILENAMES=str(a.bundle / 'mesa/egl_vendor.d/50_mesa.json'))
    env.pop('LD_PRELOAD', None)
    log_path = a.output / 'native.log'
    with log_path.open('w') as log:
        proc = subprocess.Popen([str(a.probe), base, str(a.output / 'profile'), str(a.bundle),
                                 str(a.output / 'begin')], env=env, stdout=log,
                                stderr=subprocess.STDOUT, start_new_session=True)
        report['team'] = proc.pid
        deadline = time.monotonic() + 90
        while 'LEGACY_SETTINGS_READY' not in log_path.read_text():
            if proc.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('Probe did not reach its initial documents')
            time.sleep(0.1)
        report['loaded_images'] = inventory()
        for suffix, selected in (('/libWebKit.so.1', a.bundle / 'lib/libWebKit.so.1'),
                                 ('/libWebKitLegacy.so.1', a.library)):
            check(any((path.endswith(suffix) or path.endswith(suffix + '.10.0'))
                      and report['sha256'][path] == report['sha256'][str(selected)]
                      for paths in report['loaded_images'].values() for path in paths),
                  'native host loads selected ' + suffix.rsplit('/', 1)[1])
        (a.output / 'begin').touch()
        report['exit'] = proc.wait(timeout=240)
    check(report['exit'] == 0 and 'LEGACY_SETTINGS_RESULT PASS' in log_path.read_text(),
          'native settings checks pass')
    ran = [e['query'].get('case', [None])[0] for e in events if e['path'] == '/ran']
    if a.expect_unavailable:
        check('/baseline' in ran, 'baseline page script runs despite the disabled setting')
    else:
        check(all(path not in ran for path in ('/a1', '/b2', '/c1', '/popup')),
              'disabled documents never execute their parser scripts')
        check(all(path in ran for path in ('/b1', '/a2', '/reset')),
              'enabled documents execute their parser scripts')
        expected = {'/a1': 'Summit-Settings-Page-A', '/b1': 'Summit-Settings-Global-1',
                    '/a2': 'Summit-Settings-Page-A', '/b2': 'Summit-Settings-Global-2',
                    '/c1': 'Summit-Settings-Global-2', '/popup': 'Summit-Settings-Global-2'}
        for path, agent in expected.items():
            received = [e for e in events if e['path'] == path]
            check(received and all(e['user_agent'] == agent for e in received),
                  'HTTP user agent matches committed settings for ' + path)
        check(any(e['path'] == '/reset' and 'Summit-Settings-' not in e['user_agent'] for e in events),
              'cleared page override sends the engine user agent')
        clicked = [e['query'].get('case', [None])[0] for e in events if e['path'] == '/clicked']
        check(clicked == ['/a2'], 'native input executes only the enabled page handler')
    deadline = time.monotonic() + 15
    while members(proc.pid) and time.monotonic() < deadline:
        time.sleep(0.1)
    report['remaining_processes'] = members(proc.pid)
    check(not report['remaining_processes'], 'native host and helper teams exit normally')
    report['passed'] = True
except Exception as error:
    report['error'] = repr(error)
    if proc:
        report['exit'] = proc.poll()
finally:
    if proc and (proc.poll() is None or members(proc.pid)):
        report['forced_cleanup'] = True
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait(timeout=10)
    server.shutdown(); server.server_close(); thread.join(timeout=5)
    report['native_crash_log'] = monitor.finish()
    report['inputs_unchanged'] = all(sha(path) == digest for path, digest in report['sha256'].items())
    report['passed'] = report['passed'] and report['inputs_unchanged'] and report['native_crash_log']['passed']
    (a.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: report.get(key) for key in ('passed', 'exit', 'forced_cleanup', 'error')}))
sys.exit(0 if report['passed'] else 1)

#!/usr/bin/env python3
"""Verify native user-agent behavior through a loopback-only HTTP proxy.

The proxy answers every request itself; it never forwards traffic to the named
sites. Real hostnames exercise URL matching without changing DNS or hosts files.
"""
import argparse
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
from urllib.parse import urlparse, parse_qs
from native_crash_log import NativeCrashLog

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
for name in ('probe', 'bundle', 'images-helper', 'egl-vendor', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
parser.add_argument('--baseline', action='store_true')
a = parser.parse_args()
if not sys.platform.startswith('haiku'):
    parser.error('Requires native Haiku')
for name in ('probe', 'bundle', 'images_helper', 'egl_vendor'):
    setattr(a, name, getattr(a, name).resolve(strict=True))
a.output.mkdir(mode=0o700)


def sha(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def members(group):
    lines = subprocess.check_output(['ps', '-o', 'Id'], text=True).splitlines()
    if not lines or lines[0].strip() != 'Id':
        raise RuntimeError('Cannot enumerate native process groups')
    result = []
    for line in lines[1:]:
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
        event = {'url': self.path, 'host': self.headers.get('Host'),
                 'path': url.path, 'query': parse_qs(url.query),
                 'user_agent': self.headers.get('User-Agent')}
        with lock:
            events.append(event)
        if url.path == '/echo':
            body = json.dumps(event).encode()
            mime = 'application/json'
        else:
            body = ('<!doctype html><meta charset="utf-8"><title>User agent fixture</title>'
                    '<p>Local user agent fixture</p><script>window.requestUserAgent='
                    + json.dumps(event['user_agent']).replace('<', '\\u003c')
                    + ';</script>').encode()
            mime = 'text/html; charset=utf-8'
        self.send_response(200)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)


server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
report = {'passed': False, 'forced_cleanup': False, 'baseline': a.baseline,
          'platform': subprocess.check_output(['uname', '-a'], text=True).strip(),
          'checks': [], 'events': events, 'bundle': str(a.bundle)}
inputs = [a.probe, a.images_helper, a.egl_vendor, Path(__file__), ROOT / 'tools/native_crash_log.py',
          ROOT / 'tests/ModernUserAgentTests.cpp', ROOT / 'tests/ModernPDFTests.cpp',
          ROOT / 'vendor/nlohmann/json.hpp']
selected = [a.bundle / name for name in ('WebProcess', 'NetworkProcess',
                                        'lib/libWebKit.so.1', 'lib/libJavaScriptCore.so.18')]
report['sha256'] = {str(path): sha(path) for path in inputs + selected}
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
        if result.returncode:
            raise RuntimeError('Cannot inspect live team ' + str(pid))
        images[str(pid)] = result.stdout.splitlines()
        for path in images[str(pid)]:
            if path != 'commpage' and path not in report['sha256']:
                report['sha256'][path] = sha(path)
    return images


try:
    env = dict(os.environ, WEBKIT_EXEC_PATH=str(a.bundle),
               LIBRARY_PATH=f'{a.bundle}/lib:{a.bundle}/mesa/lib:/boot/system/lib',
               __EGL_VENDOR_LIBRARY_FILENAMES=str(a.egl_vendor))
    for name in list(env):
        if name.lower().endswith('_proxy') or name in ('LD_PRELOAD', 'JSC_usePollingTraps'):
            env.pop(name)
    env.update(http_proxy=f'http://127.0.0.1:{server.server_port}', no_proxy='')
    log_path = a.output / 'native.log'
    with log_path.open('w') as log:
        proc = subprocess.Popen([str(a.probe), str(a.output / 'profile'), str(a.bundle),
                                 str(a.output / 'begin'), 'baseline' if a.baseline else 'candidate'],
                                env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        report['team'] = proc.pid
        deadline = time.monotonic() + 90
        while 'USER_AGENT_READY' not in log_path.read_text():
            if proc.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('Probe did not reach its initial document')
            time.sleep(0.1)
        report['loaded_images'] = inventory()
        loaded = [path for paths in report['loaded_images'].values() for path in paths if path != 'commpage']
        for selected_path in selected:
            check(any(report['sha256'][path] == report['sha256'][str(selected_path)] for path in loaded),
                  'native probe loads selected ' + selected_path.name)
        (a.output / 'begin').touch()
        report['exit'] = proc.wait(timeout=300)
    native_log = log_path.read_text()
    check(report['exit'] == 0 and 'USER_AGENT_RESULT PASS' in native_log, 'native user-agent checks pass')
    report['cases'] = [json.loads(line.removeprefix('USER_AGENT_CASE '))
                       for line in native_log.splitlines() if line.startswith('USER_AGENT_CASE ')]
    check(len(report['cases']) == 24, 'all navigation and live-update cases ran')
    for case in report['cases']:
        if case['navigation']:
            received = [e for e in events if e['url'] == case['url']]
            check(received and all(e['user_agent'] == case['expected'] for e in received),
                  'proxy independently observed navigation header for ' + case['url'])
    check(all(urlparse(e['url']).scheme == 'http' for e in events), 'all requests reached the loopback proxy')
    deadline = time.monotonic() + 15
    while members(proc.pid) and time.monotonic() < deadline:
        time.sleep(0.1)
    report['remaining_processes'] = members(proc.pid)
    check(not report['remaining_processes'], 'native host and helpers exit normally')
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

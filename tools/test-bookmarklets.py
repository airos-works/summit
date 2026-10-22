#!/usr/bin/env python3
"""Native bookmarklet acceptance on isolated loopback pages and private contexts."""
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
from native_crash_log import NativeCrashLog

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
for name in ('probe', 'bundle', 'images-helper', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
a = parser.parse_args()
if not sys.platform.startswith('haiku'):
    parser.error('Requires native Haiku')
for name in ('probe', 'bundle', 'images_helper'):
    setattr(a, name, getattr(a, name).resolve(strict=True))
a.output.mkdir(mode=0o700)
events = []


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        events.append(self.path)
        data = ("<!doctype html><meta charset=utf-8><title>Bookmarklet fixture</title>"
                "<p id=original>Original document</p>"
                "<script>window.inlineRan=1</script>"
                "<a id=blocked href='javascript:void(window.linkRan=1)'>Blocked link</a>").encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        if self.path == '/csp':
            self.send_header('Content-Security-Policy', "script-src 'none'; object-src 'none'")
        if self.path == '/sandbox':
            self.send_header('Content-Security-Policy', 'sandbox allow-same-origin')
        self.end_headers()
        self.wfile.write(data)


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


server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = f'http://127.0.0.1:{server.server_port}'
engine_files = [a.bundle / name for name in ('WebProcess', 'NetworkProcess',
                                           'lib/libWebKit.so.1', 'lib/libJavaScriptCore.so.18')]
inputs = [a.probe, a.images_helper, Path(__file__), ROOT / 'tools/native_crash_log.py',
          ROOT / 'tests/BookmarkletTests.cpp', ROOT / 'tests/ModernPDFTests.cpp', ROOT / 'vendor/nlohmann/json.hpp', *engine_files]
report = {'passed': False, 'forced_cleanup': False, 'checks': [], 'events': events,
          'bundle': str(a.bundle), 'sha256': {str(path): sha(path) for path in inputs}}
monitor = NativeCrashLog()
proc = None


def check(value, label):
    report['checks'].append({'passed': bool(value), 'label': label})
    if not value:
        raise AssertionError(label)


try:
    env = dict(os.environ, WEBKIT_EXEC_PATH=str(a.bundle),
               LIBRARY_PATH=f'{a.bundle}/lib:{a.bundle}/mesa/lib:/boot/system/lib',
               __EGL_VENDOR_LIBRARY_FILENAMES=str(a.bundle / 'mesa/egl_vendor.d/50_mesa.json'))
    env.pop('LD_PRELOAD', None)
    report['sha256'][env['__EGL_VENDOR_LIBRARY_FILENAMES']] = sha(env['__EGL_VENDOR_LIBRARY_FILENAMES'])
    log_path = a.output / 'native.log'
    with log_path.open('w') as log:
        proc = subprocess.Popen([str(a.probe), base, str(a.bundle), str(a.output / 'begin')], env=env,
                                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        report['team'] = proc.pid
        deadline = time.monotonic() + 60
        while 'BOOKMARKLET_READY' not in log_path.read_text():
            if proc.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('Probe did not reach its initial document')
            time.sleep(0.1)
        images = {}
        for pid in members(proc.pid):
            result = subprocess.run([str(a.images_helper), str(pid)], capture_output=True, text=True)
            if result.returncode == 0:
                images[str(pid)] = result.stdout.splitlines()
                for path in images[str(pid)]:
                    if path != 'commpage' and path not in report['sha256']:
                        report['sha256'][path] = sha(path)
        report['loaded_images'] = images
        loaded = {report['sha256'][p] for paths in images.values() for p in paths if p != 'commpage'}
        check(all(report['sha256'][str(p)] in loaded for p in engine_files), 'all four candidate engine images loaded')
        (a.output / 'begin').touch()
        report['exit'] = proc.wait(timeout=180)
    check(report['exit'] == 0 and 'BOOKMARKLET_RESULT PASS' in log_path.read_text(), 'native bookmarklet checks pass')
    deadline = time.monotonic() + 15
    while members(proc.pid) and time.monotonic() < deadline:
        time.sleep(0.1)
    report['remaining_processes'] = members(proc.pid)
    check(not report['remaining_processes'], 'owned browser and helpers exit normally')
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
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)
    report['inputs_unchanged'] = all(sha(path) == digest for path, digest in report['sha256'].items())
    report['native_crash_log'] = monitor.finish()
    if not report['inputs_unchanged'] or report['forced_cleanup'] or not report['native_crash_log']['passed']:
        report['passed'] = False
    (a.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('passed', 'forced_cleanup', 'inputs_unchanged', 'native_crash_log')}), flush=True)
sys.exit(0 if report['passed'] else 1)

#!/usr/bin/env python3
"""Check installed WebPositive's certificate UI in an owned native Haiku VM.

Uses the VM's ordinary WebPositive profile, an isolated matching engine/shim,
and temporary loopback certificates. Run serially with other native tests.
"""
import argparse
import hashlib
import html
import json
import os
from pathlib import Path
import signal
import shutil
import ssl
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from native_crash_log import NativeCrashLog

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--browser', type=Path, default=Path('/boot/system/apps/WebPositive'))
for name in ('library', 'bundle', 'ui-helper', 'images-helper', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
parser.add_argument('--egl-vendor', type=Path)
parser.add_argument('--fresh-session', action='store_true',
                    help='Temporarily preserve and restore the owned VM\'s saved WebPositive tabs')
a = parser.parse_args()
if not sys.platform.startswith('haiku'):
    parser.error('Requires native Haiku')
for name in ('browser', 'library', 'bundle', 'ui_helper', 'images_helper'):
    setattr(a, name, getattr(a, name).resolve(strict=True))
if a.egl_vendor:
    a.egl_vendor = a.egl_vendor.resolve(strict=True)
assert subprocess.run(['hey', 'WebPositive', 'get', 'Name'], capture_output=True).returncode != 0, \
    'WebPositive already running'
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


def process_snapshot(pids):
    # A new helper can start during shutdown, after the loaded-image snapshot.
    # Keep its identity before cleanup so a failure can be diagnosed.
    result = {}
    for pid in pids:
        images = subprocess.run([str(a.images_helper), str(pid)], capture_output=True, text=True)
        result[str(pid)] = {'images_exit': images.returncode, 'images': images.stdout.splitlines()}
    return {'teams': result, 'ps': subprocess.check_output(['ps'], text=True)}


contexts, fingerprints = [], []
for label in ('A', 'B'):
    cert, key = a.output / f'certificate-{label}.pem', a.output / f'key-{label}.pem'
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                    '-keyout', str(key), '-out', str(cert), '-days', '2',
                    '-subj', f'/CN=Summit certificate fixture {label}/O=Summit Tests',
                    '-addext', 'subjectAltName=IP:127.0.0.1,DNS:localhost'],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    key.chmod(0o600)
    fingerprints.append(hashlib.sha256(ssl.PEM_cert_to_DER_cert(cert.read_text())).hexdigest())
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    contexts.append(context)
events, handshakes, lock, active = [], [], threading.Lock(), 0


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.0'

    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, BrokenPipeError):
            # Closing a failed certificate negotiation can reset a connection
            # before an HTTP request arrives; it is recorded in handshakes.
            pass

    def log_message(self, *_):
        pass

    def do_GET(self):
        url = urlparse(self.path)
        with lock:
            events.append({'path': url.path, 'query': parse_qs(url.query),
                           'host': self.headers.get('Host'), 'certificate': active})
        if url.path in ('/loaded', '/favicon.ico'):
            self.send_response(204)
            self.end_headers()
            return
        body = ('<!doctype html><title>Legacy child certificate ' + html.escape(url.path)
                + '</title><style>html,body{margin:0;width:100%;height:100%;'
                  'background:rgb(66,140,222)}</style><p>Certificate accepted.</p>'
                  '<script>fetch("/loaded?case="+encodeURIComponent(location.pathname))</script>').encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)


class TLS(ThreadingHTTPServer):
    def get_request(self):
        sock, address = super().get_request()
        sock.settimeout(10)
        with lock:
            index = active
        try:
            wrapped = contexts[index].wrap_socket(sock, server_side=True)
            with lock:
                handshakes.append({'certificate': index, 'accepted': True})
            return wrapped, address
        except (ssl.SSLError, OSError):
            sock.close()
            with lock:
                handshakes.append({'certificate': index, 'accepted': False})
            raise


server = TLS(('127.0.0.1', 0), Handler)
server.daemon_threads = True
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = f'https://127.0.0.1:{server.server_port}'
alert = 'Unsecure SSL certificate'
report = {'passed': False, 'checks': [], 'events': events, 'handshakes': handshakes,
          'fingerprints': fingerprints, 'forced_cleanup': False, 'bundle': str(a.bundle),
          'platform': subprocess.check_output(['uname', '-a'], text=True).strip()}
inputs = [a.browser, a.library, a.ui_helper, a.images_helper, Path(__file__),
          ROOT / 'tools/native_crash_log.py', ROOT / 'tests/LegacyWebPositiveUITests.cpp']
inputs += [a.bundle / name for name in ('WebProcess', 'NetworkProcess',
                                       'lib/libWebKit.so.1', 'lib/libJavaScriptCore.so.18')]
if a.egl_vendor:
    inputs.append(a.egl_vendor)
report['sha256'] = {str(path): sha(path) for path in inputs}
monitor, proc = NativeCrashLog(), None
session = Path.home() / 'config/settings/WebPositive/Session'
session_backup = session.with_name('Session.summit-certificate-backup-' + str(os.getpid()))
session_prepared = False


def check(condition, label):
    report['checks'].append({'passed': bool(condition), 'label': label})
    if not condition:
        raise AssertionError(label)


def wait(predicate, label):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if predicate():
            check(True, label)
            return
        if proc.poll() is not None:
            raise RuntimeError('WebPositive exited early')
        time.sleep(0.1)
    check(False, label)


def hey(*args, required=True):
    result = subprocess.run(['hey', str(proc.pid), *args], capture_output=True, text=True, timeout=5)
    if required and result.returncode:
        raise RuntimeError('Native scripting failed: ' + result.stdout + result.stderr)
    return result


def panel():
    return hey('get', 'Messenger', 'of', 'Window', alert, required=False).returncode == 0


def answer(continue_loading):
    hey('ALTB', 'Window', alert, 'with', 'which=int32(' + ('0' if continue_loading else '1') + ')')
    wait(lambda: not panel(), 'certificate dialog closes after its native button message')


def navigate(path, host='127.0.0.1'):
    hey('_ARG', 'with', 'argc=int32(2)', 'and', 'argv=' + str(a.browser),
        'and', f'argv=https://{host}:{server.server_port}{path}')


def loaded(path):
    with lock:
        return any(e['path'] == '/loaded' and e['query'].get('case') == [path] for e in events)


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
    if a.fresh_session:
        assert not session_backup.exists(), 'Session backup already exists'
        report['session_before_sha256'] = sha(session) if session.exists() else None
        if session.exists():
            session.rename(session_backup)
        session_prepared = True
    env = dict(os.environ, WEBKIT_EXEC_PATH=str(a.bundle),
               LIBRARY_PATH=f'{a.library.parent}:{a.bundle}/lib:{a.bundle}/mesa/lib:/boot/system/lib',
               __EGL_VENDOR_LIBRARY_FILENAMES=str(a.egl_vendor or a.bundle / 'mesa/egl_vendor.d/50_mesa.json'))
    env.pop('LD_PRELOAD', None)
    with (a.output / 'native.log').open('w') as log:
        proc = subprocess.Popen([str(a.browser), base + '/stopped'], env=env,
                                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        report['team'] = proc.pid
        wait(panel, 'installed WebPositive presents its certificate dialog')
        report['initial_loaded_images'] = inventory()
        for suffix, selected in (('/libWebKit.so.1', a.bundle / 'lib/libWebKit.so.1'),
                                 ('/libWebKitLegacy.so.1', a.library)):
            check(any(path.endswith((suffix, suffix + '.10.0'))
                      and report['sha256'][path] == report['sha256'][str(selected)]
                      for paths in report['initial_loaded_images'].values() for path in paths),
                  'installed WebPositive loads selected ' + suffix.rsplit('/', 1)[1])
        check(hey('count', 'Window').returncode == 0, 'application responds while certificate dialog waits')
        text = hey('get', 'Text', '[0', 'to', '8192]', 'of', 'View', '_tv_', 'of', 'Window', alert).stdout
        report['initial_dialog_text'] = text
        normalized = ''.join(c for c in text.lower() if c in '0123456789abcdef')
        check(base in text and 'Summit certificate fixture A' in text and 'SHA-256' in text
              and fingerprints[0] in normalized, 'dialog identifies failed URL and actual certificate fingerprint')
        answer(False)
        check(not any(e['path'] == '/stopped' for e in events), 'Stop never sends the rejected HTTP request')
        navigate('/accepted')
        wait(panel, 'a later load still requires approval after Stop')
        answer(True)
        wait(lambda: loaded('/accepted'), 'Continue loads and executes the requested document')
        def visible_page():
            views = json.loads(subprocess.check_output([str(a.ui_helper), str(proc.pid), 'inspect'], text=True))
            report['rendered_windows'] = views
            return any('/accepted' in v['title'] and v.get('engineView')
                       and v.get('centerRGB') == [66, 140, 222] for v in views)
        wait(visible_page, 'accepted document renders its expected pixels in installed WebPositive')
        navigate('/reuse')
        wait(lambda: loaded('/reuse'), 'approved certificate is reused within the session')
        check(not panel(), 'session reuse needs no new certificate dialog')
        with lock:
            active = 1
        navigate('/rotated')
        wait(panel, 'a replacement certificate requires a new approval')
        check(not any(e['path'] == '/rotated' for e in events), 'replacement is rejected before HTTP')
        answer(True)
        wait(lambda: loaded('/rotated'), 'replacement certificate loads after its own approval')
        navigate('/other-host', 'localhost')
        wait(panel, 'approval for the IP address does not trust another hostname')
        answer(False)
        check(not any(e['path'] in ('/stopped', '/other-host') for e in events),
              'both rejected documents remain absent from server requests')
        report['loaded_images'] = inventory()
        subprocess.run([str(a.ui_helper), str(proc.pid), 'quit'], check=True, timeout=10)
        report['exit'] = proc.wait(timeout=25)
    deadline = time.monotonic() + 15
    while members(proc.pid) and time.monotonic() < deadline:
        time.sleep(0.1)
    report['remaining_processes'] = members(proc.pid)
    if report['remaining_processes']:
        report['remaining_process_details'] = process_snapshot(report['remaining_processes'])
    check(report['exit'] == 0 and not report['remaining_processes'], 'WebPositive and helpers exit normally')
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
    if session_prepared:
        if session.exists():
            shutil.copy2(session, a.output / 'test-session')
            session.unlink()
        if session_backup.exists():
            session_backup.rename(session)
        report['session_restored'] = ((sha(session) if session.exists() else None)
                                      == report['session_before_sha256'])
        report['passed'] = report['passed'] and report['session_restored']
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)
    report['native_crash_log'] = monitor.finish()
    report['inputs_unchanged'] = all(sha(path) == digest for path, digest in report['sha256'].items())
    report['passed'] = report['passed'] and report['inputs_unchanged'] and report['native_crash_log']['passed']
    (a.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: report.get(key) for key in ('passed', 'exit', 'forced_cleanup', 'error')}))
sys.exit(0 if report['passed'] else 1)

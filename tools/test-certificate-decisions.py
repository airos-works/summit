#!/usr/bin/env python3
"""Native engine certificate approval acceptance using only owned loopback TLS.

The selected bundle must match the native OS. No installed trust store or
browser profile is changed; the probe uses private contexts.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import ssl
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from native_crash_log import NativeCrashLog

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--probe', type=Path, required=True)
parser.add_argument('--bundle', type=Path, required=True)
parser.add_argument('--images-helper', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--legacy-library', type=Path,
                    help='Run LegacyCertificateTests with this shim and its inherited native dialog')
a = parser.parse_args()
if not sys.platform.startswith('haiku'):
    parser.error('Requires native Haiku')
for name in ('probe', 'bundle', 'images_helper'):
    setattr(a, name, getattr(a, name).resolve(strict=True))
if a.legacy_library:
    a.legacy_library = a.legacy_library.resolve(strict=True)
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
events, handshakes = [], []
lock = threading.Lock()
active = 0


class Handler(BaseHTTPRequestHandler):
    # A fresh connection exposes each certificate change; no persistent HTTP
    # connection can hide rotation. Each SSLContext has its own session cache.
    protocol_version = 'HTTP/1.0'

    def log_message(self, *_):
        pass

    def do_GET(self):
        global active
        with lock:
            if self.path == '/rotate' and self.server is plain:
                active = 1
            events.append({'tls': self.server is secure, 'path': self.path,
                           'host': self.headers.get('Host'), 'certificate': active})
        body = ('<!doctype html><title>Certificate fixture ' + self.path + '</title>'
                '<p>Native certificate decision fixture.</p>').encode()
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


plain = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
secure = TLS(('127.0.0.1', 0), Handler)
threads = [threading.Thread(target=s.serve_forever, daemon=True) for s in (plain, secure)]
for thread in threads:
    thread.start()
plain_base, secure_base = (f'{scheme}://127.0.0.1:{s.server_port}'
                           for scheme, s in (('http', plain), ('https', secure)))
report = {'passed': False, 'forced_cleanup': False, 'events': events, 'handshakes': handshakes,
          'fingerprints': fingerprints, 'bundle': str(a.bundle), 'sha256': {}, 'checks': []}
inputs = [a.probe, a.images_helper, Path(__file__), ROOT / 'tools/native_crash_log.py',
          ROOT / 'tests' / ('LegacyCertificateTests.cpp' if a.legacy_library else 'CertificateDecisionTests.cpp'), ROOT / 'tests/ModernPDFTests.cpp',
          ROOT / 'vendor/nlohmann/json.hpp']
if a.legacy_library:
    inputs.append(a.legacy_library)
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
               LIBRARY_PATH=f'{a.bundle}/lib:{a.bundle}/mesa/lib:/boot/system/lib',
               __EGL_VENDOR_LIBRARY_FILENAMES=str(a.bundle / 'mesa/egl_vendor.d/50_mesa.json'))
    env.pop('LD_PRELOAD', None)
    if a.legacy_library:
        env['LIBRARY_PATH'] = str(a.legacy_library.parent) + ':' + env['LIBRARY_PATH']
    prefix = 'LEGACY_CERTIFICATE' if a.legacy_library else 'CERTIFICATE_DECISION'
    log_path = a.output / 'native.log'
    with log_path.open('w') as log:
        arguments = ([str(a.probe), plain_base, secure_base, str(a.output / 'profile'),
                      str(a.bundle), str(a.output / 'begin'), fingerprints[0]] if a.legacy_library
                     else [str(a.probe), plain_base, secure_base, str(a.bundle),
                           str(a.output / 'begin'), *fingerprints])
        proc = subprocess.Popen(arguments, env=env,
                                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        report['team'] = proc.pid
        deadline = time.monotonic() + 60
        while prefix + '_READY' not in log_path.read_text():
            if proc.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('Probe did not reach the initial certificate failure')
            time.sleep(0.1)
        report['loaded_images'] = inventory()
        check(any(path == str(a.bundle / 'lib/libWebKit.so.1.10.0')
                  or (path.endswith('/libWebKit.so.1') and sha(path) == report['sha256'][str(a.bundle / 'lib/libWebKit.so.1')])
                  for paths in report['loaded_images'].values() for path in paths),
              'probe loads selected engine')
        if a.legacy_library:
            check(any(path.endswith(('/libWebKitLegacy.so.1', '/libWebKitLegacy.so.1.10.0'))
                      and sha(path) == report['sha256'][str(a.legacy_library)]
                      for paths in report['loaded_images'].values() for path in paths),
                  'legacy probe loads selected shim')
        (a.output / 'begin').touch()
        report['exit'] = proc.wait(timeout=180)
    check(report['exit'] == 0 and prefix + '_RESULT PASS' in log_path.read_text(),
          'native certificate decision checks pass')
    rejected = (('/ui-stop', '/abandoned', '/fresh', '/destroyed') if a.legacy_library else
                ('/initial', '/after-navigation', '/after-stop', '/closed', '/fresh-context'))
    check(not any(event['tls'] and event['path'] in rejected for event in events),
          'invalid, stale and closed-view approvals never reach HTTP')
    if not a.legacy_library:
        check(sum(event['tls'] and event['path'] == '/after-reload' for event in events) == 1,
              'stopped and reloaded failures stay blocked until the explicit current approval')
    accepted = (('/ui-continue', '/ui-reuse', '/held-continue') if a.legacy_library
                else ('/reuse', '/host-scope', '/rotated', '/after-closed'))
    check(all(any(e['tls'] and e['path'] == path for e in events)
              for path in accepted),
          'approved current certificates reach the intended pages')
    deadline = time.monotonic() + 15
    while members(proc.pid) and time.monotonic() < deadline:
        time.sleep(0.1)
    report['remaining_processes'] = members(proc.pid)
    check(not report['remaining_processes'], 'owned browser and helper teams exit normally')
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
    for server in (plain, secure):
        server.shutdown()
        server.server_close()
    for thread in threads:
        thread.join(timeout=5)
    report['native_crash_log'] = monitor.finish()
    report['inputs_unchanged'] = all(sha(path) == digest for path, digest in report['sha256'].items())
    report['passed'] = report['passed'] and report['inputs_unchanged'] and report['native_crash_log']['passed']
    (a.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: report.get(key) for key in ('passed', 'exit', 'forced_cleanup', 'error')}))
sys.exit(0 if report['passed'] else 1)

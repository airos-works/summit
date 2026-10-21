#!/usr/bin/env python3
"""Native Haiku shutdown acceptance with live TLS, workers and a secure WebSocket.

Run serially with other native tests. The output directory must be new; all
profiles, certificates and processes belong to this invocation. Uses the actual
browser's MCP server, bound only to loopback, and the native close/image helpers.
"""
import argparse
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import select
import signal
import socket
import ssl
import subprocess
import sys
import threading
import time
import traceback
import urllib.parse
import urllib.request

from native_crash_log import NativeCrashLog

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


class Fixture:
    def __init__(self, output):
        self.prefix = '/' + secrets.token_hex(12) + '/'
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.records = []
        self.assets = ROOT / 'tests/fixtures/network-shutdown'
        self.certificate, key = output / 'certificate.pem', output / 'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
            '-keyout', str(key), '-out', str(self.certificate), '-days', '2',
            '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.fingerprint = hashlib.sha256(ssl.PEM_cert_to_DER_cert(self.certificate.read_text())).hexdigest()
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *_):
                pass

            def handle(self):
                try:
                    super().handle()
                except (ConnectionResetError, BrokenPipeError, ssl.SSLError):
                    # Closing the browser can reset an idle keep-alive socket.
                    pass

            def do_GET(self):
                request = urllib.parse.urlsplit(self.path)
                if not request.path.startswith(owner.prefix):
                    self.send_error(404)
                    return
                name = request.path[len(owner.prefix):]
                query = urllib.parse.parse_qs(request.query)
                record = {'path': name, 'round': query.get('round', [''])[0],
                          'kind': query.get('kind', [''])[0], 'tls': self.connection.version(),
                          'cipher': self.connection.cipher()[0], 'closed': False}
                with owner.lock:
                    owner.records.append(record)
                try:
                    if name == 'socket':
                        self.websocket()
                    elif name == 'held':
                        self.send_response(200)
                        self.send_header('Content-Type', 'application/octet-stream')
                        self.send_header('Content-Length', '100000000')
                        self.send_header('Cache-Control', 'no-store')
                        self.end_headers()
                        while not owner.stop.is_set():
                            # Read the peer's EOF as well as checking writes: a
                            # TCP half-close can still accept buffered writes.
                            if select.select([self.connection], [], [], 0)[0]:
                                if not self.connection.recv(1):
                                    break
                            self.wfile.write(b'x' * 1024)
                            self.wfile.flush()
                            owner.stop.wait(.05)
                    elif name in ('index.html', 'page.js', 'sw.js', 'worker.js'):
                        data = (owner.assets / name).read_bytes()
                        self.send_response(200)
                        self.send_header('Content-Type', 'text/javascript' if name.endswith('.js') else 'text/html')
                        self.send_header('Content-Length', str(len(data)))
                        self.send_header('Cache-Control', 'no-store')
                        self.end_headers()
                        self.wfile.write(data)
                    else:
                        self.send_error(404)
                except (BrokenPipeError, ConnectionResetError, ssl.SSLError, OSError):
                    pass
                finally:
                    record['closed'] = True

            def websocket(self):
                key = self.headers.get('Sec-WebSocket-Key', '')
                if self.headers.get('Upgrade', '').lower() != 'websocket' or not key:
                    self.send_error(400)
                    return
                accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
                self.send_response(101)
                self.send_header('Upgrade', 'websocket')
                self.send_header('Connection', 'Upgrade')
                self.send_header('Sec-WebSocket-Accept', accept)
                self.end_headers()
                # The fixture only sends a short text message followed by close.
                self.connection.settimeout(120)
                while not owner.stop.is_set():
                    header = self.rfile.read(2)
                    if len(header) != 2:
                        return
                    opcode, length = header[0] & 15, header[1] & 127
                    if not header[1] & 128 or length > 125:
                        return
                    mask = self.rfile.read(4)
                    data = self.rfile.read(length)
                    if len(mask) != 4 or len(data) != length:
                        return
                    payload = bytes(value ^ mask[i % 4] for i, value in enumerate(data))
                    self.wfile.write(bytes([0x80 | opcode, len(payload)]) + payload)
                    self.wfile.flush()
                    if opcode == 8:
                        return

        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(self.certificate, key)

        class Server(ThreadingHTTPServer):
            daemon_threads = True

            def process_request_thread(self, request, client_address):
                # A browser's speculative TCP connection may not start TLS.
                # Keep the handshake off the accepting/server shutdown thread.
                try:
                    request.settimeout(10)
                    request = context.wrap_socket(request, server_side=True)
                    request.settimeout(120)
                    super().process_request_thread(request, client_address)
                except (OSError, ssl.SSLError):
                    request.close()

        self.server = Server(('127.0.0.1', 0), Handler)
        self.base = 'https://localhost:' + str(self.server.server_port) + self.prefix
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def live(self, number):
        with self.lock:
            return [dict(item) for item in self.records if item['round'] == str(number)
                    and item['path'] in ('held', 'socket') and not item['closed']]

    def close(self):
        self.stop.set()
        self.server.shutdown()
        self.thread.join(timeout=5)
        self.server.server_close()


def group_members(group):
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, default=Path('/boot/system/apps/Summit'))
    parser.add_argument('--close-helper', type=Path, required=True)
    parser.add_argument('--images-helper', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runs', type=int, default=5)
    args = parser.parse_args()
    if not sys.platform.startswith('haiku') or args.runs < 2:
        parser.error('Requires native Haiku and at least two runs')
    args.output.mkdir(mode=0o700)
    args.browser = args.browser.resolve(strict=True)
    args.close_helper = args.close_helper.resolve(strict=True)
    args.images_helper = args.images_helper.resolve(strict=True)
    report = {'platform': subprocess.check_output(['uname', '-a'], text=True).strip(),
              'browser': str(args.browser), 'runs': [], 'passed': False}
    inputs = [Path(__file__), ROOT / 'tools/native_crash_log.py', args.close_helper, args.images_helper,
              *sorted((ROOT / 'tests/fixtures/network-shutdown').iterdir())]
    report['input_sha256'] = {str(path): digest(path) for path in inputs}
    runtime = {str(args.browser): digest(args.browser)}
    fixture = Fixture(args.output)
    profile = args.output / 'profile'
    profile.mkdir(mode=0o700)
    (profile / 'profile.json').write_text(json.dumps({'version': 1, 'tabs': [], 'selected': 0,
        'bookmarks': [], 'history': [], 'restoreSession': False,
        'trustedCertificates': [{'host': 'localhost', 'sha256': fixture.fingerprint,
            'subject': 'Owned network shutdown fixture', 'added': int(time.time())}]}))
    token = secrets.token_hex(32)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]

    def call(name, **arguments):
        request = urllib.request.Request(f'http://127.0.0.1:{port}/mcp',
            data=json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call',
                'params': {'name': name, 'arguments': arguments}}).encode(),
            headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=10) as response:
            result = json.load(response)
        if 'error' in result or result.get('result', {}).get('isError'):
            raise RuntimeError('MCP failed: ' + str(result))
        text = result['result']['content'][0]['text']
        try:
            return json.loads(text)
        except json.JSONDecodeError:
            return text

    def evaluate(expression):
        result = call('evaluate', expression=expression)
        if not isinstance(result, dict) or 'exception' in result or 'value' not in result:
            raise RuntimeError('Evaluation failed: ' + str(result))
        return result['value']

    def save():
        (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')

    monitor = NativeCrashLog()
    try:
        for number in range(args.runs):
            record = {'round': number, 'forced_cleanup': False}
            report['runs'].append(record)
            environment = dict(os.environ, SUMMIT_MCP='1', SUMMIT_MCP_LOCAL='1',
                SUMMIT_MCP_TOKEN=token, SUMMIT_MCP_PORT=str(port), SUMMIT_QUIT_TRACE='1')
            with (args.output / f'browser-{number}.log').open('w') as log:
                browser = subprocess.Popen([str(args.browser), '--profile', str(profile),
                    fixture.base + 'index.html?round=' + str(number)], env=environment,
                    stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                record['team'] = browser.pid
                save()
                try:
                    deadline = time.monotonic() + 45
                    while time.monotonic() < deadline:
                        try:
                            state = evaluate('window.shutdownProbe || null')
                        except Exception as error:
                            record['last_probe_error'] = str(error)
                            state = None
                        if state and state.get('errors'):
                            record['probe'] = state
                            raise RuntimeError('Fixture failed: ' + str(state))
                        if state and state.get('ready'):
                            break
                        if browser.poll() is not None:
                            raise RuntimeError('Browser exited before fixture readiness')
                        time.sleep(.1)
                    else:
                        try:
                            record['page_at_timeout'] = call('evaluate', expression=
                                '({url:location.href,title:document.title,readyState:document.readyState,probe:window.shutdownProbe})')
                            record['console_at_timeout'] = call('console', limit=20)
                        except Exception as error:
                            record['diagnostic_error'] = str(error)
                        raise RuntimeError('Fixture readiness deadline: ' + str(state))
                    record['probe'] = state
                    assert state['secure'] and state['controlled'] == 'service-worker-response'
                    assert state['cacheSeed'] == 'persistent-cache' and state['visit'] == number + 1
                    assert state['cacheVisit'] == number + 1, 'Cache API data did not persist'
                    assert number == 0 or f'shutdown-visit={number}' in state['previousCookie'], 'Cookie did not persist'
                    live = fixture.live(number)
                    assert {r['kind'] for r in live if r['path'] == 'held'} == {'page', 'worker', 'service'}
                    assert sum(r['path'] == 'socket' for r in live) == 1
                    record['active_connections_before_quit'] = live
                    images = {}
                    record['loaded_images'] = images
                    for team in group_members(browser.pid):
                        result = subprocess.run([str(args.images_helper), str(team)], capture_output=True, text=True)
                        if result.returncode == 0:
                            images[str(team)] = result.stdout.splitlines()
                            for path in images[str(team)]:
                                # The kernel maps this image without a backing file.
                                if path == 'commpage':
                                    continue
                                if path not in runtime:
                                    runtime[path] = digest(path)
                    assert any(path.endswith('/NetworkProcess') for paths in images.values() for path in paths)
                    assert any('/libssl.so' in path for paths in images.values() for path in paths)
                    if number % 2:
                        record['unregistered'] = evaluate('shutdownRegistration.unregister()')
                        assert record['unregistered'] is True
                    live = fixture.live(number)
                    assert {r['kind'] for r in live if r['path'] == 'held'} == {'page', 'worker', 'service'}
                    assert sum(r['path'] == 'socket' for r in live) == 1
                    record['active_connections_before_quit'] = live
                    result = subprocess.run([str(args.close_helper), '--close-browser', str(browser.pid),
                        str(args.browser)], timeout=10)
                    record['close_exit'] = result.returncode
                    record['browser_exit'] = browser.wait(timeout=25)
                    deadline = time.monotonic() + 15
                    while (group_members(browser.pid) or fixture.live(number)) and time.monotonic() < deadline:
                        time.sleep(.1)
                    record['remaining_processes'] = group_members(browser.pid)
                    record['connections_after_quit'] = fixture.live(number)
                    assert record['close_exit'] == record['browser_exit'] == 0
                    assert not record['remaining_processes'] and not record['connections_after_quit']
                    record['passed'] = True
                except Exception:
                    if browser.poll() is None:
                        try:
                            record['console_on_failure'] = call('console', limit=20)
                            record['network_on_failure'] = call('network', limit=20)
                        except Exception as error:
                            record['diagnostic_error'] = str(error)
                    record['images_on_failure'] = {}
                    for team in group_members(browser.pid):
                        result = subprocess.run([str(args.images_helper), str(team)],
                            capture_output=True, text=True)
                        record['images_on_failure'][str(team)] = {
                            'exit': result.returncode, 'images': result.stdout.splitlines()}
                    raise
                finally:
                    if browser.poll() is None or group_members(browser.pid):
                        record['forced_cleanup'] = True
                        os.killpg(browser.pid, signal.SIGKILL)
                        browser.wait(timeout=10)
                    save()
            print(f'round {number}: TLS/worker/WebSocket shutdown and persistence passed', flush=True)
        report['passed'] = True
    except Exception as error:
        report['error'] = type(error).__name__ + ': ' + str(error)
        report['traceback'] = traceback.format_exc()
        save()
    finally:
        fixture.close()
        report['native_crash_log'] = monitor.finish()
        report['input_unchanged'] = all(digest(path) == expected for path, expected in report['input_sha256'].items())
        report['runtime_sha256'] = runtime
        report['runtime_unchanged'] = all(digest(path) == expected for path, expected in runtime.items())
        report['requests'] = fixture.records
        report['passed'] = (report['passed'] and report['native_crash_log']['passed']
            and report['input_unchanged'] and report['runtime_unchanged'])
        save()
    print(json.dumps({'passed': report['passed'], 'error': report.get('error')}), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())

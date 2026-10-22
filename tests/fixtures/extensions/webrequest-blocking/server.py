#!/usr/bin/env python3
"""Local HTTP server for the blocking webRequest fixture (python3 server.py [port])."""
import base64
import hashlib
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HITS = []

INDEX = """<!doctype html><meta charset="utf-8"><title>webRequest blocking fixture</title>
<h1>webRequest blocking fixture</h1>
<p>Each row is filled in by script; an extension-cancelled subresource shows <b>blocked</b>.</p>
<table border="1" id="results"></table>
<p><a href="/wr-block/page.html">main-frame cancel</a> |
<a href="/wr-redirect/page.html">main-frame redirect</a> |
<a href="/wr-auth/page.html">HTTP auth filled by extension</a> |
<a href="/hits">server hit log</a></p>
<script>
const rows = document.getElementById('results');
function row(name, value) { const tr = rows.insertRow(); tr.insertCell().textContent = name; tr.insertCell().textContent = value; }
async function probe(name, path, expect) {
    const started = performance.now();
    try {
        const response = await fetch(path, {cache: 'no-store'});
        const text = await response.text();
        row(name, `${response.status} ${text.slice(0, 120)} [${Math.round(performance.now() - started)} ms] expect: ${expect}`);
        return response;
    } catch (e) {
        row(name, `blocked (${e}) [${Math.round(performance.now() - started)} ms] expect: ${expect}`);
    }
}
(async () => {
    await probe('plain', '/plain.txt', '200 plain');
    await probe('cancel', '/wr-block/data.txt', 'blocked');
    await probe('redirect', '/wr-redirect/data.txt', '200 redirected');
    await probe('promise cancel', '/wr-promise-block/data.txt', 'blocked');
    await probe('invalid reply', '/wr-invalid/data.txt', '200 (reply ignored)');
    const echo = await probe('request header', '/wr-headers/echo', 'x-summit-webrequest: request-header-added');
    if (echo) row('response header', echo.headers.get('X-Summit-Response') + ' expect: response-header-added');
    await probe('auth', '/wr-auth/data.txt', '200 authorized');
    await probe('surrogate data: URL', '/wr-surrogate-data/x.txt', '200 surrogate data');
    await probe('surrogate not web-accessible', '/wr-surrogate-hidden/x.js', 'blocked');
    await new Promise(resolve => {
        const script = document.createElement('script');
        script.onload = script.onerror = () => { row('surrogate script', `ran ${window.summitSurrogateRan || 0} time(s) expect: ran 1 time(s)`); resolve(); };
        script.src = '/wr-surrogate-js/ads.js';
        document.head.appendChild(script);
    });
    const socket = (name, path, expect) => new Promise(resolve => {
        const ws = new WebSocket(`ws://${location.host}${path}`);
        let events = [];
        ws.onopen = () => events.push('open');
        ws.onmessage = event => events.push('message ' + event.data);
        ws.onerror = () => events.push('error');
        ws.onclose = event => { events.push('close ' + event.code); row(name, events.join(', ') + ' expect: ' + expect); resolve(); };
    });
    await socket('websocket allowed', '/ws/echo', 'open, message hello, close 1000');
    await socket('websocket cancel', '/wr-block/ws', 'error, close 1006');
    navigator.sendBeacon('/beacon/ok', 'ok');
    navigator.sendBeacon('/wr-block/beacon', 'blocked');
    await new Promise(resolve => setTimeout(resolve, 1500));
    const hits = await (await fetch('/hits', {cache: 'no-store'})).json();
    row('beacons', `ok ${hits.includes('/beacon/ok') ? 'sent' : 'missing'}, blocked one ${hits.includes('/wr-block/beacon') ? 'reached the server' : 'stopped'} expect: ok sent, blocked one stopped`);
    await probe('slow (listener never settles)', '/wr-slow/data.txt', '200 after about 20 s');
    const img = new Image(); img.onload = () => row('image cancel', 'loaded (unexpected)'); img.onerror = () => row('image cancel', 'blocked');
    img.src = '/wr-block/pixel.gif';
})();
</script>"""

GIF = base64.b64decode('R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==')


class Handler(BaseHTTPRequestHandler):
    # WebSocket handshakes need an HTTP/1.1 status line; every response has a length.
    protocol_version = 'HTTP/1.1'

    def send(self, status, body, content_type='text/plain; charset=utf-8', headers=()):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        HITS.append(self.path)
        self.rfile.read(int(self.headers.get('Content-Length', 0)))
        return self.send(204, b'')

    def websocket(self):
        # Just enough of RFC 6455: accept, send one text frame, close normally.
        key = self.headers.get('Sec-WebSocket-Key', '')
        accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        self.send_response(101, 'Switching Protocols')
        self.send_header('Upgrade', 'websocket')
        self.send_header('Connection', 'Upgrade')
        self.send_header('Sec-WebSocket-Accept', accept)
        self.end_headers()
        self.wfile.write(b'\x81\x05hello')
        self.wfile.write(b'\x88\x02\x03\xe8')
        self.wfile.flush()
        self.close_connection = True

    def do_GET(self):
        HITS.append(self.path)
        path = self.path.split('?')[0]
        if self.headers.get('Upgrade', '').lower() == 'websocket':
            return self.websocket()
        if path == '/':
            return self.send(200, INDEX, 'text/html; charset=utf-8')
        if path == '/hits':
            return self.send(200, json.dumps(HITS, indent=1), 'application/json')
        if path.startswith('/wr-redirected.html'):
            return self.send(200, 'redirected ' + self.path)
        if path.startswith('/wr-headers/'):
            headers = {k.lower(): v for k, v in self.headers.items()}
            return self.send(200, json.dumps(headers), 'application/json')
        if path.startswith('/wr-auth/'):
            expected = 'Basic ' + base64.b64encode(b'summit:webrequest').decode()
            if self.headers.get('Authorization') != expected:
                return self.send(401, 'authorization required', headers=[('WWW-Authenticate', 'Basic realm="summit-webrequest"')])
            return self.send(200, 'authorized')
        if path.endswith('.gif'):
            return self.send(200, GIF, 'image/gif')
        if path.endswith('.html'):
            return self.send(200, '<!doctype html><title>%s</title><p>server served %s' % (path, path), 'text/html; charset=utf-8')
        return self.send(200, 'plain ' + path)

    def log_message(self, fmt, *args):
        sys.stderr.write('[wr-server] ' + fmt % args + '\n')


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()

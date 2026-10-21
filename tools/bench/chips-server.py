#!/usr/bin/env python3
"""Local HTTPS fixture for CHIPS. Use three distinct hostnames/IPs as sites.

Supply a test certificate trusted by the isolated browser profile for
the machine's LAN IP, 127.0.0.1 and localhost. Visit /top?seed=first on the
LAN IP, then /top on 127.0.0.1: the localhost frame must not receive the
first site's cookies. These distinct IPs/hostnames are separate sites.
No real site, account or camera is used. Bind only on a test machine.
"""
import argparse
import html
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import ssl
from urllib.parse import parse_qs, urlencode, urlsplit


class Server(ThreadingHTTPServer):
    def get_request(self):
        request, address = super().get_request()
        return self.tls.wrap_socket(request, server_side=True, do_handshake_on_connect=False), address

    def process_request_thread(self, request, address):
        # Browsers can preconnect without sending TLS yet. A speculative socket
        # must not block the accept loop for every other site in this fixture.
        request.settimeout(10)
        try:
            request.do_handshake()
        except (OSError, ssl.SSLError):
            self.shutdown_request(request)
            return
        super().process_request_thread(request, address)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urlsplit(self.path)
        query = parse_qs(parsed.query)
        seed = query.get('seed', [''])[0]
        delete = query.get('delete', [''])[0] == '1'
        if seed and seed not in ('first', 'second'):
            self.send_error(400)
            return
        headers = []
        if parsed.path == '/echo':
            body = json.dumps({'cookie': self.headers.get('Cookie', '')})
            mime = 'application/json'
        elif parsed.path == '/frame':
            attributes = '; Secure; SameSite=None; Path=/; Max-Age=3600'
            if seed:
                headers = [('Set-Cookie', 'server=' + seed + attributes + '; HttpOnly; Partitioned'),
                           ('Set-Cookie', 'ordinary=shared' + attributes)]
            if delete:
                headers = [('Set-Cookie', 'server=; Secure; SameSite=None; Path=/; HttpOnly; Partitioned; Max-Age=0')]
            script_cookie = ('script=' + seed + attributes + '; Partitioned') if seed else ''
            if delete:
                script_cookie = 'script=; Secure; SameSite=None; Path=/; Partitioned; Max-Age=0'
            body = '''<!doctype html><meta charset="utf-8"><title>Embedded cookie probe</title>
<script>
(async () => {
  const write = %s;
  if (write) document.cookie = write;
  const result = {dom: document.cookie, network: await fetch('/echo', {cache:'no-store'}).then(r => r.json())};
  document.body.textContent = JSON.stringify(result);
  parent.postMessage({chipsResult: result}, '*');
})().catch(error => parent.postMessage({chipsResult: {error: String(error)}}, '*'));
</script><body></body>''' % json.dumps(script_cookie)
            mime = 'text/html'
        elif parsed.path == '/top':
            embed = 'https://localhost:' + str(self.server.server_port)
            frame = embed + '/frame?' + urlencode({'seed': seed, 'delete': '1' if delete else '0'})
            body = '''<!doctype html><meta charset="utf-8"><title>CHIPS isolation</title>
<style>body {font:16px sans-serif} iframe {width:95%%;height:180px}</style>
<h1>Partitioned cookie isolation</h1><p>Top-level site: %s</p><pre id="result">Waiting for frame…</pre>
<script>
window.chipsResult = null;
addEventListener('message', event => {
  if (event.origin !== %s || event.source !== document.querySelector('iframe').contentWindow) return;
  if (event.data.chipsResult) {
    window.chipsResult = event.data.chipsResult;
    document.getElementById('result').textContent = JSON.stringify(chipsResult, null, 2);
  }
});
</script><iframe src="%s"></iframe>''' % (html.escape(self.headers.get('Host', '')), json.dumps(embed), html.escape(frame, quote=True))
            mime = 'text/html'
        else:
            self.send_error(404)
            return
        data = body.encode()
        self.send_response(200)
        self.send_header('Content-Type', mime + '; charset=utf-8')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(data)))
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(data)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cert', required=True)
    parser.add_argument('--key', required=True)
    parser.add_argument('--port', type=int, default=8768)
    parser.add_argument('--bind', default='0.0.0.0')
    args = parser.parse_args()
    server = Server((args.bind, args.port), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(args.cert, args.key)
    server.tls = context
    server.serve_forever()

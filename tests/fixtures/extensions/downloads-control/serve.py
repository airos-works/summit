#!/usr/bin/env python3
# serve.py [PORT]: /slow.bin, 8 MiB of a repeating pattern sent at about
# 1 MiB a second, with Accept-Ranges, an ETag and Range answers (206), for
# the downloads-control fixture's pause and resume.
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import time

SIZE = 8 << 20
BODY = (bytes(range(256)) * (SIZE // 256))
ETAG = '"summit-slow-1"'

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        if self.path != "/slow.bin":
            self.send_error(404)
            return
        start = 0
        ranged = self.headers.get("Range", "")
        if ranged.startswith("bytes=") and self.headers.get("If-Range", ETAG) == ETAG:
            start = int(ranged[6:].split("-")[0] or 0)
        if start >= SIZE:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{SIZE}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(206 if start else 200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("ETag", ETAG)
        self.send_header("Content-Length", str(SIZE - start))
        if start:
            self.send_header("Content-Range", f"bytes {start}-{SIZE - 1}/{SIZE}")
        self.end_headers()
        chunk = 64 << 10
        try:
            for offset in range(start, SIZE, chunk):
                self.wfile.write(BODY[offset:offset + chunk])
                time.sleep(1 / 16)
        except (BrokenPipeError, ConnectionResetError):
            pass

ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 8798), Handler).serve_forever()

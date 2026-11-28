#!/usr/bin/env python3
"""HTTP proxy that lets an Android device reach this host's wpt server.

WPT serves web-platform.test and its subdomains (www, www1, xn--... and
not-web-platform.test). A device or emulator cannot be given hosts entries,
so Summit's WebDriver sessions use this proxy instead (the "summit-android"
port of tools/interop2026's wptrunner patch passes it as the proxy
capability): those names go to 127.0.0.1 here, everything else to where DNS
says. HTTPS and WebSockets go through CONNECT; plain HTTP requests are passed
on in origin form, one per connection.

  tools/android/wpt-proxy.py [--port 8899]      (emulator: 10.0.2.2:8899)
"""

import argparse
import asyncio
import sys

WPT_SUFFIXES = ("web-platform.test",)


def target_host(host):
    name = host.lower().rstrip(".")
    if any(name == suffix or name.endswith("." + suffix) for suffix in WPT_SUFFIXES):
        return "127.0.0.1"
    return host


async def pipe(reader, writer):
    try:
        while True:
            data = await reader.read(65536)
            if not data:
                break
            writer.write(data)
            await writer.drain()
    except (ConnectionError, asyncio.CancelledError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def handle(client_reader, client_writer):
    try:
        head = await client_reader.readuntil(b"\r\n\r\n")
    except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionError):
        client_writer.close()
        return
    lines = head.decode("latin-1").split("\r\n")
    method, target, version = (lines[0].split(" ", 2) + ["", ""])[:3]
    try:
        if method == "CONNECT":
            host, _, port = target.rpartition(":")
            server_reader, server_writer = await asyncio.open_connection(target_host(host), int(port))
            client_writer.write(b"HTTP/1.1 200 Connection Established\r\n\r\n")
            await client_writer.drain()
        else:
            # Absolute form: http://host[:port]/path
            rest = target.split("://", 1)[1] if "://" in target else target
            authority, _, path = rest.partition("/")
            host, _, port = authority.rpartition(":") if ":" in authority else (authority, "", "80")
            server_reader, server_writer = await asyncio.open_connection(target_host(host), int(port))
            headers = [h for h in lines[1:] if h and not h.lower().startswith(("proxy-connection:", "connection:"))]
            request = "%s /%s %s\r\n%s\r\nConnection: close\r\n\r\n" % (method, path, version, "\r\n".join(headers))
            server_writer.write(request.encode("latin-1"))
            await server_writer.drain()
    except (OSError, ValueError) as error:
        client_writer.write(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
        try:
            await client_writer.drain()
        finally:
            client_writer.close()
        print("wpt-proxy: %s %s: %s" % (method, target, error), file=sys.stderr)
        return
    await asyncio.gather(pipe(client_reader, server_writer), pipe(server_reader, client_writer))


async def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8899)
    parser.add_argument("--bind", default="127.0.0.1", help="the emulator's 10.0.2.2 is this host's 127.0.0.1")
    args = parser.parse_args()
    server = await asyncio.start_server(handle, args.bind, args.port, limit=1 << 20)
    print("wpt-proxy: listening on %s:%d" % (args.bind, args.port), flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass

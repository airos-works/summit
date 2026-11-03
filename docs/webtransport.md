# WebTransport

Issue [#176](https://github.com/airos-works/summit/issues/176) (Interop 2026,
the webtransport focus area). Safari's WebTransport runs on Network.framework;
Summit's network process speaks QUIC and HTTP/3 itself.

## What pages get

`new WebTransport(url, options)` for `https://` URLs over HTTP/3:
bidirectional and unidirectional streams both ways, datagrams, `close()`
with a code and reason (both ways), `draining`, `protocols` /
`protocol` (`wt-available-protocols`, `wt-protocol`), extra request
`headers`, `serverCertificateHashes` (SHA-256, certificates valid for at most
two weeks), `getStats()`, `exportKeyingMaterial()`, stream reset and
STOP_SENDING error codes, in windows and workers.

A server certificate OpenSSL cannot verify is put to the user the same way
as for page loads: one the user already trusts for that host (Summit's
certificate warnings, `trustedCertificates`) is accepted; WebTransport
itself never shows a warning.

Not done: HTTP/2 fallback (`requireUnreliable: false` still only tries
HTTP/3), pooling several sessions on one connection (`allowPooling`), send
groups and priorities (sends are round-robin), `congestionControl` (always
CUBIC), datagram `maxAge` / high-water marks in the network process.

`SUMMIT_WEBTRANSPORT=0` turns it off (the `WebTransport` interface goes
away). `SUMMIT_WEBTRANSPORT_TRACE=1` logs the session (connection, settings,
CONNECT, streams, resets, close) to stderr;
`SUMMIT_WEBTRANSPORT_TRACE_QUIC=1` adds ngtcp2's own log.

Verified on the X399 (4 Nov 2026): all 58 Interop 2026 webtransport tests
pass (window, dedicated, shared and service workers; wpt's aioquic server):
the area scores 1000, as Safari Technology Preview does, from 0.6. Sessions
also open to servers on other QUIC stacks: Cloudflare's Media over QUIC
relays (`relay.cloudflare.mediaoverquic.com`,
`draft-14.cloudflare.mediaoverquic.com`) and `cdn.moq.dev` (quinn), with
certificates verified against the system's CA bundle. A plain HTTP/3 server
(`cloudflare-quic.com`) is refused cleanly: it has no extended CONNECT.

## How it works

Engine side, in `Source/WebKit/NetworkProcess/webtransport/haiku/`:

- `WebTransportQuicConnectionHaiku` is one session's QUIC connection on a
  thread of its own: a connected UDP socket, `poll()` on it and a wake pipe,
  ngtcp2's timers. TLS 1.3 comes from OpenSSL 3.5's QUIC TLS interface
  through `ngtcp2_crypto_ossl` (ALPN `h3`, CA bundle
  `/boot/system/data/ssl/CARootCertificates.pem`).
- HTTP/3 is the port's own, small part of it: a control stream with
  SETTINGS (`H3_DATAGRAM`, the draft-04 datagram setting,
  `ENABLE_WEBTRANSPORT` and `WEBTRANSPORT_MAX_SESSIONS` of the drafts
  servers still look for), the server's SETTINGS (extended CONNECT must be
  on), and the extended CONNECT request (`:protocol: webtransport`,
  `origin`, `sec-webtransport-http3-draft02`). The QPACK dynamic table is
  off both ways: requests are encoded with the static table and literals;
  the response is decoded with nghttp3's QPACK decoder. nghttp3's HTTP/3
  connection is not used: it neither sends the WebTransport settings nor
  leaves WebTransport streams alone.
- The CONNECT stream is the session: its response (2xx) makes the session
  ready; capsules in its DATA frames close (`CLOSE_WEBTRANSPORT_SESSION`)
  or drain it; its end closes it.
- WebTransport streams carry their session ID after a stream type
  (unidirectional 0x54, bidirectional 0x41); datagrams carry the quarter
  stream ID (RFC 9297). Stream error codes are mapped to and from the
  HTTP/3 error code space as the draft says (`http3ErrorCode`).
- A host's addresses are tried in the resolver's order: the next one when
  sending fails or nothing comes back within a second, so an IPv6 address
  on a network without IPv6 (the X399) falls back to IPv4, as curl does.
  The whole set-up (address, handshake, settings, CONNECT response) has 15
  seconds.
- `NetworkTransportSessionHaiku.cpp` / `NetworkTransportStreamHaiku.cpp`
  implement WebKit's `NetworkTransportSession` and `NetworkTransportStream`
  on it, with the states and errors of the Cocoa port, so the web process
  side (`WebTransportSession`, WebCore's `WebTransport`) is unchanged.
- `defaultWebTransportEnabled()` is on for Haiku when the engine was built
  with the libraries.

Build: `Source/WebKit/PlatformHaiku.cmake` looks for ngtcp2,
`ngtcp2_crypto_ossl` and nghttp3 (air/OS ships them for its HTTP/3 curl);
without them (stock Haiku) the files build without QUIC and WebTransport
stays off. `-DHAIKU_QUIC_ROOT=DIR` points at a tree with `develop/headers`
and `develop/lib` when they are not in the sysroot: air/OS CI passes its
dependency prefix (airos-ci `summit/build-engine.sh`), where the image's
`airos_ngtcp2` and `airos_nghttp3` come from. `summit_webkit` then requires
`lib:libngtcp2`, `lib:libngtcp2_crypto_ossl` and `lib:libnghttp3`.

Traps found on the way: ngtcp2's `stream_stop_sending` callback is about
our own STOP_SENDING; the server's is `recv_stop_sending`. GCC cannot turn
a lambda with `...` into a function pointer (ngtcp2's log callback).

## Testing

The wpt webtransport tests (`tools/interop2026`, `--enable-webtransport-h3`
starts wpt's aioquic server on UDP 11000):

```sh
grep '^/webtransport/' $INTEROP_ROOT/all.txt > wt.txt
SUMMIT_REMOTE_BUILD=xw SUMMIT_REMOTE_ENV=SUMMIT_WEBTRANSPORT_TRACE=1 \
  tools/interop2026/run.sh wt.txt wt
```

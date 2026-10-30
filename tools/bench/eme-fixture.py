#!/usr/bin/env python3
"""Generate independently packaged Clear Key media; serve on trusted localhost.

Requires FFmpeg and Shaka Packager (pass --packager /path/to/packager).
Run --serve on the Haiku workstation so http://127.0.0.1:8773 is a secure
context without disabling browser security settings or installing a test CA.
"""
import argparse
import http.server
import importlib.util
import json
import pathlib
import re
import shutil
import subprocess
import urllib.parse


def generate(directory, packager, source):
    spec = importlib.util.spec_from_file_location('dash_fixture', pathlib.Path(__file__).with_name('dash-fixture.py'))
    dash = importlib.util.module_from_spec(spec); spec.loader.exec_module(dash)
    dash.generate(source)
    directory.mkdir(parents=True, exist_ok=True)
    for encrypted in [False, True]:
        prefix = '' if encrypted else 'clear-'
        streams = [f'in={source}/range-stream{index}.mp4,stream={track},init_segment={directory}/{prefix}{track}-init.mp4,segment_template={directory}/{prefix}{track}-$Number$.m4s,drm_label={label}'
            for track, index, label in [('video', 0, 'V'), ('audio', 2, 'A')]]
        command = [str(packager), *streams, '--segment_duration', '2', '--generate_static_live_mpd', '--mpd_output', str(directory / ('manifest.mpd' if encrypted else 'clear.mpd'))]
        if encrypted:
            command += ['--enable_raw_key_encryption', '--keys', 'label=V:key_id=00112233445566778899aabbccddeeff:key=000102030405060708090a0b0c0d0e0f,label=A:key_id=ffeeddccbbaa99887766554433221100:key=0f0e0d0c0b0a09080706050403020100', '--protection_systems', 'CommonSystem', '--protection_scheme', 'cenc', '--clear_lead', '0']
        subprocess.run(command, check=True)
    subprocess.run([str(packager),
        f'in={source}/range-stream0.mp4,stream=video,init_segment={directory}/iv16-video-init.mp4,segment_template={directory}/iv16-video-$Number$.m4s',
        '--enable_raw_key_encryption', '--keys', 'key_id=00112233445566778899aabbccddeeff:key=000102030405060708090a0b0c0d0e0f',
        '--protection_systems', 'CommonSystem', '--protection_scheme', 'cenc', '--clear_lead', '0',
        '--iv', '0102030405060708090a0b0c0d0e0f10', '--segment_duration', '2', '--generate_static_live_mpd', '--mpd_output', str(directory / 'iv16.mpd')], check=True)
    shutil.copyfile(pathlib.Path(__file__).parent / 'pages/eme-fixture.html', directory / 'test.html')


KEYS = 'label=V:key_id=00112233445566778899aabbccddeeff:key=000102030405060708090a0b0c0d0e0f,label=A:key_id=ffeeddccbbaa99887766554433221100:key=0f0e0d0c0b0a09080706050403020100'


def generate_schemes(directory, packager, source, ffmpeg):
    """The other protection schemes (#115): 'cbcs' with a 2 s clear lead and
    'cens', in the H.264/AAC segments of generate(), and encrypted WebM
    (VP9 subsamples, whole-frame Opus) beside the same streams in the clear,
    as one file per track and as DASH."""
    for scheme, lead in [('cbcs', '2'), ('cens', '0')]:
        streams = [f'in={source}/range-stream{index}.mp4,stream={track},init_segment={directory}/{scheme}-{track}-init.mp4,segment_template={directory}/{scheme}-{track}-$Number$.m4s,drm_label={label}'
            for track, index, label in [('video', 0, 'V'), ('audio', 2, 'A')]]
        subprocess.run([str(packager), *streams, '--segment_duration', '2', '--generate_static_live_mpd',
            '--mpd_output', str(directory / f'{scheme}.mpd'), '--enable_raw_key_encryption', '--keys', KEYS,
            '--protection_systems', 'CommonSystem', '--protection_scheme', scheme, '--clear_lead', lead], check=True)
    # Separate inputs: Shaka Packager refuses Opus blocks that start before
    # the previous video block (codec delay) in one muxed file.
    sources = {'video': directory / 'webm-source-video.webm', 'audio': directory / 'webm-source-audio.webm'}
    encode = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-y']
    subprocess.run([*encode, '-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=30:duration=12', '-c:v', 'libvpx-vp9',
        '-b:v', '600k', '-g', '60', '-deadline', 'realtime', '-cpu-used', '8', str(sources['video'])], check=True)
    subprocess.run([*encode, '-f', 'lavfi', '-i', 'sine=frequency=660:sample_rate=48000:duration=12', '-c:a', 'libopus',
        '-b:a', '96k', str(sources['audio'])], check=True)
    for encrypted in [False, True]:
        prefix = '' if encrypted else 'clear-'
        command = [str(packager), f'in={sources["video"]},stream=video,output={directory}/{prefix}webm-video.webm,drm_label=V',
            f'in={sources["audio"]},stream=audio,output={directory}/{prefix}webm-audio.webm,drm_label=A',
            '--segment_duration', '2', '--mpd_output', str(directory / f'{prefix}webm.mpd')]
        if encrypted:
            command += ['--enable_raw_key_encryption', '--keys', KEYS, '--protection_systems', 'CommonSystem', '--clear_lead', '0']
        subprocess.run(command, check=True)
    for source in sources.values():
        source.unlink()


OBSERVER_PAGE = '''<!doctype html>
<meta charset="utf-8">
<pre id="log"></pre>
<script>
window.results = { reports: [] };
const show = () => document.getElementById('log').textContent = JSON.stringify(window.results, null, 1);
new ReportingObserver((reports) => {
    for (const report of reports) window.results.reports.push({ type: report.type, url: report.url, body: report.body.toJSON() });
    show();
}, { types: ['permissions-policy-violation'] }).observe();
const feature = new URLSearchParams(location.search).get('feature');
const use = feature === 'bluetooth' ? () => navigator.bluetooth.getDevices()
    : () => navigator.requestMediaKeySystemAccess('org.w3.clearkey', [{ videoCapabilities: [{ contentType: 'video/mp4; codecs="avc1.42E01E"' }] }]);
use().then(() => { window.results.call = 'allowed'; show(); }, (error) => { window.results.call = error.name; show(); });
</script>
'''


def serve(directory, host, port):
    reports = []

    class Handler(http.server.SimpleHTTPRequestHandler):
        extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, '.mpd': 'application/dash+xml', '.m4s': 'video/iso.segment',
            '.webm': 'video/webm'}
        def __init__(self, *args, **kwargs): super().__init__(*args, directory=str(directory), **kwargs)
        def translate_path(self, path):
            if path.startswith('/cors/'): path = path[5:]
            return super().translate_path(path)
        def do_OPTIONS(self):
            # Reports are application/reports+json: a cross-origin delivery
            # is preflighted.
            self.send_response(204)
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Access-Control-Allow-Methods', 'POST')
            self.send_header('Access-Control-Allow-Headers', 'Content-Type')
            self.end_headers()
        def do_POST(self):
            # Reporting API deliveries (Permissions-Policy report-to).
            body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
            reports.append({'path': self.path, 'type': self.headers.get('Content-Type'), 'body': body.decode(errors='replace')})
            self.send_response(204)
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
        def do_GET(self):
            if urllib.parse.urlsplit(self.path).path == '/reports':
                data = json.dumps(reports).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            # Byte ranges, for SegmentBase (Shaka's WebM and single-file MP4).
            local = pathlib.Path(self.translate_path(urllib.parse.urlsplit(self.path).path))
            match = re.fullmatch(r'bytes=(\d+)-(\d*)', self.headers.get('Range', '').strip())
            if match and local.is_file():
                size = local.stat().st_size
                first = int(match.group(1))
                last = min(int(match.group(2)) if match.group(2) else size - 1, size - 1)
                if first <= last:
                    with open(local, 'rb') as stream:
                        stream.seek(first)
                        data = stream.read(last - first + 1)
                    self.send_response(206)
                    self.send_header('Content-Type', self.guess_type(str(local)))
                    self.send_header('Content-Range', f'bytes {first}-{last}/{size}')
                    self.send_header('Content-Length', str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                    return
            if urllib.parse.urlsplit(self.path).path == '/observer.html':
                # ReportingObserver view of the same violations: load with
                # ?policy=deny&feature=bluetooth[&report=1]; window.results
                # holds the bodies (and the outcome of the blocked call).
                data = OBSERVER_PAGE.encode()
                self.send_response(200)
                self.send_header('Content-Type', 'text/html; charset=utf-8')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            if urllib.parse.urlsplit(self.path).path == '/cross-clear.mpd':
                data = (directory / 'clear.mpd').read_bytes()
                prefix = f'http://localhost:{port}/'.encode()
                for attribute in [b'initialization="', b'media="']:
                    data = data.replace(attribute, attribute + prefix)
                self.send_response(200)
                self.send_header('Content-Type', 'application/dash+xml')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            super().do_GET()
        def end_headers(self):
            self.send_header('Cache-Control', 'no-store')
            if self.path.startswith('/cors/'):
                self.send_header('Access-Control-Allow-Origin', '*')
            policy = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query).get('policy', [''])[0]
            # wildcard: subdomains of localhost (secure contexts, resolved to
            # loopback by curl), not localhost itself.
            policies = {'deny': '()', 'self': '(self)', 'all': '*',
                'origins': f'(self "http://localhost:{port}")', 'other': '("https://example.invalid")',
                'path': f'("http://127.0.0.1:{port}/not-an-origin")', 'wildcard': f'("http://*.localhost:{port}")',
                'suffix': f'("http://*.com:{port}")'}
            if policy in policies:
                query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
                report = ';report-to=eme' if 'report' in query else ''
                # feature=bluetooth checks the same header parsing without EME.
                feature = 'bluetooth' if query.get('feature') == ['bluetooth'] else 'encrypted-media'
                self.send_header('Permissions-Policy', feature + '=' + policies[policy] + report)
                if report:
                    self.send_header('Reporting-Endpoints', f'eme="http://127.0.0.1:{port}/report"')
            super().end_headers()
    http.server.ThreadingHTTPServer((host, port), Handler).serve_forever()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=pathlib.Path, required=True)
    parser.add_argument('--source', type=pathlib.Path, default=pathlib.Path('.vm/issue33-fixture'))
    parser.add_argument('--packager', type=pathlib.Path)
    parser.add_argument('--schemes', action='store_true', help='also cbcs, cens and encrypted WebM (needs --packager)')
    parser.add_argument('--ffmpeg', default='ffmpeg')
    parser.add_argument('--serve', action='store_true')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8773)
    args = parser.parse_args()
    if args.packager: generate(args.directory.resolve(), args.packager.resolve(), args.source.resolve())
    if args.packager and args.schemes: generate_schemes(args.directory.resolve(), args.packager.resolve(), args.source.resolve(), args.ffmpeg)
    if args.serve: serve(args.directory.resolve(), args.host, args.port)

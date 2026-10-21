#!/usr/bin/env python3
"""Check bundled emoji through real native Canvas rendering and browser restarts.

Use the browser's usual graphics/engine environment on the remote shell. Only
the launched team and a new isolated profile are controlled by this test.
"""
import argparse
import http.server
import json
import pathlib
import shlex
import subprocess
import threading
import time
import uuid
from urllib.parse import parse_qs, urlparse

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shell', default='tools/haiku.sh')
    parser.add_argument('--host', default='10.0.2.2')
    parser.add_argument('--browser', required=True)
    parser.add_argument('--control', required=True)
    parser.add_argument('--directory', default='/SummitExtensions/summit')
    args = parser.parse_args()
    reports = {}
    condition = threading.Condition()
    results = []
    fixture = (ROOT / 'tests/fixtures/emoji.html').read_bytes()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(fixture)))
            self.end_headers()
            self.wfile.write(fixture)

        def do_POST(self):
            key = parse_qs(urlparse(self.path).query)['key'][0]
            report = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            with condition:
                reports[key] = report
                condition.notify_all()
            self.send_response(204)
            self.end_headers()

    server = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://{args.host}:{server.server_port}/emoji.html?report='
    # Exercise XML escaping in the generated fontconfig paths too.
    profile = f'{args.directory}/emoji-test-&-{uuid.uuid4().hex[:12]}'
    team = None

    def remote(command):
        return subprocess.check_output(['bash', args.shell, command], cwd=ROOT, text=True, timeout=60).strip()

    def python(script):
        return remote(shlex.join(['python3.10', '-c', script]))

    def control(*arguments):
        return remote(shlex.join([args.control, '--team', str(team), *arguments]))

    def launch(key, fontconfig=None):
        nonlocal team
        arguments = [args.browser, '--profile', profile, base + key]
        if fontconfig:
            arguments = ['env', 'FONTCONFIG_FILE=' + fontconfig, *arguments]
        command = shlex.join(arguments)
        team = int(remote(f'mkdir -p {shlex.quote(profile)}; {command} > {shlex.quote(profile + "/" + key + ".log")} 2>&1 < /dev/null & echo $!'))

    def stop():
        nonlocal team
        control('quit')
        for _ in range(60):
            if remote(f'kill -0 {team} 2>/dev/null && echo alive || echo exited') == 'exited':
                team = None
                return
            time.sleep(0.3)
        raise RuntimeError(f'Owned test browser {team} did not quit')

    def check(key):
        with condition:
            if not condition.wait_for(lambda: key in reports, timeout=60):
                raise RuntimeError(f'No emoji report for {key}; inspect {profile} (team {team})')
            report = reports[key]
        samples = report['samples']
        record = {'case': key, 'passed': False, **report}
        results.append(record)
        for name, item in samples.items():
            assert item['visible'] > 100, (key, name, item)
            # Noto's current family artwork is intentionally a gray silhouette.
            if name in ('text', 'textHeart', 'family'):
                assert item['colored'] == 0, (key, name, item)
            else:
                assert item['colored'] > 100, (key, name, item)
        # A joined/modified emoji must shape into one glyph, not several
        # individually colored symbols. The font gives these equal advances.
        for name in ('skin', 'profession', 'family', 'flag', 'keycap'):
            assert abs(samples[name]['width'] - samples['face']['width']) < 1, (key, name, samples[name])
        record['passed'] = True
        print(f'PASS {key}: emoji color, joined sequences, plain text and text presentation; dpr={report["dpr"]}', flush=True)

    crash_start = json.loads(python('import json,pathlib; p=pathlib.Path("/boot/system/var/log/syslog"); '
        's=p.stat(); print(json.dumps({"inode":s.st_ino,"offset":s.st_size}))'))
    try:
        launch('initial')
        check('initial')
        sources = json.loads((ROOT / 'resources/fonts/sources.json').read_text())
        for name in sources['files']:
            expected = sources['files'][name]['sha256']
            actual = python(f'import hashlib,pathlib; print(hashlib.sha256(pathlib.Path({profile + "/Fonts/" + name!r}).read_bytes()).hexdigest())')
            assert actual == expected, (name, actual, expected)
        # Preserve a full screenshot for native frame inspection.
        # Haiku's screenshot returns its application thread ID, not B_OK.
        # Verify that a complete PNG was saved instead of using that exit code.
        python('import pathlib,subprocess; '
            f'p=pathlib.Path({profile + "/emoji.png"!r}); '
            'subprocess.run(["screenshot","-s","-f","png",str(p)]); '
            'data=p.read_bytes(); assert data.startswith(b"\\x89PNG\\r\\n\\x1a\\n") '
            'and data.endswith(b"IEND\\xaeB`\\x82")')
        control('newprivatewindow', base + 'private')
        check('private')
        stop()
        python(f'import pathlib; pathlib.Path({profile + "/Fonts/Noto-COLRv1.ttf"!r}).write_bytes(b"corrupt cached font")')
        launch('repaired')
        check('repaired')
        stop()
        actual = python(f'import hashlib,pathlib; print(hashlib.sha256(pathlib.Path({profile + "/Fonts/Noto-COLRv1.ttf"!r}).read_bytes()).hexdigest())')
        assert actual == sources['files']['Noto-COLRv1.ttf']['sha256']
        print('PASS embedded font/license hashes, escaped paths and corrupt-font recovery', flush=True)
        custom = profile + '/original-fonts.conf'
        config = ('<?xml version="1.0"?><fontconfig>'
            '<include>/boot/system/settings/fonts/fonts.conf</include>'
            '<alias><family>SummitFixtureMono</family><prefer><family>Noto Sans Mono</family></prefer></alias>'
            '</fontconfig>')
        python(f'import pathlib; pathlib.Path({custom!r}).write_text({config!r})')
        launch('custom-config', custom)
        check('custom-config')
        matched = remote(shlex.join(['env', 'FONTCONFIG_FILE=' + profile + '/Fonts/fonts.conf',
            'fc-match', '-f', '%{family}', 'SummitFixtureMono']))
        assert matched == 'Noto Sans Mono', matched
        stop()
        print('PASS inherited fontconfig aliases remain available', flush=True)
        time.sleep(1)
        crash = json.loads(python('import json,pathlib,re; p=pathlib.Path("/boot/system/var/log/syslog"); '
            f's=p.stat(); f=p.open("rb"); f.seek({crash_start["offset"]}); text=f.read().decode(errors="replace"); '
            f'pathlib.Path({profile + "/native-syslog.log"!r}).write_text(text); '
            f'print(json.dumps({{"coverage":s.st_ino=={crash_start["inode"]} and s.st_size>={crash_start["offset"]}, '
            '"events":[x for x in text.splitlines() if re.search(r"debug_server: Thread [0-9]+ entered|DEBUGGER:|PANIC:",x)]}))'))
        assert crash['coverage'] and not crash['events'], crash
        print('PASS no fresh native debugger events', flush=True)
    finally:
        if team is not None:
            try:
                control('quit')
            except (subprocess.SubprocessError, OSError):
                pass
        server.shutdown()
        print(json.dumps({'profile': profile, 'results': results}, indent=2))


if __name__ == '__main__':
    main()

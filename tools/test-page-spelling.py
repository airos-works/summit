#!/usr/bin/env python3
"""Native page-language spelling checks, with private dictionaries and loopback pages."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from native_crash_log import NativeCrashLog

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
for name in ('probe', 'bundle', 'images-helper', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
a = parser.parse_args()
if not sys.platform.startswith('haiku'):
    parser.error('Requires native Haiku')
for name in ('probe', 'bundle', 'images_helper'):
    setattr(a, name, getattr(a, name).resolve(strict=True))
a.output.mkdir(mode=0o700)
dictionaries = a.output / 'dictionaries'
dictionaries.mkdir()
for name, words in {'en_US': ['hello', 'world', 'color'], 'de_DE': ['hallo', 'welt', 'farbe'],
                    'fr_FR': ['bonjour', 'monde', 'couleur']}.items():
    (dictionaries / (name + '.aff')).write_text('SET UTF-8\nTRY abcdefghijklmnopqrstuvwxyz\n')
    (dictionaries / (name + '.dic')).write_text(str(len(words)) + '\n' + '\n'.join(words) + '\n')
events = []


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        events.append(self.path)
        language = {'/de': 'de', '/input': 'de', '/password': 'de', '/disabled': 'de',
                    '/empty': 'fr', '/missing': 'qaa', '/invalid': '../../fr_FR'}.get(self.path, 'en-US')
        attrs = 'id=edit spellcheck=true'
        if self.path == '/field':
            attrs += ' lang=fr'
        if self.path == '/empty':
            attrs += ' lang=""'
        if self.path == '/disabled':
            attrs = 'id=edit spellcheck=false'
        editor = f'<textarea {attrs}></textarea>'
        if self.path == '/input':
            editor = f'<input type=text {attrs}>'
        if self.path == '/password':
            editor = f'<input type=password {attrs}>'
        if self.path == '/editable':
            editor = f'<div {attrs} contenteditable lang=fr></div>'
        if self.path == '/ancestor':
            editor = '<section lang=fr>' + editor + '</section>'
        if self.path == '/frame':
            editor += '<iframe id=child src=/de style="position:absolute;left:0;top:110px;width:620px;height:200px;border:0"></iframe>'
        data = (f'<!doctype html><html lang="{language}"><meta charset=utf-8><title>Spelling fixture</title>'
                '<style>body{margin:0}#edit{position:absolute;left:20px;top:20px;width:540px;height:50px;'
                'box-sizing:border-box;padding:5px;border:1px solid #888;font:24px monospace;white-space:pre}</style>'
                + editor).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(data)


def sha(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def members(group):
    result = []
    for line in subprocess.check_output(['ps', '-o', 'Id'], text=True).splitlines()[1:]:
        if not line.strip():
            continue
        pid = int(line.strip())
        try:
            if os.getpgid(pid) == group:
                result.append(pid)
        except ProcessLookupError:
            pass
    return result


server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = f'http://127.0.0.1:{server.server_port}'
engine_files = [a.bundle / name for name in ('WebProcess', 'NetworkProcess',
                                           'lib/libWebKit.so.1', 'lib/libJavaScriptCore.so.18')]
inputs = [a.probe, a.images_helper, Path(__file__), ROOT / 'tools/native_crash_log.py',
          ROOT / 'tests/PageSpellingTests.cpp', ROOT / 'tests/ModernPDFTests.cpp', ROOT / 'vendor/nlohmann/json.hpp',
          *engine_files, *dictionaries.glob('*.aff'), *dictionaries.glob('*.dic')]
report = {'passed': False, 'events': events, 'phases': []}


def phase(name):
    directory = a.output / name
    directory.mkdir()
    record = {'phase': name, 'passed': False, 'forced_cleanup': False,
              'sha256': {str(path): sha(path) for path in inputs}}
    report['phases'].append(record)
    monitor = NativeCrashLog()
    proc = None
    try:
        env = dict(os.environ, WEBKIT_EXEC_PATH=str(a.bundle),
                   LIBRARY_PATH=f'{a.bundle}/lib:{a.bundle}/mesa/lib:/boot/system/lib',
                   __EGL_VENDOR_LIBRARY_FILENAMES=str(a.bundle / 'mesa/egl_vendor.d/50_mesa.json'))
        env.pop('LD_PRELOAD', None)
        record['sha256'][env['__EGL_VENDOR_LIBRARY_FILENAMES']] = sha(env['__EGL_VENDOR_LIBRARY_FILENAMES'])
        log_path = directory / 'native.log'
        with log_path.open('w') as log:
            proc = subprocess.Popen([str(a.probe), base, str(a.bundle), str(directory / 'begin'),
                                     str(dictionaries), str(directory), name], env=env,
                                    stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            record['team'] = proc.pid
            deadline = time.monotonic() + 60
            while 'PAGE_SPELLING_READY' not in log_path.read_text():
                if proc.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Probe did not reach its initial documents')
                time.sleep(0.1)
            images = {}
            for pid in members(proc.pid):
                result = subprocess.run([str(a.images_helper), str(pid)], capture_output=True, text=True)
                if result.returncode == 0:
                    images[str(pid)] = result.stdout.splitlines()
                    for path in images[str(pid)]:
                        if path != 'commpage' and path not in record['sha256']:
                            record['sha256'][path] = sha(path)
            record['loaded_images'] = images
            loaded = {record['sha256'][p] for paths in images.values() for p in paths if p != 'commpage'}
            if not all(record['sha256'][str(p)] in loaded for p in engine_files):
                raise RuntimeError('Not all four selected engine images were loaded')
            (directory / 'begin').touch()
            record['exit'] = proc.wait(timeout=300)
        deadline = time.monotonic() + 15
        while members(proc.pid) and time.monotonic() < deadline:
            time.sleep(0.1)
        record['remaining_processes'] = members(proc.pid)
        record['passed'] = (record['exit'] == 0 and 'PAGE_SPELLING_RESULT PASS' in log_path.read_text()
                            and not record['remaining_processes'])
    except Exception as error:
        record['error'] = repr(error)
        if proc:
            record['exit'] = proc.poll()
    finally:
        if proc and (proc.poll() is None or members(proc.pid)):
            record['forced_cleanup'] = True
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait(timeout=10)
        record['inputs_unchanged'] = all(sha(path) == digest for path, digest in record['sha256'].items())
        record['native_crash_log'] = monitor.finish()
        if not record['inputs_unchanged'] or record['forced_cleanup'] or not record['native_crash_log']['passed']:
            record['passed'] = False
    return record['passed']


try:
    report['passed'] = phase('checks') and phase('verify-learned')
finally:
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)
    (a.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'phases': [
        {k: item[k] for k in ('phase', 'passed', 'forced_cleanup', 'inputs_unchanged', 'native_crash_log')}
        for item in report['phases']]}), flush=True)
sys.exit(0 if report['passed'] else 1)

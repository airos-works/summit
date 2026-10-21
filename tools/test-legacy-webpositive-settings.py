#!/usr/bin/env python3
"""Exercise installed WebPositive's font preferences in an owned Haiku VM.

Preserves/restores Application and Session; normal history/cache writes remain.
Uses an isolated matching engine and shim. Run serially with other native tests.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse
from native_crash_log import NativeCrashLog

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--browser', type=Path, default=Path('/boot/system/apps/WebPositive'))
for name in ('library', 'bundle', 'ui-helper', 'images-helper', 'egl-vendor', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
a = parser.parse_args()
if not sys.platform.startswith('haiku'):
    parser.error('Requires native Haiku')
for name in ('browser', 'library', 'bundle', 'ui_helper', 'images_helper', 'egl_vendor'):
    setattr(a, name, getattr(a, name).resolve(strict=True))
assert subprocess.run(['hey', 'WebPositive', 'get', 'Name'], capture_output=True).returncode != 0, \
    'WebPositive already running'
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


events, metrics, lock = [], {}, threading.Lock()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        url = urlparse(self.path)
        if url.path == '/metrics':
            data = json.loads(parse_qs(url.query)['data'][0])
            with lock:
                metrics[data['path']] = data
                events.append(data)
            self.send_response(204)
            self.end_headers()
            return
        if url.path == '/favicon.ico':
            self.send_response(204)
            self.end_headers()
            return
        body = b'''<!doctype html><title>Legacy child settings</title>
<style>body{background:rgb(66,140,222)}.fixed{font-family:monospace}</style>
<p><span id="standard">iiiiiiiiWWWWWWWW</span></p>
<p><span id="fixed" class="fixed">iiiiiiiiWWWWWWWW</span></p>
<script>
document.title += ' '+location.pathname;
let previous='';
function measure() {
 const data={path:location.pathname};
 for (const id of ['standard','fixed']) {
  const e=document.getElementById(id),s=getComputedStyle(e);
  const r=document.createElement('span');r.textContent=e.textContent;
  r.style.fontSize=s.fontSize;document.body.append(r);
  const refs={};
  for(const family of ['Noto Serif','Noto Sans','Noto Sans Mono']) {
   r.style.fontFamily=family;refs[family]=r.getBoundingClientRect().width;
  }
  data[id]={size:parseFloat(s.fontSize),width:e.getBoundingClientRect().width,refs};
  r.remove();
 }
 const value=JSON.stringify(data);
 if(value!==previous){previous=value;fetch('/metrics?data='+encodeURIComponent(value));}
}
measure();setInterval(measure,100);
</script>'''
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)


server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
server.daemon_threads = True
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = f'http://127.0.0.1:{server.server_port}'
report = {'passed': False, 'checks': [], 'events': events, 'forced_cleanup': False,
          'bundle': str(a.bundle), 'runs': [], 'native_scripting': [],
          'platform': subprocess.check_output(['uname', '-a'], text=True).strip()}
inputs = [a.browser, a.library, a.ui_helper, a.images_helper, a.egl_vendor, Path(__file__),
          ROOT / 'tools/native_crash_log.py', ROOT / 'tests/LegacyWebPositiveUITests.cpp']
inputs += [a.bundle / name for name in ('WebProcess', 'NetworkProcess',
                                       'lib/libWebKit.so.1', 'lib/libJavaScriptCore.so.18')]
report['sha256'] = {str(path): sha(path) for path in inputs}
monitor, proc, backups = NativeCrashLog(), None, {}
profile = Path.home() / 'config/settings/WebPositive'


def check(value, label):
    report['checks'].append({'passed': bool(value), 'label': label})
    if not value:
        raise AssertionError(label)


def wait(predicate, label):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if predicate():
            check(True, label)
            return
        if proc.poll() is not None:
            raise RuntimeError('WebPositive exited early')
        time.sleep(0.1)
    check(False, label)


def hey(*args, required=True):
    result = subprocess.run(['hey', str(proc.pid), *map(str, args)],
                            capture_output=True, text=True, timeout=5)
    report['native_scripting'].append({'args': args, 'exit': result.returncode,
                                       'stdout': result.stdout, 'stderr': result.stderr})
    if result.returncode and required:
        raise RuntimeError('Native scripting failed: ' + result.stdout + result.stderr)
    return result.stdout


def number(*args):
    text = hey(*args)
    match = re.search(r'result.*?:\s*(-?\d+)', text)
    if not match:
        raise RuntimeError('No integer scripting result: ' + text)
    return int(match[1])


def navigate(path):
    hey('_ARG', 'with', 'argc=int32(2)', 'and', 'argv=' + str(a.browser),
        'and', 'argv=' + base + path)


def latest(path):
    with lock:
        return metrics.get(path)


def matches(path, standard, fixed, families=False):
    data = latest(path)
    if not data or data['standard']['size'] != standard or data['fixed']['size'] != fixed:
        return False
    if families:
        for id, family in (('standard', 'Noto Serif'), ('fixed', 'Noto Sans Mono')):
            font = data[id]
            if abs(font['width'] - font['refs'][family]) > 0.1:
                return False
            if abs(font['refs'][family] - font['refs']['Noto Sans']) < 1:
                return False
    return True


def inventory():
    images = {}
    for pid in members(proc.pid):
        result = subprocess.run([str(a.images_helper), str(pid)], capture_output=True, text=True)
        if result.returncode == 0:
            images[str(pid)] = result.stdout.splitlines()
            for path in images[str(pid)]:
                if path != 'commpage' and path not in report['sha256']:
                    report['sha256'][path] = sha(path)
    report['runs'][-1]['loaded_images'] = images
    return images


def launch(path):
    global proc
    env = dict(os.environ, WEBKIT_EXEC_PATH=str(a.bundle),
               LIBRARY_PATH=f'{a.library.parent}:{a.bundle}/lib:{a.bundle}/mesa/lib:/boot/system/lib',
               __EGL_VENDOR_LIBRARY_FILENAMES=str(a.egl_vendor))
    env.pop('LD_PRELOAD', None)
    with (a.output / ('native-' + str(len(report['runs']) + 1) + '.log')).open('w') as log:
        proc = subprocess.Popen([str(a.browser), base + path], env=env,
                                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    report['runs'].append({'team': proc.pid})
    wait(lambda: latest(path), 'installed WebPositive loads ' + path)
    images = inventory()
    for suffix, selected in (('/libWebKit.so.1', a.bundle / 'lib/libWebKit.so.1'),
                             ('/libWebKitLegacy.so.1', a.library)):
        check(any(p.endswith((suffix, suffix + '.10.0'))
                  and report['sha256'][p] == report['sha256'][str(selected)]
                  for paths in images.values() for p in paths), 'selected ' + suffix + ' is loaded')


def quit_browser():
    inventory()
    subprocess.run([str(a.ui_helper), str(proc.pid), 'quit'], check=True, timeout=10)
    run = report['runs'][-1]
    run['exit'] = proc.wait(timeout=25)
    deadline = time.monotonic() + 15
    while members(proc.pid) and time.monotonic() < deadline:
        time.sleep(0.1)
    run['remaining_processes'] = members(proc.pid)
    if run['remaining_processes']:
        run['remaining_ps'] = subprocess.check_output(['ps'], text=True)
    check(run['exit'] == 0 and not run['remaining_processes'], 'WebPositive and helpers exit normally')


try:
    for name in ('Application', 'Session'):
        path = profile / name
        backup = a.output / (name + '.original')
        backups[name] = {'sha256': sha(path) if path.exists() else None}
        if path.exists():
            shutil.copy2(path, backup)
        if name == 'Session' and path.exists():
            path.unlink()
    report['profile_backups'] = backups
    launch('/initial')
    initial = latest('/initial')
    hey('sswd')
    hey('set', 'Selection', 'of', 'View', 'settings pages', 'of', 'Window', 'Settings', 'to', 'int32(1)')
    for id, control in (('standard', 'standard font size'), ('fixed', 'fixed font size')):
        value = number('get', 'Value', 'of', 'View', control, 'of', 'Window', 'Settings')
        check(value == initial[id]['size'], 'initial page uses the saved ' + id + ' font size')
    for value, control in ((30, 'standard font size'), (21, 'fixed font size')):
        hey('set', 'Value', 'of', 'View', control, 'of', 'Window', 'Settings', 'to', f'int32({value})')
    count = number('count', 'Handler', 'of', 'Window', 'Settings')
    handlers = {}
    for index in range(count):
        # BHandler exposes InternalName; unnamed layout handlers can decline it.
        text = hey('get', 'InternalName', 'of', 'Handler', index, 'of', 'Window', 'Settings', required=False)
        for name in ('standard', 'fixed'):
            if re.search(r'"' + name + r'"', text):
                handlers[name] = index
        if len(handlers) == 2:
            break
    check(set(handlers) == {'standard', 'fixed'}, 'real font selection handlers are available')
    # WebPositive's fixed-font menu accepts only native fixed-width families.
    for name, family in (('standard', 'Noto Serif'), ('fixed', 'Noto Sans Mono')):
        hey('fmly', 'Handler', handlers[name], 'of', 'Window', 'Settings', 'with', 'family=' + family)
    navigate('/staged')
    wait(lambda: latest('/staged'), 'new page loads before Apply')
    check(all(latest('/staged')[id] == initial[id] for id in ('standard', 'fixed')),
          'editing preferences alone leaves page settings unchanged')
    hey('aply', 'Window', 'Settings')
    wait(lambda: matches('/staged', 30, 21, True), 'Apply updates existing page sizes and font families')
    navigate('/new')
    wait(lambda: matches('/new', 30, 21, True), 'new page inherits applied sizes and font families')
    hey('set', 'Value', 'of', 'View', 'standard font size', 'of', 'Window', 'Settings', 'to', 'int32(44)')
    hey('rvrt', 'Window', 'Settings')
    check(number('get', 'Value', 'of', 'View', 'standard font size', 'of', 'Window', 'Settings') == 30,
          'Revert restores the last applied preference')
    hey('set', 'Value', 'of', 'View', 'standard font size', 'of', 'Window', 'Settings', 'to', 'int32(26)')
    hey('aply', 'Window', 'Settings')
    wait(lambda: matches('/new', 26, 21, True), 'a second Apply updates the existing page')
    quit_browser()
    # Preserve the test-created saved tabs for the ordinary restart path.
    launch('/restarted')
    wait(lambda: matches('/restarted', 26, 21, True), 'application restart restores applied fonts before page load')
    quit_browser()
    report['passed'] = True
except Exception as error:
    report['error'] = repr(error)
    if proc and proc.poll() is None:
        subprocess.run([str(a.ui_helper), str(proc.pid), 'capture', str(a.output / 'failure.ppm')],
                       capture_output=True, timeout=15)
finally:
    if proc and (proc.poll() is None or members(proc.pid)):
        report['forced_cleanup'] = True
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait(timeout=10)
    for name, saved in backups.items():
        path = profile / name
        if path.exists():
            shutil.copy2(path, a.output / (name + '.test'))
        if saved['sha256'] is None:
            path.unlink(missing_ok=True)
        else:
            shutil.copy2(a.output / (name + '.original'), path)
        saved['restored'] = (sha(path) if path.exists() else None) == saved['sha256']
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)
    report['native_crash_log'] = monitor.finish()
    report['inputs_unchanged'] = all(sha(path) == digest for path, digest in report['sha256'].items())
    report['passed'] = (report['passed'] and report['inputs_unchanged']
                        and all(v['restored'] for v in backups.values()) and report['native_crash_log']['passed'])
    (a.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: report.get(key) for key in ('passed', 'forced_cleanup', 'error')}))
sys.exit(0 if report['passed'] else 1)

#!/usr/bin/env python3
"""Exercise the real Inspector CPU/Memory domains through a native browser's MCP.

Uses a new profile, loopback fixture and private token. Run serially with other
native tests. --expect-unavailable records the published baseline's missing
domains. The output directory must not already exist.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hashlib
import json
import math
import os
from pathlib import Path
import secrets
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time
import traceback
import urllib.request

from native_crash_log import NativeCrashLog

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def group_members(group):
    lines = subprocess.check_output(['ps', '-o', 'Id'], text=True).splitlines()
    if not lines or lines[0].strip() != 'Id':
        raise RuntimeError('Cannot enumerate native process groups')
    members = []
    for line in lines[1:]:
        pid = int(line.strip())
        try:
            if os.getpgid(pid) == group:
                members.append(pid)
        except ProcessLookupError:
            pass
    return members


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--close-helper', type=Path, required=True)
    parser.add_argument('--images-helper', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expect-unavailable', action='store_true')
    args = parser.parse_args()
    if not sys.platform.startswith('haiku'):
        parser.error('Requires native Haiku')
    args.output.mkdir(mode=0o700)
    for name in ('browser', 'close_helper', 'images_helper'):
        setattr(args, name, getattr(args, name).resolve(strict=True))
    fixture = ROOT / 'tests/fixtures/resource-usage.html'
    inputs = [Path(__file__), ROOT / 'tools/native_crash_log.py', fixture,
              args.close_helper, args.images_helper]
    report = {'platform': subprocess.check_output(['uname', '-a'], text=True).strip(),
              'browser': str(args.browser), 'expect_unavailable': args.expect_unavailable,
              'input_sha256': {str(p): digest(p) for p in inputs}, 'recordings': {},
              'checks': [], 'passed': False, 'forced_cleanup': False}
    runtime = {str(args.browser): digest(args.browser)}
    token = secrets.token_hex(32)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            data = fixture.read_bytes()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    def save():
        (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')

    def check(condition, description):
        report['checks'].append({'description': description, 'passed': bool(condition)})
        save()
        if not condition:
            raise AssertionError(description)

    def call(name, **arguments):
        request = urllib.request.Request(f'http://127.0.0.1:{port}/mcp',
            data=json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call',
                'params': {'name': name, 'arguments': arguments}}).encode(),
            headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=75) as response:
            result = json.load(response)
        if 'error' in result or result.get('result', {}).get('isError'):
            raise RuntimeError('MCP failed: ' + str(result))
        return json.loads(result['result']['content'][0]['text'])

    def evaluate(expression):
        result = call('evaluate', expression=expression)
        if 'exception' in result or 'value' not in result:
            raise RuntimeError('Evaluation failed: ' + str(result))
        return result['value']

    def record(name, **options):
        result = call('resource_usage', seconds=3, **options)
        report['recordings'][name] = result
        check(not result.get('stopErrors') and result['processChanged'] is False, name + ': stopped in original process')
        for key in ('cpu', 'memory'):
            enabled = options.get(key, True)
            samples = result[key]
            check(len(samples) >= 4 if enabled else not samples, name + ': ' + key + ' sample count')
            times = [s['timestamp'] for s in samples]
            check(all(math.isfinite(t) for t in times) and all(b > a for a, b in zip(times, times[1:])),
                  name + ': ' + key + ' monotonic timestamps')
        for sample in result['cpu']:
            check(math.isfinite(sample['usage']) and sample['usage'] >= 0, name + ': valid process CPU')
            threads = sample.get('threads', [])
            check(sum(t.get('type') == 'main' for t in threads) == 1, name + ': exactly one main thread')
            check(all(math.isfinite(t['usage']) and 0 <= t['usage'] <= 100 for t in threads), name + ': bounded thread CPU')
            check(not any('ResourceUsage' in t['name'] for t in threads), name + ': collector excluded')
        for sample in result['memory']:
            categories = sample['categories']
            check({c['type'] for c in categories} == {'javascript', 'jit', 'images', 'layers', 'page', 'other'},
                  name + ': complete memory categories')
            check(all(math.isfinite(c['size']) and 0 <= c['size'] < 64 * 1024**3 for c in categories),
                  name + ': finite memory without unsigned wraparound')
        save()
        return result

    def javascript_size(result):
        return statistics.median(next(c['size'] for c in s['categories'] if c['type'] == 'javascript')
                                 for s in result['memory'])

    monitor = NativeCrashLog()
    browser = None
    log = (args.output / 'browser.log').open('w')
    try:
        environment = dict(os.environ, SUMMIT_MCP='1', SUMMIT_MCP_LOCAL='1', SUMMIT_MCP_TOKEN=token,
                           SUMMIT_MCP_PORT=str(port), SUMMIT_QUIT_TRACE='1')
        browser = subprocess.Popen([str(args.browser), '--profile', str(args.output / 'profile'),
            f'http://127.0.0.1:{server.server_port}/'], env=environment, stdout=log,
            stderr=subprocess.STDOUT, start_new_session=True)
        report['team'] = browser.pid
        save()
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            try:
                if evaluate('!!window.resourceProbe'):
                    break
            except Exception:
                pass
            if browser.poll() is not None:
                raise RuntimeError('Browser exited before fixture readiness')
            time.sleep(.1)
        else:
            raise RuntimeError('Fixture readiness deadline')

        if args.expect_unavailable:
            report['domain_errors'] = {}
            for domain in ('CPUProfiler', 'Memory'):
                try:
                    call('inspector', method=domain + '.startTracking')
                except RuntimeError as error:
                    report['domain_errors'][domain] = str(error)
                check(f"'{domain}' domain was not found" in report['domain_errors'].get(domain, ''),
                      domain + ' unavailable in baseline')
        else:
            try:
                call('resource_usage', cpu=False, memory=False)
            except RuntimeError as error:
                check('Select CPU, memory, or both' in str(error), 'empty recording rejected clearly')
            else:
                check(False, 'empty recording rejected clearly')
            idle = record('idle')
            check(evaluate('resourceProbe.startWorkers(2)') == 2, 'two workers started')
            busy = record('two_workers', memory=False)
            idle_cpu = statistics.median(s['usage'] for s in idle['cpu'][1:])
            busy_cpu = statistics.median(s['usage'] for s in busy['cpu'][1:])
            report['cpu_medians'] = {'idle': idle_cpu, 'two_workers': busy_cpu}
            check(busy_cpu > idle_cpu + 50, 'CPU responds to worker load')
            check(any(s['usage'] > 100 for s in busy['cpu'][1:]), 'multicore CPU can exceed 100 percent')
            check(any(sum(t.get('type') == 'webkit' and bool(t.get('targetId')) and t['usage'] > 20
                          for t in s['threads']) >= 2 for s in busy['cpu'][1:]), 'both loaded workers have target identities')
            check(evaluate('resourceProbe.stopWorkers()'), 'workers terminated')
            before = record('before_allocation', cpu=False)
            allocation = evaluate('resourceProbe.allocate(32)')
            check(allocation == {'bytes': 32 * 1024**2, 'first': 173, 'last': 173}, '32 MiB allocated and touched')
            allocated = record('allocated', cpu=False)
            check(any(c['size'] > 0 for s in allocated['memory'] for c in s['categories'] if c['type'] == 'jit'),
                  'committed JIT memory is reported')
            growth = javascript_size(allocated) - javascript_size(before)
            report['javascript_growth_bytes'] = growth
            check(growth >= 28 * 1024**2, 'JavaScript category accounts for retained allocation')
            check(evaluate('resourceProbe.release()'), 'allocation released')
            call('inspector', method='Heap.gc')
            after = record('after_release')
            check(javascript_size(allocated) - javascript_size(after) >= 24 * 1024**2, 'JavaScript memory drops after GC')
            check(statistics.median(s['usage'] for s in after['cpu'][1:]) < busy_cpu / 2, 'CPU drops after workers stop')
            for key in ('cpu', 'memory'):
                check(after[key][0]['timestamp'] > idle[key][-1]['timestamp'], key + ' restart has no old events')
            call('page_detach')
            reattached = record('reattached')
            check(bool(reattached['cpu']) and bool(reattached['memory']), 'recording works after detach and reattach')
            with ThreadPoolExecutor(max_workers=1) as pool:
                pending = pool.submit(call, 'resource_usage', seconds=4, memory=False)
                time.sleep(.75)
                try:
                    call('resource_usage', seconds=1)
                except RuntimeError as error:
                    check('already has a resource recording' in str(error), 'overlapping recording rejected')
                else:
                    check(False, 'overlapping recording rejected')
                report['recordings']['concurrent_owner'] = pending.result(timeout=20)
                check(len(report['recordings']['concurrent_owner']['cpu']) >= 5,
                      'rejected overlap does not stop the original recording')
            time.sleep(1)
            sampler = call('threads', tab=reattached['tab'], filter='*ResourceUsage*', sample_ms=1000)
            report['sampler_after_stop'] = sampler
            check(len(sampler['threads']) == 1, 'recordings reuse one collector thread')
            check(sampler['threads'][0]['cores'] <= .001 and sampler['threads'][0]['state'] == 'waiting',
                  'collector waits without polling when no observers remain')

        report['loaded_images'] = {}
        for team in group_members(browser.pid):
            images = subprocess.run([str(args.images_helper), str(team)], capture_output=True, text=True)
            if images.returncode:
                continue
            paths = images.stdout.splitlines()
            report['loaded_images'][str(team)] = paths
            for path in paths:
                if path != 'commpage' and path not in runtime:
                    runtime[path] = digest(path)
        closed = subprocess.run([str(args.close_helper), '--close-browser', str(browser.pid),
                                 str(args.browser)], timeout=10)
        report['close_exit'] = closed.returncode
        report['browser_exit'] = browser.wait(timeout=25)
        deadline = time.monotonic() + 15
        while group_members(browser.pid) and time.monotonic() < deadline:
            time.sleep(.1)
        report['remaining_processes'] = group_members(browser.pid)
        check(closed.returncode == report['browser_exit'] == 0 and not report['remaining_processes'],
              'clean browser and helper shutdown')
        report['passed'] = True
    except Exception as error:
        report['error'] = type(error).__name__ + ': ' + str(error)
        report['traceback'] = traceback.format_exc()
    finally:
        if browser and (browser.poll() is None or group_members(browser.pid)):
            report['forced_cleanup'] = True
            os.killpg(browser.pid, signal.SIGKILL)
            browser.wait(timeout=10)
        log.close()
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
        report['native_crash_log'] = monitor.finish()
        report['input_unchanged'] = all(digest(p) == expected for p, expected in report['input_sha256'].items())
        report['runtime_sha256'] = runtime
        report['runtime_unchanged'] = all(digest(p) == expected for p, expected in runtime.items())
        report['passed'] = (report['passed'] and report['native_crash_log']['passed']
                            and report['input_unchanged'] and report['runtime_unchanged'])
        save()
    print(json.dumps({'passed': report['passed'], 'checks': len(report['checks']), 'error': report.get('error')}), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())

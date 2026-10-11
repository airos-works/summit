#!/usr/bin/env python3
"""Run one page from tools/bench/pages/ and keep the JSON it reports.

The default page, probe.html, measures port-level latencies (timers, tasks, rAF
cadence, frame hand-off, JIT warm-up); scroll.html measures scrolling on a
content-heavy page. Both post their result to the bench server, which stores it
in .vm/bench/<run-id>/probe.json. These are diagnostics for docs/performance.md,
not benchmarks: compare runs of the same page only.

  python3 tools/bench/run-probe.py [--bundle GUEST_PATH] [--env NAME=VALUE ...]
  SUMMIT_BENCH_HOST=workstation python3 tools/bench/run-probe.py --page scroll.html
  SUMMIT_BENCH_HOST=workstation python3 tools/bench/run-probe.py --page scroll.html --browser firefox
"""
import argparse
import json
import pathlib
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import guest  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--bundle', default=guest.DEFAULT_BUNDLE)
    parser.add_argument('--port', type=int, default=8931)
    parser.add_argument('--timeout', type=int, default=600)
    parser.add_argument('--page', default='probe.html', help='a file in tools/bench/pages/, with an optional query')
    parser.add_argument('--browser', choices=['summit', 'firefox'], default='summit')
    parser.add_argument('--window', default='4,1,1270,797' if guest.IS_VM else '4,1,1916,1076')
    parser.add_argument('--env', action='append', default=[], metavar='NAME=VALUE')
    parser.add_argument('--haiku-profile', action='store_true',
                        help='run Summit under the Haiku sampling profiler and save profile.txt')
    parser.add_argument('--label', default='')
    parser.add_argument('--sample-interval', type=float, default=0, metavar='SECONDS',
                        help='sample owned memory and CPU during the probe (0 disables)')
    parser.add_argument('--owned-kernel-worker', action='append', default=[], metavar='NAME',
                        help='exact worker name reserved for this isolated run; attributed only during owned Summit samples')
    args = parser.parse_args()
    if args.haiku_profile and guest.HOST == 'workstation':
        parser.error('--haiku-profile is disabled on the workstation because Haiku profile shutdown can hang the machine')
    if args.sample_interval < 0 or 0 < args.sample_interval < 1:
        parser.error('--sample-interval must be 0 or at least 1 second')
    if args.sample_interval and args.browser != 'summit':
        parser.error('--sample-interval requires an owned Summit process group')
    if args.owned_kernel_worker and (not args.sample_interval or args.browser != 'summit'):
        parser.error('--owned-kernel-worker requires owned Summit CPU sampling')
    if any(not name or name.startswith('idle thread ') for name in args.owned_kernel_worker):
        parser.error('--owned-kernel-worker requires a non-idle worker name')

    run_id = (time.strftime('probe-%Y%m%d-%H%M%S') + f'-{args.browser}'
              + (f'-{args.label}' if args.label else ''))
    directory = guest.ROOT / '.vm/bench' / run_id
    directory.mkdir(parents=True)
    guest_dir = f'{guest.GUEST_ROOT}/runs/{run_id}'
    ctl = guest.ensure_ctl()
    guest.require_volume()
    if args.browser == 'summit':
        guest.wait_for_free_gui(ctl, args.bundle)
    before = guest.load_report(ctl, (), 3000)
    watch = guest.CrashWatch()
    server = subprocess.Popen([sys.executable, str(guest.BENCH / 'serve-speedometer.py'), '--port', str(args.port),
                               '--bind', guest.SERVER_BIND, '--out-dir', str(directory)],
                              stdout=(directory / 'server.log').open('w'), stderr=subprocess.STDOUT)
    time.sleep(1)
    if server.poll() is not None:
        raise RuntimeError(f"Fixture server exited before browser launch; see {directory / 'server.log'}")
    guest.wake_display()
    team = group = None
    shutdown = None
    leftovers = []
    outcome = 'timeout'
    samples = []
    url = f'http://{guest.HOST_ADDRESS}:{args.port}/__bench/pages/{args.page}'
    try:
        if args.browser == 'firefox':
            guest.ssh(f'mkdir -p {guest_dir}/profile')
            guest.ssh(f'/boot/system/apps/Firefox/Firefox --no-remote --profile {guest_dir}/profile '
                      f'"{url}" > {guest_dir}/browser.log 2>&1 &')
        else:
            wrapper = ('profile', '-f', '-S', '-o', f'{guest_dir}/profile.txt') if args.haiku_profile else ()
            group = guest.launch(args.bundle, f'{guest_dir}/profile', 'summit:home', f'{guest_dir}/browser.log',
                                 dict(item.split('=', 1) for item in args.env), wrapper)
            team = guest.find_browser(ctl, group)
            guest.ctl(ctl, team, 'frame', *args.window.split(','))
            guest.ctl(ctl, team, 'sidebar')
            time.sleep(3)
            guest.ctl(ctl, team, 'navigate', url)
        deadline = time.time() + args.timeout
        next_sample = time.monotonic() + args.sample_interval
        next_wake = time.monotonic() + 30
        while time.time() < deadline:
            time.sleep(min(5, args.sample_interval) if args.sample_interval else 5)
            # The blanker comes back on every idle period, and a blanked screen
            # stops app_server drawing, which is what the page is measuring.
            if time.monotonic() >= next_wake:
                guest.wake_display()
                next_wake = time.monotonic() + 30
            if args.sample_interval and time.monotonic() >= next_sample:
                members = guest.members(ctl, group)
                samples.append(guest.load_report(ctl, [pid for pid, _ in members], 1000,
                                                 args.owned_kernel_worker))
                (directory / 'samples.json').write_text(json.dumps(samples, indent=1))
                next_sample = time.monotonic() + args.sample_interval
            if (directory / 'probe.json').exists():
                outcome = 'completed'
                break
            if team is not None and not guest.alive(team):
                outcome = 'browser-exited'
                break
        time.sleep(1)
        guest.screenshot(directory / 'final.png')
    finally:
        if team is not None:
            shutdown = guest.terminate(ctl, team, group=group)
            leftovers = guest.members(ctl, group)
        elif group is not None:
            shutdown = guest.terminate(ctl, group, group=group)
            leftovers = guest.members(ctl, group)
        if args.browser == 'firefox':
            guest.ssh(r'ps | /bin/grep "[F]irefox" | awk "{print \$(NF-3)}" | xargs -r kill -9 2>/dev/null; true', check=False)
        guest.fetch_file(f'{guest_dir}/browser.log', directory / 'browser.log', tail_bytes=1024 * 1024)
        if args.haiku_profile:
            guest.fetch_file(f'{guest_dir}/profile.txt', directory / 'profile.txt')
        guest.ssh(f'rm -rf {guest_dir}/profile', check=False, timeout=300)
        server.terminate()
        server.wait(timeout=10)
    crashes = watch.poll()
    after = guest.load_report(ctl, (), 3000)
    report = {'id': run_id, 'outcome': outcome, 'browser': args.browser, 'page': args.page,
              'machine': guest.HOST, 'bundle': args.bundle, 'env': args.env,
              'haikuProfile': args.haiku_profile,
              'sampleInterval': args.sample_interval,
              'ownedKernelWorkerNames': args.owned_kernel_worker,
              'shutdown': shutdown, 'leftovers': leftovers, 'crashes': crashes,
              'load': {'before': before, 'during': samples, 'after': after},
              'contended': before['contended'] or after['contended'] or any(s['contended'] for s in samples)}
    if outcome == 'completed':
        report['probe'] = json.loads((directory / 'probe.json').read_text())['payload']
        if report['probe'].get('passed') is False:
            report['outcome'] = 'probe-failed'
    (directory / 'run.json').write_text(json.dumps(report, indent=1))
    print(json.dumps(report, indent=1))
    return 0 if (report['outcome'] == 'completed' and not leftovers
                 and not crashes['newReports'] and not crashes['syslogEvents']) else 1


if __name__ == '__main__':
    sys.exit(main())

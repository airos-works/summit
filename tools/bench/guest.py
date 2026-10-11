"""Guest-side plumbing shared by run-speedometer.py and stress-browser.py.

Rules this module enforces (the VM is shared with other engineers):
  * every guest write goes under GUEST_ROOT on the SummitExtensions volume
    (the boot volume is ~95% full);
  * a browser is only ever addressed by the team id returned by launch();
    nothing here kills or scripts Summit by name or signature;
  * compilers and other people's processes are observed, never signalled.
"""
import hashlib
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
BENCH = pathlib.Path(__file__).resolve().parent

# Two Haiku machines run these benchmarks: the QEMU VM (the default, no GPU) and
# the workstation (Threadripper 1950X, GeForce GTX 1070), selected with
# SUMMIT_BENCH_HOST=workstation. They differ in how they are reached, where a run
# may write, how the screen is captured and how the guest addresses this host.
HOST = os.environ.get('SUMMIT_BENCH_HOST', 'vm')
if HOST not in ('vm', 'workstation'):
    raise SystemExit('SUMMIT_BENCH_HOST must be "vm" or "workstation".')
IS_VM = HOST == 'vm'
REMOTE_SHELL = str(ROOT / 'tools' / ('haiku.sh' if IS_VM else 'ws.sh'))
# The VM boot volume is ~95% full, so its runs live on the separate BFS volume.
GUEST_ROOT = os.environ.get('SUMMIT_BENCH_GUEST_ROOT',
                            '/SummitExtensions/summit/claude-bench' if IS_VM else '/boot/home/summit/bench')
DEFAULT_BUNDLE = os.environ.get(
    'SUMMIT_BENCH_BUNDLE',
    '/SummitExtensions/summit/build-modern-browser/bundle-la4ytsek' if IS_VM
    else '/boot/home/summit/build-modern-browser/current')
# Address the guest uses for a server running on this host: QEMU user networking
# gateway in the VM, this machine's LAN address from the workstation.
HOST_ADDRESS = os.environ.get('SUMMIT_BENCH_HOST_ADDRESS', '10.0.2.2' if IS_VM else '192.168.1.64')
SERVER_BIND = os.environ.get('SUMMIT_BENCH_SERVER_BIND', '127.0.0.1' if IS_VM else '0.0.0.0')
COMPILER_PATTERN = re.compile(r'(^|/)(cc1plus|cc1|ninja|make|cmake|ld|ld\.lld|lld|collect2|as|g\+\+|gcc|clang\S*|meson|python\S* .*(ninja|meson))( |$)')


SYSTEM_SERVERS = ('app_server', 'registrar', 'input_server')


class GuestError(RuntimeError):
    pass


def ssh(command, timeout=60, check=True, input_bytes=None):
    """Run a shell command in the guest; returns CompletedProcess with text output."""
    result = subprocess.run(['bash', REMOTE_SHELL, command], input=input_bytes,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    result.stdout = result.stdout.decode('utf-8', 'replace')
    result.stderr = result.stderr.decode('utf-8', 'replace')
    if check and result.returncode:
        raise GuestError(f'guest command failed ({result.returncode}): {command}\n{result.stdout}{result.stderr}')
    return result


def ensure_ctl():
    """Build tools/bench/summitctl.cpp in the guest (cached by source hash); returns its path."""
    sources = (BENCH / 'summitctl.cpp').read_bytes() + (ROOT / 'src/ui/Messages.h').read_bytes()
    digest = hashlib.sha256(sources).hexdigest()[:16]
    directory = f'{GUEST_ROOT}/bin'
    binary = f'{directory}/summitctl-{digest}'
    if ssh(f'test -x {binary}', check=False).returncode == 0:
        return binary
    archive = subprocess.run(['tar', '-cf', '-', '-C', str(ROOT), 'tools/bench/summitctl.cpp', 'src/ui/Messages.h'],
                             stdout=subprocess.PIPE, check=True).stdout
    build = f'{directory}/build-{digest}'
    ssh(f'mkdir -p {build} && tar -xf - -C {build}', input_bytes=archive)
    ssh(f'export TMPDIR={build}; g++ -std=c++17 -O1 -I{build}/src {build}/tools/bench/summitctl.cpp -lbe '
        f'-o {binary}.tmp && mv {binary}.tmp {binary} && rm -rf {build}', timeout=300)
    return binary


def ctl(binary, team, *arguments, timeout_ms=5000):
    """Returns (exit_code, stdout, stderr). Exit 4 means the browser did not answer in time."""
    command = ' '.join([binary, '--team', str(int(team)), '--timeout-ms', str(int(timeout_ms))]
                       + [shlex.quote(str(a)) for a in arguments])
    try:
        result = ssh(command, timeout=timeout_ms / 1000 * 3 + 20, check=False)
    except subprocess.TimeoutExpired:
        return 4, '', 'ssh timed out'
    return result.returncode, result.stdout, result.stderr


def state(binary, team, timeout_ms=5000):
    code, out, err = ctl(binary, team, 'state', timeout_ms=timeout_ms)
    if code:
        return {'ok': False, 'exit': code, 'error': err.strip()}
    try:
        return dict(json.loads(out), ok=True)
    except ValueError:
        return {'ok': False, 'exit': code, 'error': 'unparsable state: ' + out[:200]}


def sample(binary, teams=(), interval_ms=1000):
    """System-wide CPU busy cores + per-team CPU; area memory for the listed teams."""
    result = ssh(' '.join([binary, 'sample', str(int(interval_ms))] + [str(int(t)) for t in teams]),
                 timeout=interval_ms / 1000 + 45)
    data = json.loads(result.stdout)
    data['at'] = time.time()
    return data


def load_report(binary, own_teams=(), interval_ms=2000):
    """Contention snapshot: what else is burning CPU in the VM right now."""
    data = sample(binary, own_teams, interval_ms)
    own = {int(t) for t in own_teams}
    # kernel_team owns the idle threads, so its "CPU time" is mostly idleness; real
    # kernel work is still visible in busyCores (taken from per-CPU active_time).
    # app_server, registrar and input_server work on behalf of whoever draws or sets timers,
    # which during a run is mostly the browser under test: reported, but not "foreign".
    servers = [t for t in data['teams'] if t['args'].rsplit('/', 1)[-1] in SYSTEM_SERVERS]
    others = [t for t in data['teams'] if t['team'] not in own and 'summitctl' not in t['args']
              and t['args'] != 'kernel_team' and t not in servers]
    # team_info.args is cut at 64 bytes, so a long cc1plus path may lose its basename.
    compilers = [t for t in others if COMPILER_PATTERN.search(t.get('image') or t['args'])
                 or '/develop/tools/' in t['args'] or '/lib/gcc/' in t['args']]
    foreign_cores = sum(t['cores'] for t in others)
    owned = [t for t in data['teams'] if t['team'] in own]
    own_cores = sum(t['cores'] for t in owned)
    # Teams that start and exit inside the sampling interval (a ninja build spawning short
    # compiles) never appear in both snapshots; they are only visible as unexplained busy time.
    unexplained = max(0.0, data['busyCores'] - own_cores - foreign_cores - sum(t['cores'] for t in servers))
    return {
        'at': data['at'], 'cpuCount': data['cpuCount'], 'busyCores': data['busyCores'],
        'foreignCores': round(foreign_cores, 3), 'ownCores': round(own_cores, 3),
        # Area totals can count shared mappings more than once; these are not PSS.
        'ownResidentAreaBytes': sum(t.get('ramBytes', 0) for t in owned),
        'ownProcesses': [{'team': t['team'], 'role': role(t.get('image') or t['args']),
                          'ramBytes': t.get('ramBytes', 0), 'virtualBytes': t.get('virtualBytes', 0),
                          'threads': t['threads'], 'areas': t['areas']} for t in owned],
        'serverCores': round(sum(t['cores'] for t in servers), 3),
        # Observation only: workers remain unexplained until a benchmark
        # establishes which client owns their work. Never exempt all kernel CPU.
        'kernelWorkers': data.get('kernelWorkers', []),
        'unexplainedBusyCores': round(unexplained, 3),
        'compilerTeams': len(compilers), 'compilerCores': round(sum(t['cores'] for t in compilers), 3),
        'topForeign': sorted(({'cores': t['cores'], 'args': t['args'][:80]} for t in others),
                             key=lambda t: -t['cores'])[:5],
        'usedBytes': data['usedBytes'], 'cachedBytes': data['cachedBytes'],
        # More than a quarter of one core used by somebody else counts as contended.
        # (or half a core of busy time nobody accounts for)
        'contended': bool(compilers) or foreign_cores > 0.25 or unexplained > 0.5,
    }


def launch(bundle, profile, url, log, extra_env=None, wrapper=()):
    """Start Summit in its own session/process group; returns the group id.

    Without a wrapper the group id is also Summit's team id. With a wrapper (for
    example Haiku's `profile`), use find_browser() to get the Summit team.
    """
    script = (
        'import json, os, subprocess, sys\n'
        'bundle, profile, url, log, wrapper = sys.argv[1:6]\n'
        'os.makedirs(profile, mode=0o700, exist_ok=True)\n'
        'env = dict(os.environ, WEBKIT_EXEC_PATH=bundle, LIBRARY_PATH=bundle + "/lib:/boot/system/lib")\n'
        'for name in ("LD_PRELOAD", "LD_PRELOAD_ADDONS", "DISABLE_ASLR"): env.pop(name, None)\n'
        'for item in sys.argv[6:]:\n'
        '    key, _, value = item.partition("=")\n'
        '    env[key] = value\n'
        # A private GL stack (the workstation's Mesa/zink prefix) has to come
        # before the bundle and the system libraries, not replace them.
        'prefix = env.pop("SUMMIT_LIBRARY_PATH_PREFIX", "")\n'
        'if prefix: env["LIBRARY_PATH"] = prefix + ":" + env["LIBRARY_PATH"]\n'
        # An empty URL restores the profile's saved session instead.
        'command = json.loads(wrapper) + [bundle + "/Summit", "--profile", profile] + ([url] if url else [])\n'
        'child = subprocess.Popen(command, env=env, stdin=subprocess.DEVNULL,\n'
        '    stdout=open(log, "wb"), stderr=subprocess.STDOUT, start_new_session=True, cwd=os.path.dirname(log))\n'
        'print(child.pid)\n'
    )
    extra = ' '.join(shlex.quote(f'{k}={v}') for k, v in (extra_env or {}).items())
    result = ssh(f'mkdir -p {shlex.quote(str(pathlib.PurePosixPath(log).parent))} && python3.10 - '
                 f'{shlex.quote(bundle)} {shlex.quote(profile)} {shlex.quote(url)} {shlex.quote(log)} '
                 f'{shlex.quote(json.dumps(list(wrapper)))} {extra}',
                 input_bytes=script.encode(), timeout=60)
    return int(result.stdout.strip().splitlines()[-1])


def require_volume():
    """The build volume does not auto-mount after a guest reset; never write to the boot volume instead."""
    if not IS_VM:
        return  # the workstation boot volume has room; runs live under GUEST_ROOT there
    out = ssh("df 2>/dev/null | grep -A1 '/SummitExtensions'", check=False).stdout
    if 'bfs' not in out or '/dev/disk/' not in out:
        raise GuestError('/SummitExtensions is not mounted in the guest. Ask the VM owner to mount the existing '
                         'BFS volume (docs/VM.md); this harness never mounts or initialises disks.')


def foreign_browsers(binary, bundle, own_group=None):
    """Summit UI processes started from the same bundle path that are not ours.

    Summit is B_SINGLE_LAUNCH, which Haiku applies per executable file: while an
    instance of <bundle>/Summit runs, launching that same file again (even with a
    different --profile) only forwards the URL to the running instance as a new
    tab. Instances from other bundle directories coexist; they only show up as
    foreign CPU load.
    """
    result = ssh(f'{binary} find Summit', timeout=60, check=False)
    try:
        found = json.loads(result.stdout)
    except ValueError:
        return []
    return [item for item in found if item['image'] == bundle.rstrip('/') + '/Summit'
            and (own_group is None or item['group'] != own_group)]


def wait_for_free_gui(binary, bundle, minutes=0, log=print):
    deadline = time.time() + minutes * 60
    while True:
        others = foreign_browsers(binary, bundle)
        if not others:
            return True
        if time.time() >= deadline:
            raise GuestError(f'{bundle}/Summit is already running (team '
                             + ', '.join(str(item['team']) for item in others)
                             + '); it is single-launch, so a second launch would only add a tab to that instance')
        log(f"waiting: {bundle}/Summit already running as team(s) {[item['team'] for item in others]}")
        time.sleep(30)


def contamination(current, expected_prefix):
    """Reason string when the window no longer looks like the one tab we opened, else None."""
    tabs = current.get('tabs') or []
    if len(tabs) != 1:
        return f'{len(tabs)} tabs open: ' + ', '.join(tab.get('url', '?') for tab in tabs)
    if not tabs[0].get('url', '').startswith(expected_prefix):
        return 'tab navigated to ' + tabs[0].get('url', '?')
    return None


def find_browser(binary, group, timeout=90):
    """Team id of the Summit UI process inside a launched group, once it answers scripting."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for team, image in members(binary, group):
            if role(image) == 'Summit' and state(binary, team).get('ok'):
                return team
        time.sleep(1)
    raise GuestError(f'no scriptable Summit appeared in process group {group}')


def members(binary, team):
    """Teams in the browser's process group: [(team, image path)] for Summit and its helpers."""
    result = ssh(f'{binary} group {int(team)}', timeout=60, check=False)
    try:
        return [(item['team'], item['image']) for item in json.loads(result.stdout)]
    except ValueError:
        return []


def role(image):
    """'Summit', 'WebProcess', 'NetworkProcess' ... from a full image path."""
    return image.rsplit('/', 1)[-1] or 'unknown'


def alive(team):
    return ssh(f'kill -0 {int(team)}', check=False).returncode == 0


def terminate(binary, team, grace=20, group=None):
    """Ask only our own instance to quit; escalate to signals for that process group only."""
    report = {'team': team, 'method': None}
    group = [pid for pid, image in members(binary, group or team) if role(image) != 'profile']
    if alive(team):
        ctl(binary, team, 'quit')
        deadline = time.time() + grace
        while time.time() < deadline:
            if not alive(team):
                report['method'] = 'quit-request'
                break
            time.sleep(1)
        else:
            ssh(f'kill {int(team)}', check=False)
            time.sleep(5)
            report['method'] = 'SIGTERM'
            if alive(team):
                ssh(f'kill -9 {int(team)}', check=False)
                report['method'] = 'SIGKILL'
    else:
        # A crashed/exited UI can leave helpers in its original process group.
        # They still belong to this run and need the same bounded cleanup.
        report['method'] = 'already-exited'
    # Helpers exit on their own once the UI process has gone; a profile-training
    # build writes its counters first, which takes a while
    # (SUMMIT_BENCH_LEFTOVER_GRACE=60 during training).
    leftover_grace = float(os.environ.get('SUMMIT_BENCH_LEFTOVER_GRACE', '2'))
    deadline = time.time() + leftover_grace
    while True:
        leftovers = [pid for pid in group if pid != team and alive(pid)]
        if not leftovers or time.time() >= deadline:
            break
        time.sleep(1)
    if leftovers:  # helpers of *our* instance that outlived their UI process
        ssh('kill ' + ' '.join(str(pid) for pid in leftovers), check=False)
        time.sleep(2)
        still = [pid for pid in leftovers if alive(pid)]
        if still:
            ssh('kill -9 ' + ' '.join(str(pid) for pid in still), check=False)
        report['leftoverHelpers'] = leftovers
    return report


class CrashWatch:
    """New crash reports on the guest Desktop and debugger lines in syslog since construction."""
    EVENT = re.compile(r'debug_server: Thread \d+ entered the debugger|\bDEBUGGER:|\bPANIC:|vm_page_fault|segment violation',
                       re.IGNORECASE)

    def __init__(self):
        self.reports = set(self._reports())
        self.syslog_size = self._syslog_size()

    @staticmethod
    def _reports():
        return [line for line in ssh('ls -1 /boot/home/Desktop/ 2>/dev/null', check=False).stdout.splitlines()
                if line.endswith('.report')]

    @staticmethod
    def _syslog_size():
        out = ssh("stat -c %s /var/log/syslog 2>/dev/null || echo 0", check=False).stdout.strip()
        return int(out or 0)

    def poll(self):
        new_reports = sorted(set(self._reports()) - self.reports)
        size = self._syslog_size()
        offset = self.syslog_size if size >= self.syslog_size else 0  # rotated
        text = ssh(f'tail -c +{offset + 1} /var/log/syslog', check=False).stdout if size != self.syslog_size else ''
        events = [line for line in text.splitlines() if self.EVENT.search(line)]
        return {'newReports': new_reports, 'syslogEvents': events, 'syslogBytes': size - offset}

    def syslog_tail(self, lines=80):
        return ssh(f'tail -n {int(lines)} /var/log/syslog', check=False).stdout

    def fetch_report(self, name, destination):
        data = subprocess.run(['bash', REMOTE_SHELL, 'cat ' + shlex.quote('/boot/home/Desktop/' + name)],
                              stdout=subprocess.PIPE, timeout=120).stdout
        pathlib.Path(destination).write_bytes(data)


def fetch_file(guest_path, destination, tail_bytes=None):
    command = (f'tail -c {int(tail_bytes)} ' if tail_bytes else 'cat ') + shlex.quote(guest_path)
    data = subprocess.run(['bash', REMOTE_SHELL, command], stdout=subprocess.PIPE, timeout=180).stdout
    pathlib.Path(destination).write_bytes(data)
    return len(data)


def wake_display(x=1276, y=330):
    """Reset the guest screen saver's idle timer with a pointer move (no click, no key).

    A blanked screen hides the browser window, which changes what app_server and the
    page have to paint. The default position is the desktop's right edge, away from
    the browser window, so the page under test receives no input events.
    """
    if not IS_VM:
        # No QMP on real hardware, and a machine nobody has touched for hours is
        # blanked: app_server then stops sending Draw messages, the UI process
        # never acknowledges a frame and the browser looks frozen. Stop the
        # blanker instead of faking input; it comes back on the next idle
        # period, so this is repeated for the length of a run.
        # awk's own $2 has to survive the remote login shell, so it stays single quoted.
        ssh("kill $(ps | /bin/grep '[s]creen_blanker' | awk '{print $2}') 2>/dev/null; true",
            check=False, timeout=30)
        return
    sys.path.insert(0, str(ROOT / 'tools'))
    import vm
    qmp = vm.QMP()
    for dx in (0, 2, 0):
        qmp.command('input-send-event', events=[
            {'type': 'abs', 'data': {'axis': 'x', 'value': int((x + dx) * 32767 / 1280)}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': int((y + dx) * 32767 / 800)}}])
        time.sleep(0.05)
    qmp.socket.close()


def screenshot(destination):
    if IS_VM:
        subprocess.run(['python3', str(ROOT / 'tools/vm.py'), 'screenshot', str(destination)],
                       stdout=subprocess.DEVNULL, check=True, timeout=60)
        return destination
    # Haiku's own screenshot tool writes the whole desktop, then exits non-zero
    # even when it succeeded, so the file is what says whether it worked.
    remote = f'{GUEST_ROOT}/screenshot.png'
    ssh(f'mkdir -p {shlex.quote(GUEST_ROOT)} && rm -f {shlex.quote(remote)}; '
        f'screenshot -s -f png {shlex.quote(remote)}; test -s {shlex.quote(remote)}', timeout=120)
    fetch_file(remote, destination)
    return destination


def guest_facts(bundle):
    script = (f'uname -a; echo ---; sysinfo -cpu | head -2; echo ---; sysinfo -mem | head -2; echo ---; '
              f'cat {shlex.quote(bundle)}/build-manifest.json 2>/dev/null | head -c 4000')
    return ssh(script, check=False).stdout

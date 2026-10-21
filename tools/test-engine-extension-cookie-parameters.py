#!/usr/bin/env python3
"""Run native cookie/store value conversion with frozen JavaScriptCore; no WebKit object ABI is used."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
ENGINE = Path('/boot/home/summit-webkit')
FROZEN = Path('/boot/home/summit/jsc-icu78-w8dm77f9/lib')
HELPERS = {
    'Shared/Extensions': ('WebExtensionCookieStoreIdentifier.h', 'WebExtensionCookieStoreIdentifier.cpp', 'WebExtensionCookieQueryParser.h', 'WebExtensionCookieQueryParser.cpp', 'WebExtensionCookieParameters.h'),
    'WebProcess/Extensions/Bindings': ('JSWebExtensionCookieParameters.h', 'JSWebExtensionCookieParameters.cpp', 'JSWebExtensionString.h', 'JSWebExtensionString.cpp'),
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def native():
    from native_crash_log import NativeCrashLog
    output = ROOT / 'sources'
    build = ENGINE / 'WebKitBuild/Modern'
    watched = [build / 'build.ninja', build / 'cmakeconfig.h', ENGINE / '.summit-source-manifest.json',
               ENGINE / 'Source/WebKit/WebKitPrefix.h']
    snapshot = {str(path): digest(path) for path in watched}
    manifest = json.loads((output / 'source-manifest.json').read_text())
    libraries = [FROZEN / name for name in ('libJavaScriptCore.so.18.7.4', 'libicudata.so.78.3',
                                           'libicui18n.so.78.3', 'libicuuc.so.78.3')]
    hashes = {str(path): digest(path) for path in libraries}
    if hashes[str(libraries[0])] != 'e8a83d73453da614dc1c73d1fe875ce753122a092bbf10e16ca1accedfd0f6ba':
        raise RuntimeError('Frozen WTF/JavaScriptCore dependency changed')
    if json.loads(watched[2].read_text())['patch_sha256'] != manifest['engine_patch_sha256']:
        raise RuntimeError('The staged source and native configured engine patches differ')
    for name, expected in manifest['files'].items():
        if digest(output / name) != expected['sha256']:
            raise RuntimeError('Staged source changed: ' + name)
    config = watched[1].read_text()
    if config.count('#define ENABLE_WK_WEB_EXTENSIONS 0') != 1:
        raise RuntimeError('Expected the configured feature-disabled baseline')
    (output / 'cmakeconfig.h').write_text(config.replace('#define ENABLE_WK_WEB_EXTENSIONS 0', '#define ENABLE_WK_WEB_EXTENSIONS 1').replace('#define ENABLE_CONTENT_EXTENSIONS 0', '#define ENABLE_CONTENT_EXTENSIONS 1'))
    fields = {}
    target = 'Source/WebKit/CMakeFiles/WebKit.dir/UIProcess/API/haiku/WebKitView.cpp.o'
    with watched[0].open() as stream:
        for line in stream:
            if line.startswith('build ' + target + ':'):
                for line in stream:
                    if not line.strip():
                        break
                    key, _, value = line.strip().partition(' = ')
                    fields[key] = value
                break
    raw = iter(shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS'))))
    flags = []
    for flag in raw:
        if flag == '-include':
            next(raw)
        elif not flag.startswith(('-fdiagnostics-color=', '-fmax-errors=')):
            flags.append(flag)
    flags = ['-I' + str(output), '-iquote', str(output), *flags,
             '-include', str(watched[3]), '-ffunction-sections', '-fdata-sections', '-fvisibility=hidden',
             '-fdiagnostics-color=never', '-fmax-errors=5']
    report = {'scope': 'native cookie query parsing, store identity and actual JavaScriptCore value conversion only; no cookie store access, API IPC, permissions, extension or browser runtime',
              'source_manifest': manifest, 'native_inputs': snapshot, 'frozen_libraries': hashes,
              'compile_results': [], 'linked_webcore_or_webkit': False}
    objects = []
    for name in ('WebExtensionCookieStoreIdentifier.cpp', 'WebExtensionCookieQueryParser.cpp', 'JSWebExtensionCookieParameters.cpp', 'JSWebExtensionString.cpp', 'EngineExtensionCookieParametersTests.cpp'):
        obj = output / (Path(name).stem + '.o')
        command = ['c++', *flags, '-c', str(output / name), '-o', str(obj)]
        result = subprocess.run(command, cwd=build, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(result.stdout, end='', flush=True)
        report['compile_results'].append({'source': name, 'exit': result.returncode, 'output': result.stdout})
        objects.append(obj)
    if any(unit['exit'] for unit in report['compile_results']):
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        return 1
    executable = output / 'run'
    link = subprocess.run(['c++', *map(str, objects), str(libraries[0]), '-lbe', '-lnetwork',
                    '-Wl,--no-export-dynamic', '-Wl,--gc-sections', '-Wl,-rpath,' + str(FROZEN),
                    '-o', str(executable)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    report['link'] = {'exit': link.returncode, 'output': link.stdout}
    print(link.stdout, end='', flush=True)
    if link.returncode:
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        return 1
    dynamic = subprocess.check_output(['readelf', '-d', str(executable)], text=True)
    if 'libWebKit' in dynamic or 'libWebCore' in dynamic:
        raise RuntimeError('State helper unexpectedly links WebCore or WebKit')
    report['objects'] = {path.name: digest(path) for path in objects}
    report['executable_sha256'] = digest(executable)
    environment = os.environ.copy()
    for name in ('LD_PRELOAD', 'LD_PRELOAD_ADDONS', 'DISABLE_ASLR'):
        environment.pop(name, None)
    crash_log = NativeCrashLog()
    try:
        result = subprocess.run([str(executable)], env=environment, timeout=60, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        report['exit'] = result.returncode
        report['output'] = result.stdout
    except subprocess.TimeoutExpired:
        report['exit'] = None
        report['output'] = 'Native cookie parameters test timed out and was terminated; failed run'
    report['native_crash_log'] = crash_log.finish()
    report['inputs_unchanged'] = all(digest(Path(path)) == expected for path, expected in snapshot.items())
    report['libraries_unchanged'] = all(digest(Path(path)) == expected for path, expected in hashes.items())
    report['sources_unchanged'] = all(digest(output / name) == expected['sha256'] for name, expected in manifest['files'].items())
    report['passed'] = (report['exit'] == 0 and report['native_crash_log']['passed'] and report['inputs_unchanged']
                        and report['libraries_unchanged'] and report['sources_unchanged'])
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(report['output'], end='', flush=True)
    return 0 if report['passed'] else 1


def host(overlay):
    candidate = Path(overlay).resolve() if overlay else ROOT / '.cache/WebKit'
    if (candidate / 'Source/WebKit/Shared/Extensions/WebExtensionCookiePartitionKey.h').exists():
        raise SystemExit('CHIPS site normalization requires the actual WebCore library. Use --cross-build BUILD --output OUTPUT --runtime-lib-dir HAIKU_LIB_DIR to build the current native tests.')
    files = {}
    for directory, names in HELPERS.items():
        for name in names:
            relative = Path('Source/WebKit') / directory / name
            source = Path(overlay).resolve() / relative if overlay else ROOT / '.cache/WebKit' / relative
            if not source.exists():
                source = ROOT / '.cache/WebKit' / relative
            files[name] = (source, source.read_bytes())
    relative = Path('Source/WebCore/platform/CookieChange.h')
    source = Path(overlay).resolve() / relative if overlay else ROOT / '.cache/WebKit' / relative
    if source.exists():
        files['WebCore/CookieChange.h'] = (source, source.read_bytes())
        cookie_header = ROOT / '.cache/WebKit/Source/WebCore/platform/Cookie.h'
        files['WebCore/Cookie.h'] = (cookie_header, cookie_header.read_bytes())
    test = ROOT / 'tests/EngineExtensionCookieParametersTests.cpp'
    files[test.name] = (test, test.read_bytes())
    manifest = {'engine_patch_sha256': json.loads((ROOT / 'engine/sources.lock.json').read_text())['patch']['sha256'],
                'files': {name: {'source': str(path), 'sha256': hashlib.sha256(data).hexdigest()}
                          for name, (path, data) in files.items()}}
    contents = {'sources/' + name: data for name, (_, data) in files.items()}
    contents['sources/source-manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    for name in ('test-engine-extension-cookie-parameters.py', 'native_crash_log.py'):
        contents['tools/' + name] = (ROOT / 'tools' / name).read_bytes()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w:gz') as stream:
        for name, data in contents.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            stream.addfile(info, io.BytesIO(data))
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    stage = remote('mktemp -d /boot/home/summit/extension-cookie-parameters-tests.XXXXXXXX', check=True,
                   text=True, capture_output=True).stdout.strip()
    if not stage.startswith('/boot/home/summit/extension-cookie-parameters-tests.') or '/' in stage.removeprefix('/boot/home/summit/'):
        raise RuntimeError('Unexpected native stage: ' + stage)
    output = ROOT / '.vm' / Path(stage).name
    output.mkdir(exist_ok=False)
    remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    print('Native cookie parameters helper stage: ' + stage, flush=True)
    with (output / 'native.log').open('w') as log:
        result = remote(shlex.join(['python3.10', stage + '/tools/test-engine-extension-cookie-parameters.py', '--native']),
                        stdout=log, stderr=subprocess.STDOUT)
    print((output / 'native.log').read_text(), end='', flush=True)
    read = remote('cat ' + shlex.quote(stage + '/sources/results.json'), text=True, capture_output=True)
    report = {'stage': stage, 'exit': result.returncode,
              'native': json.loads(read.stdout) if read.returncode == 0 else {'error': 'No native report'}}
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'result': str(output / 'result.json'), 'passed': report['native'].get('passed', False)}), flush=True)
    return result.returncode


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--overlay', help='Candidate source root containing Source/')
    parser.add_argument('--cross-build', type=Path, help='Configured cross-build of the current engine, including WebCore')
    parser.add_argument('--output', type=Path, help='Output directory for current native test executables')
    parser.add_argument('--runtime-lib-dir', help='Matching engine library directory on Haiku')
    args = parser.parse_args()
    if args.cross_build:
        if not args.output or not args.runtime_lib_dir:
            parser.error('--cross-build requires --output and --runtime-lib-dir')
        raise SystemExit(subprocess.call([sys.executable, str(ROOT / 'tools/test-engine-cookie-components.py'),
            '--build', str(args.cross_build), '--output', str(args.output), '--runtime-lib-dir', args.runtime_lib_dir]))
    raise SystemExit(native() if args.native else host(args.overlay))

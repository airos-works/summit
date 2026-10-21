#!/usr/bin/env python3
"""Test the actual WebCore Curl cookie database and HTTP cookie parser on Haiku."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
ENGINE = Path('/boot/home/summit-webkit-extensions')
BUILD = ENGINE / 'WebKitBuild/Modern'
TEST = 'EngineCookieStoreTests.cpp'


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def unchanged(snapshot):
    return all(digest(Path(path)) == expected for path, expected in snapshot.items())


def ninja_fields(predicate):
    selected = False
    fields = {}
    for line in (BUILD / 'build.ninja').read_text().splitlines():
        if line.startswith('build '):
            selected = predicate(line)
        elif selected:
            if not line.strip():
                return fields
            key, separator, value = line.strip().partition(' = ')
            if separator:
                fields[key] = value
    raise RuntimeError('Configured build command was not found')


def native(compile_only, run_only):
    output = ROOT / 'sources'
    report_path = output / 'results.json'
    source_manifest = json.loads((output / 'source-manifest.json').read_text())
    source = output / TEST
    if digest(source) != source_manifest['test_sha256']:
        raise RuntimeError('Staged test source changed')
    engine_manifest = ENGINE / '.summit-source-manifest.json'
    if json.loads(engine_manifest.read_text())['patch_sha256'] != source_manifest['engine_patch_sha256']:
        raise RuntimeError('Configured engine patch differs from the test source snapshot')
    config = (BUILD / 'cmakeconfig.h').read_text()
    if '#define ENABLE_WK_WEB_EXTENSIONS 1' not in config or '#define ENABLE_CONTENT_EXTENSIONS 1' not in config:
        raise RuntimeError('The real extension-enabled configuration is required')
    watched = [engine_manifest, BUILD / 'build.ninja', BUILD / 'cmakeconfig.h', ENGINE / 'Source/WebKit/WebKitPrefix.h']
    if run_only:
        report = json.loads(report_path.read_text())
        if not report.get('compiled') or not unchanged(report['native_inputs']) or not unchanged(report['headers']):
            raise RuntimeError('Compile inputs changed or the test did not compile')
        if not unchanged({str(output / name): expected for name, expected in report['objects'].items()}):
            raise RuntimeError('Compiled test object changed')
    else:
        report = {'scope': 'actual Curl CookieJarDB SQLite memory/file writes, queries, expiration, metadata and reopen plus HTTP cookie parsing; no NetworkStorageSession, API IPC, extension or browser runtime',
                  'source_manifest': source_manifest, 'native_inputs': {str(path): digest(path) for path in watched},
                  'runtime_executed': False, 'passed': False, 'linked_webcore_archive': False, 'linked_webkit': False}
        fields = ninja_fields(lambda line: line.startswith('build Source/WebKit/CMakeFiles/WebKit.dir/UIProcess/API/haiku/WebKitView.cpp.o:'))
        raw = iter(shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS'))))
        flags = []
        for flag in raw:
            if flag == '-include':
                next(raw)
            elif not flag.startswith(('-O', '-fdiagnostics-color=', '-fmax-errors=')):
                flags.append(flag)
        report['compile_results'], report['headers'], report['objects'] = [], {}, {}
        if source_manifest.get('with_ipc_events'):
            flags += ['-DSUMMIT_COOKIE_IPC_TESTS=1', '-ffunction-sections', '-fdata-sections', '-fvisibility=hidden']
        if source_manifest.get('with_cookie_changes'):
            flags.append('-DSUMMIT_COOKIE_CHANGE_TESTS=1')
        if source_manifest.get('with_api_parser'):
            flags.append('-DSUMMIT_COOKIE_API_PARSER_TESTS=1')
        for name, expected in source_manifest.get('extra_sources', {}).items():
            if digest(output / name) != expected['sha256']:
                raise RuntimeError('Staged overlay source changed: ' + name)
        for name in [*source_manifest.get('compile_sources', []), TEST]:
            stem = 'test' if name == TEST else Path(name).stem
            obj, dep = output / (stem + '.o'), output / (stem + '.d')
            core_include = '-idirafter' if name in source_manifest.get('ipc_sources', []) else '-iquote'
            command = ['c++', '-O1', '-I' + str(output), core_include, str(BUILD / 'WebCore/PrivateHeaders/WebCore'), *flags, '-include', str(watched[-1]), '-fdiagnostics-color=never', '-fmax-errors=5',
                       '-MMD', '-MF', str(dep), '-c', str(output / name), '-o', str(obj)]
            result = subprocess.run(command, cwd=BUILD, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            report['compile_results'].append({'source': name, 'command': command, 'exit': result.returncode, 'output': result.stdout})
            print(result.stdout, end='', flush=True)
            if not result.returncode:
                dependencies = dep.read_text().replace('\\\n', ' ').split(':', 1)[1]
                paths = {Path(path) if Path(path).is_absolute() else BUILD / path for path in shlex.split(dependencies)}
                report['headers'].update({str(path): digest(path) for path in sorted(paths)})
                report['objects'][obj.name] = digest(obj)
        report['compiled'] = all(unit['exit'] == 0 for unit in report['compile_results'])
        report_path.write_text(json.dumps(report, indent=2) + '\n')
        if not report['compiled'] or compile_only:
            return 0 if report['compiled'] else 1

    # Refuse to link an older archive while its source changes are still building.
    ready = subprocess.run(['ninja', '-C', str(BUILD), '-n', 'lib/libWebCore.a'], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if ready.returncode or 'ninja: no work to do.' not in ready.stdout:
        raise RuntimeError('WebCore archive is not fully rebuilt: ' + ready.stdout[-2000:])
    fields = ninja_fields(lambda line: ': CXX_SHARED_LIBRARY_LINKER__WebKit_' in line)
    libraries = shlex.split(fields['LINK_LIBRARIES'])
    paths = {Path(flag) if Path(flag).is_absolute() else BUILD / flag for flag in libraries if not flag.startswith('-')}
    report['libraries'] = {str(path): digest(path) for path in sorted(paths)}
    executable = output / 'run'
    command = ['c++', *[str(output / name) for name in report['objects']], '-Wl,--gc-sections', '-Wl,--disable-new-dtags', *libraries, '-o', str(executable)]
    result = subprocess.run(command, cwd=BUILD, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    report['link'] = {'command': command, 'exit': result.returncode, 'output': result.stdout}
    print(result.stdout, end='', flush=True)
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    if result.returncode:
        return result.returncode
    dynamic = subprocess.check_output(['readelf', '-d', str(executable)], text=True)
    if 'libWebKit' in dynamic:
        raise RuntimeError('Pipeline test unexpectedly links WebKit')
    report['linked_webcore_archive'] = True
    report['executable_sha256'] = digest(executable)
    report['dynamic_dependencies'] = dynamic
    environment = os.environ.copy()
    for name in ('LD_PRELOAD', 'LD_PRELOAD_ADDONS', 'DISABLE_ASLR'):
        environment.pop(name, None)
    from native_crash_log import NativeCrashLog
    crash_log = NativeCrashLog()
    try:
        result = subprocess.run([str(executable)], env=environment, timeout=120, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        report['exit'], report['output'] = result.returncode, result.stdout
    except subprocess.TimeoutExpired:
        report['exit'], report['output'] = None, 'Native pipeline test timed out and was terminated; failed run'
    report['native_crash_log'] = crash_log.finish()
    report['runtime_executed'] = True
    report['inputs_unchanged'] = unchanged(report['native_inputs']) and unchanged(report['headers'])
    report['libraries_unchanged'] = unchanged(report['libraries'])
    report['passed'] = report['exit'] == 0 and report['native_crash_log']['passed'] and report['inputs_unchanged'] and report['libraries_unchanged']
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    print(report['output'], end='', flush=True)
    return 0 if report['passed'] else 1


def host(compile_only, resume, overlay, with_api_parser=False, ipc_generated=None):
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    if resume:
        stage = resume.rstrip('/')
        if not stage.startswith('/boot/home/summit/cookie-store.') or not stage.rsplit('.', 1)[-1].isalnum():
            raise RuntimeError('Unexpected native stage')
        output = ROOT / '.vm' / Path(stage).name
    else:
        test = ROOT / 'tests' / TEST
        manifest = {'engine_patch_sha256': json.loads((ROOT / 'engine/sources.lock.json').read_text())['patch']['sha256'],
                    'test_source': str(test), 'test_sha256': digest(test)}
        files = {'sources/' + TEST: test.read_bytes()}
        if overlay:
            backend = Path(overlay).resolve() / 'Source/WebCore/platform/network/curl'
            manifest['extra_sources'], manifest['compile_sources'] = {}, ['CookieJarDB.cpp', 'CookieUtil.cpp']
            for name in ('CookieJarDB.cpp', 'CookieJarDB.h', 'CookieUtil.cpp', 'CookieUtil.h'):
                path = backend / name
                files['sources/' + name] = path.read_bytes()
                manifest['extra_sources'][name] = {'source': str(path), 'sha256': digest(path)}
            changes = Path(overlay).resolve() / 'Source/WebCore/platform/CookieChange.h'
            if changes.exists():
                for name in ('CookieChange.h', 'WebCore/CookieChange.h'):
                    files['sources/' + name] = changes.read_bytes()
                    manifest['extra_sources'][name] = {'source': str(changes), 'sha256': digest(changes)}
                path = ROOT / 'tests/EngineCookieChangeTests.cpp'
                files['sources/' + path.name] = path.read_bytes()
                manifest['extra_sources'][path.name] = {'source': str(path), 'sha256': digest(path)}
                manifest['compile_sources'].append(path.name)
                manifest['with_cookie_changes'] = True
            files['sources/WebCore/CookieJarDB.h'] = files['sources/CookieJarDB.h']
            manifest['extra_sources']['WebCore/CookieJarDB.h'] = manifest['extra_sources']['CookieJarDB.h']
            files['sources/WebCore/CookieUtil.h'] = files['sources/CookieUtil.h']
            manifest['extra_sources']['WebCore/CookieUtil.h'] = manifest['extra_sources']['CookieUtil.h']
            if with_api_parser:
                manifest['with_api_parser'] = True
                directory = Path('Source/WebKit/Shared/Extensions')
                for name in ('WebExtensionCookieStoreIdentifier.h', 'WebExtensionCookieStoreIdentifier.cpp',
                             'WebExtensionCookieWriteParser.h', 'WebExtensionCookieWriteParser.cpp',
                             'WebExtensionCookieParameters.h', 'WebExtensionCookiePartitionKey.h'):
                    path = Path(overlay).resolve() / directory / name
                    if not path.exists():
                        path = ROOT / '.cache/WebKit' / directory / name
                    files['sources/' + name] = path.read_bytes()
                    manifest['extra_sources'][name] = {'source': str(path), 'sha256': digest(path)}
                    if name.endswith('.cpp'):
                        manifest['compile_sources'].append(name)
                path = ROOT / 'tests/EngineExtensionCookieWriteTests.cpp'
                files['sources/' + path.name] = path.read_bytes()
                manifest['extra_sources'][path.name] = {'source': str(path), 'sha256': digest(path)}
                manifest['compile_sources'].append(path.name)
        if ipc_generated:
            generated = Path(ipc_generated).resolve()
            generation = json.loads((generated / 'generation.json').read_text())
            if not overlay or not generation['copied_unchanged'] or generation['source_manifest']['host_engine_patch_sha256'] != manifest['engine_patch_sha256']:
                raise RuntimeError('IPC generation does not match this candidate engine')
            for name, entry in generation['source_manifest']['files'].items():
                if name.startswith(('ipc-inputs/', 'serialization-inputs/')) and Path(entry['source']).is_file():
                    if digest(Path(entry['source'])) != entry['sha256']:
                        raise RuntimeError('IPC generation source changed: ' + entry['source'])
            manifest['with_ipc_events'] = True
            manifest['ipc_generation'] = generation
            manifest['ipc_sources'] = []
            def stage_ipc(name, path):
                files['sources/' + name] = path.read_bytes()
                manifest['extra_sources'][name] = {'source': str(path), 'sha256': digest(path)}
                if name.endswith('.cpp'):
                    manifest['compile_sources'].append(name)
                    manifest['ipc_sources'].append(name)
            for name in generation['files']:
                if name == 'source-manifest.json':
                    continue
                path = generated / name
                if digest(path) != generation['files'][name]:
                    raise RuntimeError('Generated IPC output changed: ' + name)
                stage_ipc(name, path)
            for name in ('Encoder.cpp', 'Decoder.cpp', 'ArgumentCoders.cpp'):
                stage_ipc(name, ROOT / '.cache/WebKit/Source/WebKit/Platform/IPC' / name)
            path = ROOT / 'tests/EngineCookieChangeIPCTests.cpp'
            stage_ipc(path.name, path)
        files['sources/source-manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
        for name in ('test-engine-cookie-store.py', 'native_crash_log.py'):
            files['tools/' + name] = (ROOT / 'tools' / name).read_bytes()
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode='w:gz') as stream:
            for name, data in files.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                stream.addfile(info, io.BytesIO(data))
        stage = remote('mktemp -d /boot/home/summit/cookie-store.XXXXXXXX', check=True, text=True, capture_output=True).stdout.strip()
        if not stage.startswith('/boot/home/summit/cookie-store.') or not stage.rsplit('.', 1)[-1].isalnum():
            raise RuntimeError('Unexpected native stage')
        output = ROOT / '.vm' / Path(stage).name
        output.mkdir(exist_ok=False)
        remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    print('Native cookie store stage: ' + stage, flush=True)
    command = ['python3.10', stage + '/tools/test-engine-cookie-store.py', '--native']
    if compile_only:
        command.append('--compile-only')
    if resume:
        command.append('--run-only')
    with (output / ('runtime.log' if resume else 'native.log')).open('w') as log:
        result = remote(shlex.join(command), stdout=log, stderr=subprocess.STDOUT)
    read = remote('cat ' + shlex.quote(stage + '/sources/results.json'), text=True, capture_output=True)
    report = {'stage': stage, 'exit': result.returncode, 'native': json.loads(read.stdout) if read.returncode == 0 else {'error': 'No native report'}}
    (output / ('runtime-result.json' if resume else 'result.json')).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'result': str(output), 'compiled': report['native'].get('compiled', False), 'passed': report['native'].get('passed', False)}), flush=True)
    return result.returncode


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--run-only', action='store_true')
    parser.add_argument('--resume', help='Resume a compiled native staging directory after WebCore has rebuilt')
    parser.add_argument('--overlay', help='Candidate source root containing Source/')
    parser.add_argument('--ipc-generated', help='Verified complete serializer/message generation export for native cookie IPC codec tests')
    parser.add_argument('--with-api-parser', action='store_true', help='Include cookie write parser, public-suffix and actual database integration tests')
    args = parser.parse_args()
    if args.compile_only and (args.run_only or args.resume):
        parser.error('Compile-only and resume/run-only are mutually exclusive')
    if args.resume and args.overlay:
        parser.error('A resumed stage uses its previously staged overlay')
    if args.with_api_parser and not args.overlay:
        parser.error('API parser tests require the candidate overlay')
    raise SystemExit(native(args.compile_only, args.run_only) if args.native else host(args.compile_only, args.resume, args.overlay, args.with_api_parser, args.ipc_generated))

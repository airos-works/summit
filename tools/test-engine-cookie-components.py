#!/usr/bin/env python3
"""Cross-build native cookie database and extension-value tests from a configured engine.

Run inside the Haiku SDK environment used to configure --build. This links the
actual WebCore archive and its configured dependencies, not a simulated cookie
store. Copy the two executables to Haiku and run them with the matching engine
libraries at --runtime-lib-dir. No browser or production profile is modified.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def build_tests(build, output, runtime_lib_dir):
    lines = (build / 'build.ninja').read_text().splitlines()

    def fields(predicate):
        selected, result = False, {}
        for line in lines:
            if line.startswith('build '):
                selected = predicate(line)
            elif selected:
                if not line.strip():
                    return result
                key, _, value = line.strip().partition(' = ')
                result[key] = value
        raise RuntimeError('Configured build rule not found')

    ready = subprocess.run(['ninja', '-C', str(build), '-n', 'lib/libWebCore.a'],
                           capture_output=True, text=True, check=True)
    if 'ninja: no work to do.' not in ready.stdout:
        raise RuntimeError('Finish rebuilding the actual WebCore archive before testing it')
    compiler_config = next((build / 'CMakeFiles').glob('*/CMakeCXXCompiler.cmake')).read_text()
    compiler = re.search(r'set\(CMAKE_CXX_COMPILER "([^"]+)"\)', compiler_config).group(1)
    rules = (build / 'CMakeFiles/rules.ninja').read_text()
    sysroot = re.search(r'--sysroot=([^\s]+)', rules)
    compiler_command = [compiler] + (['--sysroot=' + sysroot.group(1)] if sysroot else [])
    rule = fields(lambda line: line.startswith('build Source/WebKit/CMakeFiles/WebKit.dir/Shared/Extensions/WebExtensionCookieWriteParser.cpp.o:'))
    # Keep the real defines/include paths and ABI flags. Test files have no PGO
    # profile and use the prefix header without a mismatched optimized PCH.
    raw = iter(shlex.split(' '.join(rule[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS'))))
    flags = []
    for flag in raw:
        if flag == '-include':
            next(raw)
        elif not flag.startswith(('-O', '-fprofile-', '-fdiagnostics-color=', '-Wno-error=coverage', '-Wno-error=missing')):
            flags.append(flag)
    source = Path(next(line.split('=', 1)[1] for line in (build / 'CMakeCache.txt').read_text().splitlines()
                       if line.startswith('CMAKE_HOME_DIRECTORY:')))
    flags += ['-O1', '-fdiagnostics-color=never', '-include', str(source / 'Source/WebKit/WebKitPrefix.h')]
    shared = source / 'Source/WebKit/Shared/Extensions'
    bindings = source / 'Source/WebKit/WebProcess/Extensions/Bindings'
    suites = {
        'cookie-store-tests': ([ROOT / 'tests' / name for name in (
            'EngineCookieStoreTests.cpp', 'EngineCookieChangeTests.cpp', 'EngineExtensionCookieWriteTests.cpp')]
            + [shared / name for name in ('WebExtensionCookieWriteParser.cpp', 'WebExtensionCookieStoreIdentifier.cpp')],
            ['-DSUMMIT_COOKIE_CHANGE_TESTS=1', '-DSUMMIT_COOKIE_API_PARSER_TESTS=1', '-DSUMMIT_COOKIE_IPC_TESTS=0']),
        'cookie-parameters-tests': ([ROOT / 'tests/EngineExtensionCookieParametersTests.cpp']
            + [shared / name for name in ('WebExtensionCookieQueryParser.cpp', 'WebExtensionCookieStoreIdentifier.cpp')]
            + [bindings / name for name in ('JSWebExtensionCookieParameters.cpp', 'JSWebExtensionString.cpp')], []),
    }
    link_rule = fields(lambda line: ': CXX_SHARED_LIBRARY_LINKER__WebKit_' in line)
    libraries = [flag for flag in shlex.split(link_rule['LINK_LIBRARIES']) if not flag.startswith('-Wl,-rpath,')]
    link_flags = [flag for flag in shlex.split(link_rule['LINK_FLAGS']) if flag != '-shared']
    watched = [build / 'build.ninja', build / 'cmakeconfig.h', build / 'lib/libWebCore.a', build / 'lib/libJavaScriptCore.so.18.7.4']
    snapshot = {str(path): digest(path) for path in watched}
    output.mkdir(parents=True, exist_ok=True)
    report = {'build': str(build), 'inputs': snapshot, 'commands': [], 'suites': {}, 'runtime_executed': False}
    for name, (units, defines) in suites.items():
        objects = []
        suite = output / name
        suite.mkdir(exist_ok=True)
        for unit in units:
            obj = suite / (unit.stem + '.o')
            objects.append(str(obj))
            command = [*compiler_command, *flags, *defines, '-o', str(obj), '-c', str(unit)]
            report['commands'].append(command)
            print('Compiling', name, unit.name, flush=True)
            subprocess.run(command, cwd=build, check=True)
        executable = suite / 'run'
        command = [*compiler_command, *objects, *link_flags, *libraries, '-Wl,--gc-sections',
                   '-Wl,--disable-new-dtags', '-Wl,-rpath,' + runtime_lib_dir, '-o', str(executable)]
        report['commands'].append(command)
        subprocess.run(command, cwd=build, check=True)
        report['suites'][name] = {'executable': str(executable), 'sha256': digest(executable),
                                 'sources': {str(path): digest(path) for path in units}}
    report['inputs_unchanged'] = all(digest(Path(path)) == expected for path, expected in snapshot.items())
    (output / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['inputs_unchanged']:
        raise RuntimeError('Engine inputs changed while compiling tests; rebuild the test executables')
    print('Built both native test executables; runtime execution is still required.', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runtime-lib-dir', required=True)
    args = parser.parse_args()
    build_tests(args.build.resolve(), args.output.resolve(), args.runtime_lib_dir)

#!/usr/bin/env python3
"""Build the native probe using a configured Haiku engine's headers and flags.

Run in the same SDK environment used to build the engine (including CPATH).
The output uses LIBRARY_PATH at runtime; select the candidate bundle there.
"""

import argparse
import json
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="Configured WebKit Ninja build directory")
    parser.add_argument("deps", type=Path, help="Target dependency prefix containing libepoxy")
    parser.add_argument("output", type=Path, help="Output native executable")
    args = parser.parse_args()
    build, deps, output = args.build.resolve(), args.deps.resolve(), args.output.resolve()
    rules = subprocess.check_output(["ninja", "-C", str(build), "-t", "rules"], text=True).splitlines()
    rules = [rule for rule in rules if rule.startswith("CXX_COMPILER__WebCore_")]
    entries = json.loads(subprocess.check_output(["ninja", "-C", str(build), "-t", "compdb", *rules]))
    entry = next((entry for entry in entries if entry["file"].endswith("/PlatformDisplaySkia.cpp")), None)
    if not entry:
        parser.error("The build must compile PlatformDisplaySkia.cpp as a separate source")

    command = shlex.split(entry["command"])
    compiler_end = next(i for i, arg in enumerate(command) if arg.startswith("-"))
    compiler = command[:compiler_end]
    compile_args = []
    i = compiler_end
    while i < len(command):
        arg = command[i]
        if arg in ("-o", "-MF", "-MT", "-include"):
            i += 2
            continue
        if arg in ("-MD", "-c", entry["file"], "-Werror=missing-profile") or arg.startswith("-fprofile"):
            i += 1
            continue
        compile_args.append(arg)
        i += 1
    output.parent.mkdir(parents=True, exist_ok=True)
    obj = output.with_suffix(".o")
    source = Path(__file__).with_name("skia-context-teardown.cpp").resolve()
    subprocess.run(compiler + compile_args + ["-c", str(source), "-o", str(obj)], cwd=build, check=True)
    target_flags = [arg for arg in compile_args if arg.startswith(("--sysroot=", "-specs="))]
    subprocess.run(compiler + target_flags + [str(obj), "-L" + str(build / "lib"),
        "-lWebKit", "-lJavaScriptCore", "-L" + str(deps / "lib"), "-lepoxy", "-lbe", "-lnetwork",
        "-o", str(output)], cwd=build, check=True)
    print(output)


if __name__ == "__main__":
    main()

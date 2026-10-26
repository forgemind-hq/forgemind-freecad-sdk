#!/usr/bin/env python3
"""Build the pinned ForgeCAD SDK in a dedicated workspace (Linux x86_64)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

from inputs import ROOT, canonical, load_lock, prepare, sha


def run(command, work, name, cwd=None, env=None):
    logs = work / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    path = logs / (name + ".log")
    if path.exists():
        revision = 1
        while (logs / f"{name}-{revision:03}.log").exists():
            revision += 1
        shutil.copy2(path, logs / f"{name}-{revision:03}.log")
    print(f"SDK {name}: {path}", file=sys.stderr, flush=True)
    with path.open("w") as output:
        output.write(json.dumps([str(x) for x in command]) + "\n")
        output.flush()
        result = subprocess.run([str(x) for x in command], cwd=cwd, env=env,
                                stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        print("\n".join(path.read_text(errors="replace").splitlines()[-60:]), file=sys.stderr)
        raise RuntimeError(f"{name} failed ({result.returncode}); retained {path}")


def runtime(work):
    return work / "package/runtime"


def build_python(work, jobs):
    prefix = runtime(work) / "python"
    executable = prefix / "bin/python3.11"
    if executable.exists():
        output = subprocess.check_output([executable, "-I", "-c", "import sys;print(sys.version_info[:3])"], text=True)
        if output.strip() != "(3, 11, 9)":
            raise ValueError("SDK Python is not exactly 3.11.9")
        if (prefix / ".forgecad-installed").exists():
            return prefix
    build = work / "build/cpython"
    build.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment.pop("PYTHONHOME", None)
    environment.pop("PYTHONPATH", None)
    environment["LDFLAGS"] = "-Wl,-rpath,'$$ORIGIN/../lib'"
    run([work / "source/cpython/configure", f"--prefix={prefix}", "--enable-shared",
         "--with-ensurepip=no"], work, "python-configure", cwd=build, env=environment)
    run(["make", f"-j{jobs}"], work, "python-build", cwd=build, env=environment)
    run(["make", "install"], work, "python-install", cwd=build, env=environment)
    (prefix / ".forgecad-installed").write_text(sha(prefix / "lib/libpython3.11.so.1.0") + "\n")
    return prefix


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["prepare", "python", "build", "env", "package", "verify", "register"])
    parser.add_argument("--work", type=Path, default=ROOT.parent / "work")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--qt-root", type=Path)
    parser.add_argument("--compiler-root", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--library", type=Path)
    args = parser.parse_args()
    if sys.platform != "linux" or platform.machine() != "x86_64":
        parser.error("SDK source build currently requires native Linux x86_64; use an explicitly supplied SDK on other targets")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    if args.command == "register":
        if not args.manifest or not args.library:
            parser.error("register requires --manifest and --library")
        # Called by CMake while the enclosing SDK build owns .prepare.lock.
        from build_sdk import register_bridge
        register_bridge(args.manifest, args.library)
        return
    # One source/build/install owner, including concurrent Cargo invocations.
    import fcntl
    with (work / ".prepare.lock").open("a") as guard:
        fcntl.flock(guard, fcntl.LOCK_EX)
        lock = prepare(work)
        if args.command == "prepare":
            return
        if args.command == "python":
            print(build_python(work, args.jobs))
            return
        manifest = work / "package/forgecad-sdk-export.json"
        from verify_sdk import verify_export
        abi = ROOT / "resources/forgecad_sdk_python_v1.h"
        if args.command in {"build", "env", "verify", "package"}:
            from build_sdk import (artifact_manifest, build_bridge, build_freecad, bundle_libraries,
                                   contract, export_sdk, python_extras, qt_prefix)
            from build_sdk import recipe_sha256
            reusable = False
            if manifest.exists():
                existing = json.loads(manifest.read_text())
                reusable = (existing["inputs_sha256"] == hashlib.sha256(canonical(lock)).hexdigest()
                            and existing.get("recipe_sha256") == recipe_sha256())
                if reusable:
                    verify_export(manifest, lock["target"], abi)
            if not reusable:
                if args.command in {"env", "verify"}:
                    raise ValueError("No completed SDK for the current sdk.lock.json; run sdk.py build")
                python = build_python(work, args.jobs)
                python_extras(work, lock)
                qt = qt_prefix(work, args.jobs, args.qt_root, run)
                _, digest = contract(work, lock)
                build_freecad(work, args.jobs, qt, python, run)
                bundle_libraries(work)
                artifact_manifest(work, digest)
                export_sdk(work, lock, digest)
            if args.compiler_root:
                build_bridge(work, args.compiler_root.resolve(), args.jobs, run)
            value = verify_export(manifest, lock["target"], abi)
            value["runtime_library"] = str(runtime(work) / "lib/libforgecad_runtime.so")
            if args.command == "package":
                import tarfile
                destination = ROOT.parent / "dist"
                destination.mkdir(exist_ok=True)
                archive = destination / ("forgecad-sdk-1.1.3-python3.11.9-" + lock["target"] + ".tar.gz")
                with tarfile.open(archive, "w:gz", compresslevel=1) as stream:
                    stream.add(manifest.parent, arcname="forgecad-sdk")
                archive.with_suffix(archive.suffix + ".sha256").write_text(sha(archive) + "  " + archive.name + "\n")
                value["archive"] = str(archive)
            print(json.dumps(value, sort_keys=True))
            return


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"SDK_ERROR: {error}", file=sys.stderr)
        sys.exit(2)

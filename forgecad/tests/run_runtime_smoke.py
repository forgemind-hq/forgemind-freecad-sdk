#!/usr/bin/env python3
"""Exercise a built SDK's real bridge in a fresh process, without Rust."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, default=root / "work/package")
    parser.add_argument("--compiler-root", type=Path, default=root.parent / "forgemind-cad-compiler-freecad")
    args = parser.parse_args()
    package = args.package.resolve()
    compiler = args.compiler_root.resolve()
    spec = importlib.util.spec_from_file_location("bootstrap", compiler / "tools/bootstrap_freecad_sdk.py")
    bootstrap = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bootstrap)
    value = {"sdk": str(package / "sdk"), "runtime": str(package / "runtime"),
             "qt": str(package / "qt"), "runtime_library": str(package / "runtime/lib/libforgecad_runtime.so")}
    with tempfile.TemporaryDirectory(prefix="forgecad-native-smoke-") as temporary:
        executable = Path(temporary) / "probe"
        subprocess.run(["c++", "-std=c++17", "-I", str(compiler / "native/include"),
                        str(Path(__file__).with_name("runtime_smoke.cpp")),
                        "-L", str(package / "runtime/lib"), "-lforgecad_runtime",
                        "-Wl,-rpath," + str(package / "runtime/lib"), "-o", str(executable)], check=True)
        result = subprocess.run([str(executable)], cwd="/", env=bootstrap.environment(value),
                                text=True, capture_output=True)
        report = {label: json.loads(payload) for label, payload in
                  (line.split(": ", 1) for line in result.stdout.splitlines())}
        print(json.dumps({"returncode": result.returncode, "operations": report, "stderr": result.stderr}, sort_keys=True))
        if result.returncode:
            raise SystemExit(result.returncode)
        measured = report["measure"]["value"]
        if not measured["valid"] or measured["solids"] != 1 or abs(measured["volume"] - 1000) > 1e-6:
            raise RuntimeError("Real Part box geometry failed")
        if report["assembly"]["value"] is not None:
            raise RuntimeError("Real Assembly module initialization failed")


if __name__ == "__main__":
    main()

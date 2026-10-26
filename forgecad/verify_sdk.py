"""Verify a relocatable SDK export without loading or initializing FreeCAD."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def member(root, name):
    path = PurePosixPath(name)
    if (not name or path.is_absolute() or ".." in path.parts or "\\" in name
            or ":" in name or path.as_posix() != name):
        raise ValueError("unsafe SDK path: " + name)
    result = (root / name).resolve()
    if not result.is_relative_to(root):
        raise ValueError("SDK path escapes export: " + name)
    return result


def verify_export(manifest, target, abi, require_binaries=True):
    manifest = Path(manifest).resolve()
    root = manifest.parent
    value = json.loads(manifest.read_text())
    if value["kind"] != "forgecad.sdk.export.v1" or value["target"] != target:
        raise ValueError("SDK export kind/target mismatch")
    if value["freecad_version"] != "1.1.3" or value["python_version"] != "3.11.9":
        raise ValueError("SDK export version mismatch")
    contract_path = member(root, value["contract"])
    contract = json.loads(contract_path.read_text())
    if hashlib.sha256(canonical(contract)).hexdigest() != value["contract_sha256"]:
        raise ValueError("SDK contract SHA256 mismatch")
    if contract["abi_sha256"] != sha(abi):
        raise ValueError("SDK/consumer private ABI mismatch")
    files = value["files"]
    if not files:
        raise ValueError("SDK binary/file inventory missing")
    for name, digest in files.items():
        if sha(member(root, name)) != digest:
            raise ValueError("SDK SHA256 mismatch: " + name)
    symlinks = value.get("symlinks", {})
    for name, destination in symlinks.items():
        path = root / name
        member(root, name)
        if not path.is_symlink() or os.readlink(path) != destination or not path.exists():
            raise ValueError("SDK symlink mismatch: " + name)
    actual = {p.relative_to(root).as_posix() for p in root.rglob("*")
              if p.is_file() and not p.is_symlink() and "__pycache__" not in p.parts
              and p != manifest}
    actual_links = {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_symlink()}
    if actual != set(files) or actual_links != set(symlinks):
        raise ValueError("SDK file inventory differs from export")
    if require_binaries:
        binaries = ["runtime/lib/libFreeCADApp.so", "runtime/lib/libFreeCADBase.so",
                    "runtime/python/lib/libpython3.11.so.1.0", "runtime/lib/Part.so",
                    "runtime/lib/Sketcher.so", "runtime/lib/_PartDesign.so", "runtime/lib/AssemblyApp.so",
                    "runtime/bin/freecadcmd", "sdk/src/FCConfig.h", "sdk/src/ForgeCADPythonBuildContract.h"]
        if not set(binaries) <= set(files):
            raise ValueError("SDK binary/header inventory incomplete")
        expected = {"runtime/lib/libFreeCADApp.so": ["forgecad_sdk_python_query_v1"],
                    "runtime/lib/libFreeCADBase.so": ["forgecad_base_python_" + name + "_v1" for name in
                        ["profile", "initialize", "finalize_before_app", "snapshot", "mark_unknown"]]}
        for name, symbols in expected.items():
            path = member(root, name)
            header = path.read_bytes()[:20]
            if header[:6] != b"\x7fELF\x02\x01" or int.from_bytes(header[18:20], "little") != 62:
                raise ValueError("SDK binary target header mismatch: " + name)
            exports = subprocess.check_output(["nm", "-D", "--defined-only", str(path)], text=True)
            exported = {line.split()[-1] for line in exports.splitlines() if line.split()}
            if not set(symbols) <= exported:
                raise ValueError("SDK binary lacks private exports: " + name)
    return {**{name: str(member(root, value[name])) for name in ["sdk", "runtime", "qt", "occt_include"]},
            "manifest": str(manifest), "contract_sha256": value["contract_sha256"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--abi", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(verify_export(args.manifest, args.target, args.abi), sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print("SDK_EXPORT_REJECTED: " + str(error), file=sys.stderr)
        sys.exit(2)

"""Build and export one consistent Python/FreeCAD instance."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

from inputs import ROOT, canonical, download, sha


def recipe_sha256():
    return hashlib.sha256(canonical({name: sha(ROOT / name) for name in
        ["sdk.py", "inputs.py", "build_sdk.py", "verify_sdk.py"]})).hexdigest()


def python_extras(work, lock):
    site = work / "package/runtime/python/lib/python3.11/site-packages"
    for record in lock["python_wheels"].values():
        marker = site / (".forgecad-" + record["archive_sha256"])
        if marker.exists():
            continue
        archive = download(record, work / "downloads")
        site.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive) as stream:
            for entry in stream.infolist():
                name = Path(entry.filename)
                if name.is_absolute() or ".." in name.parts or "\\" in entry.filename:
                    raise ValueError("unsafe Python wheel member")
            stream.extractall(site)
        marker.touch()


def bundle_libraries(work):
    """Bundle actual resolved ELF dependencies, retaining the host glibc baseline."""
    root = work / "package/runtime"
    lib = root / "lib"
    # Start from the SDK's actual link/import roots. PySide wheels also contain
    # optional GUI plugins whose dependencies are outside this headless SDK.
    candidates = [*lib.glob("*.so*"), *root.joinpath("bin").glob("*"),
                  *root.joinpath("python/lib").glob("libpython*.so*"),
                  *root.joinpath("python/lib/python3.11/lib-dynload").glob("*.so"),
                  root / "python/lib/python3.11/site-packages/PySide6/QtCore.abi3.so",
                  root / "python/lib/python3.11/site-packages/shiboken6/Shiboken.abi3.so"]
    libraries = [p for p in candidates if p.is_file() and not p.is_symlink()]
    skipped = {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1",
               "ld-linux-x86-64.so.2", "libresolv.so.2"}
    for path in libraries:
        result = subprocess.run(["ldd", str(path)], capture_output=True, text=True)
        if "not found" in result.stdout:
            raise ValueError("Unresolved SDK dependency: " + str(path) + "\n" + result.stdout)
        for soname, filename in re.findall(r"^\s*(\S+) => (\S+) \(", result.stdout, re.M):
            dependency = Path(filename).resolve()
            if soname in skipped or dependency.is_relative_to(root):
                continue
            destination = lib / soname
            if not destination.exists():
                shutil.copy2(dependency, destination)
                subprocess.run(["patchelf", "--set-rpath", "$ORIGIN", str(destination)], check=True)
            if ".so." in soname:
                linker_name = lib / (soname.split(".so.")[0] + ".so")
                if not linker_name.exists():
                    linker_name.symlink_to(soname)
            if soname.startswith("libxerces-c-"):
                linker_name = lib / "libxerces-c.so"
                if not linker_name.exists():
                    linker_name.symlink_to(soname)


def inventory(root, exclude=()):
    return {p.relative_to(root).as_posix(): sha(p) for p in sorted(Path(root).rglob("*"))
            if p.is_file() and not p.is_symlink() and not any(x in p.parts for x in exclude)}


def qt_prefix(work, jobs, explicit, run):
    prefix = work / "package/qt"
    if explicit:
        explicit = explicit.resolve()
        if not (explicit / "lib/cmake/Qt6Core/Qt6CoreConfig.cmake").exists():
            raise ValueError("--qt-root lacks Qt6Core CMake configuration")
        if not prefix.exists():
            shutil.copytree(explicit, prefix, symlinks=True)
        return prefix
    if (prefix / "lib/cmake/Qt6Core/Qt6CoreConfig.cmake").exists():
        return prefix
    build = work / "build/qtbase"
    run(["cmake", "-S", work / "source/qtbase", "-B", build, "-G", "Ninja",
         f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_BUILD_TYPE=Release",
         "-DQT_BUILD_TESTS=OFF", "-DQT_BUILD_EXAMPLES=OFF", "-DFEATURE_gui=OFF",
         "-DFEATURE_sql=OFF", "-DFEATURE_dbus=OFF", "-DFEATURE_printsupport=OFF"], work, "qt-configure")
    run(["cmake", "--build", build, "--parallel", jobs], work, "qt-build")
    run(["cmake", "--install", build], work, "qt-install")
    return prefix


def contract(work, lock):
    python = work / "package/runtime/python"
    stdlib = python / "lib/python3.11"
    native = stdlib / "lib-dynload"
    stable = inventory(stdlib, ("__pycache__", "lib-dynload"))
    dynamic = inventory(native)
    if not {"os.py", "encodings/__init__.py"} <= set(stable) or not dynamic:
        raise ValueError("Incomplete CPython installation")
    records = lambda files: [{"path": k, "sha256": v} for k, v in files.items()]
    modules = ["Part.so", "Sketcher.so", "_PartDesign.so", "AssemblyApp.so"]
    payload = {
        "schema_kind": "forgecad.sdk.python.build-contract.v1",
        "target": lock["target"], "abi_version": 65536, "sdk_release": "1.1.3",
        "python_hex": 0x030B09F0,
        "upstream_sha256": lock["archives"]["freecad"]["archive_sha256"],
        "patch_sha256": lock["resources"]["forgecad-python-v1.patch"],
        "abi_sha256": lock["resources"]["forgecad_sdk_python_v1.h"],
        "sdk_inputs_sha256": hashlib.sha256(canonical(lock)).hexdigest(),
        "cpython": {"version": "3.11.9", "hex": 0x030B09F0,
                    "source_archive_sha256": lock["archives"]["cpython"]["archive_sha256"],
                    "build_fingerprint": sha(python / "lib/libpython3.11.so.1.0"),
                    "runtime_files": list(dynamic)},
        "anchor": {"package_root_from_module": "..", "app": "lib/libFreeCADApp.so",
                   "artifact_manifest": "forgecad-runtime-artifacts.json"},
        "layout": {"freecad_program": "bin/freecadcmd", "python_home": "python",
                   "roles": [
                       {"role": "stdlib", "path": "python/lib/python3.11", "files": records(stable), "artifacts": []},
                       {"role": "native", "path": "python/lib/python3.11/lib-dynload", "files": records(dynamic), "artifacts": []},
                       {"role": "freecad", "path": "lib", "files": [], "artifacts": modules},
                       {"role": "scripts", "path": "Mod", "files": [{"path": "PartDesign/__init__.py",
                        "sha256": sha(work / "source/freecad/src/Mod/PartDesign/__init__.py")}], "artifacts": []}],
                   "module_inventory": [
                       {"name": "FreeCAD", "kind": "builtin", "role": "app", "path": "lib/libFreeCADApp.so"},
                       {"name": "Part", "kind": "extension", "role": "freecad", "path": "Part.so"},
                       {"name": "Sketcher", "kind": "extension", "role": "freecad", "path": "Sketcher.so"},
                       {"name": "PartDesign", "kind": "source", "role": "scripts", "path": "PartDesign/__init__.py"}]}}
    encoded = canonical(payload)
    digest = hashlib.sha256(encoded).hexdigest()
    values = ",".join("0x" + digest[i:i+2] for i in range(0, 64, 2))
    header = ("// Generated from sdk.lock.json and this actual CPython build.\n#pragma once\n"
              '#define FC_PY_BUILD_TARGET "' + lock["target"] + '"\n'
              '#define FC_PY_CONTRACT_SHA256_BYTES {' + values + '}\n'
              '#define FC_PY_CONTRACT_JSON ' + json.dumps(encoded.decode()) + '\n')
    for path, data in [(work / "source/freecad/src/ForgeCADPythonBuildContract.h", header.encode()),
                       (work / "build-contract.json", encoded + b"\n")]:
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)
    return payload, digest


def build_freecad(work, jobs, qt, python, run):
    source = work / "source/freecad"
    build = work / "build/freecad"
    prefix = work / "package/runtime"
    # Enable only the transitive workbench requirements of the compiler's SDK.
    enabled = {"PART", "PART_DESIGN", "SKETCHER", "ASSEMBLY", "SPREADSHEET", "MATERIAL"}
    options = re.findall(r"option\(BUILD_([A-Z_]+) ",
                         (source / "cMake/FreeCAD_Helpers/InitializeFreeCADBuildOptions.cmake").read_text())
    command = ["cmake", "-S", source, "-B", build, "-G", "Ninja",
               "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={prefix}",
               f"-DCMAKE_PROJECT_INCLUDE={ROOT}/resources/headless.cmake",
               "-DCMAKE_INSTALL_LIBDIR=lib", f"-DCMAKE_PREFIX_PATH={qt};{python}",
               f"-DPython3_EXECUTABLE={python}/bin/python3.11",
               f"-DPython3_INCLUDE_DIR={python}/include/python3.11",
               f"-DPython3_LIBRARY={python}/lib/libpython3.11.so",
               "-DFREECAD_QT_VERSION=6", "-DBUILD_GUI=OFF", "-DENABLE_DEVELOPER_TESTS=OFF",
               "-DFREECAD_CHECK_PIVY=OFF", "-DFREECAD_USE_PCL=OFF",
               "-DFREECAD_USE_EXTERNAL_FMT=ON", "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
               "-DGSL_TEST=OFF", "-DONDSELSOLVER_BUILD_TESTS=OFF",
               "-DCMAKE_INSTALL_RPATH=$ORIGIN;$ORIGIN/../lib;$ORIGIN/../python/lib;$ORIGIN/../../qt/lib",
               "-DCMAKE_BUILD_RPATH_USE_ORIGIN=ON", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
    command += [f"-DBUILD_{name}={'ON' if name in enabled else 'OFF'}" for name in options
                if name not in {"DYNAMIC_LINK_PYTHON", "WITH_CONDA", "FORCE_DIRECTORY"}]
    run(command, work, "freecad-configure")
    run(["cmake", "--build", build, "--parallel", jobs], work, "freecad-build")
    run(["cmake", "--install", build], work, "freecad-install")
    executable = prefix / "bin/freecadcmd"
    if not executable.exists():
        shutil.copy2(prefix / "bin/FreeCADCmd", executable)
    return prefix


def export_sdk(work, lock, digest):
    package = work / "package"
    xerces_alias = package / "runtime/lib/libxerces-c.so"
    if not xerces_alias.exists():
        candidates = sorted((package / "runtime/lib").glob("libxerces-c-*.so"))
        if len(candidates) != 1:
            raise ValueError("Expected exactly one bundled Xerces C++ runtime")
        xerces_alias.symlink_to(candidates[0].name)
    sdk = package / "sdk"
    shutil.copytree(work / "source/freecad", sdk, symlinks=False, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns(".forgecad-source.json", "__pycache__"))
    build = work / "build/freecad"
    shutil.copy2(work / "source/freecad/src/ForgeCADPythonBuildContract.h", sdk / "src/ForgeCADPythonBuildContract.h")
    for name in ["FCConfig.h", "config.h"]:
        generated = build / "src" / name
        if generated.is_file():
            shutil.copy2(generated, sdk / "src" / name)
    solver = sdk / "src/OndselSolver"
    solver.mkdir(exist_ok=True)
    shutil.copy2(work / "source/OndselSolver/OndselSolver/enum.h", solver / "enum.h")
    shutil.copy2(work / "source/OndselSolver/LICENSE", solver / "LICENSE")
    includes = package / "deps/include/opencascade"
    if not includes.exists():
        shutil.copytree("/usr/include/opencascade", includes)
    shutil.copy2(work / "build-contract.json", package / "forgecad-sdk-contract.json")
    tools = package / "tools"
    tools.mkdir(exist_ok=True)
    shutil.copy2(ROOT / "verify_sdk.py", tools / "verify_sdk.py")
    result = {"kind": "forgecad.sdk.export.v1", "target": lock["target"],
              "freecad_version": "1.1.3", "python_version": "3.11.9",
              "inputs_sha256": hashlib.sha256(canonical(lock)).hexdigest(),
              "recipe_sha256": recipe_sha256(),
              "contract_sha256": digest, "contract": "forgecad-sdk-contract.json",
              "sdk": "sdk", "runtime": "runtime", "qt": "qt",
              "occt_include": "deps/include/opencascade", "files": {}}
    result["files"] = inventory(package, ("__pycache__",))
    result["files"].pop("forgecad-sdk-export.json", None)
    result["symlinks"] = {p.relative_to(package).as_posix(): os.readlink(p)
                          for p in package.rglob("*") if p.is_symlink()}
    (package / "forgecad-sdk-export.json").write_bytes(canonical(result) + b"\n")
    return result


def artifact_manifest(work, digest, bridge=None):
    root = work / "package/runtime"
    roles = {"lib/libFreeCADApp.so": "app", "lib/libFreeCADBase.so": "base",
             "python/lib/libpython3.11.so.1.0": "python"}
    if bridge is not None:
        roles["lib/libforgecad_runtime.so"] = "bridge"
    for name in ["Part.so", "Sketcher.so", "_PartDesign.so", "AssemblyApp.so"]:
        roles["lib/" + name] = "module"
    value = {"kind": "forgecad.sdk.python.artifacts.v1", "target": "x86_64-unknown-linux-gnu",
             "contract_sha256": digest, "files": [{"role": role, "path": name, "sha256": sha(root / name)}
                                                     for name, role in roles.items()]}
    (root / "forgecad-runtime-artifacts.json").write_bytes(canonical(value) + b"\n")


def register_bridge(manifest, library):
    manifest = Path(manifest).resolve()
    value = json.loads(manifest.read_text())
    root = manifest.parent
    destination = root / value["runtime"] / "lib/libforgecad_runtime.so"
    if Path(library).resolve() != destination.resolve():
        shutil.copy2(library, destination)
    # The bridge is an output, bound outside the embedded contract to avoid self hashing.
    artifacts_path = root / value["runtime"] / "forgecad-runtime-artifacts.json"
    artifacts = json.loads(artifacts_path.read_text())
    artifacts["files"] = [item for item in artifacts["files"] if item["role"] != "bridge"]
    artifacts["files"].append({"role": "bridge", "path": "lib/libforgecad_runtime.so", "sha256": sha(destination)})
    artifacts_path.write_bytes(canonical(artifacts) + b"\n")
    value["files"]["runtime/lib/libforgecad_runtime.so"] = sha(destination)
    value["files"]["runtime/forgecad-runtime-artifacts.json"] = sha(artifacts_path)
    manifest.write_bytes(canonical(value) + b"\n")


def build_bridge(work, compiler, jobs, run):
    package = work / "package"
    build = work / "build/bridge"
    command = ["cmake", "-S", compiler, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
               f"-DFORGECAD_FREECAD_SDK={package}/sdk", f"-DFORGECAD_FREECAD_RUNTIME={package}/runtime",
               f"-DFORGECAD_SDK_EXPORT_MANIFEST={package}/forgecad-sdk-export.json",
               f"-DFORGECAD_OCCT_INCLUDE_DIR={package}/deps/include/opencascade",
               f"-DCMAKE_PREFIX_PATH={package}/qt", f"-DPython3_ROOT_DIR={package}/runtime/python"]
    run(command, work, "bridge-configure")
    run(["cmake", "--build", build, "--target", "forgecad_runtime", "--parallel", jobs], work, "bridge-build")

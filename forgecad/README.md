# ForgeCAD FreeCAD SDK

[简体中文](README.zh_CN.md)

This fork builds the FreeCAD SDK used by `forgemind-cad-compiler-freecad`. A normal FreeCAD download does not provide ForgeCAD's private Python lifecycle interface. This repository maintains that interface on the official FreeCAD **1.1.3** source baseline and builds it with its own **CPython 3.11.9** instance.

For example, a compiler checkout can build its native bridge without pointing to a developer's old FreeCAD installation. If the default sibling SDK directory is absent, the compiler clones the published `feat/forgecad-python-sdk` branch automatically. The SDK repository downloads fixed source archives and Python wheels, verifies their SHA-256 values, applies exact patches, and builds a matching runtime. The compiler then uses that runtime for builds and tests.

## Build

The source build currently supports **Linux x86_64 on Ubuntu 24.04**. It builds a headless compiler SDK with Part, PartDesign, Sketcher, Assembly and their prerequisites. Desktop GUI builds and macOS/Windows binaries require separate platform work.

Install the native build prerequisites once:

```sh
sudo apt-get install --no-install-recommends build-essential cmake ninja-build swig patchelf \
  libboost-filesystem-dev libboost-program-options-dev libboost-regex-dev \
  libboost-system-dev libboost-thread-dev libeigen3-dev libfmt-dev libyaml-cpp-dev \
  libxerces-c-dev libocct-data-exchange-dev libocct-ocaf-dev libtbb-dev \
  libfreetype-dev libdouble-conversion-dev zlib1g-dev libssl-dev libffi-dev \
  libbz2-dev liblzma-dev libreadline-dev libsqlite3-dev
python3 -E -B forgecad/sdk.py build --compiler-root ../forgemind-cad-compiler-freecad
```

Qt 6.8.3 is downloaded and built when needed. `--qt-root /path/to/qt/6.8.3` can seed the SDK from an existing compatible Qt installation; the installation is copied into the SDK package. No installed FreeCAD SDK is patched.

The final command prints a JSON descriptor containing `sdk`, `runtime`, `qt`, `occt_include`, `runtime_library` and `manifest`. Build logs are under `work/logs/`; `work/package/` contains the exported SDK. Source archives, compilation outputs and packages are ignored by Git. Use `--work /absolute/directory` to select an independent build workspace and `--jobs N` to control parallelism.

## Compiler integration

Place both repositories in the same parent directory:

```sh
cd ../forgemind-cad-compiler-freecad
cargo build --locked --features native-runtime --bins
scripts/test-native.sh --test runtime_tests
```

`FORGECAD_SDK_REPOSITORY` overrides the SDK repository location. `FORGECAD_SDK_WORK` selects its build workspace. An explicit `FORGECAD_RUNTIME_LIBRARY` bypasses automatic preparation. Default Rust builds without `native-runtime` do not download an SDK.

Create and verify a standalone SDK archive:

```sh
python3 -E -B forgecad/sdk.py verify
python3 -E -B forgecad/sdk.py package --compiler-root ../forgemind-cad-compiler-freecad
```

The archive and its SHA-256 sidecar are written to `dist/`. The verifier checks the target, private ABI, file hashes and actual ELF exports; actual initialization and business behavior are verified separately by the compiler's native tests. The package retains the host glibc baseline, so it is not a promise of compatibility with older Linux distributions.

## Source and patch maintenance

`upstream` points to [FreeCAD/FreeCAD](https://github.com/FreeCAD/FreeCAD). The `feat/forgecad-python-sdk` branch starts from tag `1.1.3`, commit `145529fe741292ff0b3977a01195bf0247425794`. ForgeCAD's private interface and compatibility adaptations are visible in the branch's source changes. Build recipes, fixed archive identities, exact patch payloads and tests live under `forgecad/`.

The build uses an isolated, verified source export in `work/source/`, including the four submodules pinned by the upstream tag. It checks the fork's modified source against the locked patch payloads before building. New source changes require updating those payloads and `sdk.lock.json`; unrelated or drifting source is rejected. The historical F03 approval records in the compiler/HQ repositories remain unchanged.

The supplemental changes use explicit Qt string literals under FreeCAD's `QT_NO_CAST_FROM_ASCII` policy, omit translated UI resources in this headless SDK, and guard Assembly's GUI-only imports. PySide6 and Shiboken 6.8.3 come from fixed real wheels; no fake FreeCAD or GUI modules are installed.

## Tests and licenses

```sh
python3 -E -B -m unittest discover -s forgecad/tests -v
python3 -E -B forgecad/tests/run_runtime_smoke.py
```

`forgecad/tests/sdk_query_tests.cpp` exercises the actual exported capability query and rejects wrong ABI versions without modifying caller output. It is a read-only query test, distinct from native initialization.

FreeCAD's [original license](../LICENSE) and source-file notices are retained. Python, Qt, Python wheels and system dependencies retain their respective licenses. Check the relevant source and package notices when redistributing the SDK.

`run_runtime_smoke.py` directly calls the real bridge in a fresh process to exercise SDK initialization, a document, valid Part box geometry and volume, Assembly imports, and release. It does not require a successful Rust build. Use `--package` to test an SDK unpacked at a new location.

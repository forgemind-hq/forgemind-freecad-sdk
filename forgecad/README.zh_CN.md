# ForgeCAD FreeCAD SDK

[English](README.md)

本仓库基于官方 FreeCAD **1.1.3** 维护 ForgeCAD 的私有 Python 生命周期接口，并用独立的 **CPython 3.11.9** 编译 SDK，供 `forgemind-cad-compiler-freecad` 使用。普通 FreeCAD 发行包没有这套私有接口。

编译器会使用同级的本仓库；默认目录缺失时，会自动克隆已发布的 `feat/forgecad-python-sdk` 分支。随后编译器可以自动准备原生依赖，无须再指向某位开发者的旧 FreeCAD 安装。准备流程会下载固定的源码归档和 Python wheel，核对 SHA-256，应用精确补丁，再编译配套的运行库。

## 构建

当前源码构建支持 **Ubuntu 24.04、Linux x86_64**，产物是包含 Part、PartDesign、Sketcher、Assembly 及其前置模块的无 GUI 编译器 SDK。Desktop GUI 以及 macOS、Windows 的实际产物需要另行适配和验证。

首次构建前安装开发工具和系统依赖：

```sh
sudo apt-get install --no-install-recommends build-essential cmake ninja-build swig patchelf \
  libboost-filesystem-dev libboost-program-options-dev libboost-regex-dev \
  libboost-system-dev libboost-thread-dev libeigen3-dev libfmt-dev libyaml-cpp-dev \
  libxerces-c-dev libocct-data-exchange-dev libocct-ocaf-dev libtbb-dev \
  libfreetype-dev libdouble-conversion-dev zlib1g-dev libssl-dev libffi-dev \
  libbz2-dev liblzma-dev libreadline-dev libsqlite3-dev
python3 -E -B forgecad/sdk.py build --compiler-root ../forgemind-cad-compiler-freecad
```

缺少 Qt 时会自动下载并编译 Qt 6.8.3。已有配套 Qt 的机器可以加 `--qt-root /path/to/qt/6.8.3`，工具会把它复制到 SDK 包中。旧 FreeCAD 安装不会被打补丁。

命令最终输出 JSON，给出 `sdk`、`runtime`、`qt`、`occt_include`、`runtime_library` 和 `manifest`。构建日志在 `work/logs/`，SDK 导出在 `work/package/`。下载缓存、编译目录和制品均被 Git 忽略。可用 `--work /absolute/directory` 切换独立构建目录，用 `--jobs N` 设置并行度。

## 接入编译器

两个仓库放在同一父目录后执行：

```sh
cd ../forgemind-cad-compiler-freecad
cargo build --locked --features native-runtime --bins
scripts/test-native.sh --test runtime_tests
```

如果目录不同，设置 `FORGECAD_SDK_REPOSITORY`；需要独立构建缓存时设置 `FORGECAD_SDK_WORK`。显式配置 `FORGECAD_RUNTIME_LIBRARY` 时直接使用该桥接库。没有启用 `native-runtime` 的普通 Rust 构建不下载 SDK。

在 SDK 仓库中校验和打包：

```sh
python3 -E -B forgecad/sdk.py verify
python3 -E -B forgecad/sdk.py package --compiler-root ../forgemind-cad-compiler-freecad
```

压缩包及 SHA-256 文件写入 `dist/`。导出校验会检查目标平台、私有 ABI、文件摘要和 ELF 的实际导出符号；初始化与业务行为由编译器的原生测试单独验证。包保留构建主机的 glibc 基线，不承诺能在更旧的 Linux 发行版运行。

## 源码与补丁

`upstream` 指向 [FreeCAD 官方仓库](https://github.com/FreeCAD/FreeCAD)。补丁分支为 `feat/forgecad-python-sdk`，从 `1.1.3` tag 的提交 `145529fe741292ff0b3977a01195bf0247425794` 分出。私有接口与兼容性改动直接体现在源码差异中；构建配方、归档身份、精确补丁载荷和测试统一放在 `forgecad/`。

工具会在 `work/source/` 构建独立源码副本，补齐官方 tag 锁定的四个子模块，并先核对 fork 中的改动与固定补丁一致。维护新改动时需要同步更新补丁载荷和 `sdk.lock.json`；未声明的源码变化和摘要漂移会报错。编译器及 HQ 原有的 F03 审批材料保持原样。

补充修正包括：用显式 Qt 字符串满足 FreeCAD 的 `QT_NO_CAST_FROM_ASCII` 要求；无 GUI 构建省略界面翻译资源；为 Assembly 的 GUI 导入补充条件。PySide6 和 Shiboken 使用固定的真实 6.8.3 wheel，不安装假的 FreeCAD 或 GUI 模块。

## 测试与许可证

```sh
python3 -E -B -m unittest discover -s forgecad/tests -v
python3 -E -B forgecad/tests/run_runtime_smoke.py
```

`forgecad/tests/sdk_query_tests.cpp` 用于加载真实 SDK 并验证私有能力查询，包括错误 ABI 不改写输出的约束。它只验证查询，不能替代真实初始化。`run_runtime_smoke.py` 会在全新进程直接调用真实桥接库，验证 SDK 初始化、文档、Part 立方体的有效性及体积、Assembly 导入和释放，不依赖 Rust 编译成功。可用 `--package` 验证解包到新目录的 SDK。

保留 FreeCAD 的[原许可证](../LICENSE)和各源码文件声明。Python、Qt、Python wheel 和系统依赖分别保留自身许可证；分发时应一并核对相应源码与包内声明。

// =============================================================================
//    Copyright (c) 2026 Haixing Hu.
//
//    SPDX-License-Identifier: Apache-2.0
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.
// =============================================================================
// SDK-private integration; application and interpreter remain SDK-owned.
#define FORGECAD_SDK_BUILD_INTERNAL
#include <Base/ForgeCADPythonIntegration.h>
#include <ForgeCADPythonBuildContract.h>
#include <Base/Interpreter.h>
#include "Application.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// F03_APP_STATE_BEGIN
struct FcPyModuleProvider {
    std::string name;
    std::string kind;
    QString physical_path;
    QString zip_member;
};
struct FcPyContextV1 {
    FcPyConfigV1 config{};
    std::string program, home, application, user_parameter, system_parameter;
    std::vector<std::string> paths;
    std::vector<FcPyUtf8V1> path_views;
    std::vector<FcPyModuleProvider> providers;
    std::map<std::string, std::string> previous_config;
    std::thread::id owner;
    bool snapshot = false;
    bool started = false;
    bool entered_app = false;
    bool app_ready = false;
    bool terminal = false;
    FcPyReportV1 report{sizeof(FcPyReportV1), FC_PY_ABI_V1, 0, 0, FC_PY_PREPARED,
                        0, 0, 0, 0, FC_PY_STAGE_NONE, {0}};
};
namespace {
std::atomic<bool> forgecad_app_used{false};
std::atomic<bool> forgecad_app_poison{false};
// Application::initConfig stores argv in process-global SDK state. This backing
// belongs to the SDK process, including after an Unknown ctx is destroyed.
// One-shot admission ensures it is never replaced while App could retain it.
std::vector<char> forgecad_application_argument;
char* forgecad_application_argv[2] = {nullptr,nullptr};
const unsigned char forgecad_contract_sha[] = FC_PY_CONTRACT_SHA256_BYTES;
const char forgecad_contract_json[] = FC_PY_CONTRACT_JSON;

bool valid_report(const FcPyReportV1* out) noexcept
{
    return out && out->struct_size == sizeof(*out) && out->abi_version == FC_PY_ABI_V1;
}
bool valid_base_result(int32_t status, const FcPyReportV1& value, bool require_saved) noexcept
{
    if (!valid_report(&value) || status < 0 || status > FC_PY_E_POISONED
        || static_cast<uint32_t>(status) != value.primary_status
        || value.cleanup_status > FC_PY_E_POISONED || value.lifecycle > FC_PY_UNKNOWN
        || value.python_state > 2 || value.gil_state > 2 || value.finalize_calls > 1
        || (value.flags & ~31u) || value.failure_stage > FC_PY_STAGE_PROTECTION_RESTORE) {
        return false;
    }
    return !require_saved || status != FC_PY_OK
        || (value.lifecycle == FC_PY_SAVED && value.python_state == 1
            && value.gil_state == 0 && value.finalize_calls == 0
            && !(value.flags & FC_PY_REPORT_POISON));
}
void diagnostic(FcPyReportV1& report, const char* text) noexcept
{
    if (!text) { text = "SDK integration failed"; }
    size_t length = std::min(std::strlen(text), sizeof(report.diagnostic)-1);
    while (length && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) { --length; }
    std::memcpy(report.diagnostic, text, length);
    report.diagnostic[length] = '\0';
}
int32_t result(FcPyContextV1& ctx, FcPyReportV1* out) noexcept
{
    *out = ctx.report;
    return static_cast<int32_t>(ctx.report.primary_status);
}
int32_t error(FcPyReportV1* out, uint32_t code, const char* text) noexcept
{
    FcPyReportV1 value{sizeof(FcPyReportV1), FC_PY_ABI_V1, code, 0, FC_PY_COLD,
                       0, 0, 0, 0, FC_PY_STAGE_PREPARE, {0}};
    diagnostic(value, text);
    *out = value;
    return static_cast<int32_t>(code);
}
void unknown(FcPyContextV1& ctx, uint32_t stage, uint32_t primary, const char* text) noexcept
{
    FcPyReportV1 base{sizeof(FcPyReportV1), FC_PY_ABI_V1, 0, 0, 0, 0, 0, 0, 0, 0, {0}};
    (void)forgecad_base_python_mark_unknown_v1(stage, &base);
    ctx.report = base;
    ctx.report.primary_status = primary;
    if (ctx.entered_app) { ctx.report.flags |= FC_PY_REPORT_APP_ENTERED; }
    if (ctx.app_ready) { ctx.report.flags |= FC_PY_REPORT_APP_READY; }
    diagnostic(ctx.report, text);
    ctx.terminal = true;
    forgecad_app_poison.store(true, std::memory_order_release);
}
void launchers_absent()
{
    for (const char* key : {"PYTHONEXECUTABLE", "__PYVENV_LAUNCHER__"}) {
        if (std::getenv(key)) { throw std::runtime_error("Python launcher environment must be absent"); }
#ifdef _WIN32
        const std::wstring wide(key, key + std::strlen(key));
        SetLastError(ERROR_SUCCESS);
        const DWORD needed = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
        if (_wgetenv(wide.c_str()) || needed || GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
            throw std::runtime_error("Python launcher environment must be absent in every view");
        }
#endif
    }
}
std::string copy_utf8(FcPyUtf8V1 value, uint64_t& total, bool allow_empty = false)
{
    if ((!value.data && value.size) || value.size > 1048576
        || value.size > 16777216-total) { throw std::runtime_error("SDK input size exceeds limit"); }
    if (!value.size) {
        if (value.data || !allow_empty) { throw std::runtime_error("Invalid empty SDK UTF8 slice"); }
        return {};
    }
    const auto size = static_cast<int>(value.size);
    const std::string bytes(value.data, static_cast<size_t>(value.size));
    if (bytes.find('\0') != std::string::npos
        || QString::fromUtf8(bytes.data(), size).toUtf8() != QByteArray(bytes.data(), size)) {
        throw std::runtime_error("SDK input is not strict Unicode UTF8");
    }
    total += value.size;
    return bytes;
}
FcPyUtf8V1 view(const std::string& value) noexcept
{
    return value.empty() ? FcPyUtf8V1{nullptr,0} : FcPyUtf8V1{value.data(),value.size()};
}
// F03_APP_STATE_END

// F03_APP_LAYOUT_BEGIN
QString loaded_path(const void* address)
{
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module)) {
        throw std::runtime_error("Cannot identify loaded SDK module");
    }
    std::vector<wchar_t> buffer(32768);
    const DWORD size = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!size || size >= buffer.size()) { throw std::runtime_error("SDK module path truncated"); }
    return QFileInfo(QString::fromWCharArray(buffer.data(), static_cast<int>(size))).canonicalFilePath();
#else
    Dl_info info{};
    if (!dladdr(address, &info) || !info.dli_fname) { throw std::runtime_error("Cannot identify loaded SDK module"); }
    return QFileInfo(QString::fromUtf8(info.dli_fname)).canonicalFilePath();
#endif
}
QString safe_member(const QString& root, const QString& relative)
{
    if (relative.isEmpty() || QDir::isAbsolutePath(relative)
        || relative.contains(QLatin1Char('\\')) || relative.split(QLatin1Char('/')).contains(QStringLiteral(".."))
        || relative.split(QLatin1Char('/')).contains(QStringLiteral("."))) { throw std::runtime_error("Invalid package-relative SDK member"); }
    const QString path = QFileInfo(QDir(root).filePath(relative)).canonicalFilePath();
    if (path.isEmpty() || (path != root && !path.startsWith(root + QLatin1Char('/')))) {
        throw std::runtime_error("SDK member escapes package root or is missing");
    }
    return path;
}
QJsonObject json_file(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16777216) {
        throw std::runtime_error("SDK artifact manifest missing or oversized");
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        throw std::runtime_error("Invalid SDK artifact manifest JSON");
    }
    return document.object();
}
void check_sha(const QString& path, const QString& expected)
{
    const QByteArray bytes = expected.toLatin1();
    if (bytes.size() != 64) { throw std::runtime_error("Invalid SDK content fingerprint"); }
    for (const auto c : bytes) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            throw std::runtime_error("Invalid SDK content fingerprint");
        }
    }
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file) || hash.result().toHex() != bytes) {
        throw std::runtime_error("SDK package content fingerprint mismatch");
    }
}
void check_layout(FcPyContextV1& ctx)
{
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(forgecad_contract_json, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        throw std::runtime_error("Invalid embedded SDK build contract");
    }
    const auto contract = document.object();
    if (contract.value(QStringLiteral("schema_kind")).toString() != QStringLiteral("forgecad.sdk.python.build-contract.v1")
        || contract.value(QStringLiteral("target")).toString() != QString::fromLatin1(FC_PY_BUILD_TARGET)) {
        throw std::runtime_error("SDK build contract profile mismatch");
    }
    const auto anchor = contract.value(QStringLiteral("anchor")).toObject();
    const QString app = loaded_path(reinterpret_cast<const void*>(&forgecad_sdk_python_query_v1));
    const QString root = QFileInfo(QDir(QFileInfo(app).absolutePath()).filePath(
        anchor.value(QStringLiteral("package_root_from_module")).toString())).canonicalFilePath();
    if (root.isEmpty() || safe_member(root, anchor.value(QStringLiteral("app")).toString()) != app) {
        throw std::runtime_error("SDK module/package anchor mismatch");
    }
    const auto layout = contract.value(QStringLiteral("layout")).toObject();
    const QFileInfo program(QString::fromUtf8(ctx.program.c_str()));
    if (!program.isAbsolute() || !program.isFile() || !program.isExecutable()
        || program.canonicalFilePath() != safe_member(root,layout.value(QStringLiteral("freecad_program")).toString())) {
        throw std::runtime_error("Python program does not match the SDK executable");
    }
    for (const auto* parameter : {&ctx.user_parameter,&ctx.system_parameter}) {
        const QFileInfo file(QString::fromUtf8(parameter->c_str()));
        if (!file.isAbsolute() || !QFileInfo(file.absolutePath()).isDir()
            || file.fileName()==QStringLiteral(".") || file.fileName()==QStringLiteral("..")) {
            throw std::runtime_error("SDK parameter file must have an existing absolute parent");
        }
    }
    const auto artifact = json_file(safe_member(root, anchor.value(QStringLiteral("artifact_manifest")).toString()));
    const QString digest = QString::fromLatin1(QByteArray(
        reinterpret_cast<const char*>(forgecad_contract_sha), 32).toHex());
    if (artifact.value(QStringLiteral("contract_sha256")).toString() != digest
        || artifact.value(QStringLiteral("target")).toString() != QString::fromLatin1(FC_PY_BUILD_TARGET)) {
        throw std::runtime_error("SDK artifact/build contract mismatch");
    }
    std::map<QString,QString> loaded;
    loaded[QStringLiteral("app")] = app;
    loaded[QStringLiteral("base")] = loaded_path(reinterpret_cast<const void*>(&forgecad_base_python_profile_v1));
    loaded[QStringLiteral("python")] = loaded_path(reinterpret_cast<const void*>(&Py_IsInitialized));
    bool bridge = false;
    std::map<QString,QString> artifact_hashes;
    const auto artifacts = artifact.value(QStringLiteral("files")).toArray();
    for (const auto& value : artifacts) {
        const auto file = value.toObject();
        const auto role = file.value(QStringLiteral("role")).toString();
        const auto path = safe_member(root, file.value(QStringLiteral("path")).toString());
        const auto content_hash = file.value(QStringLiteral("sha256")).toString();
        check_sha(path, content_hash);
        const auto previous = artifact_hashes.find(path);
        if (previous != artifact_hashes.end() && previous->second != content_hash) {
            throw std::runtime_error("Conflicting SDK artifact fingerprints");
        }
        artifact_hashes[path] = content_hash;
        auto expected = loaded.find(role);
        if (expected != loaded.end()) {
            if (expected->second != path) { throw std::runtime_error("Loaded SDK dependency identity mismatch"); }
            loaded.erase(expected);
        }
        if (role == QStringLiteral("bridge")) { bridge = true; }
    }
    if (!loaded.empty() || !bridge) { throw std::runtime_error("Incomplete SDK binary artifact manifest"); }
    if (ctx.config.mode == FC_PY_MODE_EXPLICIT && QFileInfo(QString::fromUtf8(ctx.home.c_str())).canonicalFilePath()
        != safe_member(root, layout.value(QStringLiteral("python_home")).toString())) {
        throw std::runtime_error("Python home does not match SDK package");
    }
    std::vector<QString> search;
    for (const auto& item : ctx.paths) {
        const QFileInfo file(QString::fromUtf8(item.c_str()));
        const auto path = file.canonicalFilePath();
        if (!file.isAbsolute() || path.isEmpty() || (!file.isDir()
            && !(file.isFile() && file.suffix().compare(QStringLiteral("zip"),Qt::CaseInsensitive)==0))) {
            throw std::runtime_error("Invalid complete Python search member");
        }
        search.push_back(path);
    }
    bool stdlib = false, native = false, freecad = false;
    for (const auto& value : layout.value(QStringLiteral("roles")).toArray()) {
        const auto role = value.toObject();
        const auto path = safe_member(root, role.value(QStringLiteral("path")).toString());
        if (ctx.config.mode == FC_PY_MODE_EXPLICIT
            && std::find(search.begin(), search.end(), path) == search.end()) {
            throw std::runtime_error("Complete Python search list omits a required package root");
        }
        const auto name = role.value(QStringLiteral("role")).toString();
        stdlib = stdlib || name==QStringLiteral("stdlib");
        native = native || name==QStringLiteral("native");
        freecad = freecad || name==QStringLiteral("freecad");
        if (QFileInfo(path).isFile()) {
            // The complete ZIP member inventory was verified by the source
            // preparation tool. Runtime consumption binds the whole ZIP.
            check_sha(path,role.value(QStringLiteral("sha256")).toString());
            continue;
        }
        for (const auto& item : role.value(QStringLiteral("files")).toArray()) {
            const auto file = item.toObject();
            check_sha(safe_member(path,file.value(QStringLiteral("path")).toString()),file.value(QStringLiteral("sha256")).toString());
        }
        for (const auto& item : role.value(QStringLiteral("artifacts")).toArray()) {
            const auto member = safe_member(path,item.toString());
            if (artifact_hashes.find(member) == artifact_hashes.end()) {
                throw std::runtime_error("SDK output member absent from binary artifact manifest");
            }
        }
    }
    if (!stdlib || !native || !freecad) { throw std::runtime_error("SDK layout role inventory is incomplete"); }
    // Bind the required imports to concrete package members before SDK globals
    // change. The context owns these copies until start/stop/destroy completes.
    std::map<QString,QJsonObject> roles;
    for (const auto& value : layout.value(QStringLiteral("roles")).toArray()) {
        const auto role = value.toObject();
        const auto name = role.value(QStringLiteral("role")).toString();
        if (!roles.emplace(name,role).second) { throw std::runtime_error("Duplicate SDK layout role"); }
    }
    std::map<QString,FcPyModuleProvider> providers;
    std::map<QString,QString> identities;
    for (const auto& value : layout.value(QStringLiteral("module_inventory")).toArray()) {
        const auto item = value.toObject();
        const auto name = item.value(QStringLiteral("name")).toString();
        const auto kind = item.value(QStringLiteral("kind")).toString();
        const auto member = item.value(QStringLiteral("path")).toString();
        FcPyModuleProvider provider{name.toStdString(),kind.toStdString(),{}, {}};
        if (name.isEmpty() || providers.find(name) != providers.end()) {
            throw std::runtime_error("Missing/duplicate SDK import identity");
        }
        if (kind==QStringLiteral("builtin")) {
            if (name!=QStringLiteral("FreeCAD") || item.value(QStringLiteral("role")).toString()!=QStringLiteral("app")
                || safe_member(root,member)!=app) { throw std::runtime_error("Invalid builtin SDK provider"); }
            provider.physical_path=app;
        } else {
            const auto role=roles.find(item.value(QStringLiteral("role")).toString());
            if (role==roles.end() || (kind!=QStringLiteral("source") && kind!=QStringLiteral("extension"))) {
                throw std::runtime_error("Invalid SDK import role/kind");
            }
            if (member.isEmpty() || QDir::isAbsolutePath(member) || member.contains(QLatin1Char('\\'))
                || member.split(QLatin1Char('/')).contains(QStringLiteral("..")) || member.split(QLatin1Char('/')).contains(QStringLiteral("."))
                || member.split(QLatin1Char('/')).contains(QStringLiteral(""))) { throw std::runtime_error("Invalid SDK import member"); }
            const auto base=safe_member(root,role->second.value(QStringLiteral("path")).toString());
            bool declared=false;
            for (const auto& stable : role->second.value(QStringLiteral("files")).toArray()) {
                declared=declared || stable.toObject().value(QStringLiteral("path")).toString()==member;
            }
            for (const auto& output : role->second.value(QStringLiteral("artifacts")).toArray()) { declared=declared || output.toString()==member; }
            if (!declared) { throw std::runtime_error("SDK provider absent from complete role inventory"); }
            if (kind==QStringLiteral("source")) {
                const auto leaf=name.section(QLatin1Char('.'),-1);
                const QFileInfo source(member);
                if (!member.endsWith(QStringLiteral(".py")) || (source.fileName()!=leaf+QStringLiteral(".py")
                    && !(source.fileName()==QStringLiteral("__init__.py") && QFileInfo(source.path()).fileName()==leaf))) {
                    throw std::runtime_error("SDK source provider disagrees with import identity");
                }
            } else if (member.endsWith(QStringLiteral(".py"))) { throw std::runtime_error("SDK extension provider is a Python source file"); }
            if (QFileInfo(base).isFile()) {
                if (kind!=QStringLiteral("source")) { throw std::runtime_error("Native extensions cannot be loaded from ZIP roles"); }
                provider.physical_path=base;
                provider.zip_member=member;
            } else { provider.physical_path=safe_member(base,member); }
            const auto identity=provider.physical_path+(provider.zip_member.isEmpty()?QString{}:QLatin1Char('/')+provider.zip_member);
            if (!identities.emplace(identity,name).second) { throw std::runtime_error("Required SDK imports share a provider"); }
        }
        providers.emplace(name,std::move(provider));
    }
    for (const auto* name : {"FreeCAD","Part","Sketcher","PartDesign"}) {
        const auto provider=providers.find(QString::fromLatin1(name));
        if (provider==providers.end()) { throw std::runtime_error("Required SDK module inventory is incomplete"); }
        ctx.providers.push_back(provider->second);
    }
}

struct OwnedPythonReference {
    PyObject* value;
    explicit OwnedPythonReference(PyObject* object) noexcept : value(object) {}
    ~OwnedPythonReference() { if (value) { Py_DECREF(value); } }
    OwnedPythonReference(const OwnedPythonReference&)=delete;
    OwnedPythonReference& operator=(const OwnedPythonReference&)=delete;
};
void check_import_provider(const FcPyModuleProvider& provider, PyObject* imported)
{
    // All APIs below are public CPython 3.11 APIs, called with the SDK-held GIL.
    // A module definition pointer is never dereferenced or passed across the ABI.
    if (!PyModule_Check(imported)) { throw std::runtime_error("SDK import is not a module"); }
    const auto definition=PyModule_GetDef(imported);
    if (provider.kind=="builtin") {
        // FreeCAD's fixed init_freecad_module uses an App-local static PyModuleDef.
        // PyCXX extension definitions may be heap allocated: do not owner-probe them.
        if (!definition || loaded_path(static_cast<const void*>(definition))!=provider.physical_path) {
            throw std::runtime_error("FreeCAD builtin provider is not the loaded SDK App");
        }
        return;
    }
    if ((provider.kind=="extension") != (definition!=nullptr)) {
        throw std::runtime_error("SDK module provider kind mismatch");
    }
    OwnedPythonReference filename(PyModule_GetFilenameObject(imported));
    if (!filename.value) { PyErr_Clear(); throw std::runtime_error("SDK module provider has no filename"); }
    Py_ssize_t length=0;
    const char* bytes=PyUnicode_AsUTF8AndSize(filename.value,&length);
    if (!bytes || length<=0 || length>1048576 || std::memchr(bytes,0,static_cast<size_t>(length))) {
        PyErr_Clear(); throw std::runtime_error("Invalid SDK provider Unicode filename");
    }
    const auto origin=QDir::fromNativeSeparators(QString::fromUtf8(bytes,static_cast<int>(length)));
    if (provider.zip_member.isEmpty()) {
        if (QFileInfo(origin).canonicalFilePath()!=provider.physical_path) {
            throw std::runtime_error("Imported SDK module comes from a different provider");
        }
    } else {
        // A ZIP origin is virtual. Resolve only its physical ZIP prefix, and
        // compare its already verified canonical member as an exact suffix.
        const auto suffix=QLatin1Char('/')+provider.zip_member;
        if (!origin.endsWith(suffix) || QFileInfo(origin.left(origin.size()-suffix.size())).canonicalFilePath()!=provider.physical_path) {
            throw std::runtime_error("Imported SDK source comes from a different ZIP/member");
        }
    }
}
// F03_APP_LAYOUT_END

// F03_APP_LIFECYCLE_BEGIN
int32_t FC_PY_CALL stop_sdk(FcPyContextV1* ctx, FcPyReportV1* out) noexcept;
int32_t FC_PY_CALL prepare_sdk(const FcPyConfigV1* request, FcPyContextV1** output,
                              FcPyReportV1* out) noexcept
{
    if (!valid_report(out)) { return FC_PY_E_ABI; }
    if (!output) { return error(out,FC_PY_E_INPUT,"Missing SDK context output"); }
    *output = nullptr;
    if (!request || request->struct_size != sizeof(*request) || request->abi_version != FC_PY_ABI_V1) {
        return error(out,FC_PY_E_ABI,"SDK configuration ABI mismatch");
    }
    if (forgecad_app_poison.load(std::memory_order_acquire)) { return error(out,FC_PY_E_POISONED,"SDK process state unknown"); }
    if (forgecad_app_used.load(std::memory_order_acquire) || Py_IsInitialized()) { return error(out,FC_PY_E_ALREADY_USED,"Owned SDK requires a fresh process"); }
    if (request->flags != 3 || request->mode > 1 || request->reserved[0] || request->reserved[1]
        || request->path_count > 4096 || (!request->paths && request->path_count)) {
        return error(out,FC_PY_E_INPUT,"Invalid SDK configuration fields");
    }
#ifndef FC_OS_LINUX
    if (request->mode == FC_PY_MODE_SDK_AUTO) { return error(out,FC_PY_E_INPUT,"SDK_AUTO is Linux-only"); }
#endif
    try {
        launchers_absent();
        auto ctx = std::make_unique<FcPyContextV1>();
        ctx->owner = std::this_thread::get_id();
        uint64_t total = 0;
        ctx->program = copy_utf8(request->program,total);
        ctx->home = copy_utf8(request->python_home,total,request->mode==0);
        ctx->application = copy_utf8(request->application_arg,total);
        ctx->user_parameter = copy_utf8(request->user_parameter,total);
        ctx->system_parameter = copy_utf8(request->system_parameter,total);
        if (request->mode == 0 && (request->paths || request->path_count)) {
            return error(out,FC_PY_E_INPUT,"SDK_AUTO must not override module paths");
        }
        for (uint64_t i=0; i<request->path_count; ++i) { ctx->paths.push_back(copy_utf8(request->paths[i],total)); }
        if (request->mode == 1 && ctx->paths.empty()) { return error(out,FC_PY_E_INPUT,"Explicit module path list is empty"); }
        for (const auto& path : ctx->paths) { ctx->path_views.push_back(view(path)); }
        ctx->config = *request;
        ctx->config.program=view(ctx->program); ctx->config.python_home=view(ctx->home);
        ctx->config.application_arg=view(ctx->application); ctx->config.user_parameter=view(ctx->user_parameter);
        ctx->config.system_parameter=view(ctx->system_parameter);
        ctx->config.paths=ctx->path_views.empty()?nullptr:ctx->path_views.data();
        check_layout(*ctx);
        *out = ctx->report;
        *output = ctx.release();
        return FC_PY_OK;
    }
    catch (const std::bad_alloc&) { return error(out,FC_PY_E_NOMEM,"SDK configuration allocation failed"); }
    catch (const std::exception& e) { return error(out,FC_PY_E_INPUT,e.what()); }
    catch (...) { return error(out,FC_PY_E_INPUT,"SDK configuration preflight failed"); }
}
int32_t FC_PY_CALL start_sdk(FcPyContextV1* ctx, FcPyReportV1* out) noexcept
{
    if (!valid_report(out)) { return FC_PY_E_ABI; }
    if (!ctx) { return error(out,FC_PY_E_INPUT,"Missing SDK context"); }
    if (ctx->owner != std::this_thread::get_id()) { return error(out,FC_PY_E_THREAD,"SDK start on wrong thread"); }
    if (ctx->started || ctx->terminal) { return error(out,FC_PY_E_STATE,"SDK context already started or terminal"); }
    try { launchers_absent(); }
    catch (...) { return error(out,FC_PY_E_INPUT,"Python launcher environment must be absent"); }
    bool expected = false;
    if (!forgecad_app_used.compare_exchange_strong(expected,true,std::memory_order_acq_rel)) {
        return error(out,FC_PY_E_ALREADY_USED,"SDK owned initialization is one-shot");
    }
    ctx->started = true;
    int32_t base_result = forgecad_base_python_initialize_v1(&ctx->config,&ctx->report);
    if (!valid_base_result(base_result,ctx->report,true)) {
        unknown(*ctx,FC_PY_STAGE_INITIALIZE,FC_PY_E_UNKNOWN,"Base report protocol mismatch");
        return result(*ctx,out);
    }
    if (base_result != FC_PY_OK) {
        ctx->terminal = true;
        if (ctx->report.flags & FC_PY_REPORT_POISON) { forgecad_app_poison.store(true,std::memory_order_release); }
        return result(*ctx,out);
    }
    try {
        {
            Base::PyGILStateLocker gil;
            ctx->report.failure_stage = FC_PY_STAGE_APP_CONFIG;
            ctx->previous_config = App::Application::Config();
            ctx->snapshot = true;
            App::Application::Config()["UserParameter"]=ctx->user_parameter;
            App::Application::Config()["SystemParameter"]=ctx->system_parameter;
            App::Application::Config()["RunMode"]="Console";
            forgecad_application_argument.assign(ctx->application.begin(),ctx->application.end());
            forgecad_application_argument.push_back(0);
            forgecad_application_argv[0]=forgecad_application_argument.data();
            ctx->entered_app = true;
            ctx->report.flags |= FC_PY_REPORT_APP_ENTERED;
            ctx->report.failure_stage = FC_PY_STAGE_APP_INITIALIZE;
            App::Application::init(1,forgecad_application_argv);
            ctx->app_ready = true;
            ctx->report.flags |= FC_PY_REPORT_APP_READY;
            ctx->report.lifecycle = FC_PY_APP_READY;
            ctx->report.failure_stage = FC_PY_STAGE_IMPORT;
            for (const auto& provider : ctx->providers) {
                OwnedPythonReference imported(PyImport_ImportModule(provider.name.c_str()));
                if (!imported.value) { PyErr_Clear(); throw std::runtime_error("Required FreeCAD Python module import failed"); }
                check_import_provider(provider,imported.value);
            }
        }
        ctx->report.lifecycle = FC_PY_READY;
        ctx->report.failure_stage = FC_PY_STAGE_NONE;
        ctx->report.gil_state = 0;
        return result(*ctx,out);
    }
    catch (const std::exception& e) { diagnostic(ctx->report,e.what()); }
    catch (...) { diagnostic(ctx->report,"SDK application initialization failed"); }
    ctx->report.primary_status = ctx->app_ready ? FC_PY_E_IMPORT : FC_PY_E_APP;
    if (ctx->entered_app && !ctx->app_ready) {
        unknown(*ctx,FC_PY_STAGE_APP_INITIALIZE,FC_PY_E_APP,"Partial SDK App initialization cannot be destructed safely");
        return result(*ctx,out);
    }
    return stop_sdk(ctx,out);
}
int32_t FC_PY_CALL stop_sdk(FcPyContextV1* ctx, FcPyReportV1* out) noexcept
{
    if (!valid_report(out)) { return FC_PY_E_ABI; }
    if (!ctx) { return error(out,FC_PY_E_INPUT,"Missing SDK context"); }
    if (ctx->terminal) { return result(*ctx,out); }
    if (ctx->owner != std::this_thread::get_id()) {
        unknown(*ctx,FC_PY_STAGE_THREAD,FC_PY_E_THREAD,"SDK stop on wrong thread");
        return result(*ctx,out);
    }
    if (!ctx->started) {
        ctx->report.lifecycle=FC_PY_CLOSED;
        ctx->report.flags |= FC_PY_REPORT_CLEANUP_COMPLETE;
        ctx->terminal=true;
        return result(*ctx,out);
    }
    const uint32_t primary = ctx->report.primary_status;
    try {
        if (ctx->app_ready) { App::Application::destruct(); }
        else { (void)forgecad_base_python_finalize_before_app_v1(&ctx->report); }
        FcPyReportV1 base{sizeof(FcPyReportV1), FC_PY_ABI_V1, 0, 0, 0, 0, 0, 0, 0, 0, {0}};
        const int32_t snapshot_status = forgecad_base_python_snapshot_v1(&base);
        if (!valid_base_result(snapshot_status,base,false)) {
            unknown(*ctx,FC_PY_STAGE_DESTRUCT,FC_PY_E_UNKNOWN,"Base cleanup report protocol mismatch");
            return result(*ctx,out);
        }
        ctx->report = base;
        if (primary) { ctx->report.primary_status=primary; }
        if (ctx->entered_app) { ctx->report.flags |= FC_PY_REPORT_APP_ENTERED; }
        if (ctx->app_ready) { ctx->report.flags |= FC_PY_REPORT_APP_READY; }
        if (base.lifecycle != FC_PY_CLOSED && base.lifecycle != FC_PY_CLOSED_FAULT) {
            unknown(*ctx,FC_PY_STAGE_DESTRUCT,primary?primary:static_cast<uint32_t>(FC_PY_E_CLEANUP),"SDK cleanup did not reach a known terminal state");
            return result(*ctx,out);
        }
        if (ctx->snapshot) { App::Application::Config().swap(ctx->previous_config); ctx->snapshot=false; }
        ctx->terminal=true;
        if (ctx->report.cleanup_status) { forgecad_app_poison.store(true,std::memory_order_release); }
        return result(*ctx,out);
    }
    catch (...) {
        unknown(*ctx,FC_PY_STAGE_DESTRUCT,primary?primary:static_cast<uint32_t>(FC_PY_E_CLEANUP),"Partial SDK destruction cannot be retried");
        return result(*ctx,out);
    }
}
int32_t FC_PY_CALL destroy_sdk(FcPyContextV1* ctx, FcPyReportV1* out) noexcept
{
    if (!valid_report(out)) { return FC_PY_E_ABI; }
    if (!ctx) { return error(out,FC_PY_E_INPUT,"Missing SDK context"); }
    if (ctx->started && !ctx->terminal) { return error(out,FC_PY_E_STATE,"Stop SDK before destroying active context"); }
    std::unique_ptr<FcPyContextV1> owned(ctx);
    *out=ctx->report;
    out->primary_status=FC_PY_OK; // freeing copies does not receipt interpreter recovery
    if (!ctx->started) { out->lifecycle=FC_PY_CLOSED; out->flags |= FC_PY_REPORT_CLEANUP_COMPLETE; }
    return FC_PY_OK;
}
} // namespace
// F03_APP_LIFECYCLE_END

// F03_APP_QUERY_BEGIN
extern "C" AppExport int32_t FC_PY_CALL forgecad_sdk_python_query_v1(
    uint32_t version, uint32_t size, FcPyApiV1* out) noexcept
{
    if (!out || version != FC_PY_ABI_V1 || size != sizeof(*out)) { return FC_PY_E_ABI; }
    if (forgecad_app_poison.load(std::memory_order_acquire)) { return FC_PY_E_POISONED; }
    FcPyBaseProfileV1 base{};
    const int32_t status=forgecad_base_python_profile_v1(FC_PY_ABI_V1,sizeof(base),&base);
    if (status != FC_PY_OK) { return status; }
    if (base.struct_size != sizeof(base) || base.abi_version != FC_PY_ABI_V1
        || base.features != FC_PY_FEATURES_V1 || base.python_hex != FC_PY_PYTHON_HEX_V1
        || base.pointer_bits != 64 || base.wchar_bits != sizeof(wchar_t)*8
        || base.reserved[0] || base.reserved[1]
        || std::memcmp(base.contract_sha256,forgecad_contract_sha,32)) { return FC_PY_E_VERSION; }
    const FcPyApiV1 api{sizeof(FcPyApiV1),FC_PY_ABI_V1,FC_PY_FEATURES_V1,
        FC_PY_SDK_RELEASE_V1,FC_PY_PYTHON_HEX_V1,64,sizeof(wchar_t)*8,0,
        FC_PY_CONTRACT_SHA256_BYTES,{forgecad_contract_json,sizeof(forgecad_contract_json)-1},
        &prepare_sdk,&start_sdk,&stop_sdk,&destroy_sdk};
    *out=api;
    return FC_PY_OK;
}
// F03_APP_QUERY_END

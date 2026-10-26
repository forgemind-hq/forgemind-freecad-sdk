// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2002 Jürgen Riegel <juergen.riegel@web.de>              *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License (LGPL)   *
 *   as published by the Free Software Foundation; either version 2 of     *
 *   the License, or (at your option) any later version.                   *
 *   for detail see the LICENCE text file.                                 *
 *                                                                         *
 *   FreeCAD is distributed in the hope that it will be useful,            *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with FreeCAD; if not, write to the Free Software        *
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  *
 *   USA                                                                   *
 *                                                                         *
 ***************************************************************************/

#include <sstream>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>
#define FORGECAD_SDK_BUILD_INTERNAL
#include "ForgeCADPythonIntegration.h"
#include "../ForgeCADPythonBuildContract.h"
#include <boost/regex.hpp>

#include <FCConfig.h>

#include "Interpreter.h"
#include "Console.h"
#include "ExceptionFactory.h"
#include "FileInfo.h"
#include "PyObjectBase.h"
#include "PyTools.h"
#include "Stream.h"


// F03_BASE_STATE_BEGIN
namespace {
struct ForgeCADPythonState {
    std::atomic<bool> poison{false};
    bool used = false;
    bool active = false;
    bool saved_valid = false;
    bool singleton_live = false;
    bool held = false;
    const FcPyConfigV1* pending = nullptr; // synchronous, SDK-owned input only
    std::thread::id owner;
    FcPyReportV1 report{sizeof(FcPyReportV1), FC_PY_ABI_V1, 0, 0, FC_PY_COLD,
                        0, 0, 0, 0, FC_PY_STAGE_NONE, {0}};
};
ForgeCADPythonState forgecad_python;

bool forgecad_report_valid(const FcPyReportV1* out) noexcept
{
    return out && out->struct_size == sizeof(FcPyReportV1)
        && out->abi_version == FC_PY_ABI_V1;
}
void forgecad_diagnostic(FcPyReportV1& report, const char* text) noexcept
{
    if (!text) { text = "SDK Python operation failed"; }
    size_t n = std::min(std::strlen(text), sizeof(report.diagnostic) - 1);
    while (n && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80) { --n; }
    std::memcpy(report.diagnostic, text, n);
    report.diagnostic[n] = '\0';
}
int32_t forgecad_base_result(FcPyReportV1* out) noexcept
{
    *out = forgecad_python.report;
    return static_cast<int32_t>(out->primary_status);
}
void forgecad_unknown(uint32_t stage, const char* text) noexcept
{
    auto& r = forgecad_python.report;
    forgecad_python.poison.store(true, std::memory_order_release);
    r.flags |= FC_PY_REPORT_POISON;
    r.flags &= ~FC_PY_REPORT_CLEANUP_COMPLETE;
    r.cleanup_status = FC_PY_E_UNKNOWN;
    r.lifecycle = FC_PY_UNKNOWN;
    r.gil_state = 2;
    r.python_state = 2;
    r.failure_stage = stage;
    if (r.primary_status == FC_PY_OK) { r.primary_status = FC_PY_E_UNKNOWN; }
    forgecad_diagnostic(r, text);
}
} // namespace
// F03_BASE_STATE_END

// F03_CONFIG_CONSUMER_BEGIN
namespace {
std::wstring forgecad_wide(FcPyUtf8V1 input)
{
    if ((!input.data && input.size) || input.size > 1048576) {
        throw Base::RuntimeError("Invalid SDK Python UTF8 input");
    }
    std::wstring result;
    for (uint64_t i = 0; i < input.size;) {
        uint32_t c = static_cast<unsigned char>(input.data[i++]);
        uint32_t minimum = 0;
        unsigned remaining = 0;
        if (c == 0) { throw Base::RuntimeError("NUL in SDK Python input"); }
        if (c < 0x80) { /* complete ASCII scalar */ }
        else if (c >= 0xc2 && c <= 0xdf) { c &= 0x1f; remaining = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { c &= 0x0f; remaining = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { c &= 7; remaining = 3; minimum = 0x10000; }
        else { throw Base::RuntimeError("Invalid SDK Python UTF8 scalar"); }
        if (remaining > input.size - i) { throw Base::RuntimeError("Truncated UTF8"); }
        for (unsigned j = 0; j < remaining; ++j) {
            const uint32_t b = static_cast<unsigned char>(input.data[i++]);
            if ((b & 0xc0) != 0x80) { throw Base::RuntimeError("Invalid UTF8 continuation"); }
            c = (c << 6) | (b & 0x3f);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) {
            throw Base::RuntimeError("Invalid Unicode scalar");
        }
        if (sizeof(wchar_t) == 2 && c > 0xffff) {
            c -= 0x10000;
            result.push_back(static_cast<wchar_t>(0xd800 + (c >> 10)));
            result.push_back(static_cast<wchar_t>(0xdc00 + (c & 0x3ff)));
        }
        else { result.push_back(static_cast<wchar_t>(c)); }
    }
    return result;
}
void forgecad_check_status(PyStatus status)
{
    if (PyStatus_Exception(status)) {
        throw Base::RuntimeError(status.err_msg ? status.err_msg : "Python initialization requested exit");
    }
}
void forgecad_configure_python(const FcPyConfigV1& request)
{
    auto& state = forgecad_python;
    auto& r = state.report;
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    struct ClearConfig {
        PyConfig* value;
        ~ClearConfig() { PyConfig_Clear(value); }
    } clear{&config};
    config.isolated = 0;
    config.use_environment = 0;
    config.user_site_directory = 0;
    config.write_bytecode = 0;
    // Decode every input before the first potentially global CPython operation.
    auto program = forgecad_wide(request.program);
    std::wstring home;
    std::vector<std::wstring> paths;
    if (request.mode == FC_PY_MODE_EXPLICIT) {
        home = forgecad_wide(request.python_home);
        for (uint64_t i = 0; i < request.path_count; ++i) {
            paths.push_back(forgecad_wide(request.paths[i]));
        }
    }
    r.lifecycle = FC_PY_PREINITIALIZING;
    r.failure_stage = FC_PY_STAGE_PREINITIALIZE;
    r.flags |= FC_PY_REPORT_ATTEMPTED;
    r.gil_state = 2;
    PyPreConfig preconfig;
    PyPreConfig_InitIsolatedConfig(&preconfig);
    // These are exactly the four config overrides of _PyPreConfig_GetConfig.
    preconfig.isolated = 0;
    preconfig.use_environment = 0;
    preconfig.parse_argv = 0;
    preconfig.dev_mode = 0;
    forgecad_check_status(Py_PreInitialize(&preconfig));
    r.failure_stage = FC_PY_STAGE_CONFIG;
    wchar_t* argv[] = {program.data()};
    forgecad_check_status(PyConfig_SetArgv(&config, 1, argv));
    if (request.mode == FC_PY_MODE_EXPLICIT) {
        forgecad_check_status(PyConfig_SetString(&config, &config.program_name, program.c_str()));
        forgecad_check_status(PyConfig_SetString(&config, &config.executable, program.c_str()));
        forgecad_check_status(PyConfig_SetString(&config, &config.base_executable, program.c_str()));
        forgecad_check_status(PyConfig_SetString(&config, &config.home, home.c_str()));
        for (const auto& path : paths) {
            forgecad_check_status(PyWideStringList_Append(&config.module_search_paths, path.c_str()));
        }
        config.module_search_paths_set = 1;
    }
    r.failure_stage = FC_PY_STAGE_INITIALIZE;
    const PyStatus status = Py_InitializeFromConfig(&config);
    if (Py_IsInitialized() && PyGILState_Check()) {
        state.held = true;
        r.python_state = 1;
        r.gil_state = 1;
        r.lifecycle = FC_PY_PYTHON_HELD;
    }
    forgecad_check_status(status);
    if (!state.held) { throw Base::RuntimeError("SDK initialization returned without a known GIL"); }
}
} // namespace
// F03_CONFIG_CONSUMER_END

char format2[1024];  // Warning! Can't go over 512 characters!!!
unsigned int format2_len = 1024;

using namespace Base;

PyException::PyException(const Py::Object& obj)
{
    setMessage(obj.as_string());
    // WARNING: we are assuming that python type object will never be
    // destroyed, so we don't keep reference here to save book-keeping in
    // our copy constructor and destructor
    // NOLINTBEGIN
    _exceptionType = reinterpret_cast<PyObject*>(obj.ptr()->ob_type);
    _errorType = obj.ptr()->ob_type->tp_name;
    // NOLINTEND
}

PyException::PyException()
{
    PP_Fetch_Error_Text(); /* fetch (and clear) exception */

    setPyObject(PP_PyDict_Object);

    std::string prefix = PP_last_error_type; /* exception name text */
    std::string error = PP_last_error_info;  /* exception data text */

    setMessage(error);
    _errorType = prefix;

    // NOLINTNEXTLINE
    _exceptionType = PP_last_exception_type;

    if (PP_last_exception_type) {
        // WARNING: we are assuming that python type object will never be
        // destroyed, so we don't keep reference here to save book-keeping in
        // our copy constructor and destructor
        Py_DECREF(PP_last_exception_type);
        PP_last_exception_type = nullptr;
    }

    _stackTrace = PP_last_error_trace; /* exception traceback text */

    // This should be done in the constructor because when doing
    // in the destructor it's not always clear when it is called
    // and thus may clear a Python exception when it should not.
    PyGILStateLocker locker;
    PyErr_Clear();  // must be called to keep Python interpreter in a valid state (Werner)
}

PyException::~PyException() noexcept = default;

void PyException::throwException()
{
    PyException myexcp;
    myexcp.reportException();
    myexcp.raiseException();
}

void PyException::raiseException()
{
    PyGILStateLocker locker;
    if (PP_PyDict_Object) {
        // delete the Python dict upon destruction of edict
        Py::Dict edict(PP_PyDict_Object, true);
        PP_PyDict_Object = nullptr;

        std::string exceptionname;
        if (_exceptionType == Base::PyExc_FC_FreeCADAbort) {
            edict.setItem("sclassname", Py::String(typeid(AbortException).name()));
        }
        if (getReported()) {
            edict.setItem("breported", Py::True());
        }
        Base::ExceptionFactory::Instance().raiseException(edict.ptr());
    }

    PyExceptionData data {_exceptionType, getMessage(), getReported()};
    Base::ExceptionFactory::Instance().raiseExceptionByType(data);

    // Fallback
    throw *this;
}

void PyException::reportException() const
{
    if (!getReported()) {
        setReported(true);
        // set sys.last_vars to make post-mortem debugging work
        PyGILStateLocker locker;
        PySys_SetObject("last_traceback", PP_last_traceback);
        Console().developerError(
            "pyException",
            "%s%s: %s\n",
            _stackTrace.c_str(),
            _errorType.c_str(),
            what()
        );
    }
}

void PyException::setPyException() const
{
    std::stringstream str;
    str << getStackTrace() << getErrorType() << ": " << what();
    PyErr_SetString(getPyExceptionType(), str.str().c_str());
}

// ---------------------------------------------------------

SystemExitException::SystemExitException()
{
    // Set exception message and code based upon the python sys.exit() code and/or message
    // based upon the following sys.exit() call semantics.
    //
    // Invocation       |  _exitCode  |  _sErrMsg
    // ---------------- +  ---------  +  --------
    // sys.exit(int#)   |   int#      |   "System Exit"
    // sys.exit(string) |   1         |   string
    // sys.exit()       |   1         |   "System Exit"

    long int errCode = 1;
    std::string errMsg = "System exit";
    PyObject* type {};
    PyObject* value {};
    PyObject* traceback {};
    PyObject* code {};

    PyGILStateLocker locker;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);

    if (value) {
        code = PyObject_GetAttrString(value, "code");
        if (code && value != Py_None) {
            Py_DECREF(value);
            value = code;
        }

        if (PyLong_Check(value)) {
            errCode = PyLong_AsLong(value);
        }
        else {
            const char* str = PyUnicode_AsUTF8(value);
            if (str) {
                errMsg = errMsg + ": " + str;
            }
        }
    }

    setMessage(errMsg);
    _exitCode = errCode;
}

// ---------------------------------------------------------

// Fixes #0000831: python print causes File descriptor error on windows
// NOLINTNEXTLINE
class PythonStdOutput: public Py::PythonExtension<PythonStdOutput>
{
public:
    static void init_type()
    {
        behaviors().name("PythonStdOutput");
        behaviors().doc("Python standard output");
        add_varargs_method("write", &PythonStdOutput::write, "write()");
        add_varargs_method("flush", &PythonStdOutput::flush, "flush()");
    }

    PythonStdOutput() = default;
    ~PythonStdOutput() override = default;

    Py::Object write(const Py::Tuple&)
    {
        return Py::None();
    }
    Py::Object flush(const Py::Tuple&)
    {
        return Py::None();
    }
};

// ---------------------------------------------------------

InterpreterSingleton::InterpreterSingleton()
{
    this->_global = nullptr;
}

InterpreterSingleton::~InterpreterSingleton() = default;


std::string InterpreterSingleton::runString(const char* sCmd)
{
    PyObject* module {};
    PyObject* dict {};
    PyObject* presult {};

    PyGILStateLocker locker;
    module = PP_Load_Module("__main__"); /* get module, init python */
    if (!module) {
        throw PyException(); /* not incref'd */
    }
    dict = PyModule_GetDict(module); /* get dict namespace */
    if (!dict) {
        throw PyException(); /* not incref'd */
    }


    presult = PyRun_String(sCmd, Py_file_input, dict, dict); /* eval direct */
    if (!presult) {
        if (PyErr_ExceptionMatches(PyExc_SystemExit)) {
            throw SystemExitException();
        }

        PyException::throwException();
        return {};  // just to quieten code analyzers
    }

    PyObject* repr = PyObject_Repr(presult);
    Py_DECREF(presult);
    if (repr) {
        std::string ret(PyUnicode_AsUTF8(repr));
        Py_DECREF(repr);
        return ret;
    }

    PyErr_Clear();
    return {};
}

/** runStringWithKey(psCmd, key, key_initial_value)
 * psCmd is python script to run
 * key is the name of a python string variable the script will have read/write
 * access to during script execution.  It will be our return value.
 * key_initial_value is the initial value c++ will set before calling the script
 * If the script runs successfully it will be able to change the value of key as
 * the return value, but if there is a runtime error key will not be changed even
 * if the error occurs after changing it inside the script.
 */

std::string InterpreterSingleton::runStringWithKey(
    const char* psCmd,
    const char* key,
    const char* key_initial_value
)
{
    PyGILStateLocker locker;
    Py::Module module("__main__");
    Py::Dict globalDictionary = module.getDict();
    Py::Dict localDictionary;
    Py::String initial_value(key_initial_value);
    localDictionary.setItem(key, initial_value);

    PyObject* presult
        = PyRun_String(psCmd, Py_file_input, globalDictionary.ptr(), localDictionary.ptr());
    if (!presult) {
        if (PyErr_ExceptionMatches(PyExc_SystemExit)) {
            throw SystemExitException();
        }

        PyException::throwException();
        return {};  // just to quieten code analyzers
    }
    Py_DECREF(presult);

    Py::Object key_return_value = localDictionary.getItem(key);
    if (!key_return_value.isString()) {
        key_return_value = key_return_value.str();  // NOLINT
    }

    Py::Bytes str = Py::String(key_return_value).encode("utf-8");
    std::string result = static_cast<std::string>(str);
    return result;
}

Py::Object InterpreterSingleton::runStringObject(const char* sCmd)
{
    PyObject* module {};
    PyObject* dict {};
    PyObject* presult {};

    PyGILStateLocker locker;
    module = PP_Load_Module("__main__"); /* get module, init python */
    if (!module) {
        throw PyException(); /* not incref'd */
    }
    dict = PyModule_GetDict(module); /* get dict namespace */
    if (!dict) {
        throw PyException(); /* not incref'd */
    }


    presult = PyRun_String(sCmd, Py_eval_input, dict, dict); /* eval direct */
    if (!presult) {
        if (PyErr_ExceptionMatches(PyExc_SystemExit)) {
            throw SystemExitException();
        }

        throw PyException();
    }

    return Py::asObject(presult);
}

void InterpreterSingleton::systemExit()
{
    /* This code is taken from the original Python code */
    PyObject* exception {};
    PyObject* value {};
    PyObject* tb {};
    int exitcode = 0;

    PyErr_Fetch(&exception, &value, &tb);
    fflush(stdout);
    if (!value || value == Py_None) {
        goto done;  // NOLINT
    }
    if (PyExceptionInstance_Check(value)) {
        /* The error code should be in the `code' attribute. */
        PyObject* code = PyObject_GetAttrString(value, "code");
        if (code) {
            Py_DECREF(value);
            value = code;
            if (value == Py_None) {
                goto done;  // NOLINT
            }
        }
        /* If we failed to dig out the 'code' attribute,
           just let the else clause below print the error. */
    }
    if (PyLong_Check(value)) {
        exitcode = (int)PyLong_AsLong(value);
    }
    else {
        PyObject_Print(value, stderr, Py_PRINT_RAW);
        PySys_WriteStderr("\n");
        exitcode = 1;
    }
done:
    /* Restore and clear the exception info, in order to properly decref
     * the exception, value, and traceback.  If we just exit instead,
     * these leak, which confuses PYTHONDUMPREFS output, and may prevent
     * some finalizers from running.
     */
    PyErr_Restore(exception, value, tb);
    PyErr_Clear();
    Py_Exit(exitcode);
    /* NOTREACHED */
}

void InterpreterSingleton::runInteractiveString(const char* sCmd)
{
    PyObject* module {};
    PyObject* dict {};
    PyObject* presult {};

    PyGILStateLocker locker;
    module = PP_Load_Module("__main__"); /* get module, init python */
    if (!module) {
        throw PyException(); /* not incref'd */
    }
    dict = PyModule_GetDict(module); /* get dict namespace */
    if (!dict) {
        throw PyException(); /* not incref'd */
    }

    presult = PyRun_String(sCmd, Py_single_input, dict, dict); /* eval direct */
    if (!presult) {
        if (PyErr_ExceptionMatches(PyExc_SystemExit)) {
            throw SystemExitException();
        }
        /* get latest python exception information */
        /* and print the error to the error output */
        PyObject* errobj {};
        PyObject* errdata {};
        PyObject* errtraceback {};
        PyErr_Fetch(&errobj, &errdata, &errtraceback);

        RuntimeError exc("");  // do not use PyException since this clears the error indicator
        if (errdata) {
            if (PyUnicode_Check(errdata)) {
                exc.setMessage(PyUnicode_AsUTF8(errdata));
            }
        }
        PyErr_Restore(errobj, errdata, errtraceback);
        if (PyErr_Occurred()) {
            PyErr_Print();
        }
        throw exc;
    }

    Py_DECREF(presult);
}

void InterpreterSingleton::runFile(const char* pxFileName, bool local)
{
#ifdef FC_OS_WIN32
    FileInfo fi(pxFileName);
    FILE* fp = _wfopen(fi.toStdWString().c_str(), L"r");
#else
    FILE* fp = fopen(pxFileName, "r");
#endif
    if (!fp) {
        throw FileException("Unknown file", pxFileName);
    }

    PyGILStateLocker locker;
    PyObject* module {};
    PyObject* dict {};
    module = PyImport_AddModule("__main__");
    dict = PyModule_GetDict(module);
    if (local) {
        dict = PyDict_Copy(dict);
    }
    else {
        Py_INCREF(dict);  // avoid to further distinguish between local and global dict
    }

    if (!PyDict_GetItemString(dict, "__file__")) {
        PyObject* pyObj = PyUnicode_FromString(pxFileName);
        if (!pyObj) {
            fclose(fp);
            Py_DECREF(dict);
            return;
        }
        if (PyDict_SetItemString(dict, "__file__", pyObj) < 0) {
            Py_DECREF(pyObj);
            fclose(fp);
            Py_DECREF(dict);
            return;
        }
        Py_DECREF(pyObj);
    }

    PyObject* result = PyRun_File(fp, pxFileName, Py_file_input, dict, dict);
    fclose(fp);
    Py_DECREF(dict);

    if (!result) {
        if (PyErr_ExceptionMatches(PyExc_SystemExit)) {
            throw SystemExitException();
        }
        throw PyException();
    }
    Py_DECREF(result);
}

bool InterpreterSingleton::loadModule(const char* psModName)
{
    PyObject* module {};

    PyGILStateLocker locker;
    module = PP_Load_Module(psModName);

    if (!module) {
        if (PyErr_ExceptionMatches(PyExc_SystemExit)) {
            throw SystemExitException();
        }

        throw PyException();
    }

    return true;
}

PyObject* InterpreterSingleton::addModule(Py::ExtensionModuleBase* mod)
{
    _modules.push_back(mod);
    return mod->module().ptr();
}

void InterpreterSingleton::cleanupModules()
{
    // This is only needed to make the address sanitizer happy
#if defined(__has_feature)
# if __has_feature(address_sanitizer)
    for (auto it : _modules) {
        delete it;
    }
    _modules.clear();
# endif
#endif
}

void InterpreterSingleton::addType(PyTypeObject* Type, PyObject* Module, const char* Name)
{
    // NOTE: To finish the initialization of our own type objects we must
    // call PyType_Ready, otherwise we run into a segmentation fault, later on.
    // This function is responsible for adding inherited slots from a type's base class.
    if (PyType_Ready(Type) < 0) {
        return;
    }
    PyModule_AddObject(Module, Name, Base::getTypeAsObject(Type));
}

void InterpreterSingleton::addPythonPath(const char* Path)
{
    PyGILStateLocker locker;
    Py::List list(PySys_GetObject("path"));
    list.append(Py::String(Path));
}

std::string InterpreterSingleton::getPythonPath()
{
    // Construct something that looks like the output of the now-deprecated Py_GetPath
    PyGILStateLocker lock;
    PyObject* path = PySys_GetObject("path");
    std::string result;
    const char* separator = ":";  // Use ":" on Unix-like systems, ";" on Windows
#ifdef FC_OS_WIN32
    separator = ";";
#endif
    Py_ssize_t length = PyList_Size(path);
    for (Py_ssize_t i = 0; i < length; ++i) {
        PyObject* item = PyList_GetItem(path, i);  // Borrowed reference
        if (!item) {
            throw Base::RuntimeError("Failed to retrieve item from path");
        }
        const char* item_str = PyUnicode_AsUTF8(item);
        if (!item_str) {
            throw Base::RuntimeError("Failed to convert path item to UTF-8 string");
        }
        if (!result.empty()) {
            result += separator;
        }
        result += item_str;
    }
    return result;
}

#if PY_VERSION_HEX < 0x030b0000
std::string InterpreterSingleton::init(int argc, char* argv[])
{
    if (!Py_IsInitialized()) {
        Py_SetProgramName(Py_DecodeLocale(argv[0], nullptr));
        // There is a serious bug in VS from 2010 until 2013 where the file descriptor for stdin,
        // stdout or stderr returns a valid value for GUI applications (i.e. subsystem = Windows)
        // where it shouldn't. This causes Python to fail during initialization. A workaround is to
        // use freopen on stdin, stdout and stderr. See the class Redirection inside main()
        // https://bugs.python.org/issue17797#msg197474
        //
        Py_Initialize();
        const char* virtualenv = getenv("VIRTUAL_ENV");
        if (virtualenv) {
            PyRun_SimpleString(
                "# Check for virtualenv, and activate if present.\n"
                "# See "
                "https://virtualenv.pypa.io/en/latest/"
                "#using-virtualenv-without-bin-python\n"
                "import os\n"
                "import sys\n"
                "base_path = os.getenv(\"VIRTUAL_ENV\")\n"
                "if not base_path is None:\n"
                "    activate_this = os.path.join(base_path, \"bin\", \"activate_this.py\")\n"
                "    exec(open(activate_this).read(), {'__file__':activate_this})\n"
            );
        }

        size_t size = argc;
        static std::vector<wchar_t*> _argv(size);
        for (int i = 0; i < argc; i++) {
            _argv[i] = Py_DecodeLocale(argv[i], nullptr);
        }
        PySys_SetArgv(argc, _argv.data());
        PythonStdOutput::init_type();
        this->_global = PyEval_SaveThread();
    }

    PyGILStateLocker lock;
    return Py_EncodeLocale(Py_GetPath(), nullptr);
}
#else
namespace
{
void initInterpreter(int argc, char* argv[])
{
    PyStatus status;
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.isolated = 0;
    config.user_site_directory = 1;

    status = PyConfig_SetBytesArgv(&config, argc, argv);
    if (PyStatus_Exception(status)) {
        throw Base::RuntimeError("Failed to set config");
    }

    status = Py_InitializeFromConfig(&config);
    if (PyStatus_Exception(status)) {
        throw Base::RuntimeError("Failed to init from config");
    }

    // If FreeCAD was run from within a Python virtual environment, ensure that the site-packages
    // directory from that environment is used.
    const char* virtualenv = getenv("VIRTUAL_ENV");
    if (virtualenv) {
        std::wstringstream ss;
        PyConfig_Read(&config);
        ss << virtualenv << L"/lib/python" << PY_MAJOR_VERSION << "." << PY_MINOR_VERSION
           << "/site-packages";
        PyObject* venvLocation = PyUnicode_FromWideChar(ss.str().c_str(), ss.str().size());
        PyObject* path = PySys_GetObject("path");
        PyList_Append(path, venvLocation);
    }

    PyConfig_Clear(&config);

    Py_Initialize();
}
}  // namespace
std::string InterpreterSingleton::init(int argc, char* argv[])
{
    if (forgecad_python.active) {
        auto& state = forgecad_python;
        auto& r = state.report;
        if (state.owner != std::this_thread::get_id()) {
            throw Base::RuntimeError("SDK Python initialization on wrong thread");
        }
        if (!state.pending) {
            if (!state.saved_valid || state.poison.load(std::memory_order_acquire)) {
                throw Base::RuntimeError("SDK Python has no valid saved interpreter");
            }
            return getPythonPath();
        }
        forgecad_configure_python(*state.pending);
        r.failure_stage = FC_PY_STAGE_STDOUT;
        PythonStdOutput::init_type();
        r.lifecycle = FC_PY_STDOUT_READY;
        r.failure_stage = FC_PY_STAGE_SAVE_THREAD;
        // Private thread-state operations remain inside their original member.
        state.held = false; // a throwing/faulted SaveThread result is unknown
        r.gil_state = 2;
        this->_global = PyEval_SaveThread();
        if (!this->_global) {
            forgecad_unknown(FC_PY_STAGE_SAVE_THREAD, "SDK SaveThread returned NULL");
            throw Base::RuntimeError("SDK SaveThread returned NULL");
        }
        state.saved_valid = true;
        r.gil_state = 0;
        r.lifecycle = FC_PY_SAVED;
        r.failure_stage = FC_PY_STAGE_PATH_READ;
        return getPythonPath();
    }
    try {
        if (!Py_IsInitialized()) {
            initInterpreter(argc, argv);
            PythonStdOutput::init_type();
            this->_global = PyEval_SaveThread();
        }
        return getPythonPath();
    }
    catch (const Exception& e) {
        e.reportException();
        throw;
    }
}
#endif

void InterpreterSingleton::replaceStdOutput()
{
    PyGILStateLocker locker;
    PythonStdOutput* out = new PythonStdOutput();
    PySys_SetObject("stdout", out);
    PySys_SetObject("stderr", out);
}

int InterpreterSingleton::cleanup(void (*func)())
{
    return Py_AtExit(func);
}

void InterpreterSingleton::finalize()
{
    if (forgecad_python.active) {
        auto& state = forgecad_python;
        auto& r = state.report;
        if (r.finalize_calls || r.lifecycle == FC_PY_CLOSED
            || r.lifecycle == FC_PY_CLOSED_FAULT || r.lifecycle == FC_PY_UNKNOWN) {
            return;
        }
        if (state.owner != std::this_thread::get_id()
            || (!state.held && !state.saved_valid)) {
            forgecad_unknown(FC_PY_STAGE_FINALIZE, "SDK cannot prove finalize ownership");
            return;
        }
        r.lifecycle = FC_PY_FINALIZING;
        r.failure_stage = FC_PY_STAGE_FINALIZE;
        try {
            if (state.saved_valid) {
                auto* saved = this->_global;
                this->_global = nullptr;
                state.saved_valid = false;
                if (!saved) {
                    forgecad_unknown(FC_PY_STAGE_FINALIZE, "SDK saved thread state is NULL");
                    return;
                }
                PyEval_RestoreThread(saved);
                state.held = true;
            }
            try { cleanupModules(); }
            catch (...) {
                r.cleanup_status = FC_PY_E_CLEANUP;
                forgecad_diagnostic(r, "SDK cleanupModules failed; attempting known-held finalize");
            }
            // Record before calling: a failing finalizer must never be retried.
            r.finalize_calls = 1;
            const int result = Py_FinalizeEx();
            state.held = false;
            r.gil_state = 0;
            r.python_state = Py_IsInitialized() ? 1 : 0;
            if (r.python_state) {
                forgecad_unknown(FC_PY_STAGE_FINALIZE, "Python remains initialized after finalize");
                return;
            }
            if (result < 0) { r.cleanup_status = FC_PY_E_CLEANUP; }
            r.lifecycle = r.cleanup_status ? FC_PY_CLOSED_FAULT : FC_PY_CLOSED;
            if (r.cleanup_status) {
                if (!r.primary_status) { r.primary_status = FC_PY_E_CLEANUP; }
                state.poison.store(true, std::memory_order_release);
                r.flags |= FC_PY_REPORT_POISON;
            }
            else { r.flags |= FC_PY_REPORT_CLEANUP_COMPLETE; }
        }
        catch (...) { forgecad_unknown(FC_PY_STAGE_FINALIZE, "SDK finalize threw"); }
        return;
    }
    try {
        PyEval_RestoreThread(this->_global);
        cleanupModules();
        Py_Finalize();
    }
    catch (...) {
    }
}

void InterpreterSingleton::runStringArg(const char* psCom, ...)
{
    // va stuff
    va_list namelessVars;
    va_start(namelessVars, psCom);  // Get the "..." vars
    int len = vsnprintf(format2, format2_len, psCom, namelessVars);
    va_end(namelessVars);
    if (len == -1) {
        // argument too long
        assert(false);
    }

    runString(format2);
}


// Singleton:

InterpreterSingleton* InterpreterSingleton::_pcSingleton = nullptr;

InterpreterSingleton& InterpreterSingleton::Instance()
{
    // not initialized!
    if (!_pcSingleton) {
        _pcSingleton = new InterpreterSingleton();
    }
    return *_pcSingleton;
}

void InterpreterSingleton::Destruct()
{
    if (forgecad_python.active) {
        if (!forgecad_python.singleton_live) { return; }
        forgecad_python.singleton_live = false;
    }
    // not initialized or double destruct!
    assert(_pcSingleton);
    delete _pcSingleton;
    _pcSingleton = nullptr;
}

int InterpreterSingleton::runCommandLine(const char* prompt)
{
    PyGILStateLocker locker;
    return PP_Run_Command_Line(prompt);
}

/**
 *  Runs a member method of an object with no parameter and no return value
 *  void (void). There are other methods to run with returns
 */
void InterpreterSingleton::runMethodVoid(PyObject* pobject, const char* method)
{
    PyGILStateLocker locker;
    if (PP_Run_Method(
            pobject,  // object
            method,   // run method
            nullptr,  // no return type
            nullptr,  // so no return object
            "()"
        )  // no arguments
        != 0) {
        throw PyException(/*"Error running InterpreterSingleton::RunMethodVoid()"*/);
    }
}

PyObject* InterpreterSingleton::runMethodObject(PyObject* pobject, const char* method)
{
    PyObject* pcO {};

    PyGILStateLocker locker;
    if (PP_Run_Method(
            pobject,  // object
            method,   // run method
            "O",      // return type
            &pcO,     // return object
            "()"
        )  // no arguments
        != 0) {
        throw PyException();
    }

    return pcO;
}

void InterpreterSingleton::runMethod(
    PyObject* pobject,
    const char* method,
    const char* resfmt,
    void* cresult, /* convert to c/c++ */
    const char* argfmt,
    ...
) /* convert to python */
{
    PyObject* pmeth {};
    PyObject* pargs {};
    PyObject* presult {};
    va_list argslist; /* "pobject.method(args)" */
    va_start(argslist, argfmt);

    PyGILStateLocker locker;
    pmeth = PyObject_GetAttrString(pobject, method);
    if (!pmeth) { /* get callable object */
        va_end(argslist);
        throw AttributeError(
            "Error running InterpreterSingleton::RunMethod() method not defined"
        ); /* bound method?
              has self */
    }

    pargs = Py_VaBuildValue(argfmt, argslist); /* args: c->python */
    va_end(argslist);

    if (!pargs) {
        Py_DECREF(pmeth);
        throw TypeError("InterpreterSingleton::RunMethod() wrong arguments");
    }

    presult = PyObject_CallObject(pmeth, pargs); /* run interpreter */

    Py_DECREF(pmeth);
    Py_DECREF(pargs);
    if (PP_Convert_Result(presult, resfmt, cresult) != 0) {
        if (PyErr_Occurred()) {
            PyErr_Print();
        }
        throw RuntimeError(
            "Error running InterpreterSingleton::RunMethod() exception in called method"
        );
    }
}

PyObject* InterpreterSingleton::getValue(const char* key, const char* result_var)
{
    PyObject* module {};
    PyObject* dict {};
    PyObject* presult {};

    PyGILStateLocker locker;
    module = PP_Load_Module("__main__"); /* get module, init python */
    if (!module) {
        throw PyException(); /* not incref'd */
    }
    dict = PyModule_GetDict(module); /* get dict namespace */
    if (!dict) {
        throw PyException(); /* not incref'd */
    }


    presult = PyRun_String(key, Py_file_input, dict, dict); /* eval direct */
    if (!presult) {
        throw PyException();
    }
    Py_DECREF(presult);

    return PyObject_GetAttrString(module, result_var);
}

void InterpreterSingleton::dbgObserveFile(const char* sFileName)
{
    if (sFileName) {
        _cDebugFileName = sFileName;
    }
    else {
        _cDebugFileName = "";
    }
}


std::string InterpreterSingleton::strToPython(const char* Str)
{
    std::string result;
    const char* It = Str;

    while (*It != '\0') {
        switch (*It) {
            case '\\':
                result += "\\\\";
                break;
            case '\"':
                result += "\\\"";
                break;
            case '\'':
                result += "\\\'";
                break;
            default:
                result += *It;
        }
        It++;
    }

    return result;
}

// --------------------------------------------------------------------

int getSWIGVersionFromModule(const std::string& module)
{
    static std::map<std::string, int> moduleMap;
    std::map<std::string, int>::iterator it = moduleMap.find(module);
    if (it != moduleMap.end()) {
        return it->second;
    }
    try {
        // Get the module and check its __file__ attribute
        Py::Dict dict(PyImport_GetModuleDict());
        if (!dict.hasKey(module)) {
            return 0;
        }
        Py::Module mod(module);
        Py::String file(mod.getAttr("__file__"));
        std::string filename = (std::string)file;
        // file can have the extension .py or .pyc
        filename = filename.substr(0, filename.rfind('.'));
        filename += ".py";
        boost::regex rx("^# Version ([1-9])\\.([0-9])\\.([0-9]+)");
        boost::cmatch what;

        std::string line;
        Base::FileInfo fi(filename);

        Base::ifstream str(fi, std::ios::in);
        while (str && std::getline(str, line)) {
            if (boost::regex_match(line.c_str(), what, rx)) {
                int major = std::atoi(what[1].first);
                int minor = std::atoi(what[2].first);
                int micro = std::atoi(what[3].first);
                int version = (major << 16) + (minor << 8) + micro;
                moduleMap[module] = version;
                return version;
            }
        }
    }
    catch (Py::Exception& e) {
        e.clear();
    }

#if (defined(HAVE_SWIG) && (HAVE_SWIG == 1))
    moduleMap[module] = 0;
#endif
    return 0;
}

#if (defined(HAVE_SWIG) && (HAVE_SWIG == 1))
namespace Swig_python
{
extern int createSWIGPointerObj_T(const char* TypeName, void* obj, PyObject** ptr, int own);
extern int convertSWIGPointerObj_T(const char* TypeName, PyObject* obj, void** ptr, int flags);
extern void cleanupSWIG_T(const char* TypeName);
extern int getSWIGPointerTypeObj_T(const char* TypeName, PyTypeObject** ptr);
}  // namespace Swig_python
#endif

PyObject* InterpreterSingleton::createSWIGPointerObj(
    const char* Module,
    const char* TypeName,
    void* Pointer,
    int own
)
{
    int result = 0;
    PyObject* proxy = nullptr;
    PyGILStateLocker locker;
    (void)Module;
#if (defined(HAVE_SWIG) && (HAVE_SWIG == 1))
    result = Swig_python::createSWIGPointerObj_T(TypeName, Pointer, &proxy, own);
#else
    (void)TypeName;
    (void)Pointer;
    (void)own;
    result = -1;  // indicates error
#endif

    if (result == 0) {
        return proxy;
    }

    // none of the SWIG's succeeded
    throw Base::RuntimeError("No SWIG wrapped library loaded");
}

bool InterpreterSingleton::convertSWIGPointerObj(
    const char* Module,
    const char* TypeName,
    PyObject* obj,
    void** ptr,
    int flags
)
{
    int result = 0;
    PyGILStateLocker locker;
    (void)Module;
#if (defined(HAVE_SWIG) && (HAVE_SWIG == 1))
    result = Swig_python::convertSWIGPointerObj_T(TypeName, obj, ptr, flags);
#else
    (void)TypeName;
    (void)obj;
    (void)ptr;
    (void)flags;
    result = -1;  // indicates error
#endif

    if (result == 0) {
        return true;
    }

    // none of the SWIG's succeeded
    throw Base::RuntimeError("No SWIG wrapped library loaded");
}

void InterpreterSingleton::cleanupSWIG(const char* TypeName)
{
    PyGILStateLocker locker;
#if (defined(HAVE_SWIG) && (HAVE_SWIG == 1))
    Swig_python::cleanupSWIG_T(TypeName);
#else
    (void)TypeName;
#endif
}

PyTypeObject* InterpreterSingleton::getSWIGPointerTypeObj(const char* Module, const char* TypeName)
{
    int result = 0;
    PyTypeObject* proxy = nullptr;
    PyGILStateLocker locker;
    (void)Module;
#if (defined(HAVE_SWIG) && (HAVE_SWIG == 1))
    result = Swig_python::getSWIGPointerTypeObj_T(TypeName, &proxy);
#else
    (void)TypeName;
    result = -1;  // indicates error
#endif

    if (result == 0) {
        return proxy;
    }

    // none of the SWIG's succeeded
    throw Base::RuntimeError("No SWIG wrapped library loaded");
}

// F03_BASE_EXPORTS_BEGIN
namespace {
int32_t forgecad_base_error(FcPyReportV1* out, uint32_t code, const char* text) noexcept
{
    FcPyReportV1 report{sizeof(FcPyReportV1), FC_PY_ABI_V1, code, 0, FC_PY_COLD,
                         0, 0, 0, 0, FC_PY_STAGE_CAPABILITY, {0}};
    forgecad_diagnostic(report, text);
    *out = report;
    return static_cast<int32_t>(code);
}
}
extern "C" BaseExport int32_t FC_PY_CALL forgecad_base_python_profile_v1(
    uint32_t version, uint32_t size, FcPyBaseProfileV1* out) noexcept
{
    if (!out || version != FC_PY_ABI_V1 || size != sizeof(*out)) { return FC_PY_E_ABI; }
    if (forgecad_python.poison.load(std::memory_order_acquire)) { return FC_PY_E_POISONED; }
    if (PY_VERSION_HEX != FC_PY_PYTHON_HEX_V1 || sizeof(void*) != 8
        || Py_Version != FC_PY_PYTHON_HEX_V1) { return FC_PY_E_VERSION; }
    const FcPyBaseProfileV1 profile{sizeof(FcPyBaseProfileV1), FC_PY_ABI_V1,
        FC_PY_FEATURES_V1, FC_PY_PYTHON_HEX_V1, 64, sizeof(wchar_t) * 8, {0, 0},
        FC_PY_CONTRACT_SHA256_BYTES};
    *out = profile;
    return FC_PY_OK;
}
extern "C" BaseExport int32_t FC_PY_CALL forgecad_base_python_snapshot_v1(
    FcPyReportV1* out) noexcept
{
    if (!forgecad_report_valid(out)) { return FC_PY_E_ABI; }
    return forgecad_base_result(out);
}
extern "C" BaseExport int32_t FC_PY_CALL forgecad_base_python_mark_unknown_v1(
    uint32_t stage, FcPyReportV1* out) noexcept
{
    if (!forgecad_report_valid(out)) { return FC_PY_E_ABI; }
    if (stage >= FC_PY_STAGE_APP_INITIALIZE && stage <= FC_PY_STAGE_DESTRUCT) {
        forgecad_python.report.flags |= FC_PY_REPORT_APP_ENTERED;
    }
    forgecad_unknown(stage, "SDK process state cannot be safely rolled back");
    return forgecad_base_result(out);
}
extern "C" BaseExport int32_t FC_PY_CALL forgecad_base_python_finalize_before_app_v1(
    FcPyReportV1* out) noexcept
{
    if (!forgecad_report_valid(out)) { return FC_PY_E_ABI; }
    auto& state = forgecad_python;
    auto& r = state.report;
    if (r.lifecycle == FC_PY_CLOSED || r.lifecycle == FC_PY_CLOSED_FAULT
        || r.lifecycle == FC_PY_UNKNOWN) { return forgecad_base_result(out); }
    if (!state.active || state.owner != std::this_thread::get_id()
        || (r.flags & FC_PY_REPORT_APP_ENTERED)) {
        forgecad_unknown(FC_PY_STAGE_FINALIZE, "SDK pre-App finalize ownership is invalid");
        return forgecad_base_result(out);
    }
    try {
        if (!(r.flags & FC_PY_REPORT_ATTEMPTED)) {
            r.lifecycle = FC_PY_CLOSED;
            r.flags |= FC_PY_REPORT_CLEANUP_COMPLETE;
        }
        else if (state.held || state.saved_valid) { Base::Interpreter().finalize(); }
        else { forgecad_unknown(r.failure_stage, "CPython partial initialization cannot be finalized"); }
        if (state.singleton_live
            && (r.lifecycle == FC_PY_CLOSED || r.lifecycle == FC_PY_CLOSED_FAULT)) {
            Base::InterpreterSingleton::Destruct();
        }
    }
    catch (...) { forgecad_unknown(FC_PY_STAGE_FINALIZE, "SDK pre-App cleanup threw"); }
    return forgecad_base_result(out);
}
extern "C" BaseExport int32_t FC_PY_CALL forgecad_base_python_initialize_v1(
    const FcPyConfigV1* request, FcPyReportV1* out) noexcept
{
    if (!forgecad_report_valid(out)) { return FC_PY_E_ABI; }
    if (!request || request->struct_size != sizeof(*request)
        || request->abi_version != FC_PY_ABI_V1) {
        return forgecad_base_error(out, FC_PY_E_ABI, "SDK configuration ABI mismatch");
    }
    FcPyBaseProfileV1 profile{};
    const int32_t available = forgecad_base_python_profile_v1(FC_PY_ABI_V1, sizeof(profile), &profile);
    if (available != FC_PY_OK) {
        return forgecad_base_error(out, available, "SDK Python capability unavailable");
    }
    if (forgecad_python.used || Py_IsInitialized()) {
        return forgecad_base_error(out, FC_PY_E_ALREADY_USED, "SDK owned interpreter is one-shot");
    }
    if (request->flags != 3 || request->mode > FC_PY_MODE_EXPLICIT
        || request->reserved[0] || request->reserved[1]
        || request->path_count > 4096 || (!request->paths && request->path_count)) {
        return forgecad_base_error(out, FC_PY_E_INPUT, "Invalid SDK Python configuration");
    }
    try {
        if (forgecad_wide(request->program).empty()) {
            return forgecad_base_error(out, FC_PY_E_INPUT, "Empty Python program");
        }
        if (request->mode == FC_PY_MODE_EXPLICIT) {
            if (forgecad_wide(request->python_home).empty() || request->path_count == 0) {
                return forgecad_base_error(out, FC_PY_E_INPUT, "Incomplete explicit Python paths");
            }
            for (uint64_t i = 0; i < request->path_count; ++i) {
                if (forgecad_wide(request->paths[i]).empty()) {
                    return forgecad_base_error(out, FC_PY_E_INPUT, "Empty Python search item");
                }
            }
        }
        else if (request->python_home.data || request->python_home.size
                 || request->paths || request->path_count) {
            return forgecad_base_error(out, FC_PY_E_INPUT, "SDK_AUTO must not override paths");
        }
    }
    catch (...) { return forgecad_base_error(out, FC_PY_E_INPUT, "Invalid Unicode Python input"); }
    auto& state = forgecad_python;
    state.used = true;
    state.active = true;
    state.owner = std::this_thread::get_id();
    state.pending = request;
    struct ClearPending {
        ~ClearPending() { forgecad_python.pending = nullptr; }
    } pending_guard;
    try {
        auto& interpreter = Base::Interpreter();
        state.singleton_live = true;
        interpreter.init(0, nullptr);
        state.report.failure_stage = FC_PY_STAGE_NONE;
        return forgecad_base_result(out);
    }
    catch (const std::exception& error) {
        state.report.primary_status = state.report.failure_stage == FC_PY_STAGE_STDOUT
            ? FC_PY_E_STDOUT : FC_PY_E_INIT;
        forgecad_diagnostic(state.report, error.what());
    }
    catch (...) {
        state.report.primary_status = state.report.failure_stage == FC_PY_STAGE_STDOUT
            ? FC_PY_E_STDOUT : FC_PY_E_INIT;
        forgecad_diagnostic(state.report, "SDK interpreter initialization threw");
    }
    return forgecad_base_python_finalize_before_app_v1(out);
}
// F03_BASE_EXPORTS_END

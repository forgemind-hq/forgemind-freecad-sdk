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
// SDK-private ABI. See native/README.md for the complete approved contract.
// query is read-only and writes nothing on failure. All callback reports must
// be nonnull with exact struct_size/abi_version before use; invalid reports are
// never written. prepare sets *ctx=NULL before validating a valid configuration
// boundary, deepcopies UTF8 bytes and borrows no caller buffer after return.
// start/stop are serialized on prepare's thread, entered without a held GIL.
// int return must equal report.primary_status; an OK destroy only receipts ctx
// memory release, not interpreter recovery. Only SDK destroy frees SDK contexts.
// API/contract bytes and callback code stay borrowed from process-mapped SDK.
#ifndef FORGECAD_SDK_PYTHON_V1_H
#define FORGECAD_SDK_PYTHON_V1_H
#include <stdint.h>
#if defined(FORGECAD_SDK_BUILD_INTERNAL)
# include <FCConfig.h>
# define FC_PY_APP_EXPORT AppExport
# define FC_PY_BASE_EXPORT BaseExport
#else
# define FC_PY_APP_EXPORT
# define FC_PY_BASE_EXPORT
#endif
#if defined(_WIN32)
# define FC_PY_CALL __cdecl
#else
# define FC_PY_CALL
#endif
#ifdef __cplusplus
# define FC_PY_NOEXCEPT noexcept
extern "C" {
#else
# define FC_PY_NOEXCEPT
#endif
#define FC_PY_ABI_V1 0x00010000u
#define FC_PY_FLAG_NO_USER_SITE 1u
#define FC_PY_FLAG_NO_BYTECODE 2u
#define FC_PY_MODE_SDK_AUTO 0u
#define FC_PY_MODE_EXPLICIT 1u


#define FC_PY_FEATURES_V1 31u
#define FC_PY_SDK_RELEASE_V1 0x00010103u
#define FC_PY_PYTHON_HEX_V1 0x030b09f0u
enum FcPyStatusV1 {
    FC_PY_OK=0, FC_PY_E_ABI=1, FC_PY_E_INPUT=2, FC_PY_E_VERSION=3,
    FC_PY_E_ALREADY_USED=4, FC_PY_E_THREAD=5, FC_PY_E_STATE=6,
    FC_PY_E_NOMEM=7, FC_PY_E_INIT=8, FC_PY_E_STDOUT=9, FC_PY_E_APP=10,
    FC_PY_E_IMPORT=11, FC_PY_E_CLEANUP=12, FC_PY_E_UNKNOWN=13, FC_PY_E_POISONED=14
};
enum FcPyLifecycleV1 {
    FC_PY_COLD=0, FC_PY_PREPARED=1, FC_PY_PREINITIALIZING=2,
    FC_PY_PYTHON_HELD=3, FC_PY_STDOUT_READY=4, FC_PY_SAVED=5,
    FC_PY_APP_ENTERING=6, FC_PY_APP_READY=7, FC_PY_READY=8,
    FC_PY_FINALIZING=9, FC_PY_CLOSED=10, FC_PY_CLOSED_FAULT=11, FC_PY_UNKNOWN=12
};
enum FcPyStageV1 {
    FC_PY_STAGE_NONE=0, FC_PY_STAGE_CAPABILITY=1, FC_PY_STAGE_PREPARE=2,
    FC_PY_STAGE_PREINITIALIZE=3, FC_PY_STAGE_CONFIG=4, FC_PY_STAGE_INITIALIZE=5,
    FC_PY_STAGE_STDOUT=6, FC_PY_STAGE_SAVE_THREAD=7, FC_PY_STAGE_PATH_READ=8,
    FC_PY_STAGE_APP_CONFIG=9, FC_PY_STAGE_APP_INITIALIZE=10,
    FC_PY_STAGE_IMPORT=11, FC_PY_STAGE_DESTRUCT=12, FC_PY_STAGE_FINALIZE=13,
    FC_PY_STAGE_THREAD=14, FC_PY_STAGE_PROTECTION_RESTORE=15
};
#define FC_PY_REPORT_POISON 1u
#define FC_PY_REPORT_APP_ENTERED 2u
#define FC_PY_REPORT_APP_READY 4u
#define FC_PY_REPORT_ATTEMPTED 8u
#define FC_PY_REPORT_CLEANUP_COMPLETE 16u

typedef struct FcPyUtf8V1 {
    const char *data;
    uint64_t size;
} FcPyUtf8V1;

typedef struct FcPyConfigV1 {
    uint32_t struct_size;       /* 128 */
    uint32_t abi_version;       /* FC_PY_ABI_V1 */
    uint32_t flags;             /* exactly 3 */
    uint32_t mode;              /* 0 or 1 */
    FcPyUtf8V1 program;         /* offset 16 */
    FcPyUtf8V1 python_home;     /* offset 32 */
    const FcPyUtf8V1 *paths;    /* offset 48 */
    uint64_t path_count;        /* offset 56 */
    FcPyUtf8V1 application_arg; /* offset 64 */
    FcPyUtf8V1 user_parameter;  /* offset 80; isolated absolute file path */
    FcPyUtf8V1 system_parameter;/* offset 96 */
    uint64_t reserved[2];       /* offset 112; zero */
} FcPyConfigV1;

typedef struct FcPyReportV1 {
    uint32_t struct_size;       /* 296 */
    uint32_t abi_version;
    uint32_t primary_status;
    uint32_t cleanup_status;
    uint32_t lifecycle;
    uint32_t python_state;      /* 0 no, 1 yes, 2 unknown */
    uint32_t gil_state;         /* 0 detached, 1 held, 2 unknown */
    uint32_t finalize_calls;    /* actual calls, 0 or 1 */
    uint32_t flags;             /* poison=1, app_entered=2, app_ready=4,
                                  python_attempted=8, cleanup_complete=16 */
    uint32_t failure_stage;
    char diagnostic[256];      /* SDK-owned value copied into caller output */
} FcPyReportV1;

typedef struct FcPyContextV1 FcPyContextV1; /* opaque SDK allocation */
typedef int32_t (FC_PY_CALL *FcPyPrepareV1)(
    const FcPyConfigV1 *, FcPyContextV1 **, FcPyReportV1 *) FC_PY_NOEXCEPT;
typedef int32_t (FC_PY_CALL *FcPyStartV1)(
    FcPyContextV1 *, FcPyReportV1 *) FC_PY_NOEXCEPT;
typedef int32_t (FC_PY_CALL *FcPyStopV1)(
    FcPyContextV1 *, FcPyReportV1 *) FC_PY_NOEXCEPT;
typedef int32_t (FC_PY_CALL *FcPyDestroyV1)(
    FcPyContextV1 *, FcPyReportV1 *) FC_PY_NOEXCEPT;

typedef struct FcPyApiV1 {
    uint32_t struct_size;       /* 112 */
    uint32_t abi_version;
    uint32_t features;          /* exactly 0x1f */
    uint32_t sdk_release;       /* 0x00010103 = FreeCAD 1.1.3 */
    uint32_t python_hex;        /* 0x030b09f0 */
    uint32_t pointer_bits;      /* 64 */
    uint32_t wchar_bits;        /* Windows16, Linux/macOS32 */
    uint32_t reserved;          /* 0 */
    uint8_t contract_sha256[32];/* offset 32; hash of canonical contract JSON */
    FcPyUtf8V1 contract_json;   /* offset 64; borrowed immutable UTF8, no NUL required */
    FcPyPrepareV1 prepare;      /* offset 80 */
    FcPyStartV1 start;          /* offset 88 */
    FcPyStopV1 stop;            /* offset 96 */
    FcPyDestroyV1 destroy;      /* offset 104 */
} FcPyApiV1;

typedef int32_t (FC_PY_CALL *FcPyQueryV1)(
    uint32_t requested_abi, uint32_t output_size, FcPyApiV1 *output) FC_PY_NOEXCEPT;
/* Definition uses AppExport (SDK FCConfig.h), plus the default C calling convention. */
FC_PY_APP_EXPORT int32_t FC_PY_CALL forgecad_sdk_python_query_v1(
    uint32_t requested_abi, uint32_t output_size, FcPyApiV1 *output) FC_PY_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#if defined(FORGECAD_SDK_BUILD_INTERNAL)
#ifdef __cplusplus
extern "C" {
#endif
typedef struct FcPyBaseProfileV1 {
    uint32_t struct_size;       /* 64 */
    uint32_t abi_version;
    uint32_t features;
    uint32_t python_hex;
    uint32_t pointer_bits;
    uint32_t wchar_bits;
    uint32_t reserved[2];
    uint8_t contract_sha256[32];
} FcPyBaseProfileV1;
/* All definitions carry existing BaseExport. */
FC_PY_BASE_EXPORT int32_t FC_PY_CALL forgecad_base_python_profile_v1(
    uint32_t requested_abi, uint32_t output_size, FcPyBaseProfileV1 *) FC_PY_NOEXCEPT;
FC_PY_BASE_EXPORT int32_t FC_PY_CALL forgecad_base_python_initialize_v1(
    const FcPyConfigV1 *, FcPyReportV1 *) FC_PY_NOEXCEPT;
FC_PY_BASE_EXPORT int32_t FC_PY_CALL forgecad_base_python_finalize_before_app_v1(
    FcPyReportV1 *) FC_PY_NOEXCEPT;
FC_PY_BASE_EXPORT int32_t FC_PY_CALL forgecad_base_python_snapshot_v1(
    FcPyReportV1 *) FC_PY_NOEXCEPT;
FC_PY_BASE_EXPORT int32_t FC_PY_CALL forgecad_base_python_mark_unknown_v1(
    uint32_t failure_stage, FcPyReportV1 *) FC_PY_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#endif /* FORGECAD_SDK_BUILD_INTERNAL */
#endif /* FORGECAD_SDK_PYTHON_V1_H */

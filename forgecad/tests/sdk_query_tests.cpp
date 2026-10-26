// =============================================================================
//    Copyright (c) 2026 Haixing Hu.
//
//    SPDX-License-Identifier: Apache-2.0
//
//    Licensed under the Apache License, Version 2.0.
// =============================================================================
#include "forgecad_sdk_python_v1.h"

#include <cstring>
#include <iostream>
#include <dlfcn.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::cerr << dlerror() << '\n';
        return 3;
    }
    auto query = reinterpret_cast<decltype(&forgecad_sdk_python_query_v1)>(
        dlsym(library, "forgecad_sdk_python_query_v1"));
    if (!query) {
        std::cerr << "SDK query export is absent\n";
        dlclose(library);
        return 4;
    }
    FcPyApiV1 api {};
    const auto status = query(FC_PY_ABI_V1, sizeof(api), &api);
    if (status != FC_PY_OK || api.struct_size != sizeof(api)
        || api.abi_version != FC_PY_ABI_V1 || api.features != FC_PY_FEATURES_V1
        || api.sdk_release != FC_PY_SDK_RELEASE_V1 || api.python_hex != FC_PY_PYTHON_HEX_V1
        || !api.prepare || !api.start || !api.stop || !api.destroy
        || !api.contract_json.data || !api.contract_json.size) {
        std::cerr << "SDK capability/version query failed: " << status << '\n';
        dlclose(library);
        return 5;
    }
    const auto previous = api;
    if (query(FC_PY_ABI_V1 + 1, sizeof(api), &api) != FC_PY_E_ABI
        || std::memcmp(&previous, &api, sizeof(api)) != 0) {
        std::cerr << "SDK query modified output on invalid ABI\n";
        dlclose(library);
        return 6;
    }
    std::cout.write(api.contract_json.data, static_cast<std::streamsize>(api.contract_json.size));
    std::cout << '\n';
    dlclose(library);
}

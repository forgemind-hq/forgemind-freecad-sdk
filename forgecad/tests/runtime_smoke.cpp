// Copyright (c) 2026 Haixing Hu.
// SPDX-License-Identifier: Apache-2.0
// Compile with the compiler repository's native/include and the prepared bridge.
#include "forgecad_runtime.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    void* runtime = nullptr;
    void* document = nullptr;
    std::vector<std::string> results;
    auto take = [&](const char* label, char* raw) {
        if (!raw) {
            throw std::runtime_error("null ABI result");
        }
        std::string value(raw);
        forgecad_string_free(raw);
        results.push_back(std::string(label) + ": " + value);
        if (value.rfind("{\"error\":", 0) == 0) {
            throw std::runtime_error(value);
        }
    };
    int status = 0;
    try {
        take("connect", forgecad_runtime_connect(0, &runtime));
        take("document", forgecad_document_create(runtime, "SDKSmoke", 0, &document));
        take("environment", forgecad_document_operation(document, "environment", "{}"));
        take("box", forgecad_document_operation(document, "create_object",
                                               R"({"type":"Part::Box","name":"Box"})"));
        take("recompute", forgecad_document_operation(document, "recompute", "{}"));
        take("measure", forgecad_document_operation(document, "measure", R"({"object":"Box"})"));
        take("assembly", forgecad_document_operation(document, "assembly_initialize", "{}"));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    if (document) {
        forgecad_document_release(document);
    }
    if (runtime) {
        forgecad_runtime_release(runtime);
    }
    for (const auto& line : results) {
        std::cout << line << '\n';
    }
    return status;
}

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

namespace VideoCommon::Nx2Perf {

inline bool Verify() {
    static const bool verify = [] {
        const char* value = std::getenv("EDEN_NX2_PERF_VERIFY");
        return value != nullptr && value[0] == '1';
    }();
    return verify;
}

inline bool DeferFragmentBarriers() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_DEFER_FRAGMENT_BARRIER");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

inline bool AsyncNativePipelines() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_ASYNC_NATIVE_PIPELINES");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

} // namespace VideoCommon::Nx2Perf

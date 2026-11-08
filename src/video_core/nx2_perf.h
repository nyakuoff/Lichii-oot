// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
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
/// The frame period the game asks for (nx2:vi2 present word 12 at the speed limit), 0 while
/// unknown or unpaced. The host frame pacer (vk_host_pacer.h) never holds frames longer.
inline std::atomic<long long> g_guest_frame_interval_ns{0};

/// The display's measured refresh period while the host frame pacer runs, else 0. The game's
/// present pacing (nx2_gsv.h) times its frames to whole multiples of it (genlock).
inline std::atomic<long long> g_host_refresh_ns{0};

/// The refresh rate in Hz of the display showing the render window, 0 while unknown. The
/// frontends set it where the OS has no compositor timing query for the host frame pacer
/// (Linux); EDEN_NX2_HOST_REFRESH_HZ overrides it.
inline std::atomic<double> g_display_refresh_hz{0.0};
/// At 30 FPS and below the host frame pacer presents frames as soon as they are rendered and
/// publishes how long before the display's next refresh the last one was ready (ns), or -1
/// while it isn't pacing that way. The game's present pacing (nx2_gsv.h) moves its frames until
/// that is ~6 ms: each lands on a refresh with little wait (input lag) and evenly spaced.
inline std::atomic<long long> g_host_phase_ns{-1};

/// Host frame times: the interval between frames handed to the swapchain, counted in 0.25 ms
/// buckets (the last holds 63.75 ms and up). The debug server's status returns the counts; the
/// difference between two calls gives that window's percentiles (1% and 0.1% lows).
inline constexpr size_t FrameTimeBuckets = 256;
inline constexpr long long FrameTimeBucketNs = 250'000;
inline std::array<std::atomic<unsigned long long>, FrameTimeBuckets> g_frame_times{};

inline void NoteFrameTime(long long interval_ns) {
    // Longer gaps are loading screens or a paused game, not frames.
    if (interval_ns <= 0 || interval_ns > 1'000'000'000) {
        return;
    }
    const auto bucket =
        (std::min)(static_cast<size_t>(interval_ns / FrameTimeBucketNs), FrameTimeBuckets - 1);
    g_frame_times[bucket].fetch_add(1, std::memory_order_relaxed);
}

} // namespace VideoCommon::Nx2Perf

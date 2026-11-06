// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <optional>

#include "common/common_types.h"

namespace VideoCore::FrameGen {

enum class Backend : u8 { Inactive, Fsr3, Lsfg };

struct Mode {
    Backend backend{};
    u32 multiplier{};
    u32 target_rate{};
};

struct StatsResults {
    Mode mode;
    double output_fps{};
    double generated_fps{};
    double dropped_fps{};
    double skipped_fps{};
    double average_present_ms{};
    double worst_present_ms{};
};

// One presentation-thread producer and one stats consumer. Counters include only successful
// queue presents, not frames queued by the renderer. Vulkan does not report compositor scanout.
class Stats {
public:
    using Clock = std::chrono::steady_clock;

    explicit Stats(Clock::time_point now = Clock::now()) : last_sample{now} {}

    void Presented(Mode mode, bool generated, u32 skipped, Clock::time_point now) {
        const u64 packed_mode = static_cast<u64>(mode.backend) |
                                (static_cast<u64>(mode.multiplier) << 8) |
                                (static_cast<u64>(mode.target_rate) << 32);
        const bool changed = previous_configuration != packed_mode;
        if (changed) {
            previous_configuration = packed_mode;
            configuration.store(packed_mode, std::memory_order_relaxed);
        }
        if (mode.backend == Backend::Inactive || changed) {
            previous_present.reset();
        }
        if (mode.backend == Backend::Inactive) {
            return;
        }

        // Pack both counts so sampling cannot split a generated present from its output count.
        presents.fetch_add(1 + (static_cast<u64>(generated) << 32), std::memory_order_relaxed);
        skipped_frames.fetch_add(skipped, std::memory_order_relaxed);
        if (previous_present && now > *previous_present) {
            const auto ns = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - *previous_present)
                    .count());
            timing_sequence.fetch_add(1, std::memory_order_acq_rel);
            interval_ns.fetch_add(ns, std::memory_order_relaxed);
            intervals.fetch_add(1, std::memory_order_relaxed);
            timing_sequence.fetch_add(1, std::memory_order_release);
            auto worst = worst_ns.load(std::memory_order_relaxed);
            while (worst < ns &&
                   !worst_ns.compare_exchange_weak(worst, ns, std::memory_order_relaxed)) {
            }
        }
        previous_present = now;
    }

    void Dropped(bool generated, u32 skipped = 0) {
        if (generated) {
            dropped_frames.fetch_add(1, std::memory_order_relaxed);
        }
        skipped_frames.fetch_add(skipped, std::memory_order_relaxed);
    }

    StatsResults Sample(Clock::time_point now) {
        const double seconds = std::chrono::duration<double>(now - last_sample).count();
        if (seconds <= 0.0) {
            return {};
        }
        last_sample = now;
        const u64 counts = presents.exchange(0, std::memory_order_relaxed);
        const auto dropped = dropped_frames.exchange(0, std::memory_order_relaxed);
        const auto skipped = skipped_frames.exchange(0, std::memory_order_relaxed);
        u64 total_ns;
        u64 count;
        // Read cumulative timing as a pair; the producer never waits for the sampler.
        for (;;) {
            const auto sequence = timing_sequence.load(std::memory_order_acquire);
            if (sequence & 1) {
                continue;
            }
            total_ns = interval_ns.load(std::memory_order_relaxed);
            count = intervals.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence == timing_sequence.load(std::memory_order_relaxed)) {
                break;
            }
        }
        const auto window_ns = total_ns - previous_interval_ns;
        const auto window_count = count - previous_intervals;
        previous_interval_ns = total_ns;
        previous_intervals = count;
        const auto worst = worst_ns.exchange(0, std::memory_order_relaxed);
        const auto mode = configuration.load(std::memory_order_relaxed);
        if (static_cast<Backend>(mode & 0xff) == Backend::Inactive) {
            return {};
        }
        return {
            .mode = {static_cast<Backend>(mode & 0xff), static_cast<u32>((mode >> 8) & 0xffffff),
                     static_cast<u32>(mode >> 32)},
            .output_fps = static_cast<double>(static_cast<u32>(counts)) / seconds,
            .generated_fps = static_cast<double>(counts >> 32) / seconds,
            .dropped_fps = static_cast<double>(dropped) / seconds,
            .skipped_fps = static_cast<double>(skipped) / seconds,
            .average_present_ms = window_count ? static_cast<double>(window_ns) /
                                                     static_cast<double>(window_count) / 1.0e6
                                               : 0.0,
            .worst_present_ms = static_cast<double>(worst) / 1.0e6,
        };
    }

private:
    static_assert(std::atomic<u64>::is_always_lock_free);
    std::atomic<u64> configuration{};
    std::atomic<u64> presents{};
    std::atomic<u64> dropped_frames{};
    std::atomic<u64> skipped_frames{};
    std::atomic<u64> interval_ns{};
    std::atomic<u64> intervals{};
    std::atomic<u64> timing_sequence{};
    std::atomic<u64> worst_ns{};
    u64 previous_configuration{};
    u64 previous_interval_ns{};
    u64 previous_intervals{};
    std::optional<Clock::time_point> previous_present;
    Clock::time_point last_sample;
};

} // namespace VideoCore::FrameGen

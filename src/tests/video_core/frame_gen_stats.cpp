// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <thread>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "video_core/frame_gen/stats.h"

using namespace std::chrono_literals;
using namespace VideoCore::FrameGen;

TEST_CASE("Frame generation stats count successful output, not requested generations",
          "[frame_gen_stats]") {
    const Stats::Clock::time_point start{};
    Stats stats{start};
    const Mode mode{Backend::Fsr3, 3, 0};
    stats.Presented(mode, false, 2, start + 10ms);
    stats.Presented(mode, true, 0, start + 20ms);
    stats.Presented(mode, true, 0, start + 45ms);
    stats.Dropped(true);
    stats.Dropped(false, 1);

    const auto result = stats.Sample(start + 500ms);
    REQUIRE(result.mode.backend == Backend::Fsr3);
    REQUIRE(result.mode.multiplier == 3);
    REQUIRE(result.output_fps == 6.0);
    REQUIRE(result.generated_fps == 4.0);
    REQUIRE(result.dropped_fps == 2.0);
    REQUIRE(result.skipped_fps == 6.0);
    REQUIRE(result.average_present_ms == Catch::Approx(17.5));
    REQUIRE(result.worst_present_ms == Catch::Approx(25.0));

    const auto empty = stats.Sample(start + 1s);
    REQUIRE(empty.output_fps == 0.0);
    REQUIRE(empty.generated_fps == 0.0);
    REQUIRE(empty.dropped_fps == 0.0);
    REQUIRE(empty.skipped_fps == 0.0);
    REQUIRE(empty.average_present_ms == 0.0);
    REQUIRE(empty.worst_present_ms == 0.0);

    stats.Presented(mode, false, 0, start + 1010ms);
    const auto next = stats.Sample(start + 1500ms);
    REQUIRE(next.output_fps == 2.0);
    REQUIRE(next.average_present_ms == Catch::Approx(965.0));
    REQUIRE(next.worst_present_ms == Catch::Approx(965.0));
}

TEST_CASE("Inactive frame generation and backend changes reset present timing",
          "[frame_gen_stats]") {
    const Stats::Clock::time_point start{};
    Stats stats{start};
    stats.Presented({}, false, 0, start + 10ms);
    REQUIRE(stats.Sample(start + 500ms).mode.backend == Backend::Inactive);

    const Mode fsr{Backend::Fsr3, 2, 0};
    stats.Presented(fsr, false, 0, start + 510ms);
    stats.Presented(fsr, true, 0, start + 520ms);
    stats.Presented({}, false, 0, start + 530ms);
    const auto off = stats.Sample(start + 1s);
    REQUIRE(off.mode.backend == Backend::Inactive);
    REQUIRE(off.output_fps == 0.0);

    const Mode lsfg{Backend::Lsfg, 4, 120};
    stats.Presented(lsfg, false, 0, start + 1100ms);
    REQUIRE(stats.Sample(start + 1s).output_fps == 0.0);
    const auto on = stats.Sample(start + 1500ms);
    REQUIRE(on.mode.backend == Backend::Lsfg);
    REQUIRE(on.mode.multiplier == 4);
    REQUIRE(on.mode.target_rate == 120);
    REQUIRE(on.output_fps == 2.0);
    REQUIRE(on.generated_fps == 0.0);
    REQUIRE(on.average_present_ms == 0.0);
    REQUIRE(on.worst_present_ms == 0.0);
}

TEST_CASE("Frame generation stats survive concurrent sampling without losing presents",
          "[frame_gen_stats]") {
    const Stats::Clock::time_point start{};
    Stats stats{start};
    const Mode mode{Backend::Lsfg, 2, 0};
    constexpr u32 frame_count = 10000;
    std::atomic<bool> finished{};
    std::jthread producer([&] {
        for (u32 i = 0; i < frame_count; ++i) {
            stats.Presented(mode, i % 2 != 0, 0, start + i * 1ms);
            stats.Dropped(true);
        }
        finished.store(true, std::memory_order_release);
    });
    auto now = start;
    double output = 0.0;
    double generated = 0.0;
    double dropped = 0.0;
    const auto sample = [&] {
        const auto result = stats.Sample(now += 1s);
        REQUIRE(result.generated_fps <= result.output_fps);
        REQUIRE((result.average_present_ms == 0.0 || result.average_present_ms == 1.0));
        output += result.output_fps;
        generated += result.generated_fps;
        dropped += result.dropped_fps;
    };
    while (!finished.load(std::memory_order_acquire)) {
        sample();
    }
    producer.join();
    sample();
    REQUIRE(output == frame_count);
    REQUIRE(generated == frame_count / 2);
    REQUIRE(dropped == frame_count);
}

TEST_CASE("A new renderer has no stale frame generation state", "[frame_gen_stats]") {
    const Stats::Clock::time_point start{};
    Stats stats{start};
    stats.Dropped(true);
    const auto failed = stats.Sample(start + 1s);
    REQUIRE(failed.mode.backend == Backend::Inactive);
    REQUIRE(failed.output_fps == 0.0);
    Stats next{start + 1s};
    const auto clean = next.Sample(start + 2s);
    REQUIRE(clean.mode.backend == Backend::Inactive);
    REQUIRE(clean.dropped_fps == 0.0);
}

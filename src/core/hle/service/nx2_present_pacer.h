// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace Service {

/// The interval between paced presents: the vblank the game asked for, lengthened when an explicit
/// FPS limit is below the rate that vblank allows.
[[nodiscard]] inline std::chrono::nanoseconds PacedInterval(std::chrono::nanoseconds vblank_interval,
                                                            std::uint32_t fps_limit) {
    if (fps_limit == 0) {
        return vblank_interval;
    }
    const auto limited_interval = std::chrono::nanoseconds{1'000'000'000LL / fps_limit};
    return (std::max)(vblank_interval, limited_interval);
}

class Nx2PresentPacer {
public:
    using Clock = std::chrono::steady_clock;

    struct Step {
        Clock::time_point deadline;
        bool reset;
    };

    Step Advance(Clock::time_point now, std::chrono::nanoseconds interval) {
        const auto deadline = last_deadline ? *last_deadline + interval : now;
        const bool reset = !last_deadline || now > deadline + interval;
        last_deadline = reset ? now : deadline;
        return {*last_deadline, reset};
    }

private:
    std::optional<Clock::time_point> last_deadline;
};

} // namespace Service

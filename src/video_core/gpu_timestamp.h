// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/settings_enums.h"

namespace Tegra {

template <typename Timing>
[[nodiscard]] u64 ReadGpuTimestamp(const Timing& timing, bool ampere, Settings::GpuClock clock) {
    if (ampere) {
        return static_cast<u64>(timing.GetGlobalTimeNs().count());
    }
    const u64 ticks = timing.GetGPUTicks();
    switch (clock) {
    case Settings::GpuClock::Boost:
        return ticks / 256;
    case Settings::GpuClock::Overclock:
        return ticks / 512;
    default:
        return ticks;
    }
}

[[nodiscard]] constexpr u64 AmpereGpuTimeDivisor(Settings::GpuClock clock) {
    switch (clock) {
    case Settings::GpuClock::Boost:
        return 4;
    case Settings::GpuClock::Overclock:
        return 8;
    default:
        return 1;
    }
}

/// Shorten report durations without moving them out of the driver/VI nanosecond clock domain.
[[nodiscard]] constexpr u64 DivideSinceAnchor(u64 now, u64 anchor, u64 divisor) {
    // There is no frame anchor before the first present.
    if (anchor == 0 || divisor <= 1 || now <= anchor) {
        return now;
    }
    return anchor + (now - anchor) / divisor;
}

} // namespace Tegra

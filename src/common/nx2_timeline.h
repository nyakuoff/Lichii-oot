// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <string>

#include "common/common_types.h"

// A timeline of what the emulator's threads do and wait for, for a few seconds: each thread
// records spans (begin, duration, name) into its own buffer while a recording runs, and the
// recording is written as a Chrome trace (open it in ui.perfetto.dev or chrome://tracing).
// Off unless started: EDEN_NX2_TIMELINE=<dir> records once EDEN_NX2_TIMELINE_AT seconds after
// the first present (default 60) for EDEN_NX2_TIMELINE_SECONDS (default 5); the debug server's
// `timeline` method starts one at any time. While none runs, a span costs one relaxed load.
namespace Common::Nx2Timeline {

inline std::atomic<bool> g_active{false};

[[nodiscard]] inline bool Active() {
    return g_active.load(std::memory_order_relaxed);
}

/// Nanoseconds on the clock the spans use.
[[nodiscard]] u64 Now();

/// Records a span on the calling thread. `name` must outlive the recording (a literal).
void Record(const char* name, u64 begin_ns, u64 end_ns);

/// Records a zero-length marker on the calling thread.
void Instant(const char* name);

/// Starts a recording of `seconds` written to `path`. False if one is already running.
bool Start(const std::filesystem::path& path, double seconds);

/// The file the last recording wrote ("" until one finished), and whether one is running.
[[nodiscard]] std::string LastFile();

/// Called at each guest present: starts the EDEN_NX2_TIMELINE recording when it is due.
void OnPresent();

class Span {
public:
    explicit Span(const char* name_) : name{name_}, on{Active()} {
        if (on) {
            begin = Now();
        }
    }
    ~Span() {
        if (on) {
            Record(name, begin, Now());
        }
    }
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;

private:
    const char* name;
    bool on;
    u64 begin{};
};

} // namespace Common::Nx2Timeline

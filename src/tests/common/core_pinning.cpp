// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <iterator>
#include <vector>
#if defined(__linux__) && !defined(__ANDROID__)
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <sched.h>
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/thread.h"
#endif
#include <catch2/catch_test_macros.hpp>
#include "common/core_pinning.h"

namespace {

using Common::LogicalProcessor;

// Windows numbers CPU sets from 256 in logical processor order, and every SMT sibling of a core
// reports the logical processor index of the core's first sibling as its CoreIndex.
constexpr u32 FirstCpuSetId = 256;

void AddCores(std::vector<LogicalProcessor>& processors, u32 cores, u32 threads_per_core,
              u8 efficiency_class) {
    for (u32 core = 0; core < cores; ++core) {
        const u32 first = static_cast<u32>(processors.size());
        for (u32 thread = 0; thread < threads_per_core; ++thread) {
            processors.push_back({FirstCpuSetId + first + thread, first, efficiency_class});
        }
    }
}

std::vector<LogicalProcessor> Uniform(u32 cores, u32 threads_per_core) {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, cores, threads_per_core, 0);
    return processors;
}

std::vector<u32> Ids(u32 first, u32 count) {
    std::vector<u32> ids(count);
    for (u32 i = 0; i < count; ++i) {
        ids[i] = FirstCpuSetId + first + i;
    }
    return ids;
}

std::vector<u32> Concat(std::vector<u32> head, const std::vector<u32>& tail) {
    std::ranges::copy(tail, std::back_inserter(head));
    return head;
}

std::vector<u32> Sorted(std::vector<u32> ids) {
    std::ranges::sort(ids);
    return ids;
}

u32 CoreOf(const std::vector<LogicalProcessor>& processors, u32 id) {
    return std::ranges::find(processors, id, &LogicalProcessor::id)->core;
}

void CheckSkipped(const Common::GpuThreadPinning& plan) {
    CHECK(plan.front.empty());
    CHECK(plan.back.empty());
    CHECK(plan.guest.empty());
}

// Front and back each own one whole core, and no guest processor is an SMT sibling of either.
void CheckCoresAreDedicated(const std::vector<LogicalProcessor>& processors,
                            const Common::GpuThreadPinning& plan) {
    REQUIRE(!plan.front.empty());
    REQUIRE(!plan.back.empty());
    const u32 front_core = CoreOf(processors, plan.front.front());
    const u32 back_core = CoreOf(processors, plan.back.front());
    CHECK(front_core != back_core);
    for (const u32 id : plan.front) {
        CHECK(CoreOf(processors, id) == front_core);
    }
    for (const u32 id : plan.back) {
        CHECK(CoreOf(processors, id) == back_core);
    }
    for (const u32 id : plan.guest) {
        CHECK(CoreOf(processors, id) != front_core);
        CHECK(CoreOf(processors, id) != back_core);
    }
}

} // Anonymous namespace

TEST_CASE("Core pinning: 8 cores, 16 threads", "[common][core_pinning]") {
    const auto processors = Uniform(8, 2);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 8);
    CHECK(plan.front == Ids(2, 2));
    CHECK(plan.back == Ids(4, 2));
    CHECK(plan.guest == Concat(Ids(0, 2), Ids(6, 10)));
    CheckCoresAreDedicated(processors, plan);
}

TEST_CASE("Core pinning: 6 cores, 12 threads", "[common][core_pinning]") {
    const auto processors = Uniform(6, 2);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 6);
    CHECK(plan.front == Ids(2, 2));
    CHECK(plan.back == Ids(4, 2));
    CHECK(plan.guest == Concat(Ids(0, 2), Ids(6, 6)));
    CheckCoresAreDedicated(processors, plan);
}

TEST_CASE("Core pinning: 6 cores without SMT", "[common][core_pinning]") {
    const auto processors = Uniform(6, 1);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.front == Ids(1, 1));
    CHECK(plan.back == Ids(2, 1));
    CHECK(plan.guest == Concat(Ids(0, 1), Ids(3, 3)));
    CheckCoresAreDedicated(processors, plan);
}

TEST_CASE("Core pinning: hybrid 8 P-cores with SMT and 16 E-cores", "[common][core_pinning]") {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, 8, 2, 1);
    AddCores(processors, 16, 1, 0);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 8);
    CHECK(plan.front == Ids(2, 2));
    CHECK(plan.back == Ids(4, 2));
    CHECK(plan.guest == Concat(Ids(0, 2), Ids(6, 10)));
    CheckCoresAreDedicated(processors, plan);
}

TEST_CASE("Core pinning: E-cores enumerated first leave the P-cores to the pinned threads",
          "[common][core_pinning]") {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, 4, 1, 0);
    AddCores(processors, 6, 2, 1);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 6);
    CHECK(plan.front == Ids(6, 2));
    CHECK(plan.back == Ids(8, 2));
    CHECK(plan.guest == Concat(Ids(4, 2), Ids(10, 6)));
    CheckCoresAreDedicated(processors, plan);
}

TEST_CASE("Core pinning: hybrid 4 P-cores and 8 E-cores is skipped", "[common][core_pinning]") {
    std::vector<LogicalProcessor> processors;
    AddCores(processors, 4, 2, 1);
    AddCores(processors, 8, 1, 0);
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 4);
    CheckSkipped(plan);
}

TEST_CASE("Core pinning: 4 cores, 4 threads is skipped", "[common][core_pinning]") {
    const auto plan = Common::PlanGpuThreadPinning(Uniform(4, 1));
    CHECK(plan.performance_cores == 4);
    CheckSkipped(plan);
}

TEST_CASE("Core pinning: 2 cores is skipped", "[common][core_pinning]") {
    for (const u32 threads : {1U, 2U}) {
        const auto plan = Common::PlanGpuThreadPinning(Uniform(2, threads));
        CHECK(plan.performance_cores == 2);
        CheckSkipped(plan);
    }
}

TEST_CASE("Core pinning: no processors reported", "[common][core_pinning]") {
    const auto plan = Common::PlanGpuThreadPinning({});
    CHECK(plan.performance_cores == 0);
    CheckSkipped(plan);
}

TEST_CASE("Core pinning: the enumeration order does not change the cores",
          "[common][core_pinning]") {
    const auto ordered = Uniform(8, 2);
    auto reversed = ordered;
    std::ranges::reverse(reversed);
    const auto expected = Common::PlanGpuThreadPinning(ordered);
    const auto plan = Common::PlanGpuThreadPinning(reversed);
    CHECK(Sorted(plan.front) == expected.front);
    CHECK(Sorted(plan.back) == expected.back);
    CHECK(Sorted(plan.guest) == expected.guest);
}

TEST_CASE("Core pinning: process restrictions preserve physical core ownership",
          "[common][core_pinning]") {
    auto processors = Uniform(8, 2);
    // A taskset may allow only one SMT sibling, and omit a whole physical core.
    std::erase_if(processors, [](const auto& cpu) {
        return cpu.id == FirstCpuSetId + 1 || cpu.core == 6;
    });
    const auto plan = Common::PlanGpuThreadPinning(processors);
    CHECK(plan.performance_cores == 7);
    CHECK(plan.front == Ids(2, 2));
    CHECK(plan.back == Ids(4, 2));
    CHECK(plan.guest == Concat(Ids(0, 1), Ids(8, 8)));
    CheckCoresAreDedicated(processors, plan);
}

#if defined(__linux__) && !defined(__ANDROID__)
TEST_CASE("Core pinning: Linux setting and environment control the same affinity path",
          "[common][core_pinning][linux_affinity]") {
    const bool original_setting = Settings::values.pin_gpu_threads.GetValue();
    const char* original_env = std::getenv("EDEN_NX2_PIN_THREADS");
    const std::optional<std::string> saved_env = original_env
        ? std::make_optional<std::string>(original_env) : std::nullopt;
    SCOPE_EXIT {
        Settings::values.pin_gpu_threads.SetValue(original_setting);
        if (saved_env) {
            setenv("EDEN_NX2_PIN_THREADS", saved_env->c_str(), 1);
        } else {
            unsetenv("EDEN_NX2_PIN_THREADS");
        }
    };
    CHECK(Settings::values.pin_gpu_threads.GetDefault());
    cpu_set_t initial{};
    REQUIRE(sched_getaffinity(0, sizeof(initial), &initial) == 0);
    const auto masks = [] {
        std::array<cpu_set_t, 3> result{};
        size_t index = 0;
        for (const auto role : {Common::PinnedRole::GpuFront, Common::PinnedRole::GpuBack,
                                Common::PinnedRole::GuestCore}) {
            std::jthread worker{[&result, index, role] {
                Common::PinCurrentThread(role);
                sched_getaffinity(0, sizeof(cpu_set_t), &result[index]);
            }};
            worker.join();
            ++index;
        }
        return result;
    };
    unsetenv("EDEN_NX2_PIN_THREADS");
    Settings::values.pin_gpu_threads.SetValue(false);
    const auto disabled = masks();
    for (const auto& mask : disabled) {
        CHECK(CPU_EQUAL(&initial, &mask));
    }
    Settings::values.pin_gpu_threads.SetValue(true);
    const auto enabled = masks();
    setenv("EDEN_NX2_PIN_THREADS", "0", 1);
    for (const auto& mask : masks()) {
        CHECK(CPU_EQUAL(&initial, &mask));
    }
    Settings::values.pin_gpu_threads.SetValue(false);
    setenv("EDEN_NX2_PIN_THREADS", "1", 1);
    const auto forced = masks();
    for (size_t i = 0; i < forced.size(); ++i) {
        CHECK(CPU_EQUAL(&enabled[i], &forced[i]));
    }
    if (!CPU_EQUAL(&initial, &enabled[0])) {
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            CHECK_FALSE((CPU_ISSET(cpu, &enabled[0]) && CPU_ISSET(cpu, &enabled[1])));
            CHECK_FALSE((CPU_ISSET(cpu, &enabled[0]) && CPU_ISSET(cpu, &enabled[2])));
            CHECK_FALSE((CPU_ISSET(cpu, &enabled[1]) && CPU_ISSET(cpu, &enabled[2])));
            for (const auto& mask : enabled) {
                CHECK_FALSE((CPU_ISSET(cpu, &mask) && !CPU_ISSET(cpu, &initial)));
            }
        }
    }
}
#endif

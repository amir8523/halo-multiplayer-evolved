// SPDX-License-Identifier: MIT
// ForgeEvolved: Core/Pacing.cpp
#define FE_LOG_CATEGORY "Core.Pacing"

#include "Core/Pacing.h"

#include "Core/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <thread>

namespace fe::pacing {
namespace {

/// Converts a FILETIME to 100 nanosecond units.
[[nodiscard]] std::uint64_t ToTicks(const FILETIME& time) noexcept {
    ULARGE_INTEGER value{};
    value.LowPart  = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

} // namespace

void WorkPacer::Tick() noexcept {
    if (++counter_ < interval_) {
        return;
    }
    counter_ = 0;
    ++yields_;
    YieldNow();
}

void WorkPacer::YieldNow() noexcept {
    // SwitchToThread yields only to a thread ready on this core and returns immediately
    // when there is none, which is the cheap common case. Falling back to a one
    // millisecond sleep occasionally guarantees a thread on another core, such as the
    // asset loader, also gets a chance.
    if (::SwitchToThread() == FALSE) {
        ::Sleep(1);
    }
}

void ProcessLoad::Reset() noexcept {
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);
    cores_ = info.dwNumberOfProcessors > 0 ? info.dwNumberOfProcessors : 1;

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user) != FALSE) {
        last_cpu_100ns_ = ToTicks(kernel) + ToTicks(user);
    } else {
        last_cpu_100ns_ = 0;
    }
    last_sample_ = std::chrono::steady_clock::now();
}

double ProcessLoad::Sample() noexcept {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user) == FALSE) {
        return -1.0;
    }

    const std::uint64_t cpu = ToTicks(kernel) + ToTicks(user);
    const auto          now = std::chrono::steady_clock::now();

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(now - last_sample_).count();
    if (elapsed < 100000) {
        return -1.0; // Under 100 ms is too short to mean anything.
    }

    // CPU time is in 100 ns units; elapsed is in microseconds. Ten 100 ns units per
    // microsecond, divided across the cores available.
    const double cpu_delta_us = static_cast<double>(cpu - last_cpu_100ns_) / 10.0;
    const double utilization =
        cpu_delta_us / (static_cast<double>(elapsed) * static_cast<double>(cores_));

    last_cpu_100ns_ = cpu;
    last_sample_    = now;
    return utilization;
}

bool WaitFor(std::string_view description, const std::function<bool()>& predicate,
             const std::function<bool()>& should_abort,
             std::chrono::milliseconds initial_interval,
             std::chrono::milliseconds max_interval) {
    if (!predicate) {
        return false;
    }

    const auto started  = std::chrono::steady_clock::now();
    auto       interval = initial_interval;

    // Progress logging backs off too, so a ten minute wait produces a handful of lines
    // rather than hundreds.
    auto next_report        = std::chrono::seconds(15);
    auto report_step        = std::chrono::seconds(15);

    for (;;) {
        if (predicate()) {
            const auto waited = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - started);
            if (waited.count() > 1) {
                FE_LOG_INFO("{}: satisfied after {} s", description, waited.count());
            }
            return true;
        }
        if (should_abort && should_abort()) {
            FE_LOG_WARN("{}: abandoned, the host process is going away", description);
            return false;
        }

        std::this_thread::sleep_for(interval);
        interval = std::min(max_interval, interval + interval / 2);

        const auto waited = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started);
        if (waited >= next_report) {
            FE_LOG_INFO("{}: still waiting after {} s", description, waited.count());
            report_step = std::min(std::chrono::seconds(120), report_step * 2);
            next_report = waited + report_step;
        }
    }
}

bool WaitForQuiet(std::string_view description, const std::function<bool()>& should_abort,
                  double busy_threshold, int consecutive_samples,
                  std::chrono::milliseconds sample_interval) {
    ProcessLoad load;
    int         quiet_run = 0;

    // Discard the first sample: it covers the period before this call and says nothing
    // about the present.
    std::this_thread::sleep_for(sample_interval);
    (void)load.Sample();

    return WaitFor(
        description,
        [&]() {
            const double utilization = load.Sample();
            if (utilization < 0.0) {
                return false; // Sample window too short; try again.
            }
            if (utilization <= busy_threshold) {
                ++quiet_run;
            } else {
                if (quiet_run > 0) {
                    FE_LOG_DEBUG("{}: busy again at {:.0f}% utilization", description,
                                 utilization * 100.0);
                }
                quiet_run = 0;
            }
            return quiet_run >= consecutive_samples;
        },
        should_abort, sample_interval, sample_interval);
}

} // namespace fe::pacing


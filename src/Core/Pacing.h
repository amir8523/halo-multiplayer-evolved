// SPDX-License-Identifier: MIT
// ForgeEvolved: Core/Pacing.h
//
// Adaptive waiting and cooperative scanning.
//
// WHY THIS EXISTS
//
// Every timing constant in this project was originally tuned on one machine. On the
// development PC the game takes roughly 30 to 50 seconds to get from its driver warning
// to the main menu. On a slower machine, or one loading from a hard disk, or one with
// fewer cores, it takes considerably longer. Fixed deadlines are wrong everywhere except
// the machine they were measured on:
//
//   a 120 second wait for the simulation module fails on a slow disk
//   a 300 second wait for the game window fails on a very slow disk
//   "the object count did not change for 8 seconds" is not the same as "loading finished"
//     on a machine where loading stalls for longer than that
//
// Worse than any of those, the memory scanning competes with the game's asset loader for
// the process address space lock. On a fast machine that is survivable. On a slow one it
// is exactly the wrong thing to do at exactly the wrong moment, and it is the most likely
// way this mod breaks somebody else's game.
//
// Two mechanisms replace the constants:
//
//   WaitFor      Waits on a condition rather than a clock. It gives up only when the
//                host process is exiting, so a machine that needs ten minutes gets ten
//                minutes. Poll intervals back off so a long wait costs almost nothing.
//
//   WorkPacer    Yields the CPU periodically during a scan. The loader can always take
//                the address space lock, no matter how long the scan runs, because the
//                scan stops asking for it at regular intervals.
//
//   ProcessLoad  Measures how busy this process actually is, so "the game has finished
//                loading" becomes an observation instead of an assumption.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace fe::pacing {

/// Cooperative yielding for long scans.
///
/// Call Tick once per unit of work. Every so often it yields the remainder of the
/// thread's time slice, which lets any thread waiting on the address space lock,
/// including the game's asset loader, make progress.
///
/// The cost is negligible: a counter increment per unit of work, and a yield roughly
/// once per few thousand.
class WorkPacer {
public:
    /// work_between_yields is tuned so a yield happens often enough that no other thread
    /// waits long, and rarely enough that the scan is not dominated by yielding.
    explicit WorkPacer(std::size_t work_between_yields = 2048) noexcept
        : interval_(work_between_yields == 0 ? 1 : work_between_yields) {}

    /// Records one unit of work, yielding when the interval elapses.
    void Tick() noexcept;

    /// Yields immediately, regardless of the counter. For use between phases.
    static void YieldNow() noexcept;  // Not Yield: windows.h defines that as an empty macro.

    [[nodiscard]] std::size_t YieldCount() const noexcept { return yields_; }

private:
    std::size_t interval_;
    std::size_t counter_{0};
    std::size_t yields_{0};
};

/// Utilization of this process, sampled over an interval.
class ProcessLoad {
public:
    ProcessLoad() noexcept { Reset(); }

    /// Starts a new measurement window.
    void Reset() noexcept;

    /// Fraction of one core's worth of CPU time consumed since the last call, divided by
    /// the number of cores. Roughly 1.0 means every core is saturated.
    ///
    /// Returns a negative value when the sample window is too short to be meaningful.
    [[nodiscard]] double Sample() noexcept;

private:
    std::uint64_t                         last_cpu_100ns_{0};
    std::chrono::steady_clock::time_point last_sample_{};
    std::uint32_t                         cores_{1};
};

/// Waits until predicate returns true.
///
/// This is the replacement for every fixed deadline. It returns false only when
/// should_abort says the wait is pointless, which in practice means the host process is
/// shutting down. A machine that needs far longer than the developer's simply takes
/// longer, rather than silently losing a feature.
///
/// The poll interval starts short and backs off to a ceiling, so a wait that lasts
/// minutes costs a handful of wakeups rather than thousands.
///
/// description appears in the progress log, which is emitted at a decreasing rate so a
/// long wait leaves a readable trail instead of flooding the file.
[[nodiscard]] bool WaitFor(std::string_view description,
                           const std::function<bool()>& predicate,
                           const std::function<bool()>& should_abort,
                           std::chrono::milliseconds initial_interval = std::chrono::milliseconds(250),
                           std::chrono::milliseconds max_interval = std::chrono::seconds(3));

/// Waits until this process has been quiet for consecutive_samples in a row.
///
/// Quiet means utilization below busy_threshold. This is how "the game finished loading"
/// is determined without guessing a duration: a machine still streaming assets is busy,
/// and one sitting on a menu is not, whatever their relative speed.
[[nodiscard]] bool WaitForQuiet(std::string_view description,
                               const std::function<bool()>& should_abort,
                               double busy_threshold = 0.25,
                               int consecutive_samples = 3,
                               std::chrono::milliseconds sample_interval = std::chrono::seconds(2));

} // namespace fe::pacing


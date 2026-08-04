// SPDX-License-Identifier: MIT
// ForgeEvolved: Debug/AccessTrap.h
//
// Catches the code that reads or writes a specific byte of game memory.
//
// WHY THIS IS THE DECISIVE TOOL
//
// Several copies of bFriendlyFireEnabled exist and they disagree. Writing them all is
// easy; knowing which one the game actually consults is the real question, and every
// approach tried so far can only answer it indirectly:
//
//   Static cross referencing finds instructions that touch a fixed address. These fields
//   live inside heap objects whose addresses change every run, so there is no fixed
//   address to search for.
//
//   Polling the value shows that it changed, but never who changed it, and never that
//   something merely read it.
//
// A hardware breakpoint answers it directly. The CPU's debug registers trap on access to
// an address regardless of which instruction does it, and the trap reports the exact
// instruction pointer. One trigger identifies the consumer precisely.
//
// HOW IT WORKS
//
//   DR0 through DR3 hold up to four watched addresses. DR7 enables them and selects the
//   condition, here read or write of one byte. A hit raises EXCEPTION_SINGLE_STEP, which
//   a vectored exception handler catches, logs, and continues from.
//
// Debug registers are per thread, so the trap has to be installed on every thread the
// game has, and re-armed for threads created later.
//
// SAFETY
//
// The handler only reads context and logs. It never modifies execution: it clears the
// status bits and returns EXCEPTION_CONTINUE_EXECUTION, so the interrupted instruction
// completes normally. Disarming restores DR7 to zero on every thread.
//
// This is an observation tool. It is armed on request and never on startup, because
// suspending every thread in the game to write debug registers is not something to do
// unasked.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Core/Result.h"

namespace fe::debugtrap {

/// What kind of access to trap.
///
/// The values are the encoding the debug registers themselves use, so they are written
/// straight into DR7 rather than translated.
enum class Condition : std::uint8_t {
    Execute   = 0b00, ///< Instruction fetch. Length must be one byte.
    Write     = 0b01, ///< Data writes only.
    ReadWrite = 0b11, ///< Reads and writes. Cannot trap execution.
};

/// One recorded access.
struct Hit {
    std::uintptr_t instruction{0};  ///< RIP at the moment of the access.
    std::uintptr_t address{0};      ///< The watched address that was touched.
    std::uint32_t  thread_id{0};
    std::uint64_t  count{0};        ///< How many times this instruction has hit.
    std::string    module_name;     ///< Module containing the instruction.
    std::uintptr_t module_offset{0}; ///< Offset within that module, stable across runs.
};

/// Installs the exception handler. Safe to call repeatedly.
[[nodiscard]] Result Initialize();

/// Removes the handler and disarms every slot.
void Shutdown();

/// Watches one address on every thread in the process.
///
/// Up to four addresses can be watched at once, which is a hardware limit rather than an
/// implementation choice. length must be 1, 2, 4 or 8, and the address must be aligned to
/// it for lengths above 1.
[[nodiscard]] Result Arm(std::uintptr_t address, std::size_t length, Condition condition,
                        std::string label);

/// Disarms every watched address.
void DisarmAll();

/// Re-applies the current watch set to threads created since the last call.
///
/// The game spawns threads during play, and a thread created after arming has clean debug
/// registers. Without a periodic refresh the trap would silently miss any access made by
/// those threads, which is exactly the sort of gap that produces a confident wrong answer.
void RefreshThreads();

/// Everything recorded so far, most frequent first.
[[nodiscard]] std::vector<Hit> Hits();

/// Clears the recorded hits, keeping the watches armed.
void ClearHits();

/// Called from inside the exception handler, on whichever game thread was interrupted.
///
/// This is the only way to get code running on the game thread without patching any of the
/// game's own memory: arm an instruction breakpoint on something the game thread calls, and
/// the handler is then executing in that thread's context.
///
/// It runs in an exception handler, so it must be brief and must not take a lock any other
/// thread might hold. It is a plain function pointer rather than a std::function because
/// the latter can allocate.
/// Returns true to stay armed, false to stop trapping on this thread immediately.
///
/// Returning false matters for an instruction breakpoint on a hot function. Waiting for the
/// global disarm to walk every thread leaves the rest of them raising an exception per call
/// in the meantime, which is enough to stall the game visibly. Clearing the registers in
/// the interrupted thread's own context stops that thread instantly, so the storm drains as
/// each thread arrives rather than all at once at the end.
using HitCallback = bool (*)(std::uintptr_t address);

/// Installs the callback. Passing nullptr removes it.
void SetHitCallback(HitCallback callback);

/// True when the handler hit its recording budget and stopped doing work.
///
/// An execution breakpoint on a hot function can fire faster than anything can record.
/// The tick loop polls this and disarms, because disarming suspends every thread and so
/// cannot be done from inside the handler itself.
[[nodiscard]] bool OverBudget();

/// Number of addresses currently armed.
[[nodiscard]] std::size_t ArmedCount();

} // namespace fe::debugtrap


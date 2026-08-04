// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Debug/AccessTrap.cpp
#define MPE_LOG_CATEGORY "Debug.Trap"

#include "Debug/AccessTrap.h"

#include "Core/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <mutex>
#include <unordered_map>

namespace mpe::debugtrap {
namespace {

/// Hardware supports exactly four watchpoints.
constexpr std::size_t kSlotCount = 4;

struct Slot {
    std::uintptr_t address{0};
    std::size_t    length{1};
    Condition      condition{Condition::ReadWrite};
    std::string    label;
    bool           active{false};
};

/// How many hits may be recorded before the handler stops doing work.
///
/// Enough to characterise anything worth measuring, far below what it costs to keep up
/// with a hot function.
constexpr std::uint64_t kHitBudget = 20000;

/// EFlags.RF. Suppresses an instruction breakpoint for exactly one instruction.
constexpr std::uint32_t kResumeFlag = 0x00010000;

std::mutex                              g_mutex;
std::array<Slot, kSlotCount>            g_slots;
std::unordered_map<std::uintptr_t, Hit> g_hits; ///< Keyed by instruction pointer.
PVOID                                   g_handler = nullptr;
bool                                    g_initialized = false;
std::atomic<std::uint64_t>              g_hits_recorded{0};
std::atomic<bool>                       g_over_budget{false};
std::atomic<HitCallback>                g_hit_callback{nullptr};

/// Encodes the length field the way DR7 expects it.
///
/// The encoding is deliberately not sequential in the hardware: 1 byte is 00, 2 is 01,
/// 8 is 10 and 4 is 11. Getting this wrong silently watches the wrong span.
[[nodiscard]] std::uint64_t EncodeLength(std::size_t length) noexcept {
    switch (length) {
        case 1: return 0b00;
        case 2: return 0b01;
        case 8: return 0b10;
        case 4: return 0b11;
        default: return 0b00;
    }
}

/// Builds DR7 from the current slot table.
[[nodiscard]] std::uint64_t BuildDr7() noexcept {
    std::uint64_t dr7 = 0;
    for (std::size_t i = 0; i < kSlotCount; ++i) {
        if (!g_slots[i].active) {
            continue;
        }
        // Local enable bit for this slot: bits 0, 2, 4, 6.
        dr7 |= (1ull << (i * 2));

        // Condition and length live in the upper half, four bits per slot starting at 16.
        const std::uint64_t rw  = static_cast<std::uint64_t>(g_slots[i].condition);
        const std::uint64_t len = EncodeLength(g_slots[i].length);
        dr7 |= (rw << (16 + i * 4));
        dr7 |= (len << (18 + i * 4));
    }
    return dr7;
}

/// Applies the slot table to one thread.
[[nodiscard]] bool ApplyToThread(DWORD thread_id) {
    if (thread_id == ::GetCurrentThreadId()) {
        // Arming our own thread would trap on our own bookkeeping.
        return true;
    }

    const HANDLE thread = ::OpenThread(
        THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, thread_id);
    if (thread == nullptr) {
        return false;
    }

    bool ok = false;
    // The context can only be set reliably on a suspended thread.
    if (::SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;

        if (::GetThreadContext(thread, &context) != FALSE) {
            context.Dr0 = g_slots[0].active ? g_slots[0].address : 0;
            context.Dr1 = g_slots[1].active ? g_slots[1].address : 0;
            context.Dr2 = g_slots[2].active ? g_slots[2].address : 0;
            context.Dr3 = g_slots[3].active ? g_slots[3].address : 0;
            context.Dr6 = 0;
            context.Dr7 = BuildDr7();

            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            ok = (::SetThreadContext(thread, &context) != FALSE);
        }
        ::ResumeThread(thread);
    }
    ::CloseHandle(thread);
    return ok;
}

/// Applies the slot table to every thread in this process.
[[nodiscard]] std::size_t ApplyToAllThreads() {
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    const DWORD  self = ::GetCurrentProcessId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    std::size_t applied = 0;

    if (::Thread32First(snapshot, &entry) != FALSE) {
        do {
            if (entry.th32OwnerProcessID == self && ApplyToThread(entry.th32ThreadID)) {
                ++applied;
            }
        } while (::Thread32Next(snapshot, &entry) != FALSE);
    }
    ::CloseHandle(snapshot);
    return applied;
}

/// Names the module containing an address, and the offset within it.
void DescribeAddress(std::uintptr_t address, std::string& out_module, std::uintptr_t& out_offset) {
    HMODULE module = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(address), &module) == FALSE ||
        module == nullptr) {
        out_module = "?";
        out_offset = 0;
        return;
    }

    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(module, path, MAX_PATH);

    const wchar_t* name = ::wcsrchr(path, L'\\');
    name = (name != nullptr) ? name + 1 : path;

    out_module.clear();
    for (const wchar_t* c = name; *c != L'\0'; ++c) {
        out_module.push_back(*c < 128 ? static_cast<char>(*c) : '?');
    }
    out_offset = address - reinterpret_cast<std::uintptr_t>(module);
}

/// Vectored handler. Runs on whichever game thread touched the address.
LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
    if (info == nullptr || info->ExceptionRecord == nullptr ||
        info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // DR6's low four bits say which watchpoint fired. Anything else is a single step
    // that belongs to someone else, such as a debugger, and must be passed along.
    const std::uint64_t status = info->ContextRecord->Dr6;
    const std::uint64_t fired  = status & 0xFull;
    if (fired == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const std::uintptr_t rip = static_cast<std::uintptr_t>(info->ContextRecord->Rip);

    // Run the callback before the budget check.
    //
    // The budget exists to stop recording from costing more than the process can afford,
    // but the callback is the entire point of an instruction breakpoint used as a game
    // thread entry point, and it decides for itself whether it has work to do.
    if (const HitCallback callback = g_hit_callback.load(std::memory_order_acquire);
        callback != nullptr) {
        if (!callback(rip)) {
            // This thread has nothing further to do. Clearing its debug registers here
            // stops it trapping straight away instead of once the global disarm reaches it.
            info->ContextRecord->Dr7 = 0;
            info->ContextRecord->Dr6 = 0;
            info->ContextRecord->EFlags |= kResumeFlag;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    // A budget, not a nicety.
    //
    // An execution breakpoint on a hot function fires thousands of times a second on every
    // thread at once. Doing real work here then costs more than the process can afford and
    // takes the game down, which is exactly what happened when this was armed on the name
    // constructor. Past the budget the handler does the minimum the CPU requires and
    // nothing else, and the tick loop disarms it.
    if (g_hits_recorded.fetch_add(1, std::memory_order_relaxed) >= kHitBudget) {
        g_over_budget.store(true, std::memory_order_release);
        info->ContextRecord->Dr6 = 0;
        info->ContextRecord->EFlags |= kResumeFlag;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    {
        // The handler runs on a game thread, so it must be brief and must not block for
        // long. Recording into a map under a short lock is acceptable; naming the module
        // from here is not, because that takes the loader lock on a thread that may already
        // hold something the loader wants. Module names are resolved later, in Hits().
        std::lock_guard lock(g_mutex);
        for (std::size_t i = 0; i < kSlotCount; ++i) {
            if ((fired & (1ull << i)) == 0 || !g_slots[i].active) {
                continue;
            }
            Hit& hit = g_hits[rip];
            if (hit.count == 0) {
                hit.instruction = rip;
                hit.address     = g_slots[i].address;
                hit.thread_id   = ::GetCurrentThreadId();
            }
            ++hit.count;
        }
    }

    // Clearing the status bits is required, otherwise the same condition reports again.
    info->ContextRecord->Dr6 = 0;

    // The resume flag is what makes execution breakpoints survivable.
    //
    // A data watchpoint reports after the access has happened, so resuming simply carries
    // on. An execution breakpoint reports before the instruction runs, so resuming without
    // this re-executes the same instruction, trips the same breakpoint, and repeats
    // forever. The processor clears the flag itself after one instruction, so it suppresses
    // exactly the re-trigger and nothing else.
    //
    // Leaving it out raised STATUS_SINGLE_STEP endlessly until the exception reached the
    // game's own handler and took the process down.
    info->ContextRecord->EFlags |= kResumeFlag;
    return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace

Result Initialize() {
    std::lock_guard lock(g_mutex);
    if (g_initialized) {
        return Result::Success();
    }

    // First in the chain: this handler must see the trap before anything else decides to
    // swallow it.
    g_handler = ::AddVectoredExceptionHandler(1, &OnException);
    if (g_handler == nullptr) {
        return Result::Fail(ErrorCode::InvalidState,
                            "AddVectoredExceptionHandler failed; access tracing is unavailable");
    }
    g_initialized = true;
    return Result::Success();
}

void Shutdown() {
    DisarmAll();

    std::lock_guard lock(g_mutex);
    if (g_handler != nullptr) {
        ::RemoveVectoredExceptionHandler(g_handler);
        g_handler = nullptr;
    }
    g_initialized = false;
}

Result Arm(std::uintptr_t address, std::size_t length, Condition condition, std::string label) {
    if (address == 0) {
        return Result::Fail(ErrorCode::InvalidArgument, "cannot watch a null address");
    }
    if (length != 1 && length != 2 && length != 4 && length != 8) {
        return Result::Fail(ErrorCode::InvalidArgument,
                            std::format("watch length {} must be 1, 2, 4 or 8", length));
    }
    if (length > 1 && (address % length) != 0) {
        return Result::Fail(ErrorCode::InvalidArgument,
                            std::format("a {} byte watch needs an address aligned to {}", length,
                                        length));
    }
    if (condition == Condition::Execute && length != 1) {
        // The hardware requires it, and a longer length here is silently ignored rather
        // than rejected, which would leave a breakpoint that never fires.
        return Result::Fail(ErrorCode::InvalidArgument,
                            "an execution breakpoint must have a length of one byte");
    }

    if (const Result ready = Initialize(); !ready.ok()) {
        return ready;
    }

    std::size_t chosen = kSlotCount;
    {
        std::lock_guard lock(g_mutex);
        for (std::size_t i = 0; i < kSlotCount; ++i) {
            if (!g_slots[i].active) {
                chosen = i;
                break;
            }
        }
        if (chosen == kSlotCount) {
            return Result::Fail(ErrorCode::InvalidState,
                                "all four hardware watchpoints are in use");
        }
        g_slots[chosen] = Slot{address, length, condition, std::move(label), true};
    }

    const char* description = "reads and writes";
    if (condition == Condition::Write) {
        description = "writes";
    } else if (condition == Condition::Execute) {
        description = "execution";
    }

    const std::size_t threads = ApplyToAllThreads();
    MPE_LOG_INFO("armed watchpoint {} on 0x{:X} ({} byte{}, {}) across {} thread(s)", chosen,
                address, length, length == 1 ? "" : "s", description, threads);

    if (threads == 0) {
        std::lock_guard lock(g_mutex);
        g_slots[chosen].active = false;
        return Result::Fail(ErrorCode::InvalidState,
                            "no thread accepted the watchpoint; debug registers are probably "
                            "unavailable, which happens under another debugger");
    }
    return Result::Success();
}

void DisarmAll() {
    {
        std::lock_guard lock(g_mutex);
        for (Slot& slot : g_slots) {
            slot.active = false;
        }
    }
    const std::size_t threads = ApplyToAllThreads();
    MPE_LOG_INFO("disarmed all watchpoints across {} thread(s)", threads);
}

void RefreshThreads() {
    {
        std::lock_guard lock(g_mutex);
        const bool any = std::any_of(g_slots.begin(), g_slots.end(),
                                     [](const Slot& slot) { return slot.active; });
        if (!any) {
            return;
        }
    }
    (void)ApplyToAllThreads();
}

std::vector<Hit> Hits() {
    std::lock_guard lock(g_mutex);
    std::vector<Hit> out;
    out.reserve(g_hits.size());
    for (const auto& [rip, hit] : g_hits) {
        out.push_back(hit);
        // Naming the module is done here rather than in the handler: it is a loader call,
        // and the handler runs on arbitrary game threads where taking the loader lock can
        // deadlock.
        if (out.back().module_name.empty()) {
            DescribeAddress(out.back().instruction, out.back().module_name,
                            out.back().module_offset);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Hit& a, const Hit& b) { return a.count > b.count; });
    return out;
}

void ClearHits() {
    std::lock_guard lock(g_mutex);
    g_hits.clear();
}

std::size_t ArmedCount() {
    std::lock_guard lock(g_mutex);
    return static_cast<std::size_t>(
        std::count_if(g_slots.begin(), g_slots.end(), [](const Slot& s) { return s.active; }));
}

} // namespace mpe::debugtrap



namespace mpe::debugtrap {

bool OverBudget() {
    return g_over_budget.load(std::memory_order_acquire);
}

void SetHitCallback(HitCallback callback) {
    g_hit_callback.store(callback, std::memory_order_release);
}

} // namespace mpe::debugtrap

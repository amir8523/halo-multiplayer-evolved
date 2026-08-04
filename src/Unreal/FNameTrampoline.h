// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Unreal/FNameTrampoline.h
//
// Publishes an FName constructor with the calling convention UE4SS expects.
//
// WHY THIS EXISTS
//
// UE4SS refuses to initialise without FName::FName(const wchar_t*, EFindName). It scans
// for it, finds nothing in this build, retries seventy times and then aborts with
// "PS scan timed out". Every other symbol it needs resolves. That one function is the
// whole reason function calling is unavailable, and function calling is the reason the
// session and travel work cannot proceed.
//
// The function does exist. It is simply not shaped the way UE4SS looks for: the real
// construction routine takes a descriptor struct rather than a bare string, so no
// signature written against the standard form can match it.
//
// The fix is to publish a small adapter with the standard shape that forwards to the real
// one. It is written into existing padding inside the executable's .text section, so it
// lives in the module and is reachable by a module scan, which is what lets UE4SS find it.
//
// HOW THE REAL FUNCTION IS CALLED
//
//   RCX  pointer to a descriptor
//   RDX  pointer to the FName being constructed
//
//   descriptor + 0x20   uint64, the string pointer with bit 63 set
//   descriptor + 0x28   int32,  the number part of the name, zero here
//   descriptor + 0x2C   int8,   the find or add behaviour
//
// The adapter takes (FName* out, const wchar_t* name, EFindName find), builds that
// descriptor on its own stack and tail calls through. No game code is patched and nothing
// is hooked: this only fills in unused padding.
//
// SAFETY
//
// The adapter is written into a run of 0xCC padding between functions, so nothing that
// executes is overwritten. It is registered with RtlAddFunctionTable so the stack
// unwinder recognises it as a real function rather than treating a fault inside it as
// corruption. Installation is verified by constructing a name whose value is already
// known and checking the result resolves back to the same string.
#pragma once

#include <cstdint>

#include "Core/Result.h"

namespace mpe::unreal {

/// Where the adapter ended up, for logging and for the self test.
struct TrampolineInfo {
    std::uintptr_t address{0};       ///< Absolute address of the adapter.
    std::uintptr_t module_offset{0}; ///< Its RVA, which is what a signature matches on.
    std::uintptr_t target{0};        ///< The real construction function it forwards to.
    std::size_t    size{0};          ///< Bytes written.
};

/// Writes the adapter into executable padding and registers it for unwinding.
///
/// Safe to call more than once; later calls return the already installed copy rather than
/// writing a second one.
[[nodiscard]] Result InstallFNameTrampoline(TrampolineInfo& out_info);

/// Constructs a name through the adapter and reports the resulting FName index.
///
/// This is the honest check that the descriptor layout is right. Passing a string the
/// pool is guaranteed to already contain and getting back an index that resolves to that
/// same string means the adapter works. Anything else means it does not, and it is far
/// better to learn that here than from a crash inside UE4SS.
[[nodiscard]] Result TestFNameTrampoline(const wchar_t* name, std::uint32_t& out_index);

/// Builds a complete FName, both the comparison index and the number.
///
/// This is what a function parameter needs: an FName is eight bytes, and passing only the
/// index leaves the number as whatever was on the stack.
[[nodiscard]] Result MakeFName(const wchar_t* name, std::uint64_t& out_name);

} // namespace mpe::unreal

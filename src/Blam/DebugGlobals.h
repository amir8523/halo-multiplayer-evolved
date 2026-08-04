// SPDX-License-Identifier: MIT
// ForgeEvolved: Blam/DebugGlobals.h
//
// Read and write access to the engine's debug globals, by name.
//
// WHAT THIS IS
//
// The simulation module carries 1476 writable debug globals in two tables, and 2173
// string ids that are not writable. The distinction is the record stride and it
// matters enormously:
//
//   stride 0x18  { const char* name; u64 type; u64 value; }   writable state
//   stride 0x10  { const char* name; u32 id; u32 group; }     UI label, read only
//
// Every game_engine, slayer, variant, forge, megalo and team name in this binary is
// a stride 0x10 string id. They are localization tokens for a user interface, not
// switches. That is the measured reason competitive multiplayer cannot be turned on
// by writing a value: there is no value to write.
//
// IMPORTANT: A WRITE HERE IS NOT KNOWN TO CHANGE ENGINE BEHAVIOUR
//
// The stride 0x18 records exist and their memory is readable and writable. What is NOT
// established is that the engine consumes them. XrefProbe decoded every RIP relative
// encoding MSVC emits for accessing a global (byte, word, dword and quadword moves in
// both directions, movzx, movsx, cmp against an immediate) and found:
//
//   zero references to any field of any record, including the name pointer
//   zero references to any of the three table bases
//
// Nothing in 7.9 MB of .text touches this data. The most consistent explanation is
// that the descriptors are residual: compiled in with the engine's tag and debug data,
// while the subsystem that reads them is not wired up in this product.
//
// So this class does exactly what its name says, reads and writes those records, and
// nothing more. VerifyWritePath confirms the memory is writable and round trips a
// value. It does not and cannot confirm the engine notices. Do not present this as a
// way to enable cheats or alter gameplay; that claim was made earlier and was wrong.
//
// It is kept because it is the right tool for investigating this further, and because
// if the consuming code is ever located, this is the surface it will use.
//
// TYPE TAGS
//
// The type field at +0x08 classifies the value. Observed on build 2607-CU3:
//
//   5  boolean, value is 0 or 1
//   6  numeric, value is a scalar
//   7  pointer or string, value is an address inside the module
//
// Writes are type checked. A boolean write rejects anything but 0 or 1, and a write
// to a type 7 global is refused outright, because storing an integer where the
// engine expects a pointer is how you get a crash that looks like a game bug.
//
// SAFETY
//
// Every write validates the record first: correct stride, known type, value in
// range. The target page is made writable only for the width of the store and then
// restored. Nothing is written speculatively, and a global that does not resolve is
// an error rather than a silent no-op.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Blam/SymbolRegistry.h"
#include "Core/Result.h"

namespace fe::blam {

/// Type tag values observed in the debug global tables.
enum class GlobalType : std::uint64_t {
    Boolean = 5,
    Numeric = 6,
    Pointer = 7,
};

[[nodiscard]] std::string_view ToString(GlobalType type) noexcept;

/// One global as reported to a caller.
struct GlobalInfo {
    std::string    name;
    std::uintptr_t address{0}; ///< Address of the value field.
    std::uint64_t  type{0};
    std::uint64_t  value{0};
    bool           writable{false}; ///< False for string id records.
};

/// Name addressable access to the engine's debug globals.
///
/// Holds a reference to the registry, which must outlive it.
class DebugGlobals {
public:
    explicit DebugGlobals(const SymbolRegistry& registry) noexcept : registry_(registry) {}

    /// Byte offsets inside a stride 0x18 record, measured on build 2607-CU3.
    static constexpr std::size_t kTypeOffset   = 0x08;
    static constexpr std::size_t kValueOffset  = 0x10;
    static constexpr std::size_t kMinimumStride = 0x18;

    [[nodiscard]] Expected<GlobalInfo> Query(std::string_view name) const;

    /// Reads a boolean global. Fails when the global is not type 5.
    [[nodiscard]] Expected<bool> GetBool(std::string_view name) const;

    /// Writes a boolean global. Fails when the global is not type 5.
    [[nodiscard]] Result SetBool(std::string_view name, bool value);

    /// Reads a numeric global. Accepts type 5 and 6, since a boolean is a valid
    /// numeric read.
    [[nodiscard]] Expected<std::uint64_t> GetNumber(std::string_view name) const;

    /// Writes a numeric global. Refuses type 7, where the field is an address.
    [[nodiscard]] Result SetNumber(std::string_view name, std::uint64_t value);

    /// Every global whose name contains the given substring, case sensitive. An
    /// empty filter returns all of them. Sorted by name so output is stable.
    [[nodiscard]] std::vector<GlobalInfo> List(std::string_view name_contains = {},
                                              std::size_t max_results = 256) const;

    /// Writes a value and reads it back, confirming the store actually landed.
    ///
    /// Used as a startup self test: it is the difference between believing the
    /// record layout is right and knowing it. The original value is always restored.
    [[nodiscard]] Result VerifyWritePath(std::string_view name);

private:
    /// Resolves a record and validates it is a writable global.
    [[nodiscard]] Expected<const SymbolRecord*> ResolveWritable(std::string_view name) const;

    /// Performs the store, adjusting page protection for its duration.
    [[nodiscard]] static Result StoreValue(std::uintptr_t address, std::uint64_t value);

    const SymbolRegistry& registry_;
};

} // namespace fe::blam

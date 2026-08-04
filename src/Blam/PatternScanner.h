// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Blam/PatternScanner.h
//
// Primitives for locating data inside the shipped Blam module at runtime.
//
// Design rule: this project never hardcodes an absolute address or a file
// offset. Game updates shift every address, and a hardcoded offset turns a
// working mod into a crash on patch day. Instead we anchor on content that is
// stable across builds because the engine itself depends on it, namely the
// debug command and global name strings (for example
// "net_load_and_use_map_variant", "console_command", "forge_object_properties"),
// then walk outward to the structures that reference them.
//
// All scans are bounded to a Section and are read only. No page protection is
// changed here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "Blam/ModuleImage.h"
#include "Core/Result.h"

namespace mpe::blam {

/// A byte pattern with wildcards, parsed from IDA style text: "48 8B ?? 48 89".
class BytePattern {
public:
    /// Accepts space separated hex byte pairs and "?" or "??" wildcards.
    /// Returns InvalidArgument on any malformed token or an empty pattern.
    [[nodiscard]] static Expected<BytePattern> Parse(std::string_view text);

    [[nodiscard]] std::size_t Size() const noexcept { return bytes_.size(); }
    [[nodiscard]] bool MatchesAt(const std::byte* candidate) const noexcept;

private:
    BytePattern() = default;

    std::vector<std::uint8_t> bytes_;   ///< Value at each position.
    std::vector<bool>         wildcard_; ///< True where any byte is accepted.
};

/// Bounded, read only searches over a module.
class PatternScanner {
public:
    /// Hard cap on results for every search, so a degenerate pattern cannot
    /// allocate without limit.
    static constexpr std::size_t kDefaultMaxResults = 256;

    explicit PatternScanner(const ModuleImage& image) noexcept : image_(image) {}

    /// Byte pattern search within one section.
    [[nodiscard]] std::vector<std::uintptr_t> FindPattern(
        const Section& section, const BytePattern& pattern,
        std::size_t max_results = kDefaultMaxResults) const;

    /// Locates NUL terminated occurrences of an exact literal in any string
    /// bearing section.
    ///
    /// Matches require a NUL immediately after the text and, unless the match
    /// starts at the section base, a NUL immediately before it. That rejects
    /// substring hits: without the leading check, searching for "slayer" would
    /// also match inside "slayer leader traits".
    [[nodiscard]] std::vector<std::uintptr_t> FindStringLiteral(
        std::string_view text, std::size_t max_results = kDefaultMaxResults) const;

    /// Finds locations in a section whose 8 byte value equals target. This is
    /// how a table record that stores a name pointer is located from the name.
    [[nodiscard]] std::vector<std::uintptr_t> FindPointersTo(
        const Section& section, std::uintptr_t target,
        std::size_t max_results = kDefaultMaxResults) const;

    /// Finds x86-64 RIP relative references to target inside a code section,
    /// covering the common `lea reg, [rip+disp32]` and `mov reg, [rip+disp32]`
    /// forms. Used to locate the function that consumes a known string.
    ///
    /// The scan tests every instruction offset rather than decoding, so results
    /// may include coincidental matches. Callers must treat results as
    /// candidates and validate them.
    [[nodiscard]] std::vector<std::uintptr_t> FindRipRelativeReferences(
        const Section& code, std::uintptr_t target,
        std::size_t max_results = kDefaultMaxResults) const;

    /// Heuristic used when validating candidate table records: does this address
    /// point at a plausible engine identifier, meaning a non empty NUL
    /// terminated ASCII string of lowercase letters, digits, underscores or
    /// dots, within a length that identifiers actually occupy.
    [[nodiscard]] bool IsPlausibleIdentifier(std::uintptr_t address,
                                             std::size_t min_length = 3,
                                             std::size_t max_length = 96) const noexcept;

    [[nodiscard]] const ModuleImage& Image() const noexcept { return image_; }

private:
    const ModuleImage& image_;
};

} // namespace mpe::blam

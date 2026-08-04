// SPDX-License-Identifier: MIT
// ForgeEvolved: Map/MapVariantParser.h
//
// The in game map parser: JSON in, validated MapVariant out.
//
// TRUST BOUNDARY
//
// A map arrives from one of two places, and neither is trusted:
//
//   1. The local filesystem, written by Forge Studio or by hand. It may be
//      malformed, half saved, or authored against a newer schema.
//   2. A remote host over the network. It may be deliberately hostile.
//
// So parsing is total. Every field is range checked, every reference is resolved
// before anything is placed, and a failure produces a diagnostic naming the exact
// JSON path rather than a generic error. Nothing reaches the engine until the
// whole document has validated, which is what makes a bad map a clear message
// instead of a crash mid load.
//
// VALIDATION IS SEMANTIC, NOT ONLY STRUCTURAL
//
// A syntactically perfect map can still be unplayable: CTF with one flag stand,
// or a team with no spawns. Those are caught here, at author time, rather than
// discovered by eight people who just sat through a loading screen.
#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Hash.h"
#include "Core/Result.h"
#include "Map/MapVariant.h"

namespace fe::map {

enum class Severity : std::uint8_t {
    /// The map cannot be used. Parsing fails.
    Error = 0,
    /// The map is usable but something is probably not what the author intended.
    Warning,
};

[[nodiscard]] std::string_view ToString(Severity severity) noexcept;

/// One validation finding, addressed to the author.
struct Diagnostic {
    Severity    severity{Severity::Error};
    std::string json_path; ///< For example "objects[17].palette_key".
    std::string message;   ///< What is wrong and what to do about it.
};

struct ParseOptions {
    /// Promotes every warning to an error. Forge Studio uses this for a publish
    /// check; the game does not, so a map with cosmetic warnings still loads.
    bool treat_warnings_as_errors{false};

    /// Validates mode specific requirements (flag stands for CTF, spawns per
    /// team, and so on). Left on everywhere except unit tests that exercise
    /// individual fields.
    bool validate_mode_requirements{true};

    /// Ceiling on the JSON document itself, before parsing. A map that
    /// serializes within the object budget cannot legitimately exceed this, and
    /// the check stops a huge document from being parsed at all.
    std::size_t max_document_bytes{4u * 1024u * 1024u};
};

struct ParseResult {
    MapVariant              variant;
    std::vector<Diagnostic> diagnostics; ///< Warnings only on success.

    [[nodiscard]] bool HasWarnings() const noexcept { return !diagnostics.empty(); }
};

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

/// Parses and validates a .fmap.json document from disk.
///
/// On failure the error message is a multi line summary of every diagnostic, so
/// one attempt tells the author everything that is wrong rather than making them
/// fix errors one at a time.
[[nodiscard]] Expected<ParseResult> ParseJsonFile(const std::filesystem::path& path,
                                                 const ParseOptions& options = {});

/// Same, from a string already in memory.
[[nodiscard]] Expected<ParseResult> ParseJsonText(std::string_view json_text,
                                                 const ParseOptions& options = {});

/// Serializes a variant to JSON. Keys are written in a fixed order and floats
/// with a round trip exact representation, so writing a parsed document
/// reproduces it and a diff between two revisions shows only real changes.
[[nodiscard]] std::string WriteJson(const MapVariant& variant, bool pretty = true);

// ---------------------------------------------------------------------------
// Canonical binary
// ---------------------------------------------------------------------------

/// Magic and version prefixing every canonical payload.
inline constexpr std::uint32_t kCanonicalMagic   = 0x50414D46u; // "FMAP"
inline constexpr std::uint16_t kCanonicalVersion = 1;

/// Produces the deterministic byte representation.
///
/// Determinism is achieved by writing fields in a fixed order, sorting every
/// collection by id, and emitting floats as their IEEE-754 bit patterns. Two
/// machines that parsed the same logical map always produce identical bytes, so
/// the hash is a reliable identity.
[[nodiscard]] std::vector<std::byte> WriteCanonicalBinary(const MapVariant& variant);

/// Reads a canonical payload back, applying the same validation as the JSON path.
/// This is what a client runs on a map received from a host.
[[nodiscard]] Expected<MapVariant> ReadCanonicalBinary(std::span<const std::byte> payload);

/// SHA-256 over the canonical binary. The identity used in the lobby manifest,
/// in LaunchNow, and on disk.
[[nodiscard]] hash::Digest256 ComputeContentHash(const MapVariant& variant);

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

/// Validates an already constructed variant. Exposed separately so Forge Studio
/// can validate continuously while editing, using exactly the code the game runs.
///
/// Returns every finding, errors first. An empty result means the map is valid.
[[nodiscard]] std::vector<Diagnostic> Validate(const MapVariant& variant,
                                               const ParseOptions& options = {});

} // namespace fe::map

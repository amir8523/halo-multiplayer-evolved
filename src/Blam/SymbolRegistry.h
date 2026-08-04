// SPDX-License-Identifier: MIT
// ForgeEvolved: Blam/SymbolRegistry.h
//
// Runtime discovery of the Blam name tables.
//
// WHY THIS EXISTS
//
// The shipped simulation module exports exactly one symbol
// (CreateBlamEngineShell), so every other entry point has to be located by
// inspection. The engine retains its full debug name table in the shipping build.
// Verified present and located in build 2026.07.25 CU3:
//
//   net_load_and_use_map_variant        0x1807FA7F8
//   read_map_variant_and_make_current   0x1807FA8B0
//   net_simulation_set_stream_bandwidth 0x1807FAF00
//   network_session_class_system_link   0x180827888
//   net_speculative_host_migration_disable 0x1807EADE8
//
// Each is a NUL terminated literal referenced by a record in a table. Discovering
// those records gives us a name addressable handle on the engine's own command and
// global surface, which is far more stable across game patches than any byte
// pattern over code: the names are content the engine itself depends on, while code
// bytes shift on every recompile.
//
// SEVERAL TABLES, NOT ONE
//
// The first implementation assumed a single table and was wrong. The module holds
// many name tables of different shapes, and a probe run against the real binary
// found a 2173 record string enum table (stride 0x10, {const char*, uint64}, names
// like walk_front and hint_vault_step) that contained the network_session_class_*
// values but none of the net_* commands.
//
// So discovery resolves each symbol independently: find its string, find the
// pointers to it, and validate that a pointer sits inside a plausible record array.
// Every distinct table encountered is then indexed in full, so symbols nobody asked
// for are still available. This is both more robust and more honest about how the
// binary is actually laid out.
//
// FAIL CLOSED
//
// Discovery either resolves every required symbol or reports a failure and the mod
// stays inert. There is no partial activation: a half resolved registry that
// guesses at a missing entry is how mods corrupt save data and crash on patch day.
// When discovery fails, BuildDiscoveryReport emits everything a contributor needs
// to re-derive the layout for the new build, and the result goes to the log.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Blam/ModuleImage.h"
#include "Blam/PatternScanner.h"
#include "Core/Result.h"

namespace fe::blam {

/// Per build configuration, loaded from data/symbols/<build>.json.
///
/// Everything that could change between game versions lives here as data, so
/// supporting a new patch is a JSON edit and not a code change.
struct SymbolRegistryConfig {
    /// Informational. Compared against the running module for a log warning only,
    /// never used to block startup, since a validated table layout is what actually
    /// matters.
    std::string game_build;

    /// Module to inspect, for example L"HaloSimulation_tag_release.dll".
    std::wstring module_name{L"HaloSimulation_tag_release.dll"};

    /// Extra names to resolve beyond required_symbols and optional_symbols. Kept
    /// for descriptors that want a table indexed without making it a hard
    /// requirement.
    std::vector<std::string> anchors;

    /// Names that must resolve for the mod to activate. If any is missing the
    /// registry fails with SymbolValidationFailed.
    std::vector<std::string> required_symbols;

    /// Names that are looked up and reported but never block activation. Used for
    /// symbols that are nice to have, or whose presence varies between builds.
    std::vector<std::string> optional_symbols;

    /// Byte offset of the name pointer within a table record.
    std::size_t name_offset{0};

    /// Record strides to try, in order. Blam name records are pointer aligned
    /// aggregates, so plausible strides are small multiples of 8. Verified shapes
    /// so far: 0x10 for the string enum tables.
    std::vector<std::size_t> stride_candidates{0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40};

    /// How many consecutive valid records are required before a candidate array is
    /// accepted as a table. Lower values find more tables but risk a false
    /// positive; four is enough to reject coincidence while still recognizing a
    /// short command array.
    std::size_t min_consecutive_records{4};

    /// Guard rails so a malformed layout cannot spin or allocate without bound.
    std::size_t max_table_records{262144};
    std::size_t max_anchor_matches{32};
    std::size_t max_pointer_matches{256};

    /// Parses the JSON descriptor. Missing optional fields keep their defaults; a
    /// malformed file is a hard failure.
    [[nodiscard]] static Expected<SymbolRegistryConfig> LoadFromFile(
        const std::filesystem::path& path);

    /// Built in configuration used when no descriptor file is present.
    [[nodiscard]] static SymbolRegistryConfig Default();

    /// Every name discovery should attempt, in priority order.
    [[nodiscard]] std::vector<std::string> AllRequestedNames() const;
};

/// Geometry of one discovered table.
struct DebugTableLayout {
    std::uintptr_t begin{0};
    std::uintptr_t end{0}; ///< Exclusive.
    std::size_t    stride{0};
    std::size_t    name_offset{0};
    std::size_t    record_count{0};
    std::string    section_name;

    /// The symbol whose string led us to this table.
    std::string discovered_via;

    [[nodiscard]] bool operator==(const DebugTableLayout& other) const noexcept {
        return begin == other.begin && stride == other.stride;
    }
};

/// One resolved table record.
struct SymbolRecord {
    std::string    name;
    std::uintptr_t record_address{0}; ///< Base of the record.
    std::size_t    stride{0};         ///< Stride of the table it belongs to.
    std::uintptr_t table_begin{0};    ///< Which table, for the report.
};

class SymbolRegistry {
public:
    /// Runs discovery and validation. On success every required symbol resolved.
    [[nodiscard]] static Expected<SymbolRegistry> Discover(const ModuleImage& image,
                                                           const SymbolRegistryConfig& config);

    [[nodiscard]] const SymbolRecord* Find(std::string_view name) const noexcept;
    [[nodiscard]] bool Has(std::string_view name) const noexcept {
        return Find(name) != nullptr;
    }

    /// Reads a pointer sized field at a fixed offset inside a record, verifying that
    /// the result lands inside the module. Used to reach a command's handler or a
    /// global's storage once the record layout is known.
    [[nodiscard]] Expected<std::uintptr_t> ReadRecordPointer(std::string_view name,
                                                              std::size_t field_offset) const;

    [[nodiscard]] const std::vector<DebugTableLayout>& Tables() const noexcept {
        return tables_;
    }
    [[nodiscard]] std::size_t Count() const noexcept { return records_.size(); }

    /// Every indexed record. Exposed so tooling can enumerate the complete
    /// vocabulary of what the engine exposes, which is how the controllable surface
    /// was mapped rather than guessed at.
    [[nodiscard]] const std::vector<SymbolRecord>& Records() const noexcept {
        return records_;
    }

    /// Reads the inline value of a debug global.
    ///
    /// Tables 1 and 2 store {const char* name, u64 type, u64 value} at stride 0x18,
    /// with the value inline at +0x10 rather than behind a pointer. Verified: the
    /// field is zero in the file image and carries no base relocation, which a
    /// pointer to storage would.
    ///
    /// Fails for a record whose stride cannot hold the field, which is how a string
    /// id record (stride 0x10) is rejected rather than misread.
    [[nodiscard]] Expected<std::uint64_t> ReadGlobalValue(std::string_view name) const;

    /// Type tag of a debug global, the field at +0x08. Observed as 5 for booleans.
    [[nodiscard]] Expected<std::uint64_t> ReadGlobalType(std::string_view name) const;

    /// Multi line diagnostic dump: every table found, and a hex view of the record
    /// for each requested symbol. This is the artifact a contributor attaches to an
    /// issue when a game patch changes the layout.
    [[nodiscard]] std::string BuildDiscoveryReport(
        const std::vector<std::string>& focus_names = {}) const;

private:
    SymbolRegistry() = default;

    /// Expands a validated seed record into its full table extent.
    [[nodiscard]] static bool TryValidateTable(const PatternScanner& scanner,
                                               const Section& section,
                                               std::uintptr_t seed_record, std::size_t stride,
                                               std::size_t name_offset,
                                               std::size_t min_consecutive,
                                               std::size_t max_records,
                                               DebugTableLayout& out_layout);

    /// Locates the record holding one name, and the table it belongs to.
    [[nodiscard]] bool TryResolveName(const PatternScanner& scanner,
                                      const std::vector<const Section*>& sections,
                                      const SymbolRegistryConfig& config,
                                      const std::string& name,
                                      DebugTableLayout& out_table,
                                      std::uintptr_t& out_record);

    /// Indexes every record of a table. First definition wins.
    void IndexTable(const ModuleImage& image, const DebugTableLayout& table);

    /// Rebuilds by_name_ from records_. Must run after records_ stops growing,
    /// because the index holds views into the strings records_ owns.
    void RebuildIndex();

    std::vector<DebugTableLayout>                     tables_;
    std::vector<SymbolRecord>                         records_;
    std::unordered_map<std::string_view, std::size_t>  by_name_;
    const ModuleImage*                                image_{nullptr};

    /// Names requested by the config and whether each resolved, for the report.
    std::vector<std::pair<std::string, bool>> resolution_log_;
};

} // namespace fe::blam

// SPDX-License-Identifier: MIT
// ForgeEvolved: Blam/SymbolRegistry.cpp
#define FE_LOG_CATEGORY "Blam.Symbols"

#include "Blam/SymbolRegistry.h"

#include "Core/Json.h"
#include "Core/Log.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <unordered_set>

namespace fe::blam {
namespace {

using json = fe::json::Value;

/// Reads an optional field, leaving the destination untouched when absent or of the
/// wrong type. Keeps descriptor files forward compatible: an unknown or mistyped
/// field degrades to the default instead of failing the load.
template <typename T>
void ReadOptional(const json& node, const char* key, T& destination) {
    if (!node.contains(key)) {
        return;
    }
    try {
        destination = node.at(key).get<T>();
    } catch (const fe::json::exception& e) {
        FE_LOG_WARN("descriptor field '{}' ignored: {}", key, e.what());
    }
}

} // namespace

// ---------------------------------------------------------------------------
// SymbolRegistryConfig
// ---------------------------------------------------------------------------

SymbolRegistryConfig SymbolRegistryConfig::Default() {
    SymbolRegistryConfig config;
    config.game_build  = "2026.07.25.1112544.4-Rel-i343-Meteorite-2607-CU3";
    config.module_name = L"HaloSimulation_tag_release.dll";

    // Confirmed present in the shipped module by tools/probe/SymbolProbe.
    config.required_symbols = {
        // Session class and privacy. These live in a string enum table at stride
        // 0x10, verified at 0x1808303E8 in .rdata.
        "network_session_class_system_link",
        "network_session_class_offline",
        "network_session_privacy_open",
        "network_session_privacy_friends_only",
        "network_session_privacy_invitation_only",
    };

    // Wanted, but their containing structure varies between builds, so a miss must
    // not block the lobby and map layers from running.
    config.optional_symbols = {
        // Replication tuning and host migration.
        "net_simulation_set_stream_bandwidth",
        "net_speculative_host_migration_disable",
        // Map variant pipeline.
        "net_load_and_use_map_variant",
        "net_build_map_variant",
        "net_verify_map_variant",
        "write_current_map_variant",
        "read_map_variant_and_make_current",
        // Command dispatch.
        "console_command",
        "enable_console_window",
        // Forge object properties, for the sandbox path.
        "forge_object_properties_team",
        "forge_object_properties_spawn_time",
        "forge_object_properties_physics",
    };

    return config;
}

std::vector<std::string> SymbolRegistryConfig::AllRequestedNames() const {
    std::vector<std::string> names;
    names.reserve(required_symbols.size() + optional_symbols.size() + anchors.size());

    // Required first so their tables are discovered and indexed before anything
    // else, then optional, then extra anchors. Duplicates are skipped.
    std::unordered_set<std::string> seen;
    const auto append = [&](const std::vector<std::string>& source) {
        for (const std::string& name : source) {
            if (!name.empty() && seen.insert(name).second) {
                names.push_back(name);
            }
        }
    };
    append(required_symbols);
    append(optional_symbols);
    append(anchors);
    return names;
}

Expected<SymbolRegistryConfig> SymbolRegistryConfig::LoadFromFile(
    const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return Error{ErrorCode::FileNotFound,
                     std::format("symbol descriptor not found: {}", path.string())};
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return Error{ErrorCode::FileReadError,
                     std::format("cannot open symbol descriptor: {}", path.string())};
    }

    json root;
    try {
        // Comments allowed so descriptors can carry inline notes about how each
        // anchor was derived. Exceptions are contained here and converted.
        root = json::parse(stream, nullptr, true, true);
    } catch (const fe::json::exception& e) {
        return Error{ErrorCode::ParseError,
                     std::format("symbol descriptor is not valid JSON: {}", e.what())};
    }

    SymbolRegistryConfig config = Default();

    try {
        if (root.contains("schema_version")) {
            const int version = root.at("schema_version").get<std::int32_t>();
            if (version != 1) {
                return Error{ErrorCode::SchemaMismatch,
                             std::format("unsupported descriptor schema_version {}", version)};
            }
        }

        ReadOptional(root, "game_build", config.game_build);

        if (root.contains("module")) {
            const auto narrow = root.at("module").get<std::string>();
            config.module_name.assign(narrow.begin(), narrow.end());
        }

        if (root.contains("anchors")) {
            config.anchors = root.at("anchors").get<std::vector<std::string>>();
        }
        if (root.contains("required_symbols")) {
            config.required_symbols =
                root.at("required_symbols").get<std::vector<std::string>>();
        }
        if (root.contains("optional_symbols")) {
            config.optional_symbols =
                root.at("optional_symbols").get<std::vector<std::string>>();
        }

        if (root.contains("debug_table")) {
            const json& table = root.at("debug_table");
            ReadOptional(table, "name_offset", config.name_offset);
            ReadOptional(table, "min_consecutive_records", config.min_consecutive_records);
            ReadOptional(table, "max_table_records", config.max_table_records);
            ReadOptional(table, "max_anchor_matches", config.max_anchor_matches);
            ReadOptional(table, "max_pointer_matches", config.max_pointer_matches);
            if (table.contains("stride_candidates")) {
                config.stride_candidates =
                    table.at("stride_candidates").get<std::vector<std::size_t>>();
            }
        }
    } catch (const fe::json::exception& e) {
        return Error{ErrorCode::ParseError,
                     std::format("symbol descriptor has an invalid field: {}", e.what())};
    }

    if (config.required_symbols.empty()) {
        return Error{ErrorCode::ValidationFailed, "descriptor lists no required symbols"};
    }
    if (config.stride_candidates.empty()) {
        return Error{ErrorCode::ValidationFailed, "descriptor lists no stride candidates"};
    }
    if (config.min_consecutive_records < 2) {
        return Error{ErrorCode::ValidationFailed,
                     "min_consecutive_records must be at least 2 to reject false positives"};
    }

    FE_LOG_INFO("loaded symbol descriptor for build '{}' ({} required, {} optional)",
                config.game_build, config.required_symbols.size(),
                config.optional_symbols.size());
    return config;
}

// ---------------------------------------------------------------------------
// Table validation
// ---------------------------------------------------------------------------

bool SymbolRegistry::TryValidateTable(const PatternScanner& scanner, const Section& section,
                                      std::uintptr_t seed_record, std::size_t stride,
                                      std::size_t name_offset, std::size_t min_consecutive,
                                      std::size_t max_records,
                                      DebugTableLayout& out_layout) {
    const ModuleImage& image = scanner.Image();

    // A record is valid when its name field points at a plausible identifier.
    const auto record_is_valid = [&](std::uintptr_t record) -> bool {
        if (record < section.begin || record + stride > section.end) {
            return false;
        }
        std::uintptr_t name_pointer = 0;
        if (!image.TryReadPointer(record + name_offset, name_pointer)) {
            return false;
        }
        return scanner.IsPlausibleIdentifier(name_pointer);
    };

    if (!record_is_valid(seed_record)) {
        return false;
    }

    // Expand backward, then forward.
    std::uintptr_t first = seed_record;
    std::size_t    count = 1;
    while (count < max_records) {
        if (first < stride) {
            break; // Underflow guard.
        }
        const std::uintptr_t previous = first - stride;
        if (!record_is_valid(previous)) {
            break;
        }
        first = previous;
        ++count;
    }

    std::uintptr_t last_end = seed_record + stride;
    while (count < max_records) {
        if (!record_is_valid(last_end)) {
            break;
        }
        last_end += stride;
        ++count;
    }

    if (count < min_consecutive) {
        return false;
    }

    out_layout.begin        = first;
    out_layout.end          = last_end;
    out_layout.stride       = stride;
    out_layout.name_offset  = name_offset;
    out_layout.record_count = count;
    out_layout.section_name = section.name;
    return true;
}

bool SymbolRegistry::TryResolveName(const PatternScanner& scanner,
                                    const std::vector<const Section*>& sections,
                                    const SymbolRegistryConfig& config, const std::string& name,
                                    DebugTableLayout& out_table, std::uintptr_t& out_record) {
    const std::vector<std::uintptr_t> literals =
        scanner.FindStringLiteral(name, config.max_anchor_matches);
    if (literals.empty()) {
        FE_LOG_DEBUG("symbol '{}': string literal not present in this module", name);
        return false;
    }

    for (const std::uintptr_t literal : literals) {
        for (const Section* section : sections) {
            const std::vector<std::uintptr_t> holders =
                scanner.FindPointersTo(*section, literal, config.max_pointer_matches);

            for (const std::uintptr_t name_field : holders) {
                if (name_field < config.name_offset) {
                    continue;
                }
                const std::uintptr_t record = name_field - config.name_offset;

                // Smallest validating stride first, and it is the truthful one.
                //
                // The reverse was tried and is wrong: a table whose records are
                // 0x10 apart also "validates" at 0x40, because every fourth record
                // is still a valid record. That produced four overlapping copies of
                // the same table, offset by 0x10 each. A stride only validates when
                // every slot at that spacing holds a name pointer, so the smallest
                // one that passes is the real record size.
                std::vector<std::size_t> strides = config.stride_candidates;
                std::sort(strides.begin(), strides.end());

                for (const std::size_t stride : strides) {
                    if (stride == 0 || stride % sizeof(void*) != 0) {
                        continue;
                    }
                    DebugTableLayout candidate;
                    if (TryValidateTable(scanner, *section, record, stride, config.name_offset,
                                         config.min_consecutive_records,
                                         config.max_table_records, candidate)) {
                        candidate.discovered_via = name;
                        out_table  = candidate;
                        out_record = record;
                        return true;
                    }
                }
            }
        }
    }

    FE_LOG_DEBUG("symbol '{}': found {} literal(s) but none sits in a validatable record array",
                 name, literals.size());
    return false;
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

Expected<SymbolRegistry> SymbolRegistry::Discover(const ModuleImage& image,
                                                  const SymbolRegistryConfig& config) {
    const PatternScanner scanner(image);

    // Name tables live in initialized data. .rdata first because the verified
    // string enum table is there, then .data for writable tables.
    std::vector<const Section*> sections;
    if (const Section* rdata = image.FindSection(".rdata")) {
        sections.push_back(rdata);
    }
    if (const Section* data = image.FindSection(".data")) {
        sections.push_back(data);
    }
    if (const Section* rdata2 = image.FindSection("_RDATA")) {
        sections.push_back(rdata2);
    }
    if (sections.empty()) {
        return Error{ErrorCode::SectionNotFound,
                     "no initialized data section is present in the simulation module"};
    }

    SymbolRegistry registry;
    registry.image_ = &image;

    // Resolve every requested name. Each may land in a different table, and each
    // new table is indexed in full so its other members become available too.
    for (const std::string& name : config.AllRequestedNames()) {
        // Already indexed by a previously discovered table.
        if (registry.Find(name) != nullptr) {
            registry.resolution_log_.emplace_back(name, true);
            continue;
        }

        DebugTableLayout table;
        std::uintptr_t   record = 0;
        if (!registry.TryResolveName(scanner, sections, config, name, table, record)) {
            registry.resolution_log_.emplace_back(name, false);
            continue;
        }

        // Dedupe by containment rather than by exact begin: two symbols in the same
        // table resolve to different seed records, and expanding from either yields
        // the same extent, but a partially scanned table can end a record early.
        const bool already_known =
            std::any_of(registry.tables_.begin(), registry.tables_.end(),
                        [&](const DebugTableLayout& known) {
                            return known.stride == table.stride && record >= known.begin &&
                                   record < known.end;
                        });
        if (!already_known) {
            registry.tables_.push_back(table);
            registry.IndexTable(image, table);
            FE_LOG_INFO("table at 0x{:X}..0x{:X} in {} (stride 0x{:X}, {} records) via '{}'",
                        table.begin, table.end, table.section_name, table.stride,
                        table.record_count, name);
        }
        registry.resolution_log_.emplace_back(name, registry.Find(name) != nullptr);
    }

    registry.RebuildIndex();

    // Validation gate. Only required_symbols block activation.
    std::vector<std::string> missing;
    for (const std::string& required : config.required_symbols) {
        if (!registry.Has(required)) {
            missing.push_back(required);
        }
    }

    std::size_t optional_resolved = 0;
    for (const std::string& optional : config.optional_symbols) {
        if (registry.Has(optional)) {
            ++optional_resolved;
        }
    }

    FE_LOG_INFO("discovery indexed {} record(s) across {} table(s); {}/{} required, "
                "{}/{} optional resolved",
                registry.records_.size(), registry.tables_.size(),
                config.required_symbols.size() - missing.size(), config.required_symbols.size(),
                optional_resolved, config.optional_symbols.size());

    if (!missing.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < missing.size(); ++i) {
            joined += missing[i];
            if (i + 1 < missing.size()) {
                joined += ", ";
            }
        }
        FE_LOG_ERROR("discovery report follows:\n{}",
                     registry.BuildDiscoveryReport(config.required_symbols));
        return Error{ErrorCode::SymbolValidationFailed,
                     std::format("{} required symbol(s) could not be resolved: {}",
                                 missing.size(), joined)};
    }

    return registry;
}

void SymbolRegistry::IndexTable(const ModuleImage& image, const DebugTableLayout& table) {
    records_.reserve(records_.size() + table.record_count);

    for (std::uintptr_t record = table.begin; record < table.end; record += table.stride) {
        std::uintptr_t name_pointer = 0;
        if (!image.TryReadPointer(record + table.name_offset, name_pointer)) {
            continue;
        }
        const std::string_view name = image.ReadCString(name_pointer);
        if (name.empty()) {
            continue;
        }
        records_.push_back(SymbolRecord{std::string(name), record, table.stride, table.begin});
    }

    // by_name_ holds views into strings owned by records_, and records_ may have
    // reallocated. Rebuild now so Find works for the next symbol's short circuit.
    RebuildIndex();
}

void SymbolRegistry::RebuildIndex() {
    by_name_.clear();
    by_name_.reserve(records_.size());
    for (std::size_t i = 0; i < records_.size(); ++i) {
        // First definition wins. Duplicate names exist in Blam tables (the same
        // identifier registered for several scopes) and the first is the one the
        // engine itself resolves.
        by_name_.emplace(std::string_view(records_[i].name), i);
    }
}

const SymbolRecord* SymbolRegistry::Find(std::string_view name) const noexcept {
    const auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : &records_[it->second];
}

Expected<std::uintptr_t> SymbolRegistry::ReadRecordPointer(std::string_view name,
                                                            std::size_t field_offset) const {
    const SymbolRecord* record = Find(name);
    if (record == nullptr) {
        return Error{ErrorCode::SymbolNotResolved,
                     std::format("symbol '{}' was not resolved", name)};
    }
    if (field_offset + sizeof(std::uintptr_t) > record->stride) {
        return Error{ErrorCode::InvalidArgument,
                     std::format("field offset 0x{:X} exceeds the record stride 0x{:X}",
                                 field_offset, record->stride)};
    }
    if (image_ == nullptr) {
        return Error{ErrorCode::InvalidState, "registry is not bound to a module"};
    }

    std::uintptr_t value = 0;
    if (!image_->TryReadPointer(record->record_address + field_offset, value)) {
        return Error{ErrorCode::SymbolNotResolved,
                     std::format("cannot read field 0x{:X} of '{}'", field_offset, name)};
    }
    if (!image_->ContainsAddress(value)) {
        return Error{ErrorCode::SymbolValidationFailed,
                     std::format("field 0x{:X} of '{}' is 0x{:X}, which is outside the module",
                                 field_offset, name, value)};
    }
    return value;
}

namespace {

/// Field offsets inside a debug global record, measured on build 2607-CU3.
constexpr std::size_t kGlobalTypeOffset  = 0x08;
constexpr std::size_t kGlobalValueOffset = 0x10;

} // namespace

Expected<std::uint64_t> SymbolRegistry::ReadGlobalType(std::string_view name) const {
    const SymbolRecord* record = Find(name);
    if (record == nullptr) {
        return Error{ErrorCode::SymbolNotResolved,
                     std::format("symbol '{}' was not resolved", name)};
    }
    if (record->stride < kGlobalTypeOffset + sizeof(std::uint64_t)) {
        return Error{ErrorCode::InvalidState,
                     std::format("'{}' has stride 0x{:X}, too small to be a debug global",
                                 name, record->stride)};
    }
    return *reinterpret_cast<const std::uint64_t*>(record->record_address + kGlobalTypeOffset);
}

Expected<std::uint64_t> SymbolRegistry::ReadGlobalValue(std::string_view name) const {
    const SymbolRecord* record = Find(name);
    if (record == nullptr) {
        return Error{ErrorCode::SymbolNotResolved,
                     std::format("symbol '{}' was not resolved", name)};
    }
    if (record->stride < kGlobalValueOffset + sizeof(std::uint64_t)) {
        return Error{ErrorCode::InvalidState,
                     std::format("'{}' has stride 0x{:X}, so it is a string id record and has "
                                 "no inline value",
                                 name, record->stride)};
    }
    return *reinterpret_cast<const std::uint64_t*>(record->record_address + kGlobalValueOffset);
}

std::string SymbolRegistry::BuildDiscoveryReport(
    const std::vector<std::string>& focus_names) const {
    std::string out;
    out += "=== ForgeEvolved symbol discovery report ===\n";
    out += std::format("tables found    : {}\n", tables_.size());
    out += std::format("records indexed : {}\n", records_.size());

    for (std::size_t t = 0; t < tables_.size(); ++t) {
        const DebugTableLayout& table = tables_[t];
        out += std::format("\ntable {}: 0x{:X}..0x{:X} in {} stride 0x{:X} records {} via '{}'\n",
                           t, table.begin, table.end, table.section_name, table.stride,
                           table.record_count, table.discovered_via);

        // A few names from each table, which is usually enough to recognize what
        // kind of table it is.
        std::size_t shown = 0;
        for (const SymbolRecord& record : records_) {
            if (record.table_begin != table.begin) {
                continue;
            }
            out += std::format("    0x{:X}  {}\n", record.record_address, record.name);
            if (++shown >= 8) {
                out += "    ...\n";
                break;
            }
        }
    }

    out += "\n--- resolution ---\n";
    for (const auto& [name, resolved] : resolution_log_) {
        out += std::format("  {}  {}\n", resolved ? "ok     " : "MISSING", name);
    }

    // An annotated hex view of one record per focus name is what a contributor
    // needs in order to identify the value and handler field offsets.
    if (image_ != nullptr) {
        for (const std::string& name : focus_names) {
            const SymbolRecord* record = Find(name);
            if (record == nullptr) {
                continue;
            }
            out += std::format("\nhex dump of '{}' record at 0x{:X} ({} bytes):\n", name,
                               record->record_address, record->stride);
            const auto* raw = reinterpret_cast<const std::uint8_t*>(record->record_address);
            for (std::size_t offset = 0; offset < record->stride; offset += 8) {
                std::string line = std::format("  +0x{:02X}  ", offset);
                for (std::size_t b = 0; b < 8 && offset + b < record->stride; ++b) {
                    line += std::format("{:02X} ", raw[offset + b]);
                }
                std::uintptr_t as_pointer = 0;
                if (offset + sizeof(std::uintptr_t) <= record->stride &&
                    image_->TryReadPointer(record->record_address + offset, as_pointer)) {
                    const bool in_module = image_->ContainsAddress(as_pointer);
                    line += std::format(" | 0x{:016X}{}", as_pointer,
                                        in_module ? " (in module)" : "");
                    if (in_module) {
                        const std::string_view text = image_->ReadCString(as_pointer, 64);
                        if (!text.empty()) {
                            line += std::format(" \"{}\"", text);
                        }
                    }
                }
                out += line;
                out += '\n';
            }
        }
    }

    out += "=== end of report ===";
    return out;
}

} // namespace fe::blam

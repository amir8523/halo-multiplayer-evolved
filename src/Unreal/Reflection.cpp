// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Unreal/Reflection.cpp
#define MPE_LOG_CATEGORY "Unreal.Reflect"

#include "Unreal/Reflection.h"

#include "Core/Log.h"
#include "Unreal/ProcessMemory.h"

#include <algorithm>
#include <format>
#include <limits>
#include <unordered_set>

namespace mpe::unreal {
namespace {

/// True when text looks like a UE field or type name.
[[nodiscard]] bool IsPlausibleFieldName(std::string_view text) noexcept {
    if (text.size() < 2 || text.size() > 128) {
        return false;
    }
    for (const char c : text) {
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') || c == '_';
        if (!allowed) {
            return false;
        }
    }
    // A name that is all digits is a number, not an identifier.
    return !std::all_of(text.begin(), text.end(),
                        [](char c) { return c >= '0' && c <= '9'; });
}

/// Candidate offsets searched during detection. Pointer aligned, covering the range
/// any plausible UStruct or FField layout occupies.
constexpr std::size_t kMinCandidate = 0x18;
constexpr std::size_t kMaxCandidate = 0x90;

} // namespace

std::string ReflectionLayout::Describe() const {
    return std::format(
        "child_properties=+0x{:X} field_name=+0x{:X} field_next=+0x{:X} field_class=+0x{:X} "
        "offset_internal=+0x{:X} element_size=+0x{:X} properties_size=+0x{:X} "
        "super=+0x{:X} detected={} chain={}",
        child_properties_offset, field_name_offset, field_next_offset, field_class_offset,
        offset_internal_offset, element_size_offset, properties_size_offset,
        super_struct_offset, detected, detected_chain_length);
}

std::string Reflection::ResolveNameAt(std::uintptr_t address) const {
    if (names_ == nullptr) {
        return {};
    }
    // FName is { uint32 ComparisonIndex; uint32 Number; }. Only the index identifies
    // the text.
    const auto index = memory::Read<std::uint32_t>(address);
    if (!index.has_value()) {
        return {};
    }
    const Expected<std::string> text = names_->Resolve(*index);
    return text.ok() ? text.value() : std::string{};
}

std::vector<std::uintptr_t> Reflection::WalkChain(std::uintptr_t head, std::size_t name_offset,
                                                 std::size_t next_offset,
                                                 std::size_t max_nodes) const {
    std::vector<std::uintptr_t> nodes;
    std::unordered_set<std::uintptr_t> visited;
    std::uintptr_t current = head;

    while (memory::IsPlausiblePointer(current) && nodes.size() < max_nodes) {
        if (!visited.insert(current).second) {
            break; // Cycle.
        }
        if (!memory::IsReadable(current, std::max(name_offset, next_offset) + 8)) {
            break;
        }
        const std::string name = ResolveNameAt(current + name_offset);
        if (!IsPlausibleFieldName(name)) {
            break;
        }
        nodes.push_back(current);

        const auto next = memory::ReadPointer(current + next_offset);
        if (!next.has_value() || *next == 0) {
            break;
        }
        current = *next;
    }
    return nodes;
}

ReflectionLayout Reflection::DetectLayout(
    const std::vector<std::uintptr_t>& candidate_structs) const {
    ReflectionLayout best{};
    std::size_t      best_chain = 0;

    for (const std::uintptr_t struct_address : candidate_structs) {
        if (!memory::IsReadable(struct_address, kMaxCandidate + 16)) {
            continue;
        }

        // Search for a pointer that heads a chain of nodes with resolvable names.
        for (std::size_t child = kMinCandidate; child <= kMaxCandidate; child += 8) {
            const auto head = memory::ReadPointer(struct_address + child);
            if (!head.has_value() || !memory::IsPlausiblePointer(*head)) {
                continue;
            }

            for (std::size_t name_off = 0x10; name_off <= 0x38; name_off += 8) {
                // A single node proves nothing; require the chain to continue.
                for (std::size_t next_off = 0x10; next_off <= 0x38; next_off += 8) {
                    if (next_off == name_off) {
                        continue;
                    }
                    const std::vector<std::uintptr_t> nodes =
                        WalkChain(*head, name_off, next_off, 64);
                    if (nodes.size() < 2 || nodes.size() <= best_chain) {
                        continue;
                    }

                    // Names must be distinct: a chain of identical names means we are
                    // reading the same field repeatedly through a self referencing
                    // pointer.
                    std::unordered_set<std::string> names;
                    for (const std::uintptr_t node : nodes) {
                        names.insert(ResolveNameAt(node + name_off));
                    }
                    if (names.size() < nodes.size()) {
                        continue;
                    }

                    // Second signal: find the int32 whose values across the chain are in
                    // range, distinct, and non decreasing. That is Offset_Internal, and
                    // it is what makes this detection trustworthy rather than plausible.
                    std::size_t  found_offset_slot = 0;
                    bool         found_offsets     = false;
                    for (std::size_t slot = 0x28; slot <= 0x60 && !found_offsets; slot += 4) {
                        std::int32_t previous = -1;
                        bool         ok       = true;
                        std::unordered_set<std::int32_t> seen;
                        for (const std::uintptr_t node : nodes) {
                            const auto value = memory::Read<std::int32_t>(node + slot);
                            if (!value.has_value() || *value < 0 || *value > 0x20000) {
                                ok = false;
                                break;
                            }
                            if (*value < previous || !seen.insert(*value).second) {
                                ok = false;
                                break;
                            }
                            previous = *value;
                        }
                        if (ok) {
                            found_offset_slot = slot;
                            found_offsets     = true;
                        }
                    }
                    if (!found_offsets) {
                        continue;
                    }

                    ReflectionLayout candidate;
                    candidate.child_properties_offset = child;
                    candidate.field_name_offset       = name_off;
                    candidate.field_next_offset       = next_off;
                    candidate.offset_internal_offset  = found_offset_slot;
                    // ElementSize sits immediately before Offset_Internal in every
                    // layout observed; recorded as a best effort and validated by use.
                    candidate.element_size_offset =
                        (found_offset_slot >= 0x10) ? found_offset_slot - 0x10 : found_offset_slot;
                    candidate.field_class_offset = kFFieldClassOffset;
                    candidate.super_struct_offset = kStructSuperOffset;
                    candidate.detected            = true;
                    candidate.detected_chain_length = nodes.size();

                    // PropertiesSize: the int32 on the struct that is at least as large
                    // as the highest field end.
                    std::int32_t highest_end = 0;
                    for (const std::uintptr_t node : nodes) {
                        const auto off = memory::Read<std::int32_t>(node + found_offset_slot);
                        if (off.has_value()) {
                            highest_end = std::max(highest_end, *off);
                        }
                    }
                    for (std::size_t slot = 0x38; slot <= 0x60; slot += 4) {
                        const auto value = memory::Read<std::int32_t>(struct_address + slot);
                        if (value.has_value() && *value >= highest_end && *value > 0 &&
                            *value <= 0x20000) {
                            candidate.properties_size_offset = slot;
                            break;
                        }
                    }

                    best       = candidate;
                    best_chain = nodes.size();
                }
            }
        }

        // A long chain from one struct is enough; no need to scan the rest.
        if (best_chain >= 8) {
            break;
        }
    }

    return best;
}

Expected<PropertyInfo> Reflection::ReadProperty(std::uintptr_t property_address) const {
    const std::size_t needed =
        std::max({layout_.field_name_offset, layout_.offset_internal_offset,
                  layout_.element_size_offset, layout_.field_class_offset}) + 8;
    if (!memory::IsPlausiblePointer(property_address) ||
        !memory::IsReadable(property_address, needed)) {
        return Error{ErrorCode::NotFound,
                     std::format("property at 0x{:X} is unreadable", property_address)};
    }

    PropertyInfo info;
    info.address = property_address;
    info.name    = ResolveNameAt(property_address + layout_.field_name_offset);
    if (!IsPlausibleFieldName(info.name)) {
        return Error{ErrorCode::ValidationFailed,
                     std::format("property at 0x{:X} has an implausible name", property_address)};
    }

    if (const auto field_class = memory::ReadPointer(property_address + layout_.field_class_offset);
        field_class.has_value() && memory::IsPlausiblePointer(*field_class)) {
        // FFieldClass::Name is its first member. For a UProperty style layout this
        // instead reads the UClass name, which is equally useful as a type label.
        info.type_name = ResolveNameAt(*field_class);
        if (info.type_name.empty()) {
            info.type_name = ResolveNameAt(*field_class + kObjectNamePrivateOffset);
        }
    }

    const auto offset       = memory::Read<std::int32_t>(property_address +
                                                        layout_.offset_internal_offset);
    const auto element_size = memory::Read<std::int32_t>(property_address +
                                                        layout_.element_size_offset);
    if (!offset.has_value()) {
        return Error{ErrorCode::NotFound,
                     std::format("property '{}' offset is unreadable", info.name)};
    }
    if (*offset < 0 || *offset > 0x20000) {
        return Error{ErrorCode::ValidationFailed,
                     std::format("property '{}' has an implausible offset {}", info.name,
                                 *offset)};
    }

    info.offset = *offset;
    if (element_size.has_value() && *element_size >= 0 && *element_size <= 0x20000) {
        info.element_size = *element_size;
    }
    info.array_dim = 1;
    return info;
}

std::vector<PropertyInfo> Reflection::ReadProperties(std::uintptr_t struct_address) const {
    std::vector<PropertyInfo> properties;
    if (!memory::IsPlausiblePointer(struct_address) ||
        !memory::IsReadable(struct_address, layout_.child_properties_offset + 8)) {
        return properties;
    }

    const auto head = memory::ReadPointer(struct_address + layout_.child_properties_offset);
    if (!head.has_value()) {
        return properties;
    }

    const std::vector<std::uintptr_t> nodes =
        WalkChain(*head, layout_.field_name_offset, layout_.field_next_offset,
                  kMaxPropertiesPerStruct);

    properties.reserve(nodes.size());
    for (const std::uintptr_t node : nodes) {
        Expected<PropertyInfo> property = ReadProperty(node);
        if (property.ok()) {
            properties.push_back(std::move(property).value());
        }
    }
    return properties;
}

Expected<StructInfo> Reflection::ReadStruct(std::uintptr_t struct_address) const {
    if (!memory::IsPlausiblePointer(struct_address) ||
        !memory::IsReadable(struct_address, layout_.properties_size_offset + 8)) {
        return Error{ErrorCode::NotFound,
                     std::format("struct at 0x{:X} is unreadable", struct_address)};
    }

    StructInfo info;
    info.address = struct_address;
    info.name    = ResolveNameAt(struct_address + kObjectNamePrivateOffset);

    if (const auto super = memory::ReadPointer(struct_address + layout_.super_struct_offset);
        super.has_value() && memory::IsPlausiblePointer(*super)) {
        info.super_address = *super;
    }
    if (const auto size = memory::Read<std::int32_t>(struct_address +
                                                    layout_.properties_size_offset);
        size.has_value()) {
        info.properties_size = *size;
    }

    info.properties = ReadProperties(struct_address);
    return info;
}

std::vector<PropertyInfo> Reflection::ReadAllProperties(std::uintptr_t struct_address) const {
    std::vector<std::vector<PropertyInfo>> levels;
    std::uintptr_t current = struct_address;
    std::unordered_set<std::uintptr_t> visited;

    for (std::size_t depth = 0; depth < kMaxInheritanceDepth && current != 0; ++depth) {
        if (!visited.insert(current).second) {
            break;
        }
        levels.push_back(ReadProperties(current));

        const auto super = memory::ReadPointer(current + layout_.super_struct_offset);
        if (!super.has_value() || !memory::IsPlausiblePointer(*super)) {
            break;
        }
        current = *super;
    }

    // Base first, matching memory order.
    std::vector<PropertyInfo> all;
    for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
        all.insert(all.end(), it->begin(), it->end());
    }
    return all;
}

Expected<PropertyInfo> Reflection::FindProperty(std::uintptr_t struct_address,
                                                std::string_view name) const {
    const std::vector<PropertyInfo> properties = ReadAllProperties(struct_address);
    const auto it = std::find_if(properties.begin(), properties.end(),
                                 [name](const PropertyInfo& p) { return p.name == name; });
    if (it == properties.end()) {
        return Error{ErrorCode::NotFound,
                     std::format("no property named '{}' in the struct at 0x{:X} ({} searched)",
                                 name, struct_address, properties.size())};
    }
    return *it;
}

std::vector<PropertyInfo> Reflection::FindPropertiesContaining(std::uintptr_t struct_address,
                                                               std::string_view fragment) const {
    std::vector<PropertyInfo> results;
    if (fragment.empty()) {
        return results;
    }
    for (const PropertyInfo& property : ReadAllProperties(struct_address)) {
        if (property.name.find(fragment) != std::string::npos) {
            results.push_back(property);
        }
    }
    return results;
}

Result Reflection::VerifyLayout(std::uintptr_t struct_address, std::string& out_report) const {
    Expected<StructInfo> info = ReadStruct(struct_address);
    if (!info.ok()) {
        out_report = std::format("could not read the struct at 0x{:X}: {}", struct_address,
                                 info.message());
        return Result::Fail(ErrorCode::SymbolValidationFailed, out_report);
    }

    const StructInfo& s = info.value();
    std::size_t inside  = 0;
    std::size_t outside = 0;
    for (const PropertyInfo& property : s.properties) {
        if (property.offset + property.TotalSize() <= s.properties_size) {
            ++inside;
        } else {
            ++outside;
        }
    }

    out_report = std::format("struct '{}' at 0x{:X}: size {}, {} field(s), {} in bounds, {} out",
                             s.name, s.address, s.properties_size, s.properties.size(), inside,
                             outside);

    if (s.name.empty()) {
        return Result::Fail(ErrorCode::SymbolValidationFailed,
                            out_report + "; no resolvable name");
    }
    if (s.properties.empty()) {
        return Result::Fail(ErrorCode::SymbolValidationFailed,
                            out_report + "; no readable properties");
    }
    if (outside > 0) {
        return Result::Fail(ErrorCode::SymbolValidationFailed,
                            out_report + "; fields lie outside their own struct");
    }
    return Result::Success();
}

std::size_t Reflection::DetectStructPropertyInnerOffset(
    const std::vector<std::uintptr_t>& owner_structs,
    const std::vector<std::uintptr_t>& known_script_structs) const {
    if (known_script_structs.empty()) {
        return 0;
    }
    const std::unordered_set<std::uintptr_t> known(known_script_structs.begin(),
                                                   known_script_structs.end());

    // Tally how often each candidate offset lands on a known ScriptStruct. The correct
    // offset hits on every StructProperty; a coincidence hits once.
    std::vector<std::pair<std::size_t, std::size_t>> scores;
    for (std::size_t candidate = 0x28; candidate <= 0x80; candidate += 8) {
        std::size_t hits = 0;
        for (const std::uintptr_t owner : owner_structs) {
            for (const PropertyInfo& property : ReadProperties(owner)) {
                if (property.type_name != "StructProperty") {
                    continue;
                }
                const auto inner = memory::ReadPointer(property.address + candidate);
                if (inner.has_value() && known.contains(*inner)) {
                    ++hits;
                }
            }
        }
        if (hits > 0) {
            scores.emplace_back(candidate, hits);
        }
    }

    if (scores.empty()) {
        return 0;
    }
    const auto best = std::max_element(scores.begin(), scores.end(),
                                       [](const auto& a, const auto& b) {
                                           return a.second < b.second;
                                       });
    return best->first;
}

std::uintptr_t Reflection::ResolveStructPropertyInner(std::uintptr_t property_address) const {
    if (!layout_.struct_property_inner_detected) {
        return 0;
    }
    const auto inner = memory::ReadPointer(property_address +
                                          layout_.struct_property_inner_offset);
    if (!inner.has_value() || !memory::IsPlausiblePointer(*inner)) {
        return 0;
    }
    return *inner;
}

std::size_t Reflection::DetectPropertiesSizeOffset(
    const std::vector<std::uintptr_t>& structs) const {
    // Each struct's size must be at least as large as the end of its last field.
    struct Bound {
        std::uintptr_t address{0};
        std::int32_t   minimum{0};
    };
    std::vector<Bound> bounds;
    for (const std::uintptr_t address : structs) {
        std::int32_t highest = 0;
        for (const PropertyInfo& property : ReadProperties(address)) {
            highest = std::max(highest, property.offset + std::max(property.TotalSize(), 1));
        }
        if (highest > 0) {
            bounds.push_back({address, highest});
        }
    }
    if (bounds.size() < 2) {
        return 0;
    }

    std::size_t best_offset = 0;
    std::int32_t best_slack  = std::numeric_limits<std::int32_t>::max();

    for (std::size_t candidate = 0x30; candidate <= 0x70; candidate += 4) {
        std::int32_t total_slack = 0;
        bool         ok          = true;
        std::unordered_set<std::int32_t> distinct;

        for (const Bound& bound : bounds) {
            const auto value = memory::Read<std::int32_t>(bound.address + candidate);
            if (!value.has_value() || *value < bound.minimum || *value > 0x20000) {
                ok = false;
                break;
            }
            total_slack += (*value - bound.minimum);
            distinct.insert(*value);
        }
        // A single shared value across every struct is a constant, not a size. This is
        // the check that was missing: properties_size read 387 for all eight structs.
        if (!ok || distinct.size() < 2) {
            continue;
        }
        if (total_slack < best_slack) {
            best_slack  = total_slack;
            best_offset = candidate;
        }
    }
    return best_offset;
}

Expected<bool> Reflection::ReadBoolField(std::uintptr_t instance_address,
                                        const PropertyInfo& property) const {
    if (!property.IsBool()) {
        return Error{ErrorCode::InvalidArgument,
                     std::format("'{}' is a {}, not a BoolProperty", property.name,
                                 property.type_name)};
    }
    const std::uintptr_t address = instance_address + static_cast<std::uintptr_t>(property.offset);
    const auto value = memory::Read<std::uint8_t>(address);
    if (!value.has_value()) {
        return Error{ErrorCode::NotFound,
                     std::format("'{}' at 0x{:X} is unreadable", property.name, address)};
    }
    return *value != 0;
}

Result Reflection::WriteBoolField(std::uintptr_t instance_address, const PropertyInfo& property,
                                 bool value) const {
    if (!property.IsBool()) {
        return Result::Fail(ErrorCode::InvalidArgument,
                            std::format("'{}' is a {}, not a BoolProperty", property.name,
                                        property.type_name));
    }
    const std::uintptr_t address = instance_address + static_cast<std::uintptr_t>(property.offset);
    const std::uint8_t   byte    = value ? 1u : 0u;
    if (!memory::Write(address, byte)) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("could not write '{}' at 0x{:X}", property.name,
                                        address));
    }

    // Read back. A write that does not stick is worse than a refused write, because the
    // caller would believe the setting took effect.
    const auto confirmed = memory::Read<std::uint8_t>(address);
    if (!confirmed.has_value() || (*confirmed != 0) != value) {
        return Result::Fail(ErrorCode::SymbolValidationFailed,
                            std::format("wrote {} to '{}' at 0x{:X} but read back {}", byte,
                                        property.name, address,
                                        confirmed.has_value() ? *confirmed : 0xFFu));
    }
    return Result::Success();
}

std::string Reflection::ProbeStructLayout(std::uintptr_t struct_address,
                                         std::size_t bytes) const {
    std::string out = std::format("annotated dump of 0x{:X}:\n", struct_address);

    for (std::size_t offset = 0; offset + 8 <= bytes; offset += 8) {
        const auto value = memory::Read<std::uintptr_t>(struct_address + offset);
        if (!value.has_value()) {
            out += std::format("  +0x{:02X}  <unreadable>\n", offset);
            continue;
        }

        std::string note;
        // As two int32s, which is how UE packs sizes and FNames.
        const auto low  = static_cast<std::int32_t>(*value & 0xFFFFFFFFu);
        const auto high = static_cast<std::int32_t>(*value >> 32);
        note += std::format(" i32=({}, {})", low, high);

        if (memory::IsPlausiblePointer(*value) && memory::IsReadable(*value, 0x40)) {
            note += " ptr:readable";
            // If it points at something whose FName resolves, say so and what to.
            for (const std::size_t name_off : {std::size_t{0x10}, std::size_t{0x18},
                                               std::size_t{0x20}, std::size_t{0x28}}) {
                const std::string name = ResolveNameAt(*value + name_off);
                if (IsPlausibleFieldName(name)) {
                    note += std::format(" name@+0x{:X}=\"{}\"", name_off, name);
                }
            }
        }
        // The value read as an FName index directly.
        if (const std::string self = ResolveNameAt(struct_address + offset);
            IsPlausibleFieldName(self)) {
            note += std::format(" asFName=\"{}\"", self);
        }

        out += std::format("  +0x{:02X}  0x{:016X}{}\n", offset, *value, note);
    }
    return out;
}

} // namespace mpe::unreal


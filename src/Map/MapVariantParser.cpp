// SPDX-License-Identifier: MIT
// ForgeEvolved: Map/MapVariantParser.cpp
#define FE_LOG_CATEGORY "Map.Parser"

#include "Map/MapVariantParser.h"

#include "Core/ByteStream.h"
#include "Core/Json.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <format>
#include <unordered_set>

namespace fe::map {
namespace {

using json = fe::json::Value;

/// Same type as json; the distinct name documents that the writer relies on
/// objects preserving insertion order.
using OrderedJson = fe::json::Value;

// ---------------------------------------------------------------------------
// Diagnostic collection
// ---------------------------------------------------------------------------

/// Accumulates findings during a parse. Keeping the collector separate from the
/// field readers is what lets one pass report every problem in a document rather
/// than stopping at the first.
class DiagnosticSink {
public:
    void Error(std::string path, std::string message) {
        diagnostics_.push_back(Diagnostic{Severity::Error, std::move(path), std::move(message)});
        ++error_count_;
    }
    void Warn(std::string path, std::string message) {
        diagnostics_.push_back(Diagnostic{Severity::Warning, std::move(path), std::move(message)});
    }

    [[nodiscard]] bool HasErrors() const noexcept { return error_count_ > 0; }
    [[nodiscard]] std::size_t ErrorCount() const noexcept { return error_count_; }

    /// Errors first, then warnings, each group in discovery order. An author fixes
    /// errors before considering warnings, so that is the order they are shown.
    [[nodiscard]] std::vector<Diagnostic> Take() {
        std::stable_sort(diagnostics_.begin(), diagnostics_.end(),
                         [](const Diagnostic& a, const Diagnostic& b) {
                             return static_cast<int>(a.severity) < static_cast<int>(b.severity);
                         });
        return std::move(diagnostics_);
    }

private:
    std::vector<Diagnostic> diagnostics_;
    std::size_t             error_count_{0};
};

/// Renders diagnostics into one message. Used for the Error returned to callers
/// that do not display the list themselves.
[[nodiscard]] std::string Summarize(const std::vector<Diagnostic>& diagnostics,
                                    std::size_t max_lines = 20) {
    std::string out;
    std::size_t shown = 0;
    for (const Diagnostic& diagnostic : diagnostics) {
        if (shown >= max_lines) {
            out += std::format("\n  ... and {} more", diagnostics.size() - shown);
            break;
        }
        out += std::format("\n  [{}] {}: {}", ToString(diagnostic.severity),
                           diagnostic.json_path.empty() ? "<root>" : diagnostic.json_path,
                           diagnostic.message);
        ++shown;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Typed field readers
// ---------------------------------------------------------------------------
//
// Every reader takes the parent node and a field name, reports its own
// diagnostic on failure, and leaves the destination at its default. The caller
// therefore never has to check a return value to stay memory safe, only to decide
// whether to keep going.

[[nodiscard]] bool ReadString(const json& node, const char* field, std::string_view path,
                              std::size_t max_length, bool required, std::string& out,
                              DiagnosticSink& sink) {
    if (!node.contains(field)) {
        if (required) {
            sink.Error(std::format("{}.{}", path, field), "required field is missing");
        }
        return false;
    }
    if (!node.at(field).is_string()) {
        sink.Error(std::format("{}.{}", path, field), "expected a string");
        return false;
    }
    std::string value = node.at(field).get<std::string>();
    if (value.size() > max_length) {
        sink.Error(std::format("{}.{}", path, field),
                   std::format("length {} exceeds the {} character limit", value.size(),
                               max_length));
        return false;
    }
    // Control characters would corrupt log output and any UI that renders the
    // value. Stripped with a warning rather than rejected, since the intent is
    // clear and the fix is unambiguous.
    const std::size_t before = value.size();
    std::erase_if(value, [](char c) {
        const auto uc = static_cast<unsigned char>(c);
        return uc < 0x20u || uc == 0x7Fu;
    });
    if (value.size() != before) {
        sink.Warn(std::format("{}.{}", path, field), "control characters were removed");
    }
    out = std::move(value);
    return true;
}

/// Reads a float, rejecting NaN and infinity. Those propagate silently through
/// every downstream calculation and turn into an object at an undefined position,
/// so they are treated as hard errors.
[[nodiscard]] bool ReadFloat(const json& node, const char* field, std::string_view path,
                             float min_value, float max_value, bool required, float& out,
                             DiagnosticSink& sink) {
    if (!node.contains(field)) {
        if (required) {
            sink.Error(std::format("{}.{}", path, field), "required field is missing");
        }
        return false;
    }
    if (!node.at(field).is_number()) {
        sink.Error(std::format("{}.{}", path, field), "expected a number");
        return false;
    }
    const double raw = node.at(field).get<double>();
    if (!std::isfinite(raw)) {
        sink.Error(std::format("{}.{}", path, field), "value is not finite");
        return false;
    }
    if (raw < static_cast<double>(min_value) || raw > static_cast<double>(max_value)) {
        sink.Error(std::format("{}.{}", path, field),
                   std::format("{} is outside the valid range {} to {}", raw, min_value,
                               max_value));
        return false;
    }
    out = static_cast<float>(raw);
    return true;
}

template <typename T>
[[nodiscard]] bool ReadUnsigned(const json& node, const char* field, std::string_view path,
                                std::uint64_t max_value, bool required, T& out,
                                DiagnosticSink& sink) {
    if (!node.contains(field)) {
        if (required) {
            sink.Error(std::format("{}.{}", path, field), "required field is missing");
        }
        return false;
    }
    if (!node.at(field).is_number_unsigned()) {
        sink.Error(std::format("{}.{}", path, field), "expected a non negative integer");
        return false;
    }
    const std::uint64_t raw = node.at(field).get<std::uint64_t>();
    if (raw > max_value) {
        sink.Error(std::format("{}.{}", path, field),
                   std::format("{} exceeds the maximum {}", raw, max_value));
        return false;
    }
    out = static_cast<T>(raw);
    return true;
}

[[nodiscard]] bool ReadBool(const json& node, const char* field, std::string_view path,
                            bool default_value, bool& out, DiagnosticSink& sink) {
    out = default_value;
    if (!node.contains(field)) {
        return false;
    }
    if (!node.at(field).is_boolean()) {
        sink.Error(std::format("{}.{}", path, field), "expected true or false");
        return false;
    }
    out = node.at(field).get<bool>();
    return true;
}

[[nodiscard]] bool ReadVec3(const json& node, const char* field, std::string_view path,
                            bool required, Vec3& out, DiagnosticSink& sink) {
    if (!node.contains(field)) {
        if (required) {
            sink.Error(std::format("{}.{}", path, field), "required field is missing");
        }
        return false;
    }
    const json& value = node.at(field);
    const std::string child = std::format("{}.{}", path, field);

    // Both [x, y, z] and {"x":, "y":, "z":} are accepted. Hand authored maps use
    // the array form; tools tend to emit the object form.
    if (value.is_array()) {
        if (value.size() != 3) {
            sink.Error(child, std::format("expected 3 components, found {}", value.size()));
            return false;
        }
        float components[3] = {0.0f, 0.0f, 0.0f};
        for (std::size_t i = 0; i < 3; ++i) {
            if (!value[i].is_number()) {
                sink.Error(std::format("{}[{}]", child, i), "expected a number");
                return false;
            }
            const double raw = value[i].get<double>();
            if (!std::isfinite(raw) || std::abs(raw) > static_cast<double>(kWorldExtent)) {
                sink.Error(std::format("{}[{}]", child, i),
                           std::format("{} is not a finite coordinate within +/-{}", raw,
                                       kWorldExtent));
                return false;
            }
            components[i] = static_cast<float>(raw);
        }
        out = Vec3{components[0], components[1], components[2]};
        return true;
    }

    if (value.is_object()) {
        Vec3 parsed;
        const bool ok_x = ReadFloat(value, "x", child, -kWorldExtent, kWorldExtent, true,
                                    parsed.x, sink);
        const bool ok_y = ReadFloat(value, "y", child, -kWorldExtent, kWorldExtent, true,
                                    parsed.y, sink);
        const bool ok_z = ReadFloat(value, "z", child, -kWorldExtent, kWorldExtent, true,
                                    parsed.z, sink);
        if (!ok_x || !ok_y || !ok_z) {
            return false;
        }
        out = parsed;
        return true;
    }

    sink.Error(child, "expected an array of 3 numbers or an object with x, y and z");
    return false;
}

/// Reads a rotation as either a quaternion or a yaw angle in degrees, and always
/// yields a normalized quaternion.
[[nodiscard]] bool ReadRotation(const json& node, std::string_view path, Quat& out,
                                DiagnosticSink& sink) {
    // Yaw only is the common case for weapons and scenery, so it is supported
    // directly rather than forcing an author to write a quaternion by hand.
    if (node.contains("yaw_degrees")) {
        float yaw = 0.0f;
        if (!ReadFloat(node, "yaw_degrees", path, -3600.0f, 3600.0f, true, yaw, sink)) {
            return false;
        }
        const float half = yaw * 0.5f * 3.14159265358979323846f / 180.0f;
        out = Quat{0.0f, 0.0f, std::sin(half), std::cos(half)};
        return true;
    }

    if (!node.contains("rotation")) {
        out = Quat{}; // Identity.
        return true;
    }

    const json& value = node.at("rotation");
    const std::string child = std::format("{}.rotation", path);
    if (!value.is_array() || value.size() != 4) {
        sink.Error(child, "expected an array of 4 numbers as x, y, z, w");
        return false;
    }

    float components[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (std::size_t i = 0; i < 4; ++i) {
        if (!value[i].is_number()) {
            sink.Error(std::format("{}[{}]", child, i), "expected a number");
            return false;
        }
        const double raw = value[i].get<double>();
        if (!std::isfinite(raw)) {
            sink.Error(std::format("{}[{}]", child, i), "value is not finite");
            return false;
        }
        components[i] = static_cast<float>(raw);
    }

    const float length_squared = components[0] * components[0] + components[1] * components[1] +
                                 components[2] * components[2] + components[3] * components[3];
    if (length_squared < 1.0e-6f) {
        sink.Error(child, "quaternion has zero length and cannot describe a rotation");
        return false;
    }

    // Normalized rather than rejected when slightly off unit length: floating
    // point drift through an editor is expected, and downstream code is allowed to
    // assume unit length.
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    if (std::abs(1.0f - length_squared) > 0.01f) {
        sink.Warn(child, "quaternion was not unit length and has been normalized");
    }
    out = Quat{components[0] * inverse_length, components[1] * inverse_length,
               components[2] * inverse_length, components[3] * inverse_length};
    return true;
}

[[nodiscard]] bool ReadShape(const json& node, std::string_view path, Shape& out,
                             DiagnosticSink& sink) {
    if (!node.contains("shape")) {
        out = Shape{};
        return true;
    }
    const json& value = node.at("shape");
    const std::string child = std::format("{}.shape", path);
    if (!value.is_object()) {
        sink.Error(child, "expected an object");
        return false;
    }

    Shape shape;
    std::string type_text;
    if (!ReadString(value, "type", child, 16, true, type_text, sink)) {
        return false;
    }
    if (!ParseShapeType(type_text, shape.type)) {
        sink.Error(std::format("{}.type", child),
                   std::format("'{}' is not one of none, sphere, cylinder, box", type_text));
        return false;
    }

    // Only the dimensions the shape actually uses are read, so an unused field
    // cannot silently affect behaviour.
    constexpr float kMaxDimension = 10000.0f;
    switch (shape.type) {
        case ShapeType::None:
            break;
        case ShapeType::Sphere:
            if (!ReadFloat(value, "radius", child, 0.0f, kMaxDimension, true, shape.radius, sink)) {
                return false;
            }
            break;
        case ShapeType::Cylinder:
            if (!ReadFloat(value, "radius", child, 0.0f, kMaxDimension, true, shape.radius, sink) ||
                !ReadFloat(value, "top", child, 0.0f, kMaxDimension, true, shape.top, sink) ||
                !ReadFloat(value, "bottom", child, 0.0f, kMaxDimension, true, shape.bottom, sink)) {
                return false;
            }
            break;
        case ShapeType::Box:
            if (!ReadFloat(value, "width", child, 0.0f, kMaxDimension, true, shape.width, sink) ||
                !ReadFloat(value, "depth", child, 0.0f, kMaxDimension, true, shape.depth, sink) ||
                !ReadFloat(value, "top", child, 0.0f, kMaxDimension, true, shape.top, sink) ||
                !ReadFloat(value, "bottom", child, 0.0f, kMaxDimension, true, shape.bottom, sink)) {
                return false;
            }
            break;
    }

    if (shape.type != ShapeType::None && shape.type != ShapeType::Sphere &&
        shape.top + shape.bottom <= 0.0f) {
        sink.Error(child, "a shape with no height cannot contain anything");
        return false;
    }
    out = shape;
    return true;
}

/// Validates a palette key: lowercase identifier segments separated by dots, for
/// example "weapon.rocket_launcher". A restricted grammar means a key can be used
/// as a lookup token without escaping anywhere.
[[nodiscard]] bool IsValidPaletteKey(std::string_view key) noexcept {
    if (key.empty() || key.size() > kMaxPaletteKeyLength) {
        return false;
    }
    if (key.front() == '.' || key.back() == '.') {
        return false;
    }
    bool previous_was_dot = false;
    for (const char c : key) {
        const bool is_lower = (c >= 'a' && c <= 'z');
        const bool is_digit = (c >= '0' && c <= '9');
        const bool is_other = (c == '_' || c == '.');
        if (!is_lower && !is_digit && !is_other) {
            return false;
        }
        if (c == '.' && previous_was_dot) {
            return false; // No empty segments.
        }
        previous_was_dot = (c == '.');
    }
    return true;
}

// ---------------------------------------------------------------------------
// Element parsers
// ---------------------------------------------------------------------------

[[nodiscard]] bool ParseObject(const json& node, std::size_t index, ObjectPlacement& out,
                               DiagnosticSink& sink) {
    const std::string path = std::format("objects[{}]", index);
    if (!node.is_object()) {
        sink.Error(path, "expected an object");
        return false;
    }

    ObjectPlacement object;
    bool ok = ReadUnsigned(node, "id", path, 0xFFFFFFFFull, true, object.id, sink);
    ok = ReadString(node, "palette_key", path, kMaxPaletteKeyLength, true, object.palette_key,
                    sink) && ok;
    if (!object.palette_key.empty() && !IsValidPaletteKey(object.palette_key)) {
        sink.Error(std::format("{}.palette_key", path),
                   std::format("'{}' is not a valid palette key; expected lowercase segments "
                               "separated by dots, for example weapon.rocket_launcher",
                               object.palette_key));
        ok = false;
    }
    ok = ReadVec3(node, "position", path, true, object.position, sink) && ok;
    ok = ReadRotation(node, path, object.rotation, sink) && ok;
    ok = ReadShape(node, path, object.shape, sink) && ok;

    // Optional fields: absence is not an error, so the return value is ignored and
    // any type error is still recorded by the reader.
    (void)ReadFloat(node, "scale", path, 0.01f, 100.0f, false, object.scale, sink);
    (void)ReadUnsigned(node, "spawn_time_seconds", path, 0xFFFFull, false,
                       object.spawn_time_seconds, sink);
    (void)ReadUnsigned(node, "user_data", path, 0xFFFFFFFFull, false, object.user_data, sink);
    (void)ReadBool(node, "spawn_at_start", path, true, object.spawn_at_start, sink);
    (void)ReadString(node, "label", path, kMaxLabelLength, false, object.label, sink);

    if (node.contains("physics")) {
        std::string physics_text;
        if (ReadString(node, "physics", path, 16, true, physics_text, sink)) {
            if (!ParsePhysicsMode(physics_text, object.physics)) {
                sink.Error(std::format("{}.physics", path),
                           std::format("'{}' is not one of normal, fixed, phased", physics_text));
                ok = false;
            }
        }
    }

    if (node.contains("team")) {
        std::uint32_t team = kNeutralTeam;
        if (ReadUnsigned(node, "team", path, 0xFFull, true, team, sink)) {
            if (team != kNeutralTeam && team >= kMaxTeams) {
                sink.Error(std::format("{}.team", path),
                           std::format("team {} is outside 0..{} and is not the neutral value {}",
                                       team, kMaxTeams - 1, kNeutralTeam));
                ok = false;
            } else {
                object.team = static_cast<std::uint8_t>(team);
            }
        }
    }

    if (node.contains("respawn_count")) {
        if (!node.at("respawn_count").is_number_integer()) {
            sink.Error(std::format("{}.respawn_count", path),
                       "expected an integer, where a negative value means infinite");
            ok = false;
        } else {
            const std::int64_t raw = node.at("respawn_count").get<std::int64_t>();
            if (raw < -1 || raw > 0xFFFF) {
                sink.Error(std::format("{}.respawn_count", path),
                           std::format("{} is outside -1..65535", raw));
                ok = false;
            } else {
                object.respawn_count = static_cast<std::int32_t>(raw);
            }
        }
    }

    if (!ok) {
        return false;
    }
    out = std::move(object);
    return true;
}

[[nodiscard]] bool ParseSpawn(const json& node, std::size_t index, SpawnPoint& out,
                              DiagnosticSink& sink) {
    const std::string path = std::format("spawns[{}]", index);
    if (!node.is_object()) {
        sink.Error(path, "expected an object");
        return false;
    }

    SpawnPoint spawn;
    bool ok = ReadUnsigned(node, "id", path, 0xFFFFFFFFull, true, spawn.id, sink);
    ok = ReadVec3(node, "position", path, true, spawn.position, sink) && ok;
    (void)ReadFloat(node, "yaw_degrees", path, -3600.0f, 3600.0f, false, spawn.yaw_degrees, sink);
    (void)ReadBool(node, "initial_only", path, false, spawn.initial_only, sink);
    (void)ReadString(node, "label", path, kMaxLabelLength, false, spawn.label, sink);

    if (node.contains("team")) {
        std::uint32_t team = kNeutralTeam;
        if (ReadUnsigned(node, "team", path, 0xFFull, true, team, sink)) {
            if (team != kNeutralTeam && team >= kMaxTeams) {
                sink.Error(std::format("{}.team", path),
                           std::format("team {} is outside 0..{}", team, kMaxTeams - 1));
                ok = false;
            } else {
                spawn.team = static_cast<std::uint8_t>(team);
            }
        }
    }

    if (!ok) {
        return false;
    }
    out = std::move(spawn);
    return true;
}

[[nodiscard]] bool ParseObjective(const json& node, std::size_t index, Objective& out,
                                  DiagnosticSink& sink) {
    const std::string path = std::format("objectives[{}]", index);
    if (!node.is_object()) {
        sink.Error(path, "expected an object");
        return false;
    }

    Objective objective;
    bool ok = ReadUnsigned(node, "id", path, 0xFFFFFFFFull, true, objective.id, sink);
    ok = ReadVec3(node, "position", path, true, objective.position, sink) && ok;
    ok = ReadRotation(node, path, objective.rotation, sink) && ok;
    ok = ReadShape(node, path, objective.shape, sink) && ok;
    (void)ReadString(node, "label", path, kMaxLabelLength, false, objective.label, sink);

    std::string kind_text;
    if (!ReadString(node, "kind", path, 32, true, kind_text, sink)) {
        ok = false;
    } else if (!ParseObjectiveKind(kind_text, objective.kind)) {
        sink.Error(std::format("{}.kind", path),
                   std::format("'{}' is not one of flag_stand, ball_spawn, hill_marker, "
                               "territory_marker",
                               kind_text));
        ok = false;
    }

    if (node.contains("team")) {
        std::uint32_t team = kNeutralTeam;
        if (ReadUnsigned(node, "team", path, 0xFFull, true, team, sink)) {
            if (team != kNeutralTeam && team >= kMaxTeams) {
                sink.Error(std::format("{}.team", path),
                           std::format("team {} is outside 0..{}", team, kMaxTeams - 1));
                ok = false;
            } else {
                objective.team = static_cast<std::uint8_t>(team);
            }
        }
    }

    if (!ok) {
        return false;
    }
    out = std::move(objective);
    return true;
}

[[nodiscard]] bool ParseBoundary(const json& node, std::size_t index, BoundaryVolume& out,
                                 DiagnosticSink& sink) {
    const std::string path = std::format("boundaries[{}]", index);
    if (!node.is_object()) {
        sink.Error(path, "expected an object");
        return false;
    }

    BoundaryVolume boundary;
    bool ok = ReadUnsigned(node, "id", path, 0xFFFFFFFFull, true, boundary.id, sink);
    ok = ReadVec3(node, "min", path, true, boundary.min, sink) && ok;
    ok = ReadVec3(node, "max", path, true, boundary.max, sink) && ok;
    (void)ReadString(node, "name", path, kMaxNameLength, false, boundary.name, sink);

    std::string kind_text;
    if (!ReadString(node, "kind", path, 16, true, kind_text, sink)) {
        ok = false;
    } else if (!ParseBoundaryKind(kind_text, boundary.kind)) {
        sink.Error(std::format("{}.kind", path),
                   std::format("'{}' is not one of playable, soft_kill, hard_kill", kind_text));
        ok = false;
    }

    if (ok && (boundary.min.x >= boundary.max.x || boundary.min.y >= boundary.max.y ||
               boundary.min.z >= boundary.max.z)) {
        sink.Error(path, "min must be strictly less than max on every axis");
        ok = false;
    }

    if (!ok) {
        return false;
    }
    out = std::move(boundary);
    return true;
}

/// Reports duplicate ids within one collection. Ids must be unique because they
/// are the stable handle an editor and a diff rely on.
template <typename Element>
void CheckUniqueIds(const std::vector<Element>& elements, std::string_view collection,
                    DiagnosticSink& sink) {
    std::unordered_set<std::uint32_t> seen;
    seen.reserve(elements.size());
    for (std::size_t i = 0; i < elements.size(); ++i) {
        if (!seen.insert(elements[i].id).second) {
            sink.Error(std::format("{}[{}].id", collection, i),
                       std::format("id {} is used more than once in {}", elements[i].id,
                                   collection));
        }
    }
}

// ---------------------------------------------------------------------------
// Canonical binary helpers
// ---------------------------------------------------------------------------

void WriteVec3(ByteWriter& writer, const Vec3& value) {
    writer.WriteFloat(value.x);
    writer.WriteFloat(value.y);
    writer.WriteFloat(value.z);
}

[[nodiscard]] bool ReadVec3(ByteReader& reader, Vec3& out) {
    return reader.ReadFloat(out.x) && reader.ReadFloat(out.y) && reader.ReadFloat(out.z);
}

void WriteQuat(ByteWriter& writer, const Quat& value) {
    writer.WriteFloat(value.x);
    writer.WriteFloat(value.y);
    writer.WriteFloat(value.z);
    writer.WriteFloat(value.w);
}

[[nodiscard]] bool ReadQuat(ByteReader& reader, Quat& out) {
    return reader.ReadFloat(out.x) && reader.ReadFloat(out.y) && reader.ReadFloat(out.z) &&
           reader.ReadFloat(out.w);
}

void WriteShape(ByteWriter& writer, const Shape& shape) {
    writer.WriteU8(static_cast<std::uint8_t>(shape.type));
    writer.WriteFloat(shape.radius);
    writer.WriteFloat(shape.width);
    writer.WriteFloat(shape.depth);
    writer.WriteFloat(shape.top);
    writer.WriteFloat(shape.bottom);
}

[[nodiscard]] bool ReadShape(ByteReader& reader, Shape& out) {
    std::uint8_t raw_type = 0;
    if (!reader.ReadU8(raw_type)) {
        return false;
    }
    if (raw_type > static_cast<std::uint8_t>(ShapeType::Box)) {
        return false;
    }
    out.type = static_cast<ShapeType>(raw_type);
    return reader.ReadFloat(out.radius) && reader.ReadFloat(out.width) &&
           reader.ReadFloat(out.depth) && reader.ReadFloat(out.top) &&
           reader.ReadFloat(out.bottom);
}

} // namespace

std::string_view ToString(Severity severity) noexcept {
    switch (severity) {
        case Severity::Error:   return "error";
        case Severity::Warning: return "warning";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

std::vector<Diagnostic> Validate(const MapVariant& variant, const ParseOptions& options) {
    DiagnosticSink sink;

    if (variant.schema_version < kMinSupportedSchemaVersion ||
        variant.schema_version > kSchemaVersion) {
        sink.Error("schema_version",
                   std::format("version {} is not supported; this build reads {} to {}",
                               variant.schema_version, kMinSupportedSchemaVersion,
                               kSchemaVersion));
    }
    if (variant.name.empty()) {
        sink.Error("name", "a map needs a name");
    }
    if (variant.base_scenario.empty()) {
        sink.Error("base_scenario", "a map must name the scenario it is built on");
    }
    if (variant.supported_modes.empty()) {
        sink.Error("supported_modes",
                   "list at least one game mode, otherwise this map can never be selected");
    }

    if (variant.objects.size() > kMaxObjects) {
        sink.Error("objects", std::format("{} objects exceeds the sandbox budget of {}",
                                          variant.objects.size(), kMaxObjects));
    }
    if (variant.spawns.size() > kMaxSpawns) {
        sink.Error("spawns", std::format("{} spawns exceeds the limit of {}",
                                         variant.spawns.size(), kMaxSpawns));
    }
    if (variant.objectives.size() > kMaxObjectives) {
        sink.Error("objectives", std::format("{} objectives exceeds the limit of {}",
                                             variant.objectives.size(), kMaxObjectives));
    }
    if (variant.boundaries.size() > kMaxBoundaries) {
        sink.Error("boundaries", std::format("{} boundaries exceeds the limit of {}",
                                             variant.boundaries.size(), kMaxBoundaries));
    }

    CheckUniqueIds(variant.objects, "objects", sink);
    CheckUniqueIds(variant.spawns, "spawns", sink);
    CheckUniqueIds(variant.objectives, "objectives", sink);
    CheckUniqueIds(variant.boundaries, "boundaries", sink);

    if (!options.validate_mode_requirements) {
        return sink.Take();
    }

    // Mode specific playability. Each check exists because its absence produces a
    // match that cannot be finished, which is the worst possible failure mode: it
    // only shows up after everyone has loaded in.
    const bool has_teams =
        std::any_of(variant.supported_modes.begin(), variant.supported_modes.end(),
                    [](engine::GameMode mode) {
                        return mode == engine::GameMode::TeamSlayer ||
                               mode == engine::GameMode::CaptureTheFlag ||
                               mode == engine::GameMode::Territories ||
                               mode == engine::GameMode::Infection;
                    });

    if (variant.spawns.size() < 2) {
        sink.Error("spawns", "at least 2 spawn points are required for any mode");
    }

    if (has_teams) {
        for (std::uint8_t team = 0; team < 2; ++team) {
            if (variant.SpawnCountForTeam(team) == 0) {
                sink.Error("spawns",
                           std::format("team {} has no spawn points, so its players would have "
                                       "nowhere to enter the map",
                                       team));
            }
        }
    }

    if (variant.SupportsMode(engine::GameMode::CaptureTheFlag)) {
        for (std::uint8_t team = 0; team < 2; ++team) {
            const std::size_t stands = variant.ObjectiveCount(ObjectiveKind::FlagStand, team);
            if (stands == 0) {
                sink.Error("objectives",
                           std::format("capture_the_flag needs a flag_stand for team {}", team));
            } else if (stands > 1) {
                sink.Warn("objectives",
                          std::format("team {} has {} flag stands; only the first is used", team,
                                      stands));
            }
        }
    }

    if (variant.SupportsMode(engine::GameMode::Oddball) &&
        variant.ObjectiveCount(ObjectiveKind::BallSpawn, kNeutralTeam) == 0) {
        sink.Error("objectives", "oddball needs at least one neutral ball_spawn");
    }

    if (variant.SupportsMode(engine::GameMode::KingOfTheHill) &&
        variant.ObjectiveCount(ObjectiveKind::HillMarker, kNeutralTeam) == 0) {
        sink.Error("objectives", "king_of_the_hill needs at least one neutral hill_marker");
    }

    if (variant.SupportsMode(engine::GameMode::Territories)) {
        std::size_t territories = 0;
        for (std::uint8_t team = 0; team <= kMaxTeams; ++team) {
            const std::uint8_t key = (team == kMaxTeams) ? kNeutralTeam : team;
            territories += variant.ObjectiveCount(ObjectiveKind::TerritoryMarker, key);
        }
        if (territories < 2) {
            sink.Error("objectives",
                       std::format("territories needs at least 2 territory markers, found {}",
                                   territories));
        }
    }

    // A playable boundary is optional, but more than one is ambiguous.
    const std::size_t playable =
        static_cast<std::size_t>(std::count_if(variant.boundaries.begin(), variant.boundaries.end(),
                                               [](const BoundaryVolume& b) {
                                                   return b.kind == BoundaryKind::Playable;
                                               }));
    if (playable > 1) {
        sink.Warn("boundaries",
                  std::format("{} playable volumes are defined; the engine uses their union",
                              playable));
    }

    return sink.Take();
}

// ---------------------------------------------------------------------------
// JSON parsing
// ---------------------------------------------------------------------------

Expected<ParseResult> ParseJsonText(std::string_view json_text, const ParseOptions& options) {
    if (json_text.size() > options.max_document_bytes) {
        return Error{ErrorCode::ValidationFailed,
                     std::format("the document is {} bytes, which exceeds the {} byte limit",
                                 json_text.size(), options.max_document_bytes)};
    }

    json root;
    try {
        // Exceptions are contained here and converted; nothing throws past this
        // function.
        root = json::parse(json_text, nullptr, true, true);
    } catch (const fe::json::parse_error& e) {
        return Error{ErrorCode::ParseError,
                     std::format("invalid JSON at byte {}: {}", e.byte, e.what())};
    } catch (const fe::json::exception& e) {
        return Error{ErrorCode::ParseError, std::format("invalid JSON: {}", e.what())};
    }

    if (!root.is_object()) {
        return Error{ErrorCode::SchemaMismatch, "the document root must be an object"};
    }

    DiagnosticSink sink;
    MapVariant     variant;

    // Schema version first: an unsupported version makes every other diagnostic
    // noise, so it short circuits.
    if (!ReadUnsigned(root, "schema_version", "", 0xFFFFFFFFull, true, variant.schema_version,
                      sink)) {
        return Error{ErrorCode::SchemaMismatch,
                     std::format("schema_version is missing or invalid{}",
                                 Summarize(sink.Take()))};
    }
    if (variant.schema_version < kMinSupportedSchemaVersion ||
        variant.schema_version > kSchemaVersion) {
        return Error{ErrorCode::SchemaMismatch,
                     std::format("map schema_version {} is not supported; this build reads {} to "
                                 "{}. Update ForgeEvolved to open this map.",
                                 variant.schema_version, kMinSupportedSchemaVersion,
                                 kSchemaVersion)};
    }

    (void)ReadString(root, "name", "", kMaxNameLength, true, variant.name, sink);
    (void)ReadString(root, "description", "", kMaxDescriptionLength, false, variant.description,
                     sink);
    (void)ReadString(root, "author_name", "", kMaxNameLength, false, variant.author_name, sink);
    (void)ReadString(root, "base_scenario", "", 256, true, variant.base_scenario, sink);
    (void)ReadUnsigned(root, "author_platform_id", "", UINT64_MAX, false,
                       variant.author_platform_id, sink);

    // Supported modes.
    if (!root.contains("supported_modes")) {
        sink.Error("supported_modes", "required field is missing");
    } else if (!root.at("supported_modes").is_array()) {
        sink.Error("supported_modes", "expected an array of mode names");
    } else {
        const json& modes = root.at("supported_modes");
        for (std::size_t i = 0; i < modes.size(); ++i) {
            if (!modes[i].is_string()) {
                sink.Error(std::format("supported_modes[{}]", i), "expected a string");
                continue;
            }
            const auto text = modes[i].get<std::string>();
            engine::GameMode mode{};
            if (!engine::ParseGameMode(text, mode)) {
                sink.Error(std::format("supported_modes[{}]", i),
                           std::format("'{}' is not a known game mode", text));
                continue;
            }
            if (variant.SupportsMode(mode)) {
                sink.Warn(std::format("supported_modes[{}]", i),
                          std::format("'{}' is listed more than once", text));
                continue;
            }
            variant.supported_modes.push_back(mode);
        }
    }

    // Collections. Each element is parsed independently so one bad entry does not
    // hide the rest.
    const auto parse_array = [&](const char* field, std::size_t limit, auto&& parse_element) {
        if (!root.contains(field)) {
            return; // Absent collections are empty, which Validate then judges.
        }
        if (!root.at(field).is_array()) {
            sink.Error(field, "expected an array");
            return;
        }
        const json& array = root.at(field);
        if (array.size() > limit) {
            sink.Error(field, std::format("{} entries exceeds the limit of {}", array.size(),
                                          limit));
            return;
        }
        for (std::size_t i = 0; i < array.size(); ++i) {
            parse_element(array[i], i);
        }
    };

    parse_array("objects", kMaxObjects, [&](const json& node, std::size_t i) {
        ObjectPlacement object;
        if (ParseObject(node, i, object, sink)) {
            variant.objects.push_back(std::move(object));
        }
    });
    parse_array("spawns", kMaxSpawns, [&](const json& node, std::size_t i) {
        SpawnPoint spawn;
        if (ParseSpawn(node, i, spawn, sink)) {
            variant.spawns.push_back(std::move(spawn));
        }
    });
    parse_array("objectives", kMaxObjectives, [&](const json& node, std::size_t i) {
        Objective objective;
        if (ParseObjective(node, i, objective, sink)) {
            variant.objectives.push_back(std::move(objective));
        }
    });
    parse_array("boundaries", kMaxBoundaries, [&](const json& node, std::size_t i) {
        BoundaryVolume boundary;
        if (ParseBoundary(node, i, boundary, sink)) {
            variant.boundaries.push_back(std::move(boundary));
        }
    });

    // Semantic validation runs on the assembled model.
    std::vector<Diagnostic> structural = sink.Take();
    std::vector<Diagnostic> semantic   = Validate(variant, options);
    structural.insert(structural.end(), std::make_move_iterator(semantic.begin()),
                      std::make_move_iterator(semantic.end()));

    const std::size_t errors = static_cast<std::size_t>(
        std::count_if(structural.begin(), structural.end(), [](const Diagnostic& d) {
            return d.severity == Severity::Error;
        }));
    const std::size_t warnings = structural.size() - errors;

    if (errors > 0 || (options.treat_warnings_as_errors && warnings > 0)) {
        return Error{ErrorCode::ValidationFailed,
                     std::format("the map has {} error(s) and {} warning(s):{}", errors, warnings,
                                 Summarize(structural))};
    }

    ParseResult result;
    result.variant     = std::move(variant);
    result.diagnostics = std::move(structural); // Warnings only at this point.
    return result;
}

Expected<ParseResult> ParseJsonFile(const std::filesystem::path& path,
                                    const ParseOptions& options) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return Error{ErrorCode::FileNotFound, std::format("no such map file: {}", path.string())};
    }

    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return Error{ErrorCode::FileReadError,
                     std::format("cannot determine the size of {}: {}", path.string(),
                                 ec.message())};
    }
    // Checked before reading so an oversized file is never loaded into memory.
    if (size > options.max_document_bytes) {
        return Error{ErrorCode::ValidationFailed,
                     std::format("{} is {} bytes, which exceeds the {} byte limit", path.string(),
                                 size, options.max_document_bytes)};
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return Error{ErrorCode::FileReadError, std::format("cannot open {}", path.string())};
    }

    std::string text;
    text.resize(static_cast<std::size_t>(size));
    stream.read(text.data(), static_cast<std::streamsize>(size));
    if (stream.bad()) {
        return Error{ErrorCode::FileReadError, std::format("error reading {}", path.string())};
    }
    // gcount rather than size: a text mode surprise or a truncated read must not
    // leave uninitialized tail bytes in the buffer.
    text.resize(static_cast<std::size_t>(stream.gcount()));

    Expected<ParseResult> parsed = ParseJsonText(text, options);
    if (!parsed.ok()) {
        return Error{parsed.error().code,
                     std::format("{}: {}", path.filename().string(), parsed.error().message)};
    }
    FE_LOG_INFO("parsed map '{}' from {} ({} objects, {} spawns, {} objectives, {} warnings)",
                parsed.value().variant.name, path.filename().string(),
                parsed.value().variant.objects.size(), parsed.value().variant.spawns.size(),
                parsed.value().variant.objectives.size(), parsed.value().diagnostics.size());
    return parsed;
}

// ---------------------------------------------------------------------------
// JSON writing
// ---------------------------------------------------------------------------

std::string WriteJson(const MapVariant& variant, bool pretty) {
    // ordered_json preserves insertion order, so the output key order is the one
    // written here rather than alphabetical. That keeps a hand edited file and a
    // tool written file diffable against each other.
    OrderedJson root;
    root["schema_version"] = variant.schema_version;
    root["name"]           = variant.name;
    if (!variant.description.empty()) {
        root["description"] = variant.description;
    }
    if (!variant.author_name.empty()) {
        root["author_name"] = variant.author_name;
    }
    if (variant.author_platform_id != 0) {
        root["author_platform_id"] = variant.author_platform_id;
    }
    root["base_scenario"] = variant.base_scenario;

    OrderedJson modes = OrderedJson::array();
    for (const engine::GameMode mode : variant.supported_modes) {
        modes.push_back(std::string(engine::ToString(mode)));
    }
    root["supported_modes"] = std::move(modes);

    const auto write_shape = [](const Shape& shape) {
        OrderedJson node;
        node["type"] = std::string(ToString(shape.type));
        switch (shape.type) {
            case ShapeType::None:
                break;
            case ShapeType::Sphere:
                node["radius"] = shape.radius;
                break;
            case ShapeType::Cylinder:
                node["radius"] = shape.radius;
                node["top"]    = shape.top;
                node["bottom"] = shape.bottom;
                break;
            case ShapeType::Box:
                node["width"]  = shape.width;
                node["depth"]  = shape.depth;
                node["top"]    = shape.top;
                node["bottom"] = shape.bottom;
                break;
        }
        return node;
    };

    OrderedJson objects = OrderedJson::array();
    for (const ObjectPlacement& object : variant.objects) {
        OrderedJson node;
        node["id"]          = object.id;
        node["palette_key"] = object.palette_key;
        node["position"]    = {object.position.x, object.position.y, object.position.z};
        node["rotation"]    = {object.rotation.x, object.rotation.y, object.rotation.z,
                               object.rotation.w};
        if (object.scale != 1.0f) {
            node["scale"] = object.scale;
        }
        node["physics"] = std::string(ToString(object.physics));
        if (object.team != kNeutralTeam) {
            node["team"] = object.team;
        }
        if (object.spawn_time_seconds != 0) {
            node["spawn_time_seconds"] = object.spawn_time_seconds;
        }
        if (!object.spawn_at_start) {
            node["spawn_at_start"] = false;
        }
        if (object.respawn_count != -1) {
            node["respawn_count"] = object.respawn_count;
        }
        if (!object.label.empty()) {
            node["label"] = object.label;
        }
        if (object.user_data != 0) {
            node["user_data"] = object.user_data;
        }
        if (object.shape.type != ShapeType::None) {
            node["shape"] = write_shape(object.shape);
        }
        objects.push_back(std::move(node));
    }
    root["objects"] = std::move(objects);

    OrderedJson spawns = OrderedJson::array();
    for (const SpawnPoint& spawn : variant.spawns) {
        OrderedJson node;
        node["id"]          = spawn.id;
        node["position"]    = {spawn.position.x, spawn.position.y, spawn.position.z};
        node["yaw_degrees"] = spawn.yaw_degrees;
        if (spawn.team != kNeutralTeam) {
            node["team"] = spawn.team;
        }
        if (spawn.initial_only) {
            node["initial_only"] = true;
        }
        if (!spawn.label.empty()) {
            node["label"] = spawn.label;
        }
        spawns.push_back(std::move(node));
    }
    root["spawns"] = std::move(spawns);

    OrderedJson objectives = OrderedJson::array();
    for (const Objective& objective : variant.objectives) {
        OrderedJson node;
        node["id"]       = objective.id;
        node["kind"]     = std::string(ToString(objective.kind));
        node["position"] = {objective.position.x, objective.position.y, objective.position.z};
        node["rotation"] = {objective.rotation.x, objective.rotation.y, objective.rotation.z,
                            objective.rotation.w};
        if (objective.team != kNeutralTeam) {
            node["team"] = objective.team;
        }
        if (objective.shape.type != ShapeType::None) {
            node["shape"] = write_shape(objective.shape);
        }
        if (!objective.label.empty()) {
            node["label"] = objective.label;
        }
        objectives.push_back(std::move(node));
    }
    root["objectives"] = std::move(objectives);

    OrderedJson boundaries = OrderedJson::array();
    for (const BoundaryVolume& boundary : variant.boundaries) {
        OrderedJson node;
        node["id"]   = boundary.id;
        node["kind"] = std::string(ToString(boundary.kind));
        if (!boundary.name.empty()) {
            node["name"] = boundary.name;
        }
        node["min"] = {boundary.min.x, boundary.min.y, boundary.min.z};
        node["max"] = {boundary.max.x, boundary.max.y, boundary.max.z};
        boundaries.push_back(std::move(node));
    }
    root["boundaries"] = std::move(boundaries);

    return pretty ? root.dump(2) : root.dump();
}

// ---------------------------------------------------------------------------
// Canonical binary
// ---------------------------------------------------------------------------

std::vector<std::byte> WriteCanonicalBinary(const MapVariant& variant) {
    // Sorted copies: collection order in the JSON must not affect the bytes, or
    // two authors of the same logical map would compute different hashes.
    std::vector<ObjectPlacement> objects = variant.objects;
    std::vector<SpawnPoint>      spawns  = variant.spawns;
    std::vector<Objective>       objectives = variant.objectives;
    std::vector<BoundaryVolume>  boundaries = variant.boundaries;

    const auto by_id = [](const auto& a, const auto& b) { return a.id < b.id; };
    std::sort(objects.begin(), objects.end(), by_id);
    std::sort(spawns.begin(), spawns.end(), by_id);
    std::sort(objectives.begin(), objectives.end(), by_id);
    std::sort(boundaries.begin(), boundaries.end(), by_id);

    std::vector<engine::GameMode> modes = variant.supported_modes;
    std::sort(modes.begin(), modes.end());

    std::vector<std::byte> buffer;
    // Header plus a generous per element estimate, so the writer does not grow
    // repeatedly for a large map.
    buffer.reserve(256 + objects.size() * 96 + spawns.size() * 48 + objectives.size() * 80 +
                   boundaries.size() * 48);

    ByteWriter writer(buffer);
    writer.WriteU32(kCanonicalMagic);
    writer.WriteU16(kCanonicalVersion);
    writer.WriteU32(variant.schema_version);
    writer.WriteString(variant.name);
    writer.WriteString(variant.description);
    writer.WriteString(variant.author_name);
    writer.WriteU64(variant.author_platform_id);
    writer.WriteString(variant.base_scenario);

    writer.WriteU8(static_cast<std::uint8_t>(modes.size()));
    for (const engine::GameMode mode : modes) {
        writer.WriteU8(static_cast<std::uint8_t>(mode));
    }

    writer.WriteU16(static_cast<std::uint16_t>(objects.size()));
    for (const ObjectPlacement& object : objects) {
        writer.WriteU32(object.id);
        writer.WriteString(object.palette_key);
        WriteVec3(writer, object.position);
        WriteQuat(writer, object.rotation);
        writer.WriteFloat(object.scale);
        writer.WriteU8(static_cast<std::uint8_t>(object.physics));
        writer.WriteU8(object.team);
        writer.WriteU16(object.spawn_time_seconds);
        writer.WriteBool(object.spawn_at_start);
        writer.WriteI32(object.respawn_count);
        writer.WriteString(object.label);
        writer.WriteU32(object.user_data);
        WriteShape(writer, object.shape);
    }

    writer.WriteU16(static_cast<std::uint16_t>(spawns.size()));
    for (const SpawnPoint& spawn : spawns) {
        writer.WriteU32(spawn.id);
        WriteVec3(writer, spawn.position);
        writer.WriteFloat(spawn.yaw_degrees);
        writer.WriteU8(spawn.team);
        writer.WriteBool(spawn.initial_only);
        writer.WriteString(spawn.label);
    }

    writer.WriteU16(static_cast<std::uint16_t>(objectives.size()));
    for (const Objective& objective : objectives) {
        writer.WriteU32(objective.id);
        writer.WriteU8(static_cast<std::uint8_t>(objective.kind));
        writer.WriteU8(objective.team);
        WriteVec3(writer, objective.position);
        WriteQuat(writer, objective.rotation);
        WriteShape(writer, objective.shape);
        writer.WriteString(objective.label);
    }

    writer.WriteU16(static_cast<std::uint16_t>(boundaries.size()));
    for (const BoundaryVolume& boundary : boundaries) {
        writer.WriteU32(boundary.id);
        writer.WriteString(boundary.name);
        writer.WriteU8(static_cast<std::uint8_t>(boundary.kind));
        WriteVec3(writer, boundary.min);
        WriteVec3(writer, boundary.max);
    }

    return buffer;
}

Expected<MapVariant> ReadCanonicalBinary(std::span<const std::byte> payload) {
    ByteReader reader(payload);

    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    if (!reader.ReadU32(magic) || !reader.ReadU16(version)) {
        return Error{ErrorCode::ParseError, "payload is too short to be a map"};
    }
    if (magic != kCanonicalMagic) {
        return Error{ErrorCode::ParseError,
                     std::format("bad magic 0x{:08X}, expected 0x{:08X}", magic, kCanonicalMagic)};
    }
    if (version != kCanonicalVersion) {
        return Error{ErrorCode::SchemaMismatch,
                     std::format("canonical format version {} is not supported by this build "
                                 "(expected {})",
                                 version, kCanonicalVersion)};
    }

    MapVariant variant;
    if (!reader.ReadU32(variant.schema_version) ||
        !reader.ReadString(variant.name, kMaxNameLength) ||
        !reader.ReadString(variant.description, kMaxDescriptionLength) ||
        !reader.ReadString(variant.author_name, kMaxNameLength) ||
        !reader.ReadU64(variant.author_platform_id) ||
        !reader.ReadString(variant.base_scenario, 256)) {
        return Error{ErrorCode::ParseError, "map header is malformed"};
    }

    std::uint8_t mode_count = 0;
    if (!reader.ReadU8(mode_count)) {
        return Error{ErrorCode::ParseError, "mode count is missing"};
    }
    // Every count is validated against its ceiling before it drives a loop or a
    // reserve, so a hostile payload cannot force a large allocation.
    if (mode_count > 16) {
        return Error{ErrorCode::ParseError,
                     std::format("mode count {} is implausible", mode_count)};
    }
    variant.supported_modes.reserve(mode_count);
    for (std::uint8_t i = 0; i < mode_count; ++i) {
        std::uint8_t raw = 0;
        if (!reader.ReadU8(raw)) {
            return Error{ErrorCode::ParseError, "mode list is truncated"};
        }
        if (raw > static_cast<std::uint8_t>(engine::GameMode::Infection)) {
            return Error{ErrorCode::ParseError, std::format("unknown game mode {}", raw)};
        }
        variant.supported_modes.push_back(static_cast<engine::GameMode>(raw));
    }

    std::uint16_t object_count = 0;
    if (!reader.ReadU16(object_count)) {
        return Error{ErrorCode::ParseError, "object count is missing"};
    }
    if (object_count > kMaxObjects) {
        return Error{ErrorCode::ParseError,
                     std::format("object count {} exceeds the budget of {}", object_count,
                                 kMaxObjects)};
    }
    variant.objects.reserve(object_count);
    for (std::uint16_t i = 0; i < object_count; ++i) {
        ObjectPlacement object;
        std::uint8_t raw_physics = 0;
        if (!reader.ReadU32(object.id) ||
            !reader.ReadString(object.palette_key, kMaxPaletteKeyLength) ||
            !ReadVec3(reader, object.position) || !ReadQuat(reader, object.rotation) ||
            !reader.ReadFloat(object.scale) || !reader.ReadU8(raw_physics) ||
            !reader.ReadU8(object.team) || !reader.ReadU16(object.spawn_time_seconds) ||
            !reader.ReadBool(object.spawn_at_start) || !reader.ReadI32(object.respawn_count) ||
            !reader.ReadString(object.label, kMaxLabelLength) ||
            !reader.ReadU32(object.user_data) || !ReadShape(reader, object.shape)) {
            return Error{ErrorCode::ParseError, std::format("object {} is malformed", i)};
        }
        if (raw_physics > static_cast<std::uint8_t>(PhysicsMode::Phased)) {
            return Error{ErrorCode::ParseError,
                         std::format("object {} has unknown physics mode {}", i, raw_physics)};
        }
        object.physics = static_cast<PhysicsMode>(raw_physics);
        variant.objects.push_back(std::move(object));
    }

    std::uint16_t spawn_count = 0;
    if (!reader.ReadU16(spawn_count)) {
        return Error{ErrorCode::ParseError, "spawn count is missing"};
    }
    if (spawn_count > kMaxSpawns) {
        return Error{ErrorCode::ParseError,
                     std::format("spawn count {} exceeds the limit of {}", spawn_count,
                                 kMaxSpawns)};
    }
    variant.spawns.reserve(spawn_count);
    for (std::uint16_t i = 0; i < spawn_count; ++i) {
        SpawnPoint spawn;
        if (!reader.ReadU32(spawn.id) || !ReadVec3(reader, spawn.position) ||
            !reader.ReadFloat(spawn.yaw_degrees) || !reader.ReadU8(spawn.team) ||
            !reader.ReadBool(spawn.initial_only) ||
            !reader.ReadString(spawn.label, kMaxLabelLength)) {
            return Error{ErrorCode::ParseError, std::format("spawn {} is malformed", i)};
        }
        variant.spawns.push_back(std::move(spawn));
    }

    std::uint16_t objective_count = 0;
    if (!reader.ReadU16(objective_count)) {
        return Error{ErrorCode::ParseError, "objective count is missing"};
    }
    if (objective_count > kMaxObjectives) {
        return Error{ErrorCode::ParseError,
                     std::format("objective count {} exceeds the limit of {}", objective_count,
                                 kMaxObjectives)};
    }
    variant.objectives.reserve(objective_count);
    for (std::uint16_t i = 0; i < objective_count; ++i) {
        Objective objective;
        std::uint8_t raw_kind = 0;
        if (!reader.ReadU32(objective.id) || !reader.ReadU8(raw_kind) ||
            !reader.ReadU8(objective.team) || !ReadVec3(reader, objective.position) ||
            !ReadQuat(reader, objective.rotation) || !ReadShape(reader, objective.shape) ||
            !reader.ReadString(objective.label, kMaxLabelLength)) {
            return Error{ErrorCode::ParseError, std::format("objective {} is malformed", i)};
        }
        if (raw_kind > static_cast<std::uint8_t>(ObjectiveKind::TerritoryMarker)) {
            return Error{ErrorCode::ParseError,
                         std::format("objective {} has unknown kind {}", i, raw_kind)};
        }
        objective.kind = static_cast<ObjectiveKind>(raw_kind);
        variant.objectives.push_back(std::move(objective));
    }

    std::uint16_t boundary_count = 0;
    if (!reader.ReadU16(boundary_count)) {
        return Error{ErrorCode::ParseError, "boundary count is missing"};
    }
    if (boundary_count > kMaxBoundaries) {
        return Error{ErrorCode::ParseError,
                     std::format("boundary count {} exceeds the limit of {}", boundary_count,
                                 kMaxBoundaries)};
    }
    variant.boundaries.reserve(boundary_count);
    for (std::uint16_t i = 0; i < boundary_count; ++i) {
        BoundaryVolume boundary;
        std::uint8_t raw_kind = 0;
        if (!reader.ReadU32(boundary.id) || !reader.ReadString(boundary.name, kMaxNameLength) ||
            !reader.ReadU8(raw_kind) || !ReadVec3(reader, boundary.min) ||
            !ReadVec3(reader, boundary.max)) {
            return Error{ErrorCode::ParseError, std::format("boundary {} is malformed", i)};
        }
        if (raw_kind > static_cast<std::uint8_t>(BoundaryKind::HardKill)) {
            return Error{ErrorCode::ParseError,
                         std::format("boundary {} has unknown kind {}", i, raw_kind)};
        }
        boundary.kind = static_cast<BoundaryKind>(raw_kind);
        variant.boundaries.push_back(std::move(boundary));
    }

    // Trailing bytes mean the payload is not what it claims to be.
    if (!reader.AtEnd()) {
        return Error{ErrorCode::ParseError,
                     std::format("{} unexpected trailing byte(s) after the map",
                                 reader.Remaining())};
    }

    // A payload that decodes still has to be playable. The same validation the
    // JSON path applies runs here, so a hostile host cannot bypass it by sending
    // binary instead of JSON.
    ParseOptions options;
    const std::vector<Diagnostic> diagnostics = Validate(variant, options);
    const std::size_t errors = static_cast<std::size_t>(
        std::count_if(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& d) {
            return d.severity == Severity::Error;
        }));
    if (errors > 0) {
        return Error{ErrorCode::ValidationFailed,
                     std::format("the received map failed validation with {} error(s):{}", errors,
                                 Summarize(diagnostics))};
    }

    return variant;
}

hash::Digest256 ComputeContentHash(const MapVariant& variant) {
    const std::vector<std::byte> canonical = WriteCanonicalBinary(variant);
    return hash::Sha256::Compute(canonical);
}

} // namespace fe::map

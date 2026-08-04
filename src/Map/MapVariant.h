// SPDX-License-Identifier: MIT
// ForgeEvolved: Map/MapVariant.h
//
// In memory model of a custom map layout.
//
// TWO REPRESENTATIONS, ONE MODEL
//
//   JSON (.fmap.json)  Authoring and interchange. Human readable, diffable,
//                      reviewable in a pull request, editable by hand, and the
//                      format Forge Studio reads and writes.
//
//   Canonical binary   Transfer and identity. Deterministic byte for byte, so
//                      the SHA-256 of a map is stable no matter which tool wrote
//                      the JSON, in what key order, or with what whitespace.
//
// The distinction matters because map identity has to be exact. If two peers
// hashed formatted JSON, a trailing newline or a reordered key would make
// identical maps appear different and block the launch. So JSON is parsed into
// this model, the model is written to canonical binary, and the hash is taken
// over that. The binary is also what travels over the wire, which keeps a
// multi megabyte pretty printed document off the network.
//
// BUDGETS
//
// The object and spawn ceilings mirror the sandbox limits the engine itself
// enforces. Rejecting an over budget map at parse time gives the author a clear
// message, instead of the engine silently dropping the objects past its limit and
// producing a map that plays differently for different people.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Engine/IEngineControl.h"

namespace fe::map {

/// Bumped only for an incompatible change. The parser accepts any version in
/// [kMinSupportedSchemaVersion, kSchemaVersion] and upgrades older documents in
/// memory, so a map authored today keeps loading after the format grows.
inline constexpr std::uint32_t kSchemaVersion             = 1;
inline constexpr std::uint32_t kMinSupportedSchemaVersion = 1;

/// Sandbox ceilings, matching the engine's own budget.
inline constexpr std::size_t kMaxObjects    = 640;
inline constexpr std::size_t kMaxSpawns     = 128;
inline constexpr std::size_t kMaxObjectives = 64;
inline constexpr std::size_t kMaxBoundaries = 32;

/// Half extent of the addressable world in engine units. A coordinate beyond
/// this is a unit conversion mistake in the authoring tool, not a valid position,
/// and is rejected rather than clamped.
inline constexpr float kWorldExtent = 100000.0f;

inline constexpr std::size_t kMaxNameLength        = 64;
inline constexpr std::size_t kMaxDescriptionLength = 512;
inline constexpr std::size_t kMaxLabelLength       = 48;
inline constexpr std::size_t kMaxPaletteKeyLength  = 96;

/// Neutral, meaning the object belongs to no team.
inline constexpr std::uint8_t kNeutralTeam = 0xFFu;
inline constexpr std::uint8_t kMaxTeams    = 8;

struct Vec3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

/// Unit quaternion. The parser normalizes on load, so downstream code may assume
/// unit length.
struct Quat {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float w{1.0f};
};

/// How the engine simulates a placed object.
enum class PhysicsMode : std::uint8_t {
    Normal = 0, ///< Gravity and collision.
    Fixed,      ///< Immovable, still collides.
    Phased,     ///< Immovable, no collision. Used for decorative geometry.
};

[[nodiscard]] std::string_view ToString(PhysicsMode mode) noexcept;
[[nodiscard]] bool ParsePhysicsMode(std::string_view text, PhysicsMode& out) noexcept;

/// Trigger volume shape, mirroring the engine's forge_object_properties_shape
/// family (none, sphere, cylinder, box) and its top, bottom, radius and width
/// parameters.
enum class ShapeType : std::uint8_t {
    None = 0,
    Sphere,
    Cylinder,
    Box,
};

[[nodiscard]] std::string_view ToString(ShapeType type) noexcept;
[[nodiscard]] bool ParseShapeType(std::string_view text, ShapeType& out) noexcept;

struct Shape {
    ShapeType type{ShapeType::None};
    float     radius{0.0f}; ///< Sphere and cylinder.
    float     width{0.0f};  ///< Box, along local X.
    float     depth{0.0f};  ///< Box, along local Y.
    float     top{0.0f};    ///< Cylinder and box, above the origin.
    float     bottom{0.0f}; ///< Cylinder and box, below the origin.
};

/// One placed object.
struct ObjectPlacement {
    /// Author assigned, unique within the variant. Stable across edits so a diff
    /// of two revisions is readable.
    std::uint32_t id{0};

    /// Stable content key, for example "weapon.rocket_launcher" or
    /// "vehicle.warthog". Resolved to an engine palette index at load time, which
    /// is what keeps a map portable across game patches that renumber palettes.
    std::string palette_key;

    Vec3  position;
    Quat  rotation;
    float scale{1.0f};

    PhysicsMode   physics{PhysicsMode::Normal};
    std::uint8_t  team{kNeutralTeam};
    std::uint16_t spawn_time_seconds{0}; ///< Zero means the engine default.
    bool          spawn_at_start{true};
    std::int32_t  respawn_count{-1};     ///< Negative means infinite.

    /// Gametype label, for example "ctf_flag_return". Empty for scenery.
    std::string   label;
    std::uint32_t user_data{0};
    Shape         shape;
};

/// A player spawn position.
struct SpawnPoint {
    std::uint32_t id{0};
    Vec3          position;
    float         yaw_degrees{0.0f};
    std::uint8_t  team{kNeutralTeam};

    /// True for a spawn used only at match start, false for a respawn point.
    bool        initial_only{false};
    std::string label;
};

/// What an objective marker is for.
enum class ObjectiveKind : std::uint8_t {
    FlagStand = 0,
    BallSpawn,
    HillMarker,
    TerritoryMarker,
};

[[nodiscard]] std::string_view ToString(ObjectiveKind kind) noexcept;
[[nodiscard]] bool ParseObjectiveKind(std::string_view text, ObjectiveKind& out) noexcept;

struct Objective {
    std::uint32_t id{0};
    ObjectiveKind kind{ObjectiveKind::FlagStand};
    std::uint8_t  team{kNeutralTeam};
    Vec3          position;
    Quat          rotation;
    Shape         shape;
    std::string   label;
};

/// What crossing a boundary does.
enum class BoundaryKind : std::uint8_t {
    Playable = 0, ///< Inside is in bounds.
    SoftKill,     ///< Countdown then death.
    HardKill,     ///< Immediate death.
};

[[nodiscard]] std::string_view ToString(BoundaryKind kind) noexcept;
[[nodiscard]] bool ParseBoundaryKind(std::string_view text, BoundaryKind& out) noexcept;

/// Axis aligned bounding volume.
struct BoundaryVolume {
    std::uint32_t id{0};
    std::string   name;
    BoundaryKind  kind{BoundaryKind::Playable};
    Vec3          min;
    Vec3          max;
};

/// A complete custom layout.
struct MapVariant {
    std::uint32_t schema_version{kSchemaVersion};

    std::string   name;
    std::string   description;
    std::string   author_name;
    std::uint64_t author_platform_id{0};

    /// Scenario this layout is built on. Dictates which base geometry loads, so
    /// it is never chosen independently of the map.
    std::string base_scenario;

    /// Modes this layout supports. A mode absent here is not offered for this
    /// map, which prevents a Slayer only layout being picked for CTF and starting
    /// a match nobody can finish.
    std::vector<engine::GameMode> supported_modes;

    std::vector<ObjectPlacement> objects;
    std::vector<SpawnPoint>      spawns;
    std::vector<Objective>       objectives;
    std::vector<BoundaryVolume>  boundaries;

    [[nodiscard]] bool SupportsMode(engine::GameMode mode) const noexcept;

    /// Number of spawns available to a team, counting neutral spawns.
    [[nodiscard]] std::size_t SpawnCountForTeam(std::uint8_t team) const noexcept;

    /// Number of objectives of a kind belonging to a team.
    [[nodiscard]] std::size_t ObjectiveCount(ObjectiveKind kind,
                                             std::uint8_t team) const noexcept;
};

} // namespace fe::map

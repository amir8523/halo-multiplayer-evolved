// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Map/MapVariantInjector.cpp
#define MPE_LOG_CATEGORY "Map.Injector"

#include "Map/MapVariantInjector.h"

#include "Core/Log.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_set>

namespace mpe::map {
namespace {

/// Converts a yaw in degrees to a quaternion about the vertical axis. Spawn
/// points are authored as a facing angle because that is what a level designer
/// thinks in; the engine wants a quaternion.
[[nodiscard]] Quat YawToQuaternion(float yaw_degrees) noexcept {
    const float half = yaw_degrees * 0.5f * 3.14159265358979323846f / 180.0f;
    return Quat{0.0f, 0.0f, std::sin(half), std::cos(half)};
}

/// Centre of an axis aligned volume.
[[nodiscard]] Vec3 CentreOf(const BoundaryVolume& boundary) noexcept {
    return Vec3{(boundary.min.x + boundary.max.x) * 0.5f,
                (boundary.min.y + boundary.max.y) * 0.5f,
                (boundary.min.z + boundary.max.z) * 0.5f};
}

[[nodiscard]] const char* MarkerKeyFor(ObjectiveKind kind) noexcept {
    switch (kind) {
        case ObjectiveKind::FlagStand:       return markers::kFlagStand;
        case ObjectiveKind::BallSpawn:       return markers::kBallSpawn;
        case ObjectiveKind::HillMarker:      return markers::kHillMarker;
        case ObjectiveKind::TerritoryMarker: return markers::kTerritoryMarker;
    }
    return markers::kFlagStand;
}

[[nodiscard]] const char* MarkerKeyFor(BoundaryKind kind) noexcept {
    switch (kind) {
        case BoundaryKind::Playable: return markers::kPlayableVolume;
        case BoundaryKind::SoftKill: return markers::kSoftKillVolume;
        case BoundaryKind::HardKill: return markers::kHardKillVolume;
    }
    return markers::kPlayableVolume;
}

} // namespace

MapVariantInjector::~MapVariantInjector() {
    // The injector owns every handle it created. Leaving them behind would leak
    // sandbox objects into the next match.
    Revert();
}

engine::SandboxPlacement MapVariantInjector::PlacementFor(const ObjectPlacement& object) {
    engine::SandboxPlacement placement;
    placement.palette_key = object.palette_key;
    placement.position[0] = object.position.x;
    placement.position[1] = object.position.y;
    placement.position[2] = object.position.z;
    placement.rotation[0] = object.rotation.x;
    placement.rotation[1] = object.rotation.y;
    placement.rotation[2] = object.rotation.z;
    placement.rotation[3] = object.rotation.w;
    placement.scale       = object.scale;

    placement.team               = object.team;
    placement.spawn_time_seconds = object.spawn_time_seconds;
    placement.spawn_at_start     = object.spawn_at_start;
    placement.physics_fixed      = (object.physics == PhysicsMode::Fixed);
    placement.physics_phased     = (object.physics == PhysicsMode::Phased);
    placement.respawn_count      = object.respawn_count;
    placement.label              = object.label;
    placement.user_data          = object.user_data;
    return placement;
}

engine::SandboxPlacement MapVariantInjector::PlacementFor(const SpawnPoint& spawn) {
    const Quat rotation = YawToQuaternion(spawn.yaw_degrees);

    engine::SandboxPlacement placement;
    placement.palette_key = spawn.initial_only ? markers::kInitialSpawn : markers::kSpawnPoint;
    placement.position[0] = spawn.position.x;
    placement.position[1] = spawn.position.y;
    placement.position[2] = spawn.position.z;
    placement.rotation[0] = rotation.x;
    placement.rotation[1] = rotation.y;
    placement.rotation[2] = rotation.z;
    placement.rotation[3] = rotation.w;

    placement.team = spawn.team;
    // Markers are immovable and non colliding: a spawn point that could be shot
    // out of place would be a match ending bug.
    placement.physics_fixed  = true;
    placement.physics_phased = true;
    placement.spawn_at_start = true;
    placement.label          = spawn.label;
    return placement;
}

engine::SandboxPlacement MapVariantInjector::PlacementFor(const Objective& objective) {
    engine::SandboxPlacement placement;
    placement.palette_key = MarkerKeyFor(objective.kind);
    placement.position[0] = objective.position.x;
    placement.position[1] = objective.position.y;
    placement.position[2] = objective.position.z;
    placement.rotation[0] = objective.rotation.x;
    placement.rotation[1] = objective.rotation.y;
    placement.rotation[2] = objective.rotation.z;
    placement.rotation[3] = objective.rotation.w;

    placement.team           = objective.team;
    placement.physics_fixed  = true;
    placement.spawn_at_start = true;
    placement.label = objective.label.empty() ? std::string(ToString(objective.kind))
                                              : objective.label;
    return placement;
}

engine::SandboxPlacement MapVariantInjector::PlacementFor(const BoundaryVolume& boundary) {
    const Vec3 centre = CentreOf(boundary);

    engine::SandboxPlacement placement;
    placement.palette_key = MarkerKeyFor(boundary.kind);
    placement.position[0] = centre.x;
    placement.position[1] = centre.y;
    placement.position[2] = centre.z;
    // Volumes are axis aligned, so the rotation stays identity.
    placement.physics_fixed  = true;
    placement.physics_phased = true;
    placement.spawn_at_start = true;
    placement.label          = boundary.name;

    // The volume's extent travels in user_data as packed half extents in whole
    // units, which is what the engine's shape fields consume. Clamped so a large
    // volume saturates rather than wrapping.
    const auto pack = [](float extent) -> std::uint32_t {
        const float half = std::max(0.0f, extent * 0.5f);
        return static_cast<std::uint32_t>(std::min(half, 1023.0f));
    };
    placement.user_data = (pack(boundary.max.x - boundary.min.x) << 20) |
                          (pack(boundary.max.y - boundary.min.y) << 10) |
                          pack(boundary.max.z - boundary.min.z);
    return placement;
}

Result MapVariantInjector::ResolveAllPaletteKeys(const MapVariant& variant) const {
    // Deduplicated so a map with 200 identical crates performs one lookup.
    std::unordered_set<std::string> keys;
    keys.reserve(variant.objects.size() + 8);

    for (const ObjectPlacement& object : variant.objects) {
        keys.insert(object.palette_key);
    }
    for (const SpawnPoint& spawn : variant.spawns) {
        keys.insert(spawn.initial_only ? markers::kInitialSpawn : markers::kSpawnPoint);
    }
    for (const Objective& objective : variant.objectives) {
        keys.insert(MarkerKeyFor(objective.kind));
    }
    for (const BoundaryVolume& boundary : variant.boundaries) {
        keys.insert(MarkerKeyFor(boundary.kind));
    }

    std::vector<std::string> unresolved;
    for (const std::string& key : keys) {
        if (!engine_.ResolvePaletteIndex(key).ok()) {
            unresolved.push_back(key);
        }
    }

    if (unresolved.empty()) {
        return Result::Success();
    }

    // Sorted so the message is stable between runs and easy to compare against a
    // previous report.
    std::sort(unresolved.begin(), unresolved.end());
    std::string joined;
    for (std::size_t i = 0; i < unresolved.size(); ++i) {
        joined += unresolved[i];
        if (i + 1 < unresolved.size()) {
            joined += ", ";
        }
    }
    return Result::Fail(
        ErrorCode::NotFound,
        std::format("scenario '{}' does not provide {} of the required content key(s): {}",
                    variant.base_scenario, unresolved.size(), joined));
}

Result MapVariantInjector::Place(const engine::SandboxPlacement& placement) {
    Expected<engine::SandboxObjectHandle> handle = engine_.SpawnSandboxObject(placement);
    if (!handle.ok()) {
        return Result{handle.error()};
    }
    if (handle.value() == engine::SandboxObjectHandle::Invalid) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("the engine returned an invalid handle for '{}'",
                                        placement.palette_key));
    }
    spawned_.push_back(handle.value());
    return Result::Success();
}

Expected<InjectionReport> MapVariantInjector::Apply(const MapVariant& variant) {
    const engine::EngineCapabilities capabilities = engine_.Capabilities();
    if (!capabilities.can_place_sandbox_objects) {
        return Error{ErrorCode::InvalidState,
                     "this game build cannot place sandbox objects; custom layouts are "
                     "unavailable"};
    }

    // Anything from a previous apply goes first, so two applies in a row cannot
    // stack layouts.
    Revert();
    MPE_TRY_EXPECTED(engine_.ClearSandbox());

    // Phase 1. Nothing is placed until every key is known to resolve.
    MPE_TRY_EXPECTED(ResolveAllPaletteKeys(variant));

    InjectionReport report;

    // Phase 2. Any failure unwinds the whole apply.
    const auto place_or_unwind = [&](const engine::SandboxPlacement& placement,
                                     std::size_t& counter) -> Result {
        const Result placed = Place(placement);
        if (!placed.ok()) {
            return placed;
        }
        ++counter;
        return Result::Success();
    };

    for (const ObjectPlacement& object : variant.objects) {
        if (const Result placed = place_or_unwind(PlacementFor(object), report.objects_placed);
            !placed.ok()) {
            MPE_LOG_ERROR("placing object {} ('{}') failed: {}", object.id, object.palette_key,
                         placed.message());
            Revert();
            return Error{placed.error()};
        }
    }
    for (const SpawnPoint& spawn : variant.spawns) {
        if (const Result placed = place_or_unwind(PlacementFor(spawn), report.markers_placed);
            !placed.ok()) {
            MPE_LOG_ERROR("placing spawn {} failed: {}", spawn.id, placed.message());
            Revert();
            return Error{placed.error()};
        }
    }
    for (const Objective& objective : variant.objectives) {
        if (const Result placed = place_or_unwind(PlacementFor(objective), report.markers_placed);
            !placed.ok()) {
            MPE_LOG_ERROR("placing objective {} failed: {}", objective.id, placed.message());
            Revert();
            return Error{placed.error()};
        }
    }
    for (const BoundaryVolume& boundary : variant.boundaries) {
        if (const Result placed = place_or_unwind(PlacementFor(boundary), report.volumes_placed);
            !placed.ok()) {
            MPE_LOG_ERROR("placing boundary {} failed: {}", boundary.id, placed.message());
            Revert();
            return Error{placed.error()};
        }
    }

    // Advisory notes that do not justify failing an otherwise good map.
    if (variant.objects.size() > kMaxObjects * 3 / 4) {
        report.warnings.push_back(
            std::format("{} of {} sandbox objects used; performance may suffer on lower end "
                        "machines",
                        variant.objects.size(), kMaxObjects));
    }
    if (variant.boundaries.empty()) {
        report.warnings.push_back(
            "no boundary volumes are defined, so players can leave the intended play space");
    }

    MPE_LOG_INFO("applied map '{}': {} object(s), {} marker(s), {} volume(s)", variant.name,
                report.objects_placed, report.markers_placed, report.volumes_placed);
    return report;
}

void MapVariantInjector::Revert() {
    if (spawned_.empty()) {
        return;
    }

    // Reverse order so anything the engine created as a dependency of a later
    // object is removed after its dependent.
    std::size_t failures = 0;
    for (auto it = spawned_.rbegin(); it != spawned_.rend(); ++it) {
        if (const Result removed = engine_.DespawnSandboxObject(*it); !removed.ok()) {
            ++failures;
        }
    }
    if (failures > 0) {
        // Not fatal: ClearSandbox at the start of the next apply is the backstop.
        MPE_LOG_WARN("{} of {} sandbox object(s) could not be despawned", failures,
                    spawned_.size());
    } else {
        MPE_LOG_DEBUG("reverted {} sandbox object(s)", spawned_.size());
    }
    spawned_.clear();
}

} // namespace mpe::map

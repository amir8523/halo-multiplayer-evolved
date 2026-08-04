// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Map/MapVariantInjector.h
//
// Applies a validated MapVariant to the live game world.
//
// TWO PHASE, TRANSACTIONAL
//
// Phase 1 resolves every palette key against the loaded scenario without placing
// anything. If any key is unresolvable the whole apply fails and the world is
// untouched. This matters because a half applied map is worse than no map: the
// host would be playing a layout that no client can reproduce, and the engine's
// replication would spend the match fighting the difference.
//
// Phase 2 places objects, recording every handle. Any failure triggers a rollback
// that despawns everything this apply created, returning the scenario to its base
// state.
//
// The engine's own map variant loader is preferred when it is available, since it
// is what the shipped netcode already synchronizes. This injector is the path for
// layouts assembled at runtime, and for the Forge Studio live preview where a map
// is being edited rather than loaded.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "Core/Result.h"
#include "Engine/IEngineControl.h"
#include "Map/MapVariant.h"

namespace mpe::map {

/// Palette keys the injector uses for gameplay markers. Defined here so the
/// authoring tool, the parser and the injector cannot disagree.
namespace markers {
inline constexpr const char* kSpawnPoint      = "marker.spawn_point";
inline constexpr const char* kInitialSpawn    = "marker.initial_spawn";
inline constexpr const char* kFlagStand       = "marker.flag_stand";
inline constexpr const char* kBallSpawn       = "marker.ball_spawn";
inline constexpr const char* kHillMarker      = "marker.hill_marker";
inline constexpr const char* kTerritoryMarker = "marker.territory_marker";
inline constexpr const char* kSoftKillVolume  = "volume.soft_kill";
inline constexpr const char* kHardKillVolume  = "volume.hard_kill";
inline constexpr const char* kPlayableVolume  = "volume.playable";
} // namespace markers

/// Outcome of an apply.
struct InjectionReport {
    std::size_t objects_placed{0};
    std::size_t markers_placed{0};
    std::size_t volumes_placed{0};

    /// Non fatal notes, for example an object whose spawn time the engine clamped.
    std::vector<std::string> warnings;

    [[nodiscard]] std::size_t TotalPlaced() const noexcept {
        return objects_placed + markers_placed + volumes_placed;
    }
};

class MapVariantInjector {
public:
    explicit MapVariantInjector(engine::IEngineControl& engine) noexcept : engine_(engine) {}

    ~MapVariantInjector();

    MapVariantInjector(const MapVariantInjector&)            = delete;
    MapVariantInjector& operator=(const MapVariantInjector&) = delete;

    /// Clears the sandbox, then applies the variant. All or nothing.
    ///
    /// Preconditions: the base scenario named by the variant is loaded, and the
    /// engine reports can_place_sandbox_objects.
    [[nodiscard]] Expected<InjectionReport> Apply(const MapVariant& variant);

    /// Despawns everything the last successful Apply created. Called
    /// automatically on failure and from the destructor.
    void Revert();

    [[nodiscard]] std::size_t PlacedCount() const noexcept { return spawned_.size(); }

private:
    /// Phase 1. Returns the first unresolvable key, if any.
    [[nodiscard]] Result ResolveAllPaletteKeys(const MapVariant& variant) const;

    /// Places one object, recording its handle for rollback.
    [[nodiscard]] Result Place(const engine::SandboxPlacement& placement);

    /// Builds the placement for a spawn point, objective or boundary.
    [[nodiscard]] static engine::SandboxPlacement PlacementFor(const ObjectPlacement& object);
    [[nodiscard]] static engine::SandboxPlacement PlacementFor(const SpawnPoint& spawn);
    [[nodiscard]] static engine::SandboxPlacement PlacementFor(const Objective& objective);
    [[nodiscard]] static engine::SandboxPlacement PlacementFor(const BoundaryVolume& boundary);

    engine::IEngineControl&                        engine_;
    std::vector<engine::SandboxObjectHandle>       spawned_;
};

} // namespace mpe::map

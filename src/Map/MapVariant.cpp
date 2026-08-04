// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Map/MapVariant.cpp
#include "Map/MapVariant.h"

#include <algorithm>

namespace mpe::map {

std::string_view ToString(PhysicsMode mode) noexcept {
    switch (mode) {
        case PhysicsMode::Normal: return "normal";
        case PhysicsMode::Fixed:  return "fixed";
        case PhysicsMode::Phased: return "phased";
    }
    return "normal";
}

bool ParsePhysicsMode(std::string_view text, PhysicsMode& out) noexcept {
    if (text == "normal") { out = PhysicsMode::Normal; return true; }
    if (text == "fixed")  { out = PhysicsMode::Fixed;  return true; }
    if (text == "phased") { out = PhysicsMode::Phased; return true; }
    return false;
}

std::string_view ToString(ShapeType type) noexcept {
    switch (type) {
        case ShapeType::None:     return "none";
        case ShapeType::Sphere:   return "sphere";
        case ShapeType::Cylinder: return "cylinder";
        case ShapeType::Box:      return "box";
    }
    return "none";
}

bool ParseShapeType(std::string_view text, ShapeType& out) noexcept {
    if (text == "none")     { out = ShapeType::None;     return true; }
    if (text == "sphere")   { out = ShapeType::Sphere;   return true; }
    if (text == "cylinder") { out = ShapeType::Cylinder; return true; }
    if (text == "box")      { out = ShapeType::Box;      return true; }
    return false;
}

std::string_view ToString(ObjectiveKind kind) noexcept {
    switch (kind) {
        case ObjectiveKind::FlagStand:       return "flag_stand";
        case ObjectiveKind::BallSpawn:       return "ball_spawn";
        case ObjectiveKind::HillMarker:      return "hill_marker";
        case ObjectiveKind::TerritoryMarker: return "territory_marker";
    }
    return "flag_stand";
}

bool ParseObjectiveKind(std::string_view text, ObjectiveKind& out) noexcept {
    if (text == "flag_stand")       { out = ObjectiveKind::FlagStand;       return true; }
    if (text == "ball_spawn")       { out = ObjectiveKind::BallSpawn;       return true; }
    if (text == "hill_marker")      { out = ObjectiveKind::HillMarker;      return true; }
    if (text == "territory_marker") { out = ObjectiveKind::TerritoryMarker; return true; }
    return false;
}

std::string_view ToString(BoundaryKind kind) noexcept {
    switch (kind) {
        case BoundaryKind::Playable:  return "playable";
        case BoundaryKind::SoftKill:  return "soft_kill";
        case BoundaryKind::HardKill:  return "hard_kill";
    }
    return "playable";
}

bool ParseBoundaryKind(std::string_view text, BoundaryKind& out) noexcept {
    if (text == "playable")  { out = BoundaryKind::Playable; return true; }
    if (text == "soft_kill") { out = BoundaryKind::SoftKill; return true; }
    if (text == "hard_kill") { out = BoundaryKind::HardKill; return true; }
    return false;
}

bool MapVariant::SupportsMode(engine::GameMode mode) const noexcept {
    return std::find(supported_modes.begin(), supported_modes.end(), mode) !=
           supported_modes.end();
}

std::size_t MapVariant::SpawnCountForTeam(std::uint8_t team) const noexcept {
    return static_cast<std::size_t>(
        std::count_if(spawns.begin(), spawns.end(), [team](const SpawnPoint& spawn) {
            // A neutral spawn is available to every team, so it counts toward each.
            return spawn.team == team || spawn.team == kNeutralTeam;
        }));
}

std::size_t MapVariant::ObjectiveCount(ObjectiveKind kind, std::uint8_t team) const noexcept {
    return static_cast<std::size_t>(
        std::count_if(objectives.begin(), objectives.end(),
                      [kind, team](const Objective& objective) {
                          return objective.kind == kind && objective.team == team;
                      }));
}

} // namespace mpe::map

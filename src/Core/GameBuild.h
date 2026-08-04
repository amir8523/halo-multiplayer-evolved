// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Core/GameBuild.h
//
// Identity of the running game build.
//
// Host and client must be on the same build. The Blam simulation replicates
// object state by index into tables baked at cook time, so a mismatched build
// does not fail cleanly, it desynchronizes and then diverges. Comparing the
// build string during the handshake turns that into an immediate, explained
// rejection.
//
// The value comes from the executable's own version resource, verified on the
// shipping build as:
//   2026.07.25.1112544.4-Rel-i343-Meteorite-2607-CU3
#pragma once

#include <string>

namespace mpe {

/// Version string of the running executable, read once and cached.
///
/// Never empty: if the version resource cannot be read, a stable synthetic value
/// derived from the executable's size and timestamp is returned instead, so two
/// identical installs still agree and two different ones still differ.
[[nodiscard]] const std::string& GameBuildString();

/// Executable directory, with no trailing separator. Used to locate the mod's
/// own data files, which install alongside the game binary.
[[nodiscard]] const std::string& ExecutableDirectory();

} // namespace mpe

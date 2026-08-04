// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Update/UpdateCheck.h
//
// Asks GitHub what the newest published release is.
//
// WHY THIS EXISTS
//
// A mod that talks to other players over the network is only useful when everybody is
// running a build that agrees on the protocol. Telling a player their copy is out of date
// is the cheapest way to prevent a session that fails for reasons nobody can see, so the
// lobby reports it and the loader can act on it.
//
// Deliberately small: one HTTPS GET against the public releases endpoint, parsed with the
// project's own JSON reader. No third party dependency, no token, nothing written to disk.
#pragma once

#include <functional>
#include <string>

#include "Core/Result.h"

namespace mpe::update {

/// A published release, as GitHub describes it.
struct ReleaseInfo {
    /// The tag with any leading v removed, so it compares against the built in version.
    std::string version;
    /// Direct download for the release asset, empty when the release publishes none.
    std::string download_url;
    std::string asset_name;
    long long   asset_bytes{0};
};

/// Fetches the newest release for a repository, for example "k3sra/halo-multiplayer-evolved".
///
/// Blocking: performs network I/O and must not be called from the game thread. Fails rather
/// than throwing, and a failure is not an error worth surfacing to the player: being unable
/// to reach GitHub means the version is simply unknown, not that anything is wrong.
[[nodiscard]] Expected<ReleaseInfo> FetchLatestRelease(std::string_view repository);

/// Downloads a release asset next to the running mod, ready to be applied.
///
/// The file is written as `MultiplayerEvolved.dll.pending` rather than over the mod itself: the
/// mod is loaded and executing while this runs, and Windows will not let a mapped image be
/// replaced. The loader swaps it in at the next start, before anything is loaded, which is
/// the only moment the file is not in use.
///
/// Downloads to a temporary name and renames on success, so an interrupted download cannot
/// leave a half written DLL in the position the loader will pick up.
///
/// progress is called with (bytes so far, total bytes) and may be empty. Blocking; must not
/// run on the game thread.
[[nodiscard]] Result DownloadRelease(const ReleaseInfo& release,
                                     const std::wstring& game_binaries_directory,
                                     const std::function<void(long long, long long)>& progress);

/// Compares two dotted version strings.
///
/// Returns true when candidate is newer than current. Compares numerically component by
/// component, so 0.10.0 is correctly newer than 0.9.0, which a string comparison would get
/// backwards.
[[nodiscard]] bool IsNewer(std::string_view candidate, std::string_view current);

} // namespace mpe::update

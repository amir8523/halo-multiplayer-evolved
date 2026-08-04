// SPDX-License-Identifier: MIT
// ForgeEvolved: Unreal/NamePool.h
//
// Locates Unreal Engine 5's FName pool inside the running executable and resolves
// name indices to text.
//
// WHY THIS IS THE KEYSTONE
//
// The gameplay simulation is Blam, but the shell is UE5, and the shell is where the
// game engine variant system actually lives. Reflected type and property names found
// in the executable include:
//
//   BlamGameEngineSocialOptions     BlamSocialOptionsFlags
//   BlamGameEnginePlayerTraits      BlamPlayerTraitVitality / Weapons / Movement
//   BlamGameEngineTimer             BlamPlayerRespawn
//   BlamGameEngineBaseVariantStorage
//   bFriendlyFireEnabled            bFriendlyFire
//
// bFriendlyFireEnabled is the control that decides whether players can damage each
// other. An earlier conclusion in this project said no such control existed; that was
// wrong, and it was wrong because only the Blam DLL's descriptor tables were searched.
// The control is a reflected UE property, not Blam data.
//
// Reaching reflected properties requires two things: the object array, and the name
// pool that gives every object and property its identity. This file is the second, and
// it comes first because the object array is validated by checking that the names it
// yields are sane.
//
// HOW THE POOL IS FOUND
//
// No hardcoded offsets, same rule as the rest of this project. UE5 stores names in
// FNamePool, whose entry allocator holds an array of block pointers. Every entry begins
// with a 16 bit header:
//
//   struct FNameEntryHeader { uint16 bIsWide : 1; uint16 LowercaseProbeHash : 5;
//                             uint16 Len : 10; };
//
// so Len is header >> 6. Index 0 of block 0 is always the name "None", which gives an
// unambiguous fingerprint: a 16 bit header whose length field reads 4, immediately
// followed by the bytes "None". Scanning for a pointer to such a block locates
// Blocks[0], and the block array base with it.
//
// From there an FName index resolves arithmetically:
//   block  = index >> FNameBlockOffsetBits
//   offset = (index & block mask) * stride
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Result.h"

namespace fe::unreal {

/// UE5 constants. These are compile time constants of the engine, not addresses, so
/// they are stable across builds of the same engine major version.
inline constexpr std::uint32_t kNameBlockOffsetBits = 16;
inline constexpr std::uint32_t kNameBlockOffsetMask = (1u << kNameBlockOffsetBits) - 1u;
inline constexpr std::size_t   kNameEntryStride     = 2; ///< Entries are 2 byte aligned.
inline constexpr std::size_t   kNameMaxBlocks       = 8192;

/// Largest plausible name length, used to reject a false positive block.
inline constexpr std::uint16_t kMaxNameLength = 1024;

/// One resolved name.
struct NameEntry {
    std::uint32_t index{0};
    std::string   text;
    bool          wide{false};
};

/// Read only access to the engine's name pool.
class NamePool {
public:
    /// Scans the host executable for the pool. Returns a failure when the fingerprint
    /// is not found, which means the engine version changed its layout and this needs
    /// revisiting rather than that the mod should guess.
    [[nodiscard]] static Expected<NamePool> Locate();

    /// Builds a pool from an already known Blocks address.
    ///
    /// Locate searches the heap and takes around ten seconds, which is far too slow to
    /// finish before the game builds its menu. The address can instead be read from an
    /// instruction that references it, which costs one pass over the static image. The
    /// result is still verified the same way, so a wrong address is rejected rather than
    /// trusted.
    [[nodiscard]] static Expected<NamePool> FromBlocks(std::uintptr_t blocks_address);

    /// Address of the Blocks array, for the log and for diagnostics.
    [[nodiscard]] std::uintptr_t BlocksAddress() const noexcept { return blocks_address_; }

    /// How many block pointers are non null. Grows as the game loads content.
    [[nodiscard]] std::size_t PopulatedBlockCount() const;

    /// Resolves one FName index. Fails for an index whose block is absent or whose
    /// header is implausible, rather than returning garbage.
    [[nodiscard]] Expected<std::string> Resolve(std::uint32_t index) const;

    /// Walks a block sequentially, returning up to max_entries names.
    ///
    /// Sequential walking is how the pool is verified: the first names in block 0 are
    /// always engine intrinsics, so seeing them proves the location is correct.
    [[nodiscard]] std::vector<NameEntry> DumpBlock(std::uint32_t block,
                                                  std::size_t max_entries = 64) const;

    /// Finds the index of an exact name by walking populated blocks.
    ///
    /// Linear, and only intended for a handful of lookups during bring up. A real
    /// implementation would use the engine's own hash table; this is deliberately the
    /// simple version because correctness matters more than speed here.
    [[nodiscard]] Expected<std::uint32_t> FindIndexOf(std::string_view name,
                                                      std::size_t max_blocks_to_search = 64) const;

private:
    NamePool() = default;

    /// Reads a block pointer, or zero when the slot is empty or unreadable.
    [[nodiscard]] std::uintptr_t BlockAt(std::uint32_t block) const;

    std::uintptr_t blocks_address_{0}; ///< Address of Blocks[0].
};

} // namespace fe::unreal

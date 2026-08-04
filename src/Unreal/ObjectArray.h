// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Unreal/ObjectArray.h
//
// Locates UE5's global UObject array and reads live objects out of it.
//
// WHY THIS IS THE SECOND KEYSTONE
//
// NamePool gives identity. This gives instances. Together they are UE reflection, and
// reflection is how the game engine variant becomes reachable, because the variant,
// its player traits and its social options are all reflected UE objects rather than
// Blam data:
//
//   BlamGameEngineBaseVariantStorage   resolved to FName 67359
//   BlamGameEngineSocialOptions        resolved to FName 67344
//   BlamGameEnginePlayerTraits         resolved to FName 67318
//   BlamPlayerRespawn                  resolved to FName 67486
//
// With the object array we can find the live instance of each, and with the property
// layout we can read and write its fields. bFriendlyFireEnabled lives in there.
//
// LAYOUTS
//
// These are UE5 engine structure layouts, not addresses, so they hold across builds of
// the same engine version and are validated at runtime rather than trusted.
//
//   FUObjectItem                       24 bytes
//     +0x00  UObjectBase* Object
//     +0x08  int32 Flags
//     +0x0C  int32 ClusterRootIndex
//     +0x10  int32 SerialNumber
//
//   FChunkedFixedUObjectArray
//     +0x00  FUObjectItem** Objects            array of chunk pointers
//     +0x08  FUObjectItem*  PreAllocatedObjects
//     +0x10  int32 MaxElements
//     +0x14  int32 NumElements
//     +0x18  int32 MaxChunks
//     +0x1C  int32 NumChunks
//
//   UObjectBase                        0x28 bytes
//     +0x00  void** VTable
//     +0x08  int32 ObjectFlags
//     +0x0C  int32 InternalIndex
//     +0x10  UClass* ClassPrivate
//     +0x18  FName NamePrivate                 uint32 index, uint32 number
//     +0x20  UObject* OuterPrivate
//
// HOW IT IS FOUND
//
// No hardcoded offsets. The array is located by fingerprint: a candidate address is
// accepted only when its element counts are internally consistent, its chunk pointers
// are readable, and the objects it yields have readable classes and names that resolve
// through the name pool to plausible identifiers. A wrong location fails that last
// test immediately, because garbage does not resolve to "Class" and "Package".
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Result.h"
#include "Unreal/NamePool.h"

namespace mpe::unreal {

/// Elements per chunk in FChunkedFixedUObjectArray. An engine constant.
inline constexpr std::uint32_t kElementsPerChunk = 64 * 1024;

/// Size of one FUObjectItem.
inline constexpr std::size_t kObjectItemSize = 24;

/// UObjectBase field offsets.
inline constexpr std::size_t kObjectClassOffset = 0x10;
inline constexpr std::size_t kObjectNameOffset  = 0x18;
inline constexpr std::size_t kObjectOuterOffset = 0x20;

/// A live object, resolved far enough to be useful.
struct ObjectInfo {
    std::uintptr_t address{0};       ///< The UObject itself.
    std::uintptr_t class_address{0}; ///< Its UClass.
    std::uintptr_t outer_address{0};
    std::uint32_t  index{0};         ///< Index in the global array.
    std::uint32_t  name_index{0};    ///< FName comparison index.
    std::string    name;
    std::string    class_name;

    [[nodiscard]] bool IsValid() const noexcept { return address != 0 && !name.empty(); }
};

/// Read only access to the engine's global object array.
///
/// Holds a reference to the name pool, which must outlive it.
class ObjectArray {
public:
    /// Scans the host executable for the array and validates it against the name pool.
    [[nodiscard]] static Expected<ObjectArray> Locate(const NamePool& names);

    /// Builds an array from an already known address, skipping the search.
    ///
    /// The candidate is still scored exactly as Locate scores one, so an address that does
    /// not actually describe the object array is rejected.
    [[nodiscard]] static Expected<ObjectArray> FromAddress(const NamePool& names,
                                                           std::uintptr_t array_address);

    /// Address of the FChunkedFixedUObjectArray, for the log.
    [[nodiscard]] std::uintptr_t Address() const noexcept { return array_address_; }

    /// Live element count, re-read each call because the game keeps allocating.
    [[nodiscard]] std::uint32_t Count() const;

    /// Capacity, for reporting.
    [[nodiscard]] std::uint32_t Capacity() const;

    /// Reads one object. Fails for an empty slot or an unreadable object, which is
    /// normal: the array is sparse and entries are destroyed during play.
    [[nodiscard]] Expected<ObjectInfo> At(std::uint32_t index) const;

    /// Calls the visitor for every readable object until it returns false.
    ///
    /// Iteration skips unreadable and empty slots silently, because in a live process
    /// a slot can be freed between the count read and the element read.
    void ForEach(const std::function<bool(const ObjectInfo&)>& visitor,
                 std::uint32_t max_to_visit = 0) const;

    /// Every object whose class name matches exactly.
    ///
    /// This is the workhorse for finding a live instance of a known type, for example
    /// every BlamGameEngineSocialOptions in memory.
    [[nodiscard]] std::vector<ObjectInfo> FindByClassName(std::string_view class_name,
                                                          std::size_t max_results = 32) const;

    /// Every object whose own name matches exactly.
    [[nodiscard]] std::vector<ObjectInfo> FindByName(std::string_view name,
                                                     std::size_t max_results = 32) const;

    /// Objects whose class name contains the given substring. Used for discovery when
    /// the exact type name is not yet known.
    [[nodiscard]] std::vector<ObjectInfo> FindByClassNameContains(
        std::string_view fragment, std::size_t max_results = 64) const;

    /// Builds the full path of an object by walking its Outer chain, which is how UE
    /// identifies an object unambiguously, for example
    /// /Script/Meteorite.BlamGameEngineSocialOptions.
    [[nodiscard]] std::string BuildPath(const ObjectInfo& object,
                                        std::size_t max_depth = 16) const;

    /// Every object of a class whose name matches, plus every subclass instance.
    ///
    /// Matching on the class name alone misses instances of derived classes, which is
    /// usually what a caller actually wants when hunting for a live component.
    [[nodiscard]] std::vector<ObjectInfo> FindInstancesOfClassAddress(
        std::uintptr_t class_address, std::size_t max_results = 64) const;

    /// Reads an object's class pointer.
    [[nodiscard]] std::uintptr_t ClassOf(std::uintptr_t object_address) const;

private:
    explicit ObjectArray(const NamePool& names) noexcept : names_(&names) {}

    /// Reads the chunk pointer for an element index.
    [[nodiscard]] std::uintptr_t ChunkFor(std::uint32_t index) const;

    /// Reads an object's fields without resolving names. Used by the validator, where
    /// resolving is the thing being tested.
    [[nodiscard]] bool ReadRaw(std::uintptr_t object, std::uintptr_t& out_class,
                               std::uint32_t& out_name_index) const;

    /// Scores a candidate array by how many of its first objects look real.
    [[nodiscard]] static std::size_t ScoreCandidate(const ObjectArray& candidate,
                                                    std::size_t sample_size);

    const NamePool* names_{nullptr};
    std::uintptr_t  array_address_{0};
};

} // namespace mpe::unreal

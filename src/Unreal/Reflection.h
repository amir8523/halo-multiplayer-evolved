// SPDX-License-Identifier: MIT
// ForgeEvolved: Unreal/Reflection.h
//
// Walks UE5 UStruct property chains to recover field names, types and offsets.
//
// WHY THIS IS THE THIRD KEYSTONE
//
// NamePool gave identity. ObjectArray gave instances. Neither tells us where a field
// lives inside a struct, and without an offset a located struct is just an address.
//
// The structs that matter were located in the running game, all eight in one pass:
//
//   /Script/BlamGlue.BlamGameEngineBaseVariantStorage
//   /Script/BlamGlue.BlamGameEngineSocialOptions
//   /Script/BlamGlue.BlamGameEnginePlayerTraits
//   /Script/BlamGlue.BlamGameEngineCampaignVariantStorage
//   /Script/BlamEngine.BlamScenarioGameOptions
//
// Finding bFriendlyFireEnabled's offset inside the social options is the concrete goal.
//
// OFFSETS ARE DETECTED, NOT ASSUMED
//
// The documented UE5 layout puts ChildProperties at 0x40 and PropertiesSize at 0x48.
// On this build that is wrong: reading BlamScenarioGameOptions that way reported size 0
// and zero fields, while its name resolved correctly, proving the UObject offsets are
// right and the UStruct ones are not.
//
// So the constants below are starting guesses only, and DetectLayout establishes the
// real values empirically. Two independent signals make detection reliable:
//
//   A property chain's nodes have names that resolve to plausible identifiers, and a
//   chain two or more nodes long with distinct resolvable names is not coincidence.
//
//   A struct's field offsets increase across that chain. Scanning for the int32 slot
//   whose values are distinct, in range, and non decreasing identifies Offset_Internal
//   without knowing the engine version.
//
// This is the same approach that located the Blam tables: anchor on content the engine
// itself depends on, then validate before trusting.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Result.h"
#include "Unreal/NamePool.h"

namespace fe::unreal {

/// Starting guesses, the documented UE5 layout. Replaced at runtime by DetectLayout.
inline constexpr std::size_t kStructSuperOffset            = 0x30;
inline constexpr std::size_t kStructChildrenOffset         = 0x38;
inline constexpr std::size_t kStructChildPropertiesOffset  = 0x40;
inline constexpr std::size_t kStructPropertiesSizeOffset   = 0x48;

inline constexpr std::size_t kFFieldClassOffset        = 0x08;
inline constexpr std::size_t kFFieldNextOffset         = 0x20;
inline constexpr std::size_t kFFieldNameOffset         = 0x28;
inline constexpr std::size_t kFPropertyElementSize     = 0x3C;
inline constexpr std::size_t kFPropertyOffsetInternal  = 0x4C;

/// UObject::NamePrivate. Confirmed correct on this build, since struct names resolve.
inline constexpr std::size_t kObjectNamePrivateOffset = 0x18;

/// Guard rails.
inline constexpr std::size_t kMaxPropertiesPerStruct = 4096;
inline constexpr std::size_t kMaxInheritanceDepth    = 64;

/// One reflected field.
struct PropertyInfo {
    std::string    name;
    std::string    type_name;
    std::uintptr_t address{0};
    std::int32_t   offset{0};
    std::int32_t   element_size{0};
    std::int32_t   array_dim{1};

    [[nodiscard]] std::int32_t TotalSize() const noexcept {
        return element_size * (array_dim > 0 ? array_dim : 1);
    }
    [[nodiscard]] bool IsBool() const noexcept { return type_name == "BoolProperty"; }
};

/// One reflected struct or class.
struct StructInfo {
    std::string    name;
    std::uintptr_t address{0};
    std::uintptr_t super_address{0};
    std::int32_t   properties_size{0};
    std::vector<PropertyInfo> properties;
};

/// The set of offsets this reader uses. Discovered at runtime.
struct ReflectionLayout {
    std::size_t child_properties_offset{kStructChildPropertiesOffset};
    std::size_t field_name_offset{kFFieldNameOffset};
    std::size_t field_next_offset{kFFieldNextOffset};
    std::size_t field_class_offset{kFFieldClassOffset};
    std::size_t offset_internal_offset{kFPropertyOffsetInternal};
    std::size_t element_size_offset{kFPropertyElementSize};
    std::size_t properties_size_offset{kStructPropertiesSizeOffset};
    std::size_t super_struct_offset{kStructSuperOffset};

    /// FStructProperty::Struct, the inner UScriptStruct a StructProperty refers to.
    /// Detected by looking for a pointer to a ScriptStruct we already located.
    std::size_t struct_property_inner_offset{0};
    bool        struct_property_inner_detected{false};

    bool        detected{false};
    std::size_t detected_chain_length{0};

    [[nodiscard]] std::string Describe() const;
};

/// Where a struct type is embedded inside an owning class or struct.
///
/// This is what makes a live instance findable: a ScriptStruct has no presence in the
/// object array, so the only way to reach one is through the object that contains it.
struct StructUsage {
    std::uintptr_t owner_address{0};   ///< The owning UClass or UScriptStruct.
    std::string    owner_name;
    std::string    owner_class_name;   ///< "Class", "ScriptStruct", ...
    std::string    property_name;      ///< The field name inside the owner.
    std::int32_t   property_offset{0}; ///< Byte offset of the embedded struct.
};

/// Reads UE5 reflection metadata.
///
/// Holds a reference to the name pool, which must outlive it.
class Reflection {
public:
    explicit Reflection(const NamePool& names) noexcept : names_(&names) {}

    [[nodiscard]] const ReflectionLayout& Layout() const noexcept { return layout_; }
    void SetLayout(const ReflectionLayout& layout) { layout_ = layout; }

    /// Determines the real offsets by inspecting a struct known to have fields.
    ///
    /// Pass several candidates: a struct with no fields of its own cannot reveal the
    /// chain layout, so the first one that yields a chain wins.
    [[nodiscard]] ReflectionLayout DetectLayout(
        const std::vector<std::uintptr_t>& candidate_structs) const;

    [[nodiscard]] Expected<StructInfo> ReadStruct(std::uintptr_t struct_address) const;
    [[nodiscard]] std::vector<PropertyInfo> ReadProperties(std::uintptr_t struct_address) const;
    [[nodiscard]] std::vector<PropertyInfo> ReadAllProperties(std::uintptr_t struct_address) const;

    [[nodiscard]] Expected<PropertyInfo> FindProperty(std::uintptr_t struct_address,
                                                      std::string_view name) const;
    [[nodiscard]] std::vector<PropertyInfo> FindPropertiesContaining(
        std::uintptr_t struct_address, std::string_view fragment) const;

    /// Confirms the active layout produces self consistent results.
    [[nodiscard]] Result VerifyLayout(std::uintptr_t struct_address,
                                     std::string& out_report) const;

    /// Annotated hex dump of a struct header, for when detection itself fails.
    [[nodiscard]] std::string ProbeStructLayout(std::uintptr_t struct_address,
                                               std::size_t bytes = 0x90) const;

    /// Determines FStructProperty::Struct's offset.
    ///
    /// Detected rather than assumed, using the same trick as everything else here: we
    /// already know several ScriptStruct addresses, so the correct offset is the one
    /// where a StructProperty's pointer lands on one of them.
    ///
    /// known_script_structs maps address to name, and owner_structs are structs whose
    /// StructProperty fields should point into that set.
    [[nodiscard]] std::size_t DetectStructPropertyInnerOffset(
        const std::vector<std::uintptr_t>& owner_structs,
        const std::vector<std::uintptr_t>& known_script_structs) const;

    /// Reads the inner UScriptStruct a StructProperty refers to, or zero.
    [[nodiscard]] std::uintptr_t ResolveStructPropertyInner(
        std::uintptr_t property_address) const;

    /// Refines properties_size_offset using several structs at once.
    ///
    /// A single struct cannot distinguish the real size field from an unrelated constant
    /// that happens to be large enough. Requiring the value to bound each struct's own
    /// fields, and to differ between structs, does.
    [[nodiscard]] std::size_t DetectPropertiesSizeOffset(
        const std::vector<std::uintptr_t>& structs) const;

    /// Sets one boolean field on a live instance.
    ///
    /// address is the instance base, not the ScriptStruct. Writes a single byte, because
    /// that is what a BoolProperty of size 1 occupies.
    [[nodiscard]] Result WriteBoolField(std::uintptr_t instance_address,
                                       const PropertyInfo& property, bool value) const;

    /// Reads one boolean field from a live instance.
    [[nodiscard]] Expected<bool> ReadBoolField(std::uintptr_t instance_address,
                                              const PropertyInfo& property) const;

    /// Resolves an FName stored at an address.
    [[nodiscard]] std::string ResolveNameAt(std::uintptr_t address) const;

private:
    [[nodiscard]] Expected<PropertyInfo> ReadProperty(std::uintptr_t property_address) const;

    /// Walks a candidate chain, returning the node addresses whose names resolve.
    [[nodiscard]] std::vector<std::uintptr_t> WalkChain(std::uintptr_t head,
                                                       std::size_t name_offset,
                                                       std::size_t next_offset,
                                                       std::size_t max_nodes) const;

    const NamePool*  names_{nullptr};
    ReflectionLayout layout_{};
};

} // namespace fe::unreal

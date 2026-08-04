// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Blam/ModuleImage.h
//
// Read only view of a loaded PE module.
//
// The Blam simulation ships as HaloSimulation_tag_release.dll, which the UE5
// shell loads dynamically (it is absent from the executable import table, and
// its sole export is CreateBlamEngineShell). Everything we need to reach lives
// inside that module's sections, so all scanning is scoped to a ModuleImage
// rather than the whole address space. Scoping matters for correctness as much
// as speed: an unscoped scan can match bytes in an unrelated module and produce
// a pointer that is valid but meaningless.
//
// Verified layout for build 2026.07.25 CU3:
//   .text   0x0078DD0C   code
//   .rdata  0x001EC706   string literals, read only tables
//   .data   0x022DAD35   engine globals, the debug command table
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Result.h"

namespace mpe::blam {

/// One PE section mapped into memory.
struct Section {
    std::string                name;
    std::span<const std::byte> bytes;      ///< Live, in process memory.
    std::uintptr_t             begin{0};
    std::uintptr_t             end{0};     ///< Exclusive.

    [[nodiscard]] bool Contains(std::uintptr_t address) const noexcept {
        return address >= begin && address < end;
    }
};

class ModuleImage {
public:
    /// Locates an already loaded module by name. Does not load it: if the
    /// engine shell has not been created yet the call fails with
    /// ErrorCode::ModuleNotLoaded and the caller retries later.
    [[nodiscard]] static Expected<ModuleImage> Attach(std::wstring_view module_name);

    /// Parses an image that is already mapped at a known base, without consulting
    /// the module list.
    ///
    /// Used by the offline probe, which maps the simulation module manually so no
    /// engine code is ever executed. The live mod uses Attach instead.
    [[nodiscard]] static Expected<ModuleImage> FromMappedImage(std::uintptr_t base,
                                                              std::string name);

    [[nodiscard]] std::uintptr_t Base() const noexcept { return base_; }
    [[nodiscard]] std::size_t    Size() const noexcept { return size_; }
    [[nodiscard]] const std::string& Name() const noexcept { return name_; }

    /// Nullptr when the section is absent.
    [[nodiscard]] const Section* FindSection(std::string_view name) const noexcept;

    /// Convenience accessors for the three sections every scan needs. These
    /// fail closed: a missing section is reported rather than substituted.
    [[nodiscard]] Expected<const Section*> Text() const;
    [[nodiscard]] Expected<const Section*> RData() const;
    [[nodiscard]] Expected<const Section*> Data() const;

    [[nodiscard]] const std::vector<Section>& Sections() const noexcept { return sections_; }

    /// True when the address lies inside any mapped section of this module.
    [[nodiscard]] bool ContainsAddress(std::uintptr_t address) const noexcept;

    /// True when the address lies inside a section that holds string literals.
    /// Used to validate candidate name pointers in the command table.
    [[nodiscard]] bool ContainsStringAddress(std::uintptr_t address) const noexcept;

    /// Safely dereferences a NUL terminated string, refusing to read past the
    /// end of its containing section. Returns an empty view when the address is
    /// not inside a string bearing section or the string is unterminated.
    [[nodiscard]] std::string_view ReadCString(std::uintptr_t address,
                                               std::size_t max_length = 256) const noexcept;

    /// Reads a pointer sized value if the address is inside the module.
    [[nodiscard]] bool TryReadPointer(std::uintptr_t address,
                                      std::uintptr_t& out) const noexcept;

private:
    ModuleImage() = default;

    std::uintptr_t       base_{0};
    std::size_t          size_{0};
    std::string          name_;
    std::vector<Section> sections_;
};

} // namespace mpe::blam

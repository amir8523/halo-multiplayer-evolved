// SPDX-License-Identifier: MIT
// ForgeEvolved: Blam/ModuleImage.cpp
#define FE_LOG_CATEGORY "Blam.Module"

#include "Blam/ModuleImage.h"

#include "Core/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <format>

namespace fe::blam {
namespace {

/// Sections that may hold NUL terminated string literals. The Blam debug
/// command table stores name pointers into these.
[[nodiscard]] bool IsStringSection(std::string_view name) noexcept {
    return name == ".rdata" || name == ".data" || name == "_RDATA";
}

} // namespace

Expected<ModuleImage> ModuleImage::Attach(std::wstring_view module_name) {
    const std::wstring name_z(module_name);
    const HMODULE handle = ::GetModuleHandleW(name_z.c_str());
    if (handle == nullptr) {
        return Error{ErrorCode::ModuleNotLoaded,
                     "module is not loaded in this process"};
    }

    // Narrow the wide name for logging without pulling in a locale dependent
    // conversion. Module names are ASCII in practice.
    std::string narrow;
    narrow.reserve(module_name.size());
    for (const wchar_t wc : module_name) {
        narrow.push_back(wc < 128 ? static_cast<char>(wc) : '?');
    }

    return FromMappedImage(reinterpret_cast<std::uintptr_t>(handle), std::move(narrow));
}

Expected<ModuleImage> ModuleImage::FromMappedImage(std::uintptr_t base, std::string name) {
    if (base == 0) {
        return Error{ErrorCode::InvalidArgument, "image base is null"};
    }

    auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return Error{ErrorCode::IncompatibleGameBuild, "module has no DOS header"};
    }

    const std::uintptr_t module_base = base;
    auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        module_base + static_cast<std::uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return Error{ErrorCode::IncompatibleGameBuild, "module has no NT header"};
    }
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return Error{ErrorCode::IncompatibleGameBuild, "module is not PE32+"};
    }

    ModuleImage image;
    image.base_ = module_base;
    image.size_ = nt->OptionalHeader.SizeOfImage;
    image.name_ = std::move(name);

    const auto* section = IMAGE_FIRST_SECTION(nt);
    const std::size_t section_count = nt->FileHeader.NumberOfSections;
    image.sections_.reserve(section_count);

    for (std::size_t i = 0; i < section_count; ++i, ++section) {
        // Section names are 8 bytes and only NUL padded when shorter.
        const char* raw = reinterpret_cast<const char*>(section->Name);
        std::size_t length = 0;
        while (length < IMAGE_SIZEOF_SHORT_NAME && raw[length] != '\0') {
            ++length;
        }

        // The mapped size is the virtual size; the raw size can be smaller when
        // the tail is zero filled. Clamp to virtual size so a scan never walks
        // past the mapping and faults.
        const std::size_t virtual_size = section->Misc.VirtualSize;
        if (virtual_size == 0) {
            continue;
        }
        const std::uintptr_t begin = module_base + section->VirtualAddress;

        Section entry;
        entry.name  = std::string(raw, length);
        entry.begin = begin;
        entry.end   = begin + virtual_size;
        entry.bytes = std::span(reinterpret_cast<const std::byte*>(begin), virtual_size);
        image.sections_.push_back(std::move(entry));
    }

    if (image.sections_.empty()) {
        return Error{ErrorCode::SectionNotFound, "module has no mapped sections"};
    }

    FE_LOG_INFO("attached to {} at 0x{:X} ({} sections, image size {} bytes)",
                image.name_, image.base_, image.sections_.size(), image.size_);
    for (const Section& s : image.sections_) {
        FE_LOG_DEBUG("  section {:<8} 0x{:X}..0x{:X} ({} bytes)", s.name, s.begin,
                     s.end, s.bytes.size());
    }

    return image;
}

const Section* ModuleImage::FindSection(std::string_view name) const noexcept {
    for (const Section& s : sections_) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

Expected<const Section*> ModuleImage::Text() const {
    if (const Section* s = FindSection(".text")) {
        return s;
    }
    return Error{ErrorCode::SectionNotFound, ".text section not present"};
}

Expected<const Section*> ModuleImage::RData() const {
    if (const Section* s = FindSection(".rdata")) {
        return s;
    }
    return Error{ErrorCode::SectionNotFound, ".rdata section not present"};
}

Expected<const Section*> ModuleImage::Data() const {
    if (const Section* s = FindSection(".data")) {
        return s;
    }
    return Error{ErrorCode::SectionNotFound, ".data section not present"};
}

bool ModuleImage::ContainsAddress(std::uintptr_t address) const noexcept {
    for (const Section& s : sections_) {
        if (s.Contains(address)) {
            return true;
        }
    }
    return false;
}

bool ModuleImage::ContainsStringAddress(std::uintptr_t address) const noexcept {
    for (const Section& s : sections_) {
        if (s.Contains(address) && IsStringSection(s.name)) {
            return true;
        }
    }
    return false;
}

std::string_view ModuleImage::ReadCString(std::uintptr_t address,
                                          std::size_t max_length) const noexcept {
    const Section* owner = nullptr;
    for (const Section& s : sections_) {
        if (s.Contains(address) && IsStringSection(s.name)) {
            owner = &s;
            break;
        }
    }
    if (owner == nullptr) {
        return {};
    }

    const std::size_t available = owner->end - address;
    const std::size_t limit     = available < max_length ? available : max_length;
    const char* const text      = reinterpret_cast<const char*>(address);

    for (std::size_t i = 0; i < limit; ++i) {
        if (text[i] == '\0') {
            return std::string_view(text, i);
        }
    }
    // Unterminated within the limit: treat as not a string.
    return {};
}

bool ModuleImage::TryReadPointer(std::uintptr_t address, std::uintptr_t& out) const noexcept {
    // The read itself must be inside the module and must not straddle the end
    // of its section.
    for (const Section& s : sections_) {
        if (address >= s.begin && address + sizeof(std::uintptr_t) <= s.end) {
            out = *reinterpret_cast<const std::uintptr_t*>(address);
            return true;
        }
    }
    return false;
}

} // namespace fe::blam

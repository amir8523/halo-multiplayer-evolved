// SPDX-License-Identifier: MIT
// MultiplayerEvolved: tools/probe/SymbolProbe.cpp
//
// Standalone validator for the Blam symbol discovery.
//
// WHY THIS EXISTS
//
// Symbol discovery is the highest risk part of the project: it is the one piece
// that cannot be proven correct by reading code, because it depends on the layout
// of a table inside a shipped binary. Testing it by launching the game is slow and
// tells you almost nothing when it fails.
//
// So this tool loads HaloSimulation_tag_release.dll into its own process, runs the
// exact discovery code the mod runs, and prints the result. It takes a second and
// gives a definitive answer, including the annotated record hex dump needed to
// derive a layout for a new game build.
//
// Usage:
//   SymbolProbe.exe "<game>\Meteorite\Binaries\Win64\HaloSimulation_tag_release.dll"
//
// The DLL is mapped by hand rather than by LoadLibrary. That is not paranoia: a
// normal LoadLibrary of this module runs its DllMain, which fails fast because the
// engine expects to be initialized by its host shell, and takes the probe with it.
// DONT_RESOLVE_DLL_REFERENCES was tried first and is not reliable here either.
//
// So the mapper below reads the file, copies each section to its virtual address,
// and applies base relocations itself. The result is byte for byte what the loader
// would produce, every pointer inside .data is correctly fixed up, and not one
// instruction of engine code is executed.

#include "Blam/ModuleImage.h"
#include "Blam/PatternScanner.h"
#include "Blam/SymbolRegistry.h"
#include "Core/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace {

void Print(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

/// Reads a whole file.
[[nodiscard]] bool ReadFileBytes(const std::wstring& path, std::vector<std::byte>& out,
                                 std::string& out_error) {
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        out_error = std::format("CreateFile failed with {}", ::GetLastError());
        return false;
    }

    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) == FALSE || size.QuadPart <= 0) {
        out_error = "GetFileSizeEx failed";
        ::CloseHandle(file);
        return false;
    }

    out.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t total = 0;
    while (total < out.size()) {
        // ReadFile takes a 32 bit count, so a file larger than 4 GB needs chunking.
        const DWORD chunk = static_cast<DWORD>(
            (out.size() - total) > 0x10000000u ? 0x10000000u : (out.size() - total));
        DWORD read = 0;
        if (::ReadFile(file, out.data() + total, chunk, &read, nullptr) == FALSE || read == 0) {
            out_error = std::format("ReadFile failed with {}", ::GetLastError());
            ::CloseHandle(file);
            return false;
        }
        total += read;
    }
    ::CloseHandle(file);
    return true;
}

/// Maps a PE image by hand and applies base relocations. Executes nothing.
///
/// Returns the image base, or zero on failure.
[[nodiscard]] std::uintptr_t ManualMapImage(const std::wstring& path, std::string& out_note) {
    std::vector<std::byte> file;
    std::string            error;
    if (!ReadFileBytes(path, file, error)) {
        out_note = error;
        return 0;
    }
    if (file.size() < sizeof(IMAGE_DOS_HEADER)) {
        out_note = "file is too small to be a PE";
        return 0;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        out_note = "missing MZ signature";
        return 0;
    }
    const auto nt_offset = static_cast<std::size_t>(dos->e_lfanew);
    if (nt_offset + sizeof(IMAGE_NT_HEADERS64) > file.size()) {
        out_note = "NT headers are outside the file";
        return 0;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data() + nt_offset);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        out_note = "not a PE32+ image";
        return 0;
    }

    const std::size_t    image_size   = nt->OptionalHeader.SizeOfImage;
    const std::uintptr_t preferred    = static_cast<std::uintptr_t>(nt->OptionalHeader.ImageBase);

    // Try the preferred base first: if it is available no relocation is needed and
    // the image is bit identical to a normal load.
    auto* base = static_cast<std::byte*>(::VirtualAlloc(reinterpret_cast<LPVOID>(preferred),
                                                        image_size, MEM_RESERVE | MEM_COMMIT,
                                                        PAGE_READWRITE));
    bool relocated_needed = false;
    if (base == nullptr) {
        base = static_cast<std::byte*>(
            ::VirtualAlloc(nullptr, image_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        relocated_needed = true;
    }
    if (base == nullptr) {
        out_note = std::format("VirtualAlloc of {} bytes failed with {}", image_size,
                               ::GetLastError());
        return 0;
    }

    // Headers, then each section at its virtual address.
    std::memcpy(base, file.data(), nt->OptionalHeader.SizeOfHeaders);

    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (section->SizeOfRawData == 0) {
            continue; // Virtual only, already zero from VirtualAlloc.
        }
        if (static_cast<std::size_t>(section->PointerToRawData) + section->SizeOfRawData >
            file.size()) {
            out_note = "a section's raw data lies outside the file";
            ::VirtualFree(base, 0, MEM_RELEASE);
            return 0;
        }
        std::memcpy(base + section->VirtualAddress, file.data() + section->PointerToRawData,
                    section->SizeOfRawData);
    }

    // Base relocations. Every absolute pointer stored in .data, which is exactly
    // what the debug table is made of, needs this.
    const std::intptr_t delta =
        static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(base)) -
        static_cast<std::intptr_t>(preferred);

    if (relocated_needed && delta != 0) {
        const auto& directory =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (directory.Size == 0) {
            out_note = "image needs relocation but has no relocation directory";
            ::VirtualFree(base, 0, MEM_RELEASE);
            return 0;
        }

        std::size_t consumed = 0;
        while (consumed < directory.Size) {
            const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(
                base + directory.VirtualAddress + consumed);
            if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) {
                break;
            }
            const std::size_t entry_count =
                (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            const auto* entries = reinterpret_cast<const WORD*>(
                reinterpret_cast<const std::byte*>(block) + sizeof(IMAGE_BASE_RELOCATION));

            for (std::size_t e = 0; e < entry_count; ++e) {
                const WORD type   = static_cast<WORD>(entries[e] >> 12);
                const WORD offset = static_cast<WORD>(entries[e] & 0x0FFF);
                if (type == IMAGE_REL_BASED_DIR64) {
                    auto* target = reinterpret_cast<std::uintptr_t*>(
                        base + block->VirtualAddress + offset);
                    *target = static_cast<std::uintptr_t>(
                        static_cast<std::intptr_t>(*target) + delta);
                } else if (type != IMAGE_REL_BASED_ABSOLUTE) {
                    // Only DIR64 and the ABSOLUTE padding entry occur in an x64
                    // image. Anything else means the file is not what we think.
                    out_note = std::format("unexpected relocation type {}", type);
                    ::VirtualFree(base, 0, MEM_RELEASE);
                    return 0;
                }
            }
            consumed += block->SizeOfBlock;
        }
    }

    out_note = std::format(
        "manually mapped {} bytes at 0x{:X} (preferred 0x{:X}, delta 0x{:X}, relocations {})",
        image_size, reinterpret_cast<std::uintptr_t>(base), preferred,
        static_cast<std::uintptr_t>(delta),
        (relocated_needed && delta != 0) ? "applied" : "not needed");

    return reinterpret_cast<std::uintptr_t>(base);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        Print("usage: SymbolProbe.exe <path to HaloSimulation_tag_release.dll>");
        return 2;
    }

    const std::wstring path(argv[1]);

    // Discovery logs at debug level; send it to the console so the reasoning is
    // visible rather than only the verdict.
    mpe::log::Initialize(std::filesystem::path("SymbolProbe.log"), mpe::log::Level::Debug);

    std::string note;
    const std::uintptr_t base = ManualMapImage(path, note);
    Print(std::format("load: {}", note));
    if (base == 0) {
        return 1;
    }

    const auto image =
        mpe::blam::ModuleImage::FromMappedImage(base, "HaloSimulation_tag_release.dll");
    if (!image.ok()) {
        Print(std::format("FAIL parse: {}", image.message()));
        return 1;
    }
    Print(std::format("attached at 0x{:X}, {} sections", image.value().Base(),
                      image.value().Sections().size()));

    // Step 1: prove the anchors exist. If an anchor is missing, discovery cannot
    // work and the reason is the binary, not the algorithm.
    const mpe::blam::PatternScanner scanner(image.value());
    const auto config = mpe::blam::SymbolRegistryConfig::Default();

    Print("");
    Print("--- string literal search ---");
    for (const std::string& anchor : config.AllRequestedNames()) {
        const auto hits = scanner.FindStringLiteral(anchor, 8);
        if (hits.empty()) {
            Print(std::format("  MISSING  {}", anchor));
            continue;
        }
        std::string locations;
        for (std::size_t i = 0; i < hits.size(); ++i) {
            locations += std::format("0x{:X}", hits[i]);
            if (i + 1 < hits.size()) {
                locations += " ";
            }
        }
        Print(std::format("  found    {}  at {}", anchor, locations));
    }

    // Step 2: run the real discovery.
    Print("");
    Print("--- discovery ---");
    auto registry = mpe::blam::SymbolRegistry::Discover(image.value(), config);
    if (!registry.ok()) {
        Print(std::format("FAIL discovery: {}", registry.message()));
        Print("");
        Print("The mod would stay inert on this build. See SymbolProbe.log for the "
              "candidate strides that were tried.");
        return 1;
    }

    Print(registry.value().BuildDiscoveryReport(config.AllRequestedNames()));

    // Step 3: report where every requested symbol landed, required and optional.
    Print("");
    Print("--- symbol resolution ---");
    std::size_t required_resolved = 0;
    for (const std::string& required : config.required_symbols) {
        const auto* record = registry.value().Find(required);
        if (record == nullptr) {
            Print(std::format("  REQUIRED MISSING  {}", required));
            continue;
        }
        ++required_resolved;
        Print(std::format("  required 0x{:X} stride 0x{:X}  {}", record->record_address,
                          record->stride, required));
    }
    std::size_t optional_resolved = 0;
    for (const std::string& optional : config.optional_symbols) {
        const auto* record = registry.value().Find(optional);
        if (record == nullptr) {
            Print(std::format("  optional missing   {}", optional));
            continue;
        }
        ++optional_resolved;
        Print(std::format("  optional 0x{:X} stride 0x{:X}  {}", record->record_address,
                          record->stride, optional));
    }

    Print("");
    Print(std::format("RESULT: required {}/{}, optional {}/{}, {} records across {} table(s)",
                      required_resolved, config.required_symbols.size(), optional_resolved,
                      config.optional_symbols.size(), registry.value().Count(),
                      registry.value().Tables().size()));

    // Dump the complete vocabulary. This is how the controllable surface gets mapped
    // rather than guessed at: every name the engine exposes, with its table stride
    // and, for debug globals, its type tag and current value. Grepping this file is
    // far more productive than speculating about what might exist.
    {
        const char* const kDumpPath = "all_symbols.txt";
        std::FILE* dump = nullptr;
        if (fopen_s(&dump, kDumpPath, "wb") == 0 && dump != nullptr) {
            std::fprintf(dump, "# MultiplayerEvolved symbol dump\n");
            std::fprintf(dump, "# %zu records across %zu tables\n",
                         registry.value().Count(), registry.value().Tables().size());
            std::fprintf(dump, "# columns: address stride table name [type value]\n");
            std::fprintf(dump, "# stride 0x18 records are debug globals with an inline value\n");
            std::fprintf(dump, "# stride 0x10 records are string ids and have no value\n\n");

            for (const auto& record : registry.value().Records()) {
                std::fprintf(dump, "0x%llX 0x%02zX 0x%llX %s",
                             static_cast<unsigned long long>(record.record_address),
                             record.stride,
                             static_cast<unsigned long long>(record.table_begin),
                             record.name.c_str());

                const auto type  = registry.value().ReadGlobalType(record.name);
                const auto value = registry.value().ReadGlobalValue(record.name);
                if (type.ok() && value.ok()) {
                    std::fprintf(dump, " type=%llu value=0x%llX",
                                 static_cast<unsigned long long>(type.value()),
                                 static_cast<unsigned long long>(value.value()));
                }
                std::fprintf(dump, "\n");
            }
            std::fclose(dump);
            Print(std::format("wrote {} ({} records)", kDumpPath, registry.value().Count()));
        } else {
            Print("could not write all_symbols.txt");
        }
    }

    return required_resolved == config.required_symbols.size() ? 0 : 1;
}

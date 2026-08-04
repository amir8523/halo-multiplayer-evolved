// SPDX-License-Identifier: MIT
// ForgeEvolved: tools/probe/XrefProbe.cpp
//
// Locates the Blam debug command registrar by static cross reference analysis.
//
// THE PROBLEM
//
// SymbolProbe established that these command names exist as string literals but that
// no pointer to them lives in any static table:
//
//   net_load_and_use_map_variant        0x1807FA7F8
//   net_build_map_variant               0x1807FA838
//   net_verify_map_variant              0x1807FA850
//   write_current_map_variant           0x1807FA818
//   read_map_variant_and_make_current   0x1807FA8B0
//   net_simulation_set_stream_bandwidth 0x1807FAF00
//   console_command                     0x180861A78
//
// They are registered at runtime by code that passes the literal as an argument. So
// the handler address is not in any table; it is an operand of a call instruction.
//
// THE METHOD
//
//   1. For each command name, find every RIP relative reference to its string in
//      .text. That is the registration site.
//   2. Establish the containing function exactly, using the .pdata exception
//      directory rather than guessing at a prologue. Every x64 function has a
//      RUNTIME_FUNCTION entry giving its precise bounds.
//   3. Around each site, decode the other RIP relative operands. One of them
//      resolves into .text, and that is the candidate handler.
//   4. Decode the following direct call. Tally the call targets across all command
//      names.
//
// Step 4 is the decisive one. If many unrelated command names all call the same
// function, that function is the registrar, and its argument order tells us how a
// command maps to a handler. A single shared target across seven independent call
// sites is not a coincidence.
//
// Nothing is executed. The module is mapped by hand exactly as in SymbolProbe.
//
// Usage:
//   XrefProbe.exe "<game>\Meteorite\Binaries\Win64\HaloSimulation_tag_release.dll"

#include "Blam/ModuleImage.h"
#include "Blam/PatternScanner.h"
#include "Blam/SymbolRegistry.h"
#include "Core/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <string>
#include <vector>

namespace {

void Print(const std::string& text) {
    std::fputs(text.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// Manual mapping, identical to SymbolProbe so both tools see the same image
// ---------------------------------------------------------------------------

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

struct MappedImage {
    std::uintptr_t base{0};
    std::uintptr_t pdata_begin{0};
    std::uint32_t  pdata_size{0};
};

[[nodiscard]] MappedImage ManualMapImage(const std::wstring& path, std::string& out_note) {
    MappedImage result;

    std::vector<std::byte> file;
    std::string            error;
    if (!ReadFileBytes(path, file, error)) {
        out_note = error;
        return result;
    }
    if (file.size() < sizeof(IMAGE_DOS_HEADER)) {
        out_note = "file is too small to be a PE";
        return result;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        out_note = "missing MZ signature";
        return result;
    }
    const auto nt_offset = static_cast<std::size_t>(dos->e_lfanew);
    if (nt_offset + sizeof(IMAGE_NT_HEADERS64) > file.size()) {
        out_note = "NT headers are outside the file";
        return result;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data() + nt_offset);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        out_note = "not a PE32+ image";
        return result;
    }

    const std::size_t    image_size = nt->OptionalHeader.SizeOfImage;
    const std::uintptr_t preferred  = static_cast<std::uintptr_t>(nt->OptionalHeader.ImageBase);

    auto* base = static_cast<std::byte*>(::VirtualAlloc(reinterpret_cast<LPVOID>(preferred),
                                                        image_size, MEM_RESERVE | MEM_COMMIT,
                                                        PAGE_READWRITE));
    bool needs_relocation = false;
    if (base == nullptr) {
        base = static_cast<std::byte*>(
            ::VirtualAlloc(nullptr, image_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        needs_relocation = true;
    }
    if (base == nullptr) {
        out_note = std::format("VirtualAlloc of {} bytes failed with {}", image_size,
                               ::GetLastError());
        return result;
    }

    std::memcpy(base, file.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (section->SizeOfRawData == 0) {
            continue;
        }
        std::memcpy(base + section->VirtualAddress, file.data() + section->PointerToRawData,
                    section->SizeOfRawData);
    }

    const std::intptr_t delta =
        static_cast<std::intptr_t>(reinterpret_cast<std::uintptr_t>(base)) -
        static_cast<std::intptr_t>(preferred);
    if (needs_relocation && delta != 0) {
        const auto& directory =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
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
                if (static_cast<WORD>(entries[e] >> 12) == IMAGE_REL_BASED_DIR64) {
                    auto* target = reinterpret_cast<std::uintptr_t*>(
                        base + block->VirtualAddress + (entries[e] & 0x0FFF));
                    *target = static_cast<std::uintptr_t>(
                        static_cast<std::intptr_t>(*target) + delta);
                }
            }
            consumed += block->SizeOfBlock;
        }
    }

    // The exception directory is the function table. Recorded here so the caller
    // does not have to reparse the headers.
    const auto& exception_directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    result.pdata_begin = reinterpret_cast<std::uintptr_t>(base) + exception_directory.VirtualAddress;
    result.pdata_size  = exception_directory.Size;
    result.base        = reinterpret_cast<std::uintptr_t>(base);

    out_note = std::format("mapped {} bytes at 0x{:X} (preferred 0x{:X}), pdata {} entries",
                           image_size, result.base, preferred,
                           exception_directory.Size / sizeof(RUNTIME_FUNCTION));
    return result;
}

// ---------------------------------------------------------------------------
// Function table
// ---------------------------------------------------------------------------

/// Exact function bounds from .pdata, which is authoritative. Guessing at a
/// prologue is unreliable in optimized code with tail merging.
class FunctionTable {
public:
    FunctionTable(std::uintptr_t image_base, std::uintptr_t pdata, std::uint32_t size)
        : image_base_(image_base) {
        const std::size_t count = size / sizeof(RUNTIME_FUNCTION);
        const auto* entries = reinterpret_cast<const RUNTIME_FUNCTION*>(pdata);
        functions_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            functions_.push_back(entries[i]);
        }
        // .pdata is required to be sorted by BeginAddress, but sorting defensively
        // costs nothing and makes the binary search safe regardless.
        std::sort(functions_.begin(), functions_.end(),
                  [](const RUNTIME_FUNCTION& a, const RUNTIME_FUNCTION& b) {
                      return a.BeginAddress < b.BeginAddress;
                  });
    }

    [[nodiscard]] std::size_t Count() const noexcept { return functions_.size(); }

    /// Returns the start address of the function containing address, or zero.
    [[nodiscard]] std::uintptr_t FunctionContaining(std::uintptr_t address) const {
        if (address < image_base_) {
            return 0;
        }
        const auto rva = static_cast<std::uint32_t>(address - image_base_);

        // Last entry whose BeginAddress <= rva.
        auto it = std::upper_bound(functions_.begin(), functions_.end(), rva,
                                   [](std::uint32_t value, const RUNTIME_FUNCTION& entry) {
                                       return value < entry.BeginAddress;
                                   });
        if (it == functions_.begin()) {
            return 0;
        }
        --it;
        if (rva >= it->BeginAddress && rva < it->EndAddress) {
            return image_base_ + it->BeginAddress;
        }
        return 0;
    }

    /// True when address is the exact start of a known function, which is what a
    /// legitimate handler pointer looks like. Expressed in terms of
    /// FunctionContaining so there is one lookup path rather than two that could
    /// disagree.
    [[nodiscard]] bool IsFunctionStart(std::uintptr_t address) const {
        return address != 0 && FunctionContaining(address) == address;
    }

private:
    std::uintptr_t                    image_base_;
    std::vector<RUNTIME_FUNCTION>     functions_;
};

// ---------------------------------------------------------------------------
// Instruction decoding, only the forms this analysis needs
// ---------------------------------------------------------------------------

struct RipOperand {
    std::uintptr_t site{0};   ///< Address of the instruction.
    std::uintptr_t target{0}; ///< Address it resolves to.
    std::uint8_t   opcode{0};
    std::uint8_t   reg{0};    ///< Destination register index, including REX.R.
};

/// Decodes `lea r64, [rip+disp32]` and `mov r64, [rip+disp32]` at a given address.
/// Returns false when the bytes are not one of those forms.
[[nodiscard]] bool DecodeRipOperand(const fe::blam::Section& code, std::uintptr_t address,
                                    RipOperand& out) {
    constexpr std::size_t kLength = 7;
    if (address < code.begin || address + kLength > code.end) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(address);

    const std::uint8_t rex   = bytes[0];
    const std::uint8_t op    = bytes[1];
    const std::uint8_t modrm = bytes[2];

    if ((rex & 0xF8) != 0x48) {
        return false; // Needs a REX.W prefix in the 48..4F range.
    }
    if (op != 0x8D && op != 0x8B) {
        return false; // lea or mov r64, m64.
    }
    if ((modrm & 0xC7) != 0x05) {
        return false; // mod == 00 and rm == 101 selects RIP relative.
    }

    std::int32_t displacement = 0;
    std::memcpy(&displacement, bytes + 3, sizeof(displacement));

    out.site   = address;
    out.target = address + kLength + static_cast<std::intptr_t>(displacement);
    out.opcode = op;
    out.reg    = static_cast<std::uint8_t>(((modrm >> 3) & 0x07) | ((rex & 0x04) << 1));
    return true;
}

/// Decodes a direct `call rel32` (E8) at address.
[[nodiscard]] bool DecodeDirectCall(const fe::blam::Section& code, std::uintptr_t address,
                                    std::uintptr_t& out_target) {
    constexpr std::size_t kLength = 5;
    if (address < code.begin || address + kLength > code.end) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(address);
    if (bytes[0] != 0xE8) {
        return false;
    }
    std::int32_t relative = 0;
    std::memcpy(&relative, bytes + 1, sizeof(relative));
    out_target = address + kLength + static_cast<std::intptr_t>(relative);
    return true;
}

/// One RIP relative encoding shape this scanner recognizes.
///
/// The earlier decoder only handled REX.W forms (`48 8B` and `48 8D`), which is why
/// it found zero readers of anything: a boolean global is read with `movzx`,
/// `cmp byte`, or a plain 32 bit `mov`, none of which carry a REX.W prefix. Missing
/// those meant missing every access to every global.
struct RipEncoding {
    const char*  mnemonic;
    std::uint8_t prefix[3];      ///< Fixed leading bytes.
    std::size_t  prefix_length;  ///< How many of those are fixed.
    bool         has_modrm;      ///< True when a modrm byte follows the prefix.
    std::size_t  displacement_at;///< Byte offset of the disp32.
    std::size_t  total_length;   ///< Full instruction length.
};

/// Encodings that reach a fixed address through RIP relative addressing. Covers the
/// forms MSVC emits for reading and writing a global of 1, 4 or 8 bytes.
constexpr RipEncoding kRipEncodings[] = {
    // Plain 32 bit and 8 bit moves, no REX.
    {"mov r32, [rip]",   {0x8B},       1, true,  2,  6},
    {"mov [rip], r32",   {0x89},       1, true,  2,  6},
    {"mov r8, [rip]",    {0x8A},       1, true,  2,  6},
    {"mov [rip], r8",    {0x88},       1, true,  2,  6},
    // REX.W forms: 64 bit moves and lea. The REX byte varies across 48..4F, so it is
    // validated separately rather than as a fixed prefix.
    {"mov r64, [rip]",   {0x00, 0x8B}, 2, true,  3,  7},
    {"mov [rip], r64",   {0x00, 0x89}, 2, true,  3,  7},
    {"lea r64, [rip]",   {0x00, 0x8D}, 2, true,  3,  7},
    // Zero and sign extending loads, which is how a byte sized boolean is read.
    {"movzx r, byte [rip]",  {0x0F, 0xB6}, 2, true,  3,  7},
    {"movzx r, word [rip]",  {0x0F, 0xB7}, 2, true,  3,  7},
    {"movsx r, byte [rip]",  {0x0F, 0xBE}, 2, true,  3,  7},
    // Compares against an immediate, the most common way a flag is tested.
    {"cmp byte [rip], imm8",  {0x80, 0x3D}, 2, false, 2,  7},
    {"cmp dword [rip], imm8", {0x83, 0x3D}, 2, false, 2,  7},
    {"mov byte [rip], imm8",  {0xC6, 0x05}, 2, false, 2,  7},
    {"mov dword [rip], imm32",{0xC7, 0x05}, 2, false, 2, 10},
};

/// True when the bytes at address match one encoding and resolve to target.
[[nodiscard]] bool MatchesRipEncoding(const fe::blam::Section& code, std::uintptr_t address,
                                      std::uintptr_t target, const RipEncoding& encoding) {
    if (address < code.begin || address + encoding.total_length > code.end) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(address);

    std::size_t index = 0;
    if (encoding.prefix[0] == 0x00) {
        // REX.W form: first byte must be in the 48..4F range.
        if ((bytes[0] & 0xF8) != 0x48) {
            return false;
        }
        index = 1;
        for (std::size_t i = 1; i < encoding.prefix_length; ++i, ++index) {
            if (bytes[index] != encoding.prefix[i]) {
                return false;
            }
        }
    } else {
        for (; index < encoding.prefix_length; ++index) {
            if (bytes[index] != encoding.prefix[index]) {
                return false;
            }
        }
    }

    // When a modrm byte is present it must select RIP relative addressing, meaning
    // mod == 00 and rm == 101.
    if (encoding.has_modrm && (bytes[index] & 0xC7) != 0x05) {
        return false;
    }

    std::int32_t displacement = 0;
    std::memcpy(&displacement, bytes + encoding.displacement_at, sizeof(displacement));

    const std::uintptr_t resolved =
        address + encoding.total_length + static_cast<std::intptr_t>(displacement);
    return resolved == target;
}

/// Every instruction in code that reaches target through RIP relative addressing.
struct GlobalAccess {
    std::uintptr_t site{0};
    const char*    mnemonic{""};
    std::uintptr_t function{0};
};

[[nodiscard]] std::vector<GlobalAccess> FindGlobalAccesses(const fe::blam::Section& code,
                                                          std::uintptr_t target,
                                                          std::size_t max_results) {
    std::vector<GlobalAccess> results;
    for (std::uintptr_t address = code.begin; address + 16 < code.end; ++address) {
        for (const RipEncoding& encoding : kRipEncodings) {
            if (MatchesRipEncoding(code, address, target, encoding)) {
                results.push_back(GlobalAccess{address, encoding.mnemonic, 0});
                break; // One match per address is enough.
            }
        }
        if (results.size() >= max_results) {
            break;
        }
    }
    return results;
}

/// x64 register names, for readable output.
[[nodiscard]] const char* RegisterName(std::uint8_t index) {
    static constexpr const char* kNames[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                                               "rsi", "rdi", "r8",  "r9",  "r10", "r11",
                                               "r12", "r13", "r14", "r15"};
    return index < 16 ? kNames[index] : "?";
}

struct SiteAnalysis {
    std::string    name;
    std::uintptr_t xref_site{0};
    std::uintptr_t containing_function{0};
    std::uintptr_t call_target{0};
    std::vector<RipOperand> nearby_code_pointers; ///< Candidate handlers.
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        Print("usage: XrefProbe.exe <path to HaloSimulation_tag_release.dll>");
        return 2;
    }

    fe::log::Initialize(std::filesystem::path("XrefProbe.log"), fe::log::Level::Info);

    std::string note;
    const MappedImage mapped = ManualMapImage(std::wstring(argv[1]), note);
    Print(std::format("load: {}", note));
    if (mapped.base == 0) {
        return 1;
    }

    const auto image =
        fe::blam::ModuleImage::FromMappedImage(mapped.base, "HaloSimulation_tag_release.dll");
    if (!image.ok()) {
        Print(std::format("FAIL parse: {}", image.message()));
        return 1;
    }

    const auto text = image.value().Text();
    if (!text.ok()) {
        Print("FAIL: no .text section");
        return 1;
    }
    const fe::blam::Section& code = *text.value();

    const FunctionTable functions(mapped.base, mapped.pdata_begin, mapped.pdata_size);
    Print(std::format(".pdata describes {} functions", functions.Count()));

    const fe::blam::PatternScanner scanner(image.value());

    // The command names with no static table entry, plus two that do resolve, as a
    // control: if a name that lives in a table shows a different pattern, that is
    // informative rather than noise.
    const std::vector<std::string> targets = {
        "net_load_and_use_map_variant",
        "net_build_map_variant",
        "net_verify_map_variant",
        "write_current_map_variant",
        "read_map_variant_and_make_current",
        "net_simulation_set_stream_bandwidth",
        "console_command",
    };

    std::vector<SiteAnalysis>              analyses;
    std::map<std::uintptr_t, std::size_t>  call_target_tally;
    std::map<std::uintptr_t, std::size_t>  handler_candidate_tally;

    Print("");
    Print("=== cross reference analysis ===");

    for (const std::string& name : targets) {
        const auto literals = scanner.FindStringLiteral(name, 8);
        if (literals.empty()) {
            Print(std::format("\n{}\n  string not found", name));
            continue;
        }

        Print(std::format("\n{}  (string at 0x{:X})", name, literals.front()));

        std::size_t total_refs = 0;
        for (const std::uintptr_t literal : literals) {
            const auto refs = scanner.FindRipRelativeReferences(code, literal, 16);
            total_refs += refs.size();

            for (const std::uintptr_t ref : refs) {
                SiteAnalysis analysis;
                analysis.name                = name;
                analysis.xref_site           = ref;
                analysis.containing_function = functions.FunctionContaining(ref);

                RipOperand self{};
                (void)DecodeRipOperand(code, ref, self);

                Print(std::format("  xref 0x{:X}  {} {}, [rip -> string]   in function 0x{:X}",
                                  ref, self.opcode == 0x8D ? "lea" : "mov",
                                  RegisterName(self.reg), analysis.containing_function));

                // Walk forward looking for the registration call, decoding any other
                // RIP relative operands on the way. The window is generous because a
                // registration site may set up several arguments first.
                constexpr std::uintptr_t kWindow = 0x60;
                for (std::uintptr_t probe = ref; probe < ref + kWindow && probe < code.end;
                     ++probe) {
                    RipOperand operand{};
                    if (DecodeRipOperand(code, probe, operand) && operand.target != literal) {
                        const bool into_code =
                            operand.target >= code.begin && operand.target < code.end;
                        if (into_code && operand.opcode == 0x8D) {
                            const bool is_function_start =
                                functions.FunctionContaining(operand.target) == operand.target;
                            Print(std::format(
                                "      +0x{:02X}  lea {}, 0x{:X}  -> .text{}", probe - ref,
                                RegisterName(operand.reg), operand.target,
                                is_function_start ? "  (function start, CANDIDATE HANDLER)" : ""));
                            if (is_function_start) {
                                analysis.nearby_code_pointers.push_back(operand);
                                ++handler_candidate_tally[operand.target];
                            }
                        } else if (!into_code) {
                            const std::string_view text_at =
                                image.value().ReadCString(operand.target, 48);
                            if (!text_at.empty()) {
                                Print(std::format("      +0x{:02X}  {} {}, 0x{:X}  -> \"{}\"",
                                                  probe - ref,
                                                  operand.opcode == 0x8D ? "lea" : "mov",
                                                  RegisterName(operand.reg), operand.target,
                                                  text_at));
                            }
                        }
                    }

                    std::uintptr_t call_target = 0;
                    if (DecodeDirectCall(code, probe, call_target)) {
                        const bool known = functions.FunctionContaining(call_target) == call_target;
                        Print(std::format("      +0x{:02X}  call 0x{:X}{}", probe - ref,
                                          call_target, known ? "" : "  (not a function start)"));
                        if (known && analysis.call_target == 0) {
                            analysis.call_target = call_target;
                            ++call_target_tally[call_target];
                        }
                        break; // The first call after the name is the registration.
                    }
                }

                analyses.push_back(std::move(analysis));
            }
        }
        if (total_refs == 0) {
            Print("  no RIP relative reference from .text");
        }
    }

    // The decisive tally.
    Print("");
    Print("=== call target frequency ===");
    Print("A function called from many independent command sites is the registrar.");
    std::vector<std::pair<std::uintptr_t, std::size_t>> ranked(call_target_tally.begin(),
                                                              call_target_tally.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [target, count] : ranked) {
        Print(std::format("  0x{:X}  called from {} command site(s)", target, count));
    }
    if (ranked.empty()) {
        Print("  none found");
    }

    Print("");
    Print("=== candidate handler frequency ===");
    Print("A handler should appear exactly once. A repeat means the pattern is wrong.");
    std::vector<std::pair<std::uintptr_t, std::size_t>> handlers(handler_candidate_tally.begin(),
                                                                handler_candidate_tally.end());
    std::sort(handlers.begin(), handlers.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [target, count] : handlers) {
        Print(std::format("  0x{:X}  referenced by {} site(s)", target, count));
    }
    if (handlers.empty()) {
        Print("  none found");
    }

    Print("");
    Print(std::format("=== summary: {} site(s) analysed, {} distinct call target(s), "
                      "{} candidate handler(s) ===",
                      analyses.size(), call_target_tally.size(),
                      handler_candidate_tally.size()));

    // -----------------------------------------------------------------------
    // Pointer holder layout analysis
    // -----------------------------------------------------------------------
    //
    // A name with no code reference and no record at name_offset 0 is still
    // described by something, because the engine resolves it. So find every 8 byte
    // pointer to the string anywhere in the module and dump the qwords around it.
    //
    // This answers the question the earlier resolver could not: if the name sits at
    // a non zero offset inside its record, the holder's neighbourhood shows it
    // directly. Reading the surrounding qwords with each one annotated as a code
    // pointer, a string pointer or a plain integer is enough to recover the record
    // shape without a disassembler.

    Print("");
    Print("=== pointer holder layout ===");
    Print("For each name with no static record at offset 0, every pointer to it, with");
    Print("surrounding qwords annotated. C=code, S=string, .=integer or null.");

    for (const std::string& name : targets) {
        const auto literals = scanner.FindStringLiteral(name, 4);
        if (literals.empty()) {
            continue;
        }
        const std::uintptr_t literal = literals.front();

        // Search every section that can hold initialized data, plus .text, since a
        // read only table can be emitted next to code.
        std::vector<std::uintptr_t> holders;
        for (const fe::blam::Section& section : image.value().Sections()) {
            if (section.name == ".reloc" || section.name == ".pdata") {
                continue;
            }
            const auto found = scanner.FindPointersTo(section, literal, 16);
            holders.insert(holders.end(), found.begin(), found.end());
        }

        Print(std::format("\n{}  string 0x{:X}  {} pointer holder(s)", name, literal,
                          holders.size()));
        if (holders.empty()) {
            Print("  none: this name is not referenced by any 8 byte pointer in the module,");
            Print("  so it is not described by a pointer table at all.");
            continue;
        }

        for (const std::uintptr_t holder : holders) {
            // Which section the holder lives in, for context.
            std::string section_name = "?";
            for (const fe::blam::Section& section : image.value().Sections()) {
                if (section.Contains(holder)) {
                    section_name = section.name;
                    break;
                }
            }
            Print(std::format("  holder 0x{:X} in {}", holder, section_name));

            // Four qwords before and eight after, which covers any plausible record.
            for (int slot = -4; slot <= 8; ++slot) {
                const std::uintptr_t address =
                    holder + static_cast<std::intptr_t>(slot) * 8;
                std::uintptr_t value = 0;
                if (!image.value().TryReadPointer(address, value)) {
                    continue;
                }

                char kind = '.';
                std::string annotation;
                if (value >= code.begin && value < code.end) {
                    kind = 'C';
                    const std::uintptr_t owner = functions.FunctionContaining(value);
                    annotation = (owner == value)
                                     ? "  function start"
                                     : std::format("  inside function 0x{:X}", owner);
                } else if (image.value().ContainsStringAddress(value)) {
                    const std::string_view text = image.value().ReadCString(value, 56);
                    if (!text.empty()) {
                        kind       = 'S';
                        annotation = std::format("  \"{}\"", text);
                    }
                }

                Print(std::format("    {}{:+3}  0x{:X}  {}  0x{:016X}{}",
                                  (slot == 0 ? ">" : " "), slot * 8, address, kind, value,
                                  annotation));
            }
        }
    }

    // -----------------------------------------------------------------------
    // Global reader analysis: locating the damage pipeline
    // -----------------------------------------------------------------------
    //
    // cheat_deathless_player and cheat_omnipotent exist to make damage not apply to a
    // player. Whatever code reads them is therefore inside the damage application
    // path, and the allegiance test that stops co-op players hurting each other lives
    // in that same path.
    //
    // So rather than hunting for a "friendly fire" symbol that does not exist, this
    // finds the functions that read the cheats and reports them as the entry point for
    // that work.

    Print("");
    Print("=== global reader analysis ===");
    Print("Finding code that reads specific globals, to locate the damage pipeline.");

    // Discover the globals so their value addresses are known.
    auto registry = fe::blam::SymbolRegistry::Discover(image.value(),
                                                       fe::blam::SymbolRegistryConfig::Default());
    if (!registry.ok()) {
        Print(std::format("  cannot analyse globals: discovery failed: {}", registry.message()));
        Print("");
        Print("=== end ===");
        return 0;
    }

    const std::vector<std::string> damage_globals = {
        "cheat_deathless_player",
        "cheat_omnipotent",
        "debug_player_damage",
        "debug_damage",
        "cheat_medusa",
    };

    // Functions that read more than one of these are the strongest candidates: a
    // single function reading both cheat_deathless_player and cheat_omnipotent is
    // almost certainly the player damage gate.
    std::map<std::uintptr_t, std::vector<std::string>> functions_by_global;

    for (const std::string& name : damage_globals) {
        const auto* record = registry.value().Find(name);
        if (record == nullptr) {
            Print(std::format("\n{}  not resolved", name));
            continue;
        }
        if (record->stride < 0x18) {
            Print(std::format("\n{}  is a string id, no value to read", name));
            continue;
        }

        // Probe every field of the record, not just the one assumed to be the value.
        // If nothing references any of them, the record is a descriptor consumed by
        // enumeration rather than a storage location code reads directly, and writing
        // to it would change nothing that matters.
        Print(std::format("\n{}  record at 0x{:X}", name, record->record_address));
        for (std::size_t field = 0; field <= 0x10; field += 8) {
            const std::uintptr_t field_address = record->record_address + field;
            const auto field_accesses = FindGlobalAccesses(code, field_address, 8);
            const auto lea_refs = scanner.FindRipRelativeReferences(code, field_address, 8);
            Print(std::format("    +0x{:02X} at 0x{:X}: {} data access(es), {} lea reference(s)",
                              field, field_address, field_accesses.size(), lea_refs.size()));
        }

        const std::uintptr_t value_address = record->record_address + 0x10;
        const auto accesses = FindGlobalAccesses(code, value_address, 24);

        for (const GlobalAccess& access : accesses) {
            const std::uintptr_t owner = functions.FunctionContaining(access.site);
            Print(std::format("    0x{:X}  {:<24}  in function 0x{:X}", access.site,
                              access.mnemonic, owner));
            if (owner != 0) {
                auto& names = functions_by_global[owner];
                if (std::find(names.begin(), names.end(), name) == names.end()) {
                    names.push_back(name);
                }
            }
        }
    }

    // If individual records are never referenced, the table must be walked from its
    // base. Finding who references the base identifies the code that owns this system,
    // which is the correct entry point rather than poking records directly.
    Print("");
    Print("=== table base references ===");
    Print("Individual records are unreferenced, so the table is walked from its base.");

    for (const auto& table : registry.value().Tables()) {
        const auto lea_refs  = scanner.FindRipRelativeReferences(code, table.begin, 16);
        const auto data_refs = FindGlobalAccesses(code, table.begin, 16);

        Print(std::format("\ntable 0x{:X} in {} stride 0x{:X} ({} records via '{}')",
                          table.begin, table.section_name, table.stride, table.record_count,
                          table.discovered_via));
        Print(std::format("  {} lea reference(s), {} data access(es)", lea_refs.size(),
                          data_refs.size()));

        for (const std::uintptr_t ref : lea_refs) {
            const std::uintptr_t owner = functions.FunctionContaining(ref);
            Print(std::format("    lea  0x{:X}  in function 0x{:X}", ref, owner));
        }
        for (const GlobalAccess& access : data_refs) {
            const std::uintptr_t owner = functions.FunctionContaining(access.site);
            Print(std::format("    {:<22}  0x{:X}  in function 0x{:X}", access.mnemonic,
                              access.site, owner));
        }
    }

    Print("");
    Print("=== candidate damage functions ===");
    Print("Ranked by how many of the damage related globals each one reads.");

    std::vector<std::pair<std::uintptr_t, std::vector<std::string>>> ranked_functions(
        functions_by_global.begin(), functions_by_global.end());
    std::sort(ranked_functions.begin(), ranked_functions.end(),
              [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });

    for (const auto& [function, names] : ranked_functions) {
        std::string joined;
        for (std::size_t i = 0; i < names.size(); ++i) {
            joined += names[i];
            if (i + 1 < names.size()) {
                joined += ", ";
            }
        }
        Print(std::format("  0x{:X}  reads {} global(s): {}", function, names.size(), joined));
    }
    if (ranked_functions.empty()) {
        Print("  none found");
    }

    Print("");
    Print("=== end ===");
    return 0;
}

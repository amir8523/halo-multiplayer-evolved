// SPDX-License-Identifier: MIT
// ForgeEvolved: Unreal/FNameTrampoline.cpp
#define FE_LOG_CATEGORY "Unreal.FName"

#include "Unreal/FNameTrampoline.h"

#include "Core/Log.h"
#include "Unreal/ProcessMemory.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <array>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

namespace fe::unreal {
namespace {

/// RVA of the real name construction routine.
///
/// Taken from prior work on this title and re-verified at install time against the bytes
/// below, because an RVA alone is a promise about a build rather than a fact about the
/// one that is running.
/// Determined by reading the function rather than taken on trust.
///
/// An earlier attempt used 0x36FFEF0, which is the immediately preceding function and a
/// different overload entirely: it indexes a table by an enum and cannot build a name from
/// a string. It was accepted because its first bytes are a prologue shared by thousands of
/// functions. That produced no crash and no working result, which is the worst combination
/// to debug, so the check below was made specific enough to tell the two apart.
constexpr std::uintptr_t kConstructRva = 0x36FFF40;

/// Opening bytes:
///     mov [rsp+18h], rbx
///     push rbp / push rsi / push rdi
///     mov rbp, rsp
///     sub rsp, 30h
constexpr std::array<std::uint8_t, 14> kConstructPrologue = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC};

/// The bytes that actually identify this function, at kMarkOffset into it:
///     shr rax, 3Fh
///
/// This is the test of bit 63 that decides whether the descriptor holds a string pointer
/// or an already resolved name. A generic prologue proves nothing; this does.
constexpr std::array<std::uint8_t, 4> kConstructMark       = {0x48, 0xC1, 0xE8, 0x3F};
constexpr std::size_t                 kConstructMarkOffset = 0x1B;

/// Marker placed at the adapter's entry so a signature scan can find it.
///
/// Encoded as two multi byte NOPs whose displacement bytes spell MJOL and NIR!. They
/// execute harmlessly, which means the marker can sit in the instruction stream instead
/// of needing somewhere separate to live. This particular marker is kept compatible with
/// the signature the existing tooling already ships.
constexpr std::array<std::uint8_t, 16> kMarker = {0x0F, 0x1F, 0x84, 0x00, 0x4D, 0x4A,
                                                  0x4F, 0x4C, 0x0F, 0x1F, 0x84, 0x00,
                                                  0x4E, 0x49, 0x52, 0x21};

/// Padding bytes between functions. A run of these is free space.
constexpr std::uint8_t kPadding = 0xCC;

/// Room to look for. The adapter and its unwind data come to about 128 bytes; the rest is
/// slack so that rounding the start up to a sixteen byte boundary cannot run past the end
/// of the run.
constexpr std::size_t kNeededRoom = 160;

std::mutex     g_mutex;
TrampolineInfo g_installed;
bool           g_done = false;

/// Kept alive for the process lifetime: the unwinder keeps a pointer to this table.
RUNTIME_FUNCTION g_function_table[1] = {};

using FNameCtorFn = void*(__fastcall*)(void*, const wchar_t*, int);

void Emit(std::vector<std::uint8_t>& out, std::initializer_list<std::uint8_t> bytes) {
    out.insert(out.end(), bytes);
}

void EmitU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

/// Builds the adapter.
///
/// Stack frame, 0x58 bytes, which also restores the sixteen byte alignment a call needs:
///     [rsp+0x00 .. 0x1F]  shadow space for the callee
///     [rsp+0x20 .. 0x4F]  the descriptor, so descriptor+0x20 lands at rsp+0x40
///     [rsp+0x50]          the caller's FName* saved across the call
[[nodiscard]] std::vector<std::uint8_t> BuildAdapter(std::uintptr_t target) {
    std::vector<std::uint8_t> code;
    code.reserve(128);

    code.insert(code.end(), kMarker.begin(), kMarker.end());

    Emit(code, {0x48, 0x83, 0xEC, 0x58});             // sub  rsp, 58h
    Emit(code, {0x48, 0x89, 0x4C, 0x24, 0x50});       // mov  [rsp+50h], rcx   save FName*

    // Clear the descriptor. The routine reads fields this adapter does not set, so they
    // have to be zero rather than whatever the stack happened to hold.
    Emit(code, {0x48, 0x33, 0xC0});                   // xor  rax, rax
    for (std::uint8_t off = 0x20; off <= 0x48; off = static_cast<std::uint8_t>(off + 8)) {
        Emit(code, {0x48, 0x89, 0x44, 0x24, off});    // mov  [rsp+off], rax
    }

    // descriptor+0x08 stays zero: the routine measures the string itself, with
    // "sub rcx, rbx" followed by "sar rcx, 1" after walking to the terminator.
    //
    // descriptor+0x00 = string pointer with bit 63 set, marking it as a string rather
    // than an already resolved name.
    //
    // The offsets here come from reading the routine, not from documentation. It does
    // "mov rbx, [rcx]" and "mov eax, [rcx+8]", so the fields are at +0x00 and +0x08.
    // A previous version used +0x20 and +0x28 as published elsewhere, which put the
    // string pointer past where the routine looks; it read zeros and returned index 0 for
    // every name, including names that certainly exist.
    Emit(code, {0x48, 0x8B, 0xC2});                   // mov  rax, rdx
    Emit(code, {0x48, 0xB9});                         // mov  rcx, 8000000000000000h
    EmitU64(code, 0x8000000000000000ull);
    Emit(code, {0x48, 0x0B, 0xC1});                   // or   rax, rcx
    Emit(code, {0x48, 0x89, 0x44, 0x24, 0x20});       // mov  [rsp+20h], rax   desc+0x00

    // descriptor+0x0C must be non zero.
    //
    // This is not the find or add mode, which is what it was mistaken for. The routine
    // does "cmp byte ptr [rcx+0x0C], 0" and jumps clear of the entire string scan when it
    // is zero, so passing the mode here meant a Find request skipped the lookup and
    // returned whatever happened to be on the stack. It is a flag saying the descriptor
    // carries a string that still needs scanning.
    Emit(code, {0xC6, 0x44, 0x24, 0x2C, 0x01});       // mov  byte ptr [rsp+2Ch], 1

    Emit(code, {0x48, 0x8D, 0x4C, 0x24, 0x20});       // lea  rcx, [rsp+20h]   descriptor
    Emit(code, {0x48, 0x8B, 0x54, 0x24, 0x50});       // mov  rdx, [rsp+50h]   FName*
    Emit(code, {0x48, 0xB8});                         // mov  rax, target
    EmitU64(code, static_cast<std::uint64_t>(target));
    Emit(code, {0xFF, 0xD0});                         // call rax

    // Callers expect the constructed FName back in rax.
    Emit(code, {0x48, 0x8B, 0x44, 0x24, 0x50});       // mov  rax, [rsp+50h]
    Emit(code, {0x48, 0x83, 0xC4, 0x58});             // add  rsp, 58h
    Emit(code, {0xC3});                               // ret

    return code;
}

/// Locates the executable's .text section.
[[nodiscard]] bool FindTextSection(std::uintptr_t base, std::uintptr_t& out_start,
                                   std::size_t& out_size) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (std::memcmp(section->Name, ".text", 5) == 0) {
            out_start = base + section->VirtualAddress;
            out_size  = section->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

/// Finds a run of padding large enough to hold the adapter.
///
/// The search runs backwards from the end of the section because the tail of .text is
/// where the longest padding runs are, and starting there avoids walking megabytes of
/// live code.
/// The section is large, so it is copied out in blocks and scanned locally. Guarding each
/// individual byte would mean tens of millions of protected reads, which is slow enough to
/// stall startup on its own.
[[nodiscard]] std::uintptr_t FindCave(std::uintptr_t start, std::size_t size,
                                      std::size_t needed) {
    if (size < needed) {
        return 0;
    }

    constexpr std::size_t      kBlock = 64 * 1024;
    std::vector<std::uint8_t>  buffer(kBlock);
    std::size_t                run = 0;
    std::size_t                position = size;

    while (position > 0) {
        const std::size_t    length = (position >= kBlock) ? kBlock : position;
        const std::uintptr_t block  = start + position - length;

        if (!memory::GuardedRead(block, buffer.data(), length)) {
            // An unreadable block breaks any run that was building across it.
            run = 0;
            position -= length;
            continue;
        }

        for (std::size_t i = length; i-- > 0;) {
            if (buffer[i] != kPadding) {
                run = 0;
                continue;
            }
            if (++run >= needed) {
                // i is the lowest byte of the run, so the run covers [block+i, +needed).
                // Rounding up to sixteen keeps the entry point where a function is
                // expected to start, and the slack in needed covers the rounding.
                const std::uintptr_t candidate = block + i;
                return (candidate + 15) & ~static_cast<std::uintptr_t>(15);
            }
        }
        position -= length;
    }
    return 0;
}

/// Performs the call under a guard.
///
/// This is separated out because a function containing __try cannot also hold objects that
/// need unwinding, and everything that reports a result here owns a string.
[[nodiscard]] bool CallGuarded(FNameCtorFn call, std::uint32_t* out, const wchar_t* name,
                               std::uint32_t& out_exception) noexcept {
    __try {
        (void)call(out, name, 0 /* find only, never add */);
        out_exception = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out_exception = static_cast<std::uint32_t>(::GetExceptionCode());
        return false;
    }
}

} // namespace

Result InstallFNameTrampoline(TrampolineInfo& out_info) {
    std::lock_guard lock(g_mutex);
    if (g_done) {
        out_info = g_installed;
        return Result::Success();
    }

    const HMODULE module = ::GetModuleHandleW(nullptr);
    if (module == nullptr) {
        return Result::Fail(ErrorCode::InvalidState, "could not resolve the game module");
    }
    const auto base = reinterpret_cast<std::uintptr_t>(module);

    // --- Confirm the target is the function we think it is ---------------------
    const std::uintptr_t target = base + kConstructRva;
    std::array<std::uint8_t, kConstructPrologue.size()> actual{};
    if (!memory::GuardedRead(target, actual.data(), actual.size())) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("could not read the construction routine at 0x{:X}",
                                        target));
    }
    if (std::memcmp(actual.data(), kConstructPrologue.data(), actual.size()) != 0) {
        std::string got;
        for (const std::uint8_t byte : actual) {
            got += std::format("{:02X} ", byte);
        }
        return Result::Fail(
            ErrorCode::InvalidState,
            std::format("the bytes at RVA 0x{:X} are not the expected construction routine "
                        "(got {}); the game was probably updated, so the address needs "
                        "rechecking before this can be used",
                        kConstructRva, got));
    }

    // The prologue alone is not evidence. Thousands of functions share it, and trusting it
    // once already meant forwarding to the wrong overload.
    std::array<std::uint8_t, kConstructMark.size()> mark{};
    if (!memory::GuardedRead(target + kConstructMarkOffset, mark.data(), mark.size()) ||
        std::memcmp(mark.data(), kConstructMark.data(), mark.size()) != 0) {
        std::string got;
        for (const std::uint8_t byte : mark) {
            got += std::format("{:02X} ", byte);
        }
        return Result::Fail(
            ErrorCode::InvalidState,
            std::format("RVA 0x{:X} has the right prologue but not the bit 63 test at +0x{:X} "
                        "(got {}); this is a different function and forwarding to it would "
                        "silently return wrong names",
                        kConstructRva, kConstructMarkOffset, got));
    }

    // --- Find somewhere to put the adapter -------------------------------------
    std::uintptr_t text_start = 0;
    std::size_t    text_size  = 0;
    if (!FindTextSection(base, text_start, text_size)) {
        return Result::Fail(ErrorCode::InvalidState, "could not locate the .text section");
    }

    const std::uintptr_t cave = FindCave(text_start, text_size, kNeededRoom);
    if (cave == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("no run of {} padding bytes was found in .text",
                                        kNeededRoom));
    }

    const std::vector<std::uint8_t> code = BuildAdapter(target);

    // Unwind data goes just past the code, still inside the module, because the address
    // recorded in the function table has to be an offset from the module base.
    const std::size_t    unwind_offset = (code.size() + 3) & ~static_cast<std::size_t>(3);
    const std::uintptr_t unwind_address = cave + unwind_offset;

    // Version 1, no flags; prologue is the marker plus the stack allocation; one code,
    // describing that allocation as (0x58 / 8) - 1.
    const std::array<std::uint8_t, 8> unwind = {
        0x01,
        static_cast<std::uint8_t>(kMarker.size() + 4),
        0x01,
        0x00,
        static_cast<std::uint8_t>(kMarker.size() + 4),
        static_cast<std::uint8_t>((((0x58 / 8) - 1) << 4) | 0x02), // UWOP_ALLOC_SMALL
        0x00,
        0x00};

    const std::size_t total = unwind_offset + unwind.size();

    // --- Write it ---------------------------------------------------------------
    DWORD previous = 0;
    if (::VirtualProtect(reinterpret_cast<void*>(cave), total, PAGE_EXECUTE_READWRITE,
                         &previous) == FALSE) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("could not make the padding at 0x{:X} writable, "
                                        "error {}",
                                        cave, ::GetLastError()));
    }

    std::memcpy(reinterpret_cast<void*>(cave), code.data(), code.size());
    std::memcpy(reinterpret_cast<void*>(unwind_address), unwind.data(), unwind.size());

    DWORD restored = 0;
    ::VirtualProtect(reinterpret_cast<void*>(cave), total, previous, &restored);
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(cave), total);

    // --- Make it a real function as far as the unwinder is concerned -------------
    g_function_table[0].BeginAddress = static_cast<DWORD>(cave - base);
    g_function_table[0].EndAddress   = static_cast<DWORD>(cave + code.size() - base);
    g_function_table[0].UnwindInfoAddress = static_cast<DWORD>(unwind_address - base);

    if (::RtlAddFunctionTable(g_function_table, 1, base) == FALSE) {
        FE_LOG_WARN("RtlAddFunctionTable was refused; the adapter still runs, but a fault "
                    "unwinding through it would not be described");
    }

    g_installed.address       = cave;
    g_installed.module_offset = cave - base;
    g_installed.target        = target;
    g_installed.size          = code.size();
    g_done                    = true;
    out_info                  = g_installed;

    FE_LOG_INFO("FName adapter installed at 0x{:X} (RVA 0x{:X}, {} bytes), forwarding to "
                "0x{:X} (RVA 0x{:X})",
                cave, g_installed.module_offset, code.size(), target, kConstructRva);
    return Result::Success();
}

Result TestFNameTrampoline(const wchar_t* name, std::uint32_t& out_index) {
    TrampolineInfo info;
    if (const Result installed = InstallFNameTrampoline(info); !installed.ok()) {
        return installed;
    }
    if (name == nullptr) {
        return Result::Fail(ErrorCode::InvalidArgument, "no name given");
    }

    // Two words: an FName is a comparison index and a number.
    std::uint32_t constructed[2] = {0, 0};
    std::uint32_t exception      = 0;

    // The call runs game code, so a fault inside it is caught rather than allowed to take
    // the process down. A wrong descriptor layout would show up exactly here.
    if (!CallGuarded(reinterpret_cast<FNameCtorFn>(info.address), constructed, name,
                     exception)) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("the adapter faulted with code 0x{:X}; the "
                                        "descriptor layout is wrong",
                                        exception));
    }

    out_index = constructed[0];
    return Result::Success();
}

Result MakeFName(const wchar_t* name, std::uint64_t& out_name) {
    TrampolineInfo info;
    if (const Result installed = InstallFNameTrampoline(info); !installed.ok()) {
        return installed;
    }
    if (name == nullptr) {
        return Result::Fail(ErrorCode::InvalidArgument, "no name given");
    }

    std::uint32_t constructed[2] = {0, 0};
    std::uint32_t exception      = 0;
    if (!CallGuarded(reinterpret_cast<FNameCtorFn>(info.address), constructed, name,
                     exception)) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("the adapter faulted with code 0x{:X}", exception));
    }

    std::uint64_t packed = 0;
    std::memcpy(&packed, constructed, sizeof(packed));
    out_name = packed;
    return Result::Success();
}

} // namespace fe::unreal

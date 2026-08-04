// SPDX-License-Identifier: MIT
// ForgeEvolved: Unreal/NamePool.cpp
#define FE_LOG_CATEGORY "Unreal.Names"

#include "Unreal/NamePool.h"

#include "Blam/ModuleImage.h"
#include "Core/Log.h"
#include "Core/Pacing.h"
#include "Unreal/ProcessMemory.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>
#include <format>

namespace fe::unreal {
namespace {

/// Readability test, delegated to the cached implementation.
///
/// This used to call VirtualQuery directly for every check, which made one scan of the
/// executable's .data take 18 seconds for 487347 candidate slots. The cached version
/// answers from the last resolved region, so a linear scan pays one query per region
/// instead of one per address.
[[nodiscard]] bool IsReadable(std::uintptr_t address, std::size_t size) noexcept {
    return memory::IsReadable(address, size);
}

/// Reads the length out of an FName entry header.
[[nodiscard]] std::uint16_t HeaderLength(std::uint16_t header) noexcept {
    return static_cast<std::uint16_t>(header >> 6);
}

[[nodiscard]] bool HeaderIsWide(std::uint16_t header) noexcept {
    return (header & 1u) != 0;
}

/// True when a candidate block begins with the entry for "None".
///
/// Index 0 of block 0 is always "None" in UE5, which makes this an unambiguous
/// fingerprint. Both the declared length and the bytes must agree, so a coincidental
/// 0x01xx value followed by unrelated data cannot match.
[[nodiscard]] bool BlockStartsWithNone(std::uintptr_t block) noexcept {
    // Cheap arithmetic rejection before any syscall.
    //
    // Block pointers are scattered heap addresses, so each IsReadable on one is a cache
    // miss and therefore a real VirtualQuery. Screening obvious garbage first cut this
    // scan from roughly 500000 syscalls to a few thousand, which matters because those
    // syscalls contend on the process address space lock while the game is loading.
    if (!memory::IsPlausiblePointer(block)) {
        return false;
    }
    if (!IsReadable(block, 2 + 4)) {
        return false;
    }
    std::uint16_t header = 0;
    std::memcpy(&header, reinterpret_cast<const void*>(block), sizeof(header));

    if (HeaderIsWide(header) || HeaderLength(header) != 4) {
        return false;
    }
    char text[4] = {};
    std::memcpy(text, reinterpret_cast<const void*>(block + 2), sizeof(text));
    return std::memcmp(text, "None", 4) == 0;
}

/// True when a run of block pointers looks like a real Blocks array.
///
/// Requires the first slot to be the "None" block and every populated slot after it to
/// be readable. A single stray pointer to a "None" like allocation elsewhere in memory
/// will not satisfy this.
[[nodiscard]] bool LooksLikeBlocksArray(std::uintptr_t candidate) noexcept {
    if (!IsReadable(candidate, sizeof(std::uintptr_t) * 4)) {
        return false;
    }

    std::uintptr_t first = 0;
    if (!memory::GuardedRead(candidate, &first, sizeof(first))) {
        return false;
    }
    if (!BlockStartsWithNone(first)) {
        return false;
    }

    // Blocks are allocated in large aligned chunks. Requiring the second slot to be
    // either populated and readable, or null, rejects arrays of unrelated pointers.
    std::uintptr_t second = 0;
    std::memcpy(&second, reinterpret_cast<const void*>(candidate + sizeof(std::uintptr_t)),
                sizeof(second));
    if (second != 0 && !IsReadable(second, 2)) {
        return false;
    }
    return true;
}

} // namespace

Expected<NamePool> NamePool::Locate() {
    // The pool lives in the executable's own data, so scan the host module rather than
    // the simulation DLL.
    Expected<blam::ModuleImage> image = blam::ModuleImage::Attach(L"HaloCampaignEvolved.exe");
    if (!image.ok()) {
        // GetModuleHandle(nullptr) equivalent: fall back to the process image by name
        // independent lookup.
        const HMODULE self = ::GetModuleHandleW(nullptr);
        if (self == nullptr) {
            return Error{ErrorCode::ModuleNotLoaded, "cannot locate the host executable"};
        }
        image = blam::ModuleImage::FromMappedImage(reinterpret_cast<std::uintptr_t>(self),
                                                  "host.exe");
        if (!image.ok()) {
            return Error{image.error()};
        }
    }

    const blam::Section* data = image.value().FindSection(".data");
    if (data == nullptr) {
        return Error{ErrorCode::SectionNotFound, "the host executable has no .data section"};
    }

    // Pointer aligned scan: the Blocks array is a member of a global struct and is
    // therefore pointer aligned.
    memory::ResetStats();
    std::size_t inspected = 0;

    // Cooperative: this scan touches half a million addresses, and every readability
    // check can take the process address space lock. Yielding periodically guarantees
    // the game's asset loader is never blocked behind us, which matters most on slower
    // machines where loading takes far longer than it does on a fast one.
    pacing::WorkPacer pacer;
    for (std::uintptr_t address = (data->begin + 7) & ~static_cast<std::uintptr_t>(7);
         address + sizeof(std::uintptr_t) * 4 <= data->end;
         address += sizeof(std::uintptr_t)) {
        ++inspected;
        pacer.Tick();
        if (!LooksLikeBlocksArray(address)) {
            continue;
        }

        NamePool pool;
        pool.blocks_address_ = address;

        // Final confirmation: index 0 must resolve to "None" through the real
        // arithmetic, not just the fingerprint check.
        Expected<std::string> none = pool.Resolve(0);
        if (!none.ok() || none.value() != "None") {
            continue;
        }

        const memory::CacheStats stats = memory::Stats();
        FE_LOG_INFO("FName pool located: Blocks at 0x{:X} ({} populated block(s), inspected {} "
                    "slot(s) with {} VirtualQuery call(s), {} cache hit(s), {} yield(s))",
                    pool.blocks_address_, pool.PopulatedBlockCount(), inspected, stats.queries,
                    stats.hits, pacer.YieldCount());
        return pool;
    }

    return Error{ErrorCode::SymbolNotResolved,
                 std::format("the FName pool fingerprint was not found after inspecting {} "
                             "aligned slot(s) in .data; the engine layout may have changed",
                             inspected)};
}

Expected<NamePool> NamePool::FromBlocks(std::uintptr_t blocks_address) {
    if (blocks_address == 0) {
        return Error{ErrorCode::InvalidArgument, "no Blocks address given"};
    }

    NamePool pool;
    pool.blocks_address_ = blocks_address;

    // Exactly the confirmation Locate uses: index zero has to resolve to "None" through the
    // real arithmetic. A supplied address is treated with the same suspicion as a searched
    // one, so a bad pattern match fails here instead of producing nonsense later.
    const Expected<std::string> none = pool.Resolve(0);
    if (!none.ok() || none.value() != "None") {
        return Error{ErrorCode::SymbolNotResolved,
                     std::format("0x{:X} does not look like the FName pool: index 0 did not "
                                 "resolve to \"None\"",
                                 blocks_address)};
    }

    FE_LOG_INFO("FName pool taken directly: Blocks at 0x{:X} ({} populated block(s))",
                pool.blocks_address_, pool.PopulatedBlockCount());
    return pool;
}

std::uintptr_t NamePool::BlockAt(std::uint32_t block) const {
    if (blocks_address_ == 0 || block >= kNameMaxBlocks) {
        return 0;
    }
    const std::uintptr_t slot = blocks_address_ + static_cast<std::uintptr_t>(block) *
                                                     sizeof(std::uintptr_t);
    if (!IsReadable(slot, sizeof(std::uintptr_t))) {
        return 0;
    }
    std::uintptr_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(slot), sizeof(value));
    return value;
}

std::size_t NamePool::PopulatedBlockCount() const {
    std::size_t count = 0;
    for (std::uint32_t block = 0; block < kNameMaxBlocks; ++block) {
        const std::uintptr_t address = BlockAt(block);
        if (address == 0) {
            break; // Blocks are allocated in order; the first null ends the run.
        }
        ++count;
    }
    return count;
}

Expected<std::string> NamePool::Resolve(std::uint32_t index) const {
    const std::uint32_t block  = index >> kNameBlockOffsetBits;
    const std::uint32_t offset = index & kNameBlockOffsetMask;

    const std::uintptr_t block_address = BlockAt(block);
    if (block_address == 0) {
        return Error{ErrorCode::NotFound,
                     std::format("FName index {} refers to block {}, which is not allocated",
                                 index, block)};
    }

    const std::uintptr_t entry = block_address + static_cast<std::uintptr_t>(offset) *
                                                     kNameEntryStride;
    if (!IsReadable(entry, sizeof(std::uint16_t))) {
        return Error{ErrorCode::NotFound,
                     std::format("FName index {} is not readable", index)};
    }

    std::uint16_t header = 0;
    std::memcpy(&header, reinterpret_cast<const void*>(entry), sizeof(header));

    const std::uint16_t length = HeaderLength(header);
    if (length == 0 || length > kMaxNameLength) {
        return Error{ErrorCode::ValidationFailed,
                     std::format("FName index {} has an implausible length of {}", index,
                                 length)};
    }

    const bool        wide      = HeaderIsWide(header);
    const std::size_t byte_size = static_cast<std::size_t>(length) * (wide ? 2 : 1);
    if (!IsReadable(entry + 2, byte_size)) {
        return Error{ErrorCode::NotFound,
                     std::format("FName index {} text is not readable", index)};
    }

    if (!wide) {
        return std::string(reinterpret_cast<const char*>(entry + 2), length);
    }

    // Wide names are rare but legal. Narrowed for logging; anything outside ASCII
    // becomes '?' rather than producing invalid UTF-8.
    const auto* source = reinterpret_cast<const wchar_t*>(entry + 2);
    std::string narrow;
    narrow.reserve(length);
    for (std::uint16_t i = 0; i < length; ++i) {
        narrow.push_back(source[i] < 128 ? static_cast<char>(source[i]) : '?');
    }
    return narrow;
}

std::vector<NameEntry> NamePool::DumpBlock(std::uint32_t block, std::size_t max_entries) const {
    std::vector<NameEntry> entries;

    const std::uintptr_t block_address = BlockAt(block);
    if (block_address == 0) {
        return entries;
    }

    // Walk entries sequentially. Each is a header plus its text, padded to the entry
    // stride, so the next entry is found by advancing past both.
    std::uintptr_t cursor = block_address;
    const std::uintptr_t block_end = block_address + (static_cast<std::uintptr_t>(1)
                                                     << kNameBlockOffsetBits) * kNameEntryStride;

    while (entries.size() < max_entries && cursor + 2 <= block_end) {
        if (!IsReadable(cursor, sizeof(std::uint16_t))) {
            break;
        }
        std::uint16_t header = 0;
        std::memcpy(&header, reinterpret_cast<const void*>(cursor), sizeof(header));

        const std::uint16_t length = HeaderLength(header);
        if (length == 0 || length > kMaxNameLength) {
            break; // End of the populated region, or padding.
        }

        const bool        wide      = HeaderIsWide(header);
        const std::size_t byte_size = static_cast<std::size_t>(length) * (wide ? 2 : 1);
        if (!IsReadable(cursor + 2, byte_size)) {
            break;
        }

        NameEntry entry;
        entry.index = static_cast<std::uint32_t>(
            (block << kNameBlockOffsetBits) +
            ((cursor - block_address) / kNameEntryStride));
        entry.wide = wide;

        if (wide) {
            const auto* source = reinterpret_cast<const wchar_t*>(cursor + 2);
            entry.text.reserve(length);
            for (std::uint16_t i = 0; i < length; ++i) {
                entry.text.push_back(source[i] < 128 ? static_cast<char>(source[i]) : '?');
            }
        } else {
            entry.text.assign(reinterpret_cast<const char*>(cursor + 2), length);
        }
        entries.push_back(std::move(entry));

        // Advance: header plus text, rounded up to the entry stride.
        const std::size_t consumed = 2 + byte_size;
        const std::size_t padded =
            ((consumed + kNameEntryStride - 1) / kNameEntryStride) * kNameEntryStride;
        cursor += padded;
    }

    return entries;
}

Expected<std::uint32_t> NamePool::FindIndexOf(std::string_view name,
                                              std::size_t max_blocks_to_search) const {
    const std::size_t populated = PopulatedBlockCount();
    const std::size_t limit = populated < max_blocks_to_search ? populated : max_blocks_to_search;

    for (std::uint32_t block = 0; block < limit; ++block) {
        // A full block holds many thousands of entries; the cap here is generous
        // rather than tight because this only runs a handful of times.
        const std::vector<NameEntry> entries = DumpBlock(block, 65536);
        for (const NameEntry& entry : entries) {
            if (entry.text == name) {
                return entry.index;
            }
        }
    }

    return Error{ErrorCode::NotFound,
                 std::format("the name '{}' was not found in the first {} block(s)", name,
                             limit)};
}

} // namespace fe::unreal

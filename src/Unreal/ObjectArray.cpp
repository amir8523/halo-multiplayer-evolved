// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Unreal/ObjectArray.cpp
#define MPE_LOG_CATEGORY "Unreal.Objects"

#include "Unreal/ObjectArray.h"

#include "Blam/ModuleImage.h"
#include "Core/Log.h"
#include "Core/Pacing.h"
#include "Unreal/ProcessMemory.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <format>

namespace mpe::unreal {
namespace {

/// Field offsets inside FChunkedFixedUObjectArray.
constexpr std::size_t kObjectsOffset      = 0x00;
constexpr std::size_t kPreAllocatedOffset = 0x08;
constexpr std::size_t kMaxElementsOffset  = 0x10;
constexpr std::size_t kNumElementsOffset  = 0x14;
constexpr std::size_t kMaxChunksOffset    = 0x18;
constexpr std::size_t kNumChunksOffset    = 0x1C;
constexpr std::size_t kArrayStructSize    = 0x20;

/// Plausible bounds for the element counts. UE's default MaxObjectsInGame is about
/// 2.1 million; the window is wide enough for any configuration and narrow enough to
/// reject noise.
constexpr std::int32_t kMinMaxElements = 1024;
constexpr std::int32_t kMaxMaxElements = 64 * 1024 * 1024;

/// How many objects to sample when scoring a candidate, and how many must resolve.
constexpr std::size_t kSampleSize      = 24;
constexpr std::size_t kRequiredMatches = 12;

/// True when text looks like a UE object or class name.
///
/// UE names are identifiers, sometimes with digits or underscores, and package names
/// contain slashes. Anything with control characters or spaces is not a name and marks
/// a false positive candidate.
[[nodiscard]] bool IsPlausibleObjectName(std::string_view text) noexcept {
    if (text.empty() || text.size() > 256) {
        return false;
    }
    for (const char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F) {
            return false;
        }
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') || c == '_' || c == '/' || c == '.' ||
                             c == '-' || c == '$';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

} // namespace

Expected<ObjectArray> ObjectArray::FromAddress(const NamePool& names,
                                               std::uintptr_t array_address) {
    if (array_address == 0) {
        return Error{ErrorCode::InvalidArgument, "no array address given"};
    }

    ObjectArray candidate(names);
    candidate.array_address_ = array_address;

    // Scored exactly as a searched candidate would be, so a wrong address is rejected
    // rather than accepted just because a caller supplied it.
    const std::size_t score = ScoreCandidate(candidate, 24);
    if (score == 0) {
        return Error{ErrorCode::SymbolNotResolved,
                     std::format("0x{:X} does not describe the object array", array_address)};
    }

    MPE_LOG_INFO("GUObjectArray taken directly at 0x{:X}: {} live object(s), sample score {}",
                array_address, candidate.Count(), score);
    return candidate;
}

std::uintptr_t ObjectArray::ChunkFor(std::uint32_t index) const {
    const std::uint32_t chunk = index / kElementsPerChunk;

    const auto chunk_list = memory::ReadPointer(array_address_ + kObjectsOffset);
    if (!chunk_list.has_value() || !memory::IsPlausiblePointer(*chunk_list)) {
        return 0;
    }
    const auto chunk_pointer =
        memory::ReadPointer(*chunk_list + static_cast<std::uintptr_t>(chunk) * sizeof(std::uintptr_t));
    if (!chunk_pointer.has_value() || !memory::IsPlausiblePointer(*chunk_pointer)) {
        return 0;
    }
    return *chunk_pointer;
}

bool ObjectArray::ReadRaw(std::uintptr_t object, std::uintptr_t& out_class,
                          std::uint32_t& out_name_index) const {
    if (!memory::IsPlausiblePointer(object) || !memory::IsReadable(object, 0x28)) {
        return false;
    }
    const auto class_pointer = memory::ReadPointer(object + kObjectClassOffset);
    const auto name_index    = memory::Read<std::uint32_t>(object + kObjectNameOffset);
    if (!class_pointer.has_value() || !name_index.has_value()) {
        return false;
    }
    if (!memory::IsPlausiblePointer(*class_pointer)) {
        return false;
    }
    out_class      = *class_pointer;
    out_name_index = *name_index;
    return true;
}

std::uint32_t ObjectArray::Count() const {
    const auto value = memory::Read<std::int32_t>(array_address_ + kNumElementsOffset);
    if (!value.has_value() || *value < 0) {
        return 0;
    }
    return static_cast<std::uint32_t>(*value);
}

std::uint32_t ObjectArray::Capacity() const {
    const auto value = memory::Read<std::int32_t>(array_address_ + kMaxElementsOffset);
    if (!value.has_value() || *value < 0) {
        return 0;
    }
    return static_cast<std::uint32_t>(*value);
}

Expected<ObjectInfo> ObjectArray::At(std::uint32_t index) const {
    const std::uintptr_t chunk = ChunkFor(index);
    if (chunk == 0) {
        return Error{ErrorCode::NotFound, std::format("no chunk for index {}", index)};
    }

    const std::uintptr_t item =
        chunk + static_cast<std::uintptr_t>(index % kElementsPerChunk) * kObjectItemSize;
    const auto object = memory::ReadPointer(item);
    if (!object.has_value() || *object == 0) {
        return Error{ErrorCode::NotFound, std::format("slot {} is empty", index)};
    }

    std::uintptr_t class_address = 0;
    std::uint32_t  name_index    = 0;
    if (!ReadRaw(*object, class_address, name_index)) {
        return Error{ErrorCode::NotFound, std::format("object at slot {} is unreadable", index)};
    }

    ObjectInfo info;
    info.address       = *object;
    info.class_address = class_address;
    info.index         = index;
    info.name_index    = name_index;

    if (names_ != nullptr) {
        if (const Expected<std::string> name = names_->Resolve(name_index); name.ok()) {
            info.name = name.value();
        }
        // The class is itself a UObject, so its name resolves the same way.
        std::uintptr_t class_of_class = 0;
        std::uint32_t  class_name_idx = 0;
        if (ReadRaw(class_address, class_of_class, class_name_idx)) {
            if (const Expected<std::string> cls = names_->Resolve(class_name_idx); cls.ok()) {
                info.class_name = cls.value();
            }
        }
    }

    if (const auto outer = memory::ReadPointer(*object + kObjectOuterOffset);
        outer.has_value() && memory::IsPlausiblePointer(*outer)) {
        info.outer_address = *outer;
    }

    return info;
}

std::size_t ObjectArray::ScoreCandidate(const ObjectArray& candidate, std::size_t sample_size) {
    std::size_t matches = 0;
    const std::uint32_t count = candidate.Count();
    if (count == 0) {
        return 0;
    }

    const std::uint32_t limit =
        static_cast<std::uint32_t>(std::min<std::size_t>(sample_size, count));
    for (std::uint32_t index = 0; index < limit; ++index) {
        const Expected<ObjectInfo> object = candidate.At(index);
        if (!object.ok()) {
            continue;
        }
        // Both the object's own name and its class name must resolve to something that
        // looks like a name. This is the test garbage cannot pass.
        if (IsPlausibleObjectName(object.value().name) &&
            IsPlausibleObjectName(object.value().class_name)) {
            ++matches;
        }
    }
    return matches;
}

Expected<ObjectArray> ObjectArray::Locate(const NamePool& names) {
    const HMODULE self = ::GetModuleHandleW(nullptr);
    if (self == nullptr) {
        return Error{ErrorCode::ModuleNotLoaded, "cannot locate the host executable"};
    }
    Expected<blam::ModuleImage> image =
        blam::ModuleImage::FromMappedImage(reinterpret_cast<std::uintptr_t>(self), "host.exe");
    if (!image.ok()) {
        return Error{image.error()};
    }

    const blam::Section* data = image.value().FindSection(".data");
    if (data == nullptr) {
        return Error{ErrorCode::SectionNotFound, "the host executable has no .data section"};
    }

    memory::ResetStats();

    // Cooperative: the loader must never wait behind this scan, however slow the machine.


    pacing::WorkPacer pacer;


    std::size_t    inspected = 0;
    std::size_t    best_score = 0;
    std::uintptr_t best_address = 0;

    for (std::uintptr_t address = (data->begin + 7) & ~static_cast<std::uintptr_t>(7);
         address + kArrayStructSize <= data->end;
         address += sizeof(std::uintptr_t)) {
        ++inspected;

        pacer.Tick();

        // Cheap structural gate first, so the expensive object walk only runs for
        // candidates whose bookkeeping is already self consistent.
        const auto objects      = memory::ReadPointer(address + kObjectsOffset);
        const auto max_elements = memory::Read<std::int32_t>(address + kMaxElementsOffset);
        const auto num_elements = memory::Read<std::int32_t>(address + kNumElementsOffset);
        const auto max_chunks   = memory::Read<std::int32_t>(address + kMaxChunksOffset);
        const auto num_chunks   = memory::Read<std::int32_t>(address + kNumChunksOffset);

        if (!objects.has_value() || !max_elements.has_value() || !num_elements.has_value() ||
            !max_chunks.has_value() || !num_chunks.has_value()) {
            continue;
        }
        if (!memory::IsPlausiblePointer(*objects)) {
            continue;
        }
        if (*max_elements < kMinMaxElements || *max_elements > kMaxMaxElements) {
            continue;
        }
        if (*num_elements <= 0 || *num_elements > *max_elements) {
            continue;
        }
        if (*num_chunks <= 0 || *max_chunks <= 0 || *num_chunks > *max_chunks) {
            continue;
        }
        // Chunk count and element count must agree with the engine's chunk size.
        const std::int32_t implied_chunks = static_cast<std::int32_t>(
            (static_cast<std::uint32_t>(*num_elements) + kElementsPerChunk - 1) /
            kElementsPerChunk);
        if (*num_chunks < implied_chunks || *num_chunks > implied_chunks + 1) {
            continue;
        }
        if (*max_chunks != static_cast<std::int32_t>(
                              (static_cast<std::uint32_t>(*max_elements) + kElementsPerChunk - 1) /
                              kElementsPerChunk)) {
            continue;
        }

        ObjectArray candidate(names);
        candidate.array_address_ = address;

        const std::size_t score = ScoreCandidate(candidate, kSampleSize);
        if (score > best_score) {
            best_score   = score;
            best_address = address;
            // A perfect sample is as good as it gets; stop rather than scan 8 MB more.
            if (score >= kSampleSize) {
                break;
            }
        }
    }

    const memory::CacheStats stats = memory::Stats();

    if (best_score < kRequiredMatches || best_address == 0) {
        return Error{ErrorCode::SymbolNotResolved,
                     std::format("GUObjectArray was not found: best candidate resolved {} of {} "
                                 "sampled objects, {} required. Inspected {} slot(s) with {} "
                                 "VirtualQuery call(s).",
                                 best_score, kSampleSize, kRequiredMatches, inspected,
                                 stats.queries)};
    }

    ObjectArray result(names);
    result.array_address_ = best_address;

    MPE_LOG_INFO("GUObjectArray located at 0x{:X}: {} of {} live object(s), capacity {}, "
                "sample score {}/{}",
                best_address, result.Count(), result.Count(), result.Capacity(), best_score,
                kSampleSize);
    MPE_LOG_INFO("scan cost: {} slot(s) inspected, {} VirtualQuery call(s), {} cache hit(s)",
                inspected, stats.queries, stats.hits);

    return result;
}

void ObjectArray::ForEachRaw(const std::function<bool(const RawObject&)>& visitor) const {
    const std::uint32_t count = Count();

    // Paced like the resolving walk, because the reads still take the address space lock
    // and the game's loader still needs it.
    pacing::WorkPacer pacer;

    for (std::uint32_t index = 0; index < count; ++index) {
        pacer.Tick();

        const std::uintptr_t chunk = ChunkFor(index);
        if (chunk == 0) {
            continue;
        }
        const std::uintptr_t item =
            chunk + static_cast<std::uintptr_t>(index % kElementsPerChunk) * kObjectItemSize;
        const auto object = memory::ReadPointer(item);
        if (!object.has_value() || *object == 0) {
            continue; // Sparse array: empty and freed slots are normal.
        }

        RawObject raw;
        raw.index   = index;
        raw.address = *object;
        if (!ReadRaw(*object, raw.class_address, raw.name_index)) {
            continue;
        }

        // The class is itself a UObject, so its name index is one more read and no
        // allocation at all.
        std::uintptr_t class_of_class = 0;
        if (!ReadRaw(raw.class_address, class_of_class, raw.class_name_index)) {
            continue;
        }

        if (!visitor(raw)) {
            return;
        }
    }
}

void ObjectArray::ForEach(const std::function<bool(const ObjectInfo&)>& visitor,
                          std::uint32_t max_to_visit) const {
    const std::uint32_t count = Count();
    const std::uint32_t limit = (max_to_visit == 0) ? count : std::min(count, max_to_visit);

    // Walking every object resolves a name for each one, so this is paced too.
    pacing::WorkPacer pacer;

    for (std::uint32_t index = 0; index < limit; ++index) {
        pacer.Tick();

        const Expected<ObjectInfo> object = At(index);
        if (!object.ok()) {
            continue; // Sparse array: empty and freed slots are normal.
        }
        if (!visitor(object.value())) {
            return;
        }
    }
}

std::vector<ObjectInfo> ObjectArray::FindByClassName(std::string_view class_name,
                                                     std::size_t max_results) const {
    std::vector<ObjectInfo> results;
    ForEach([&](const ObjectInfo& object) {
        if (object.class_name == class_name) {
            results.push_back(object);
        }
        return results.size() < max_results;
    });
    return results;
}

std::vector<ObjectInfo> ObjectArray::FindByName(std::string_view name,
                                                std::size_t max_results) const {
    std::vector<ObjectInfo> results;
    ForEach([&](const ObjectInfo& object) {
        if (object.name == name) {
            results.push_back(object);
        }
        return results.size() < max_results;
    });
    return results;
}

std::vector<ObjectInfo> ObjectArray::FindByClassNameContains(std::string_view fragment,
                                                             std::size_t max_results) const {
    std::vector<ObjectInfo> results;
    ForEach([&](const ObjectInfo& object) {
        if (!fragment.empty() && object.class_name.find(fragment) != std::string::npos) {
            results.push_back(object);
        }
        return results.size() < max_results;
    });
    return results;
}

std::uintptr_t ObjectArray::ClassOf(std::uintptr_t object_address) const {
    const auto class_pointer = memory::ReadPointer(object_address + kObjectClassOffset);
    if (!class_pointer.has_value() || !memory::IsPlausiblePointer(*class_pointer)) {
        return 0;
    }
    return *class_pointer;
}

std::vector<ObjectInfo> ObjectArray::FindInstancesOfClassAddress(std::uintptr_t class_address,
                                                                 std::size_t max_results) const {
    std::vector<ObjectInfo> results;
    if (class_address == 0) {
        return results;
    }
    ForEach([&](const ObjectInfo& object) {
        if (object.class_address == class_address) {
            results.push_back(object);
        }
        return results.size() < max_results;
    });
    return results;
}

std::string ObjectArray::BuildPath(const ObjectInfo& object, std::size_t max_depth) const {
    // Walk the Outer chain collecting names, then reverse, which yields the engine's
    // own outermost-first path form.
    std::vector<std::string> parts;
    parts.push_back(object.name);

    std::uintptr_t outer = object.outer_address;
    for (std::size_t depth = 0; depth < max_depth && outer != 0; ++depth) {
        std::uintptr_t class_address = 0;
        std::uint32_t  name_index    = 0;
        if (!ReadRaw(outer, class_address, name_index) || names_ == nullptr) {
            break;
        }
        const Expected<std::string> name = names_->Resolve(name_index);
        if (!name.ok()) {
            break;
        }
        parts.push_back(name.value());

        const auto next = memory::ReadPointer(outer + kObjectOuterOffset);
        if (!next.has_value() || !memory::IsPlausiblePointer(*next)) {
            break;
        }
        outer = *next;
    }

    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!path.empty()) {
            path += '.';
        }
        path += *it;
    }
    return path;
}

} // namespace mpe::unreal


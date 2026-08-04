// SPDX-License-Identifier: MIT
// ForgeEvolved: Blam/DebugGlobals.cpp
#define FE_LOG_CATEGORY "Blam.Globals"

#include "Blam/DebugGlobals.h"

#include "Core/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <format>

namespace fe::blam {

std::string_view ToString(GlobalType type) noexcept {
    switch (type) {
        case GlobalType::Boolean: return "boolean";
        case GlobalType::Numeric: return "numeric";
        case GlobalType::Pointer: return "pointer";
    }
    return "unknown";
}

Expected<const SymbolRecord*> DebugGlobals::ResolveWritable(std::string_view name) const {
    const SymbolRecord* record = registry_.Find(name);
    if (record == nullptr) {
        return Error{ErrorCode::SymbolNotResolved,
                     std::format("'{}' is not a known engine symbol", name)};
    }
    if (record->stride < kMinimumStride) {
        // A stride 0x10 record is a string id: a UI label with an id and a group, and
        // no value field at all. This is the check that stops a caller from trying to
        // "turn on" something like forge_main_menu_palettes, which is a menu label.
        return Error{ErrorCode::InvalidState,
                     std::format("'{}' is a string id (stride 0x{:X}), not a writable global; "
                                 "it names a user interface label, not engine state",
                                 name, record->stride)};
    }
    return record;
}

Expected<GlobalInfo> DebugGlobals::Query(std::string_view name) const {
    const SymbolRecord* record = registry_.Find(name);
    if (record == nullptr) {
        return Error{ErrorCode::SymbolNotResolved,
                     std::format("'{}' is not a known engine symbol", name)};
    }

    GlobalInfo info;
    info.name     = record->name;
    info.writable = (record->stride >= kMinimumStride);

    if (!info.writable) {
        return info; // String id: no type or value to report.
    }

    info.address = record->record_address + kValueOffset;
    info.type    = *reinterpret_cast<const std::uint64_t*>(record->record_address + kTypeOffset);
    info.value   = *reinterpret_cast<const std::uint64_t*>(info.address);
    return info;
}

Expected<bool> DebugGlobals::GetBool(std::string_view name) const {
    FE_ASSIGN_OR_RETURN(const GlobalInfo info, Query(name));
    if (!info.writable) {
        return Error{ErrorCode::InvalidState, std::format("'{}' is a string id", name)};
    }
    if (info.type != static_cast<std::uint64_t>(GlobalType::Boolean)) {
        return Error{ErrorCode::InvalidState,
                     std::format("'{}' has type {}, not boolean", name, info.type)};
    }
    return info.value != 0;
}

Expected<std::uint64_t> DebugGlobals::GetNumber(std::string_view name) const {
    FE_ASSIGN_OR_RETURN(const GlobalInfo info, Query(name));
    if (!info.writable) {
        return Error{ErrorCode::InvalidState, std::format("'{}' is a string id", name)};
    }
    if (info.type == static_cast<std::uint64_t>(GlobalType::Pointer)) {
        return Error{ErrorCode::InvalidState,
                     std::format("'{}' holds an address, not a number", name)};
    }
    return info.value;
}

Result DebugGlobals::StoreValue(std::uintptr_t address, std::uint64_t value) {
    // The globals live in .data, which is normally already writable. Protection is
    // adjusted anyway: a shipping build can mark parts of .data read only, and
    // discovering that with an access violation inside the engine's own thread would
    // be a crash with no explanation.
    DWORD previous = 0;
    if (::VirtualProtect(reinterpret_cast<LPVOID>(address), sizeof(std::uint64_t),
                         PAGE_READWRITE, &previous) == FALSE) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("VirtualProtect on 0x{:X} failed with {}", address,
                                        ::GetLastError()));
    }

    *reinterpret_cast<volatile std::uint64_t*>(address) = value;

    DWORD ignored = 0;
    if (::VirtualProtect(reinterpret_cast<LPVOID>(address), sizeof(std::uint64_t), previous,
                         &ignored) == FALSE) {
        // The store succeeded, so this is not a failure of the operation. Logged
        // because leaving a page more permissive than the engine expects is worth
        // knowing about.
        FE_LOG_WARN("could not restore page protection on 0x{:X}", address);
    }
    return Result::Success();
}

Result DebugGlobals::SetBool(std::string_view name, bool value) {
    FE_ASSIGN_OR_RETURN(const SymbolRecord* record, ResolveWritable(name));

    const std::uint64_t type =
        *reinterpret_cast<const std::uint64_t*>(record->record_address + kTypeOffset);
    if (type != static_cast<std::uint64_t>(GlobalType::Boolean)) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("'{}' has type {}, refusing to write a boolean into it",
                                        name, type));
    }

    const std::uintptr_t address = record->record_address + kValueOffset;
    FE_TRY(StoreValue(address, value ? 1u : 0u));

    FE_LOG_INFO("set {} = {}", record->name, value ? 1 : 0);
    return Result::Success();
}

Result DebugGlobals::SetNumber(std::string_view name, std::uint64_t value) {
    FE_ASSIGN_OR_RETURN(const SymbolRecord* record, ResolveWritable(name));

    const std::uint64_t type =
        *reinterpret_cast<const std::uint64_t*>(record->record_address + kTypeOffset);
    if (type == static_cast<std::uint64_t>(GlobalType::Pointer)) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("'{}' holds an address; writing an integer would "
                                        "corrupt it",
                                        name));
    }
    if (type == static_cast<std::uint64_t>(GlobalType::Boolean) && value > 1) {
        return Result::Fail(ErrorCode::InvalidArgument,
                            std::format("'{}' is boolean, so {} is out of range", name, value));
    }

    const std::uintptr_t address = record->record_address + kValueOffset;
    FE_TRY(StoreValue(address, value));

    FE_LOG_INFO("set {} = 0x{:X}", record->name, value);
    return Result::Success();
}

std::vector<GlobalInfo> DebugGlobals::List(std::string_view name_contains,
                                           std::size_t max_results) const {
    std::vector<GlobalInfo> results;

    for (const SymbolRecord& record : registry_.Records()) {
        if (!name_contains.empty() && record.name.find(name_contains) == std::string::npos) {
            continue;
        }
        Expected<GlobalInfo> info = Query(record.name);
        if (!info.ok()) {
            continue;
        }
        results.push_back(std::move(info).value());
        if (results.size() >= max_results) {
            break;
        }
    }

    std::sort(results.begin(), results.end(),
              [](const GlobalInfo& a, const GlobalInfo& b) { return a.name < b.name; });
    return results;
}

Result DebugGlobals::VerifyWritePath(std::string_view name) {
    FE_ASSIGN_OR_RETURN(const GlobalInfo before, Query(name));
    if (!before.writable) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("'{}' is not writable, so it cannot verify the path",
                                        name));
    }
    if (before.type != static_cast<std::uint64_t>(GlobalType::Boolean)) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("'{}' is type {}; the self test needs a boolean", name,
                                        before.type));
    }

    // Flip it, read it back, then restore. Reading back through a separate Query
    // rather than trusting the store is the entire point of the test.
    const std::uint64_t probe = (before.value == 0) ? 1u : 0u;
    FE_TRY(StoreValue(before.address, probe));

    Expected<GlobalInfo> after = Query(name);
    const bool           stuck = after.ok() && after.value().value == probe;

    // Restore regardless of outcome, so a failed test leaves no trace.
    const Result restored = StoreValue(before.address, before.value);
    if (!restored.ok()) {
        FE_LOG_ERROR("could not restore '{}' to its original value", name);
        return restored;
    }

    if (!stuck) {
        return Result::Fail(ErrorCode::SymbolValidationFailed,
                            std::format("wrote {} to '{}' at 0x{:X} but read back {}", probe,
                                        name, before.address,
                                        after.ok() ? after.value().value : 0));
    }

    FE_LOG_INFO("write path verified on '{}' at 0x{:X} (wrote {}, read back {}, restored {})",
                name, before.address, probe, probe, before.value);
    return Result::Success();
}

} // namespace fe::blam

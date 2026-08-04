// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Core/GameBuild.cpp
#define MPE_LOG_CATEGORY "Core.Build"

#include "Core/GameBuild.h"

#include "Core/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <format>
#include <vector>

// The game already imports VERSION.dll, so linking it adds no new dependency.
#pragma comment(lib, "Version.lib")

namespace mpe {
namespace {

/// Full path of the running executable.
[[nodiscard]] std::wstring ExecutablePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD written =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            return std::wstring(buffer.data(), written);
        }
        // Truncated. Grow and retry, bounded so a pathological path cannot loop.
        if (buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
}

/// Reads FileVersion out of the version resource.
[[nodiscard]] std::string ReadFileVersionString(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }

    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) {
        return {};
    }

    std::vector<std::byte> block(size);
    if (::GetFileVersionInfoW(path.c_str(), 0, size, block.data()) == FALSE) {
        return {};
    }

    // Resolve the first translation rather than assuming en-US, since the value
    // is stored per language and the shipping build need not be English.
    struct LangCodePage {
        WORD language;
        WORD code_page;
    };
    LangCodePage* translations = nullptr;
    UINT          translation_bytes = 0;
    if (::VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation",
                         reinterpret_cast<LPVOID*>(&translations), &translation_bytes) == FALSE ||
        translations == nullptr || translation_bytes < sizeof(LangCodePage)) {
        return {};
    }

    const std::wstring query = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\FileVersion",
                                           translations[0].language, translations[0].code_page);

    wchar_t* value = nullptr;
    UINT     value_length = 0;
    if (::VerQueryValueW(block.data(), query.c_str(), reinterpret_cast<LPVOID*>(&value),
                         &value_length) == FALSE ||
        value == nullptr || value_length == 0) {
        return {};
    }

    // The version string is ASCII in practice; narrow defensively.
    std::string narrow;
    narrow.reserve(value_length);
    for (UINT i = 0; i < value_length && value[i] != L'\0'; ++i) {
        narrow.push_back(value[i] < 128 ? static_cast<char>(value[i]) : '?');
    }
    return narrow;
}

/// Fallback identity when the version resource is unreadable. Two identical
/// installs produce the same value and two different builds produce different
/// ones, which is all the handshake needs.
[[nodiscard]] std::string SyntheticBuildString(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (path.empty() ||
        ::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) == FALSE) {
        return "unknown-build";
    }
    const std::uint64_t size = (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) |
                               attributes.nFileSizeLow;
    const std::uint64_t stamp =
        (static_cast<std::uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32) |
        attributes.ftLastWriteTime.dwLowDateTime;
    return std::format("synthetic-{:X}-{:X}", size, stamp);
}

} // namespace

const std::string& GameBuildString() {
    // Function local static: initialized once, thread safe under C++11 and later.
    static const std::string value = [] {
        const std::wstring path = ExecutablePath();
        std::string        build = ReadFileVersionString(path);
        if (build.empty()) {
            build = SyntheticBuildString(path);
            MPE_LOG_WARN("could not read the executable version resource; using '{}'", build);
        } else {
            MPE_LOG_INFO("game build is '{}'", build);
        }
        return build;
    }();
    return value;
}

const std::string& ExecutableDirectory() {
    static const std::string value = [] {
        const std::wstring path = ExecutablePath();
        const std::size_t  slash = path.find_last_of(L"\\/");
        const std::wstring directory =
            (slash == std::wstring::npos) ? std::wstring{} : path.substr(0, slash);

        std::string narrow;
        narrow.reserve(directory.size());
        for (const wchar_t wc : directory) {
            narrow.push_back(wc < 128 ? static_cast<char>(wc) : '?');
        }
        return narrow;
    }();
    return value;
}

} // namespace mpe

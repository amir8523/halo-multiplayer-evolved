// SPDX-License-Identifier: MIT
// ForgeEvolved: Core/Log.h
//
// Thread safe, allocation-light logging with a file sink and an optional
// debugger sink.
//
// Rationale: the Blam simulation calls into our code from its own simulation
// thread while Steam callbacks run on the thread that pumps SteamAPI. Log
// output is therefore mutex guarded. Formatting uses std::format, which is
// evaluated only when the level passes the filter.
#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace fe::log {

enum class Level : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Off   = 5,
};

[[nodiscard]] std::string_view ToString(Level level) noexcept;

/// Parses "trace" | "debug" | "info" | "warn" | "error" | "off".
/// Unrecognized input yields Level::Info so a typo in the config never silences
/// the log entirely.
[[nodiscard]] Level ParseLevel(std::string_view text) noexcept;

/// Opens the log file, truncating any previous run. Safe to call once per
/// process; subsequent calls are ignored. If the file cannot be opened the
/// debugger sink still receives output.
void Initialize(const std::filesystem::path& file, Level min_level);

/// Flushes and closes the sink. Called from the module unload path.
void Shutdown();

void SetMinLevel(Level level) noexcept;
[[nodiscard]] Level MinLevel() noexcept;

/// Writes one pre-formatted line. Prefer the macros below.
void Write(Level level, std::string_view category, std::string_view message);

/// Internal: used by the macros so std::format runs only when enabled.
template <typename... Args>
void WriteFormatted(Level level, std::string_view category,
                    std::format_string<Args...> fmt, Args&&... args) {
    if (level < MinLevel()) {
        return;
    }
    Write(level, category, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace fe::log

// Each translation unit defines FE_LOG_CATEGORY before including this header,
// keeping call sites free of a repeated literal.
#ifndef FE_LOG_CATEGORY
#define FE_LOG_CATEGORY "ForgeEvolved"
#endif

#define FE_LOG_TRACE(...) \
    ::fe::log::WriteFormatted(::fe::log::Level::Trace, FE_LOG_CATEGORY, __VA_ARGS__)
#define FE_LOG_DEBUG(...) \
    ::fe::log::WriteFormatted(::fe::log::Level::Debug, FE_LOG_CATEGORY, __VA_ARGS__)
#define FE_LOG_INFO(...) \
    ::fe::log::WriteFormatted(::fe::log::Level::Info,  FE_LOG_CATEGORY, __VA_ARGS__)
#define FE_LOG_WARN(...) \
    ::fe::log::WriteFormatted(::fe::log::Level::Warn,  FE_LOG_CATEGORY, __VA_ARGS__)
#define FE_LOG_ERROR(...) \
    ::fe::log::WriteFormatted(::fe::log::Level::Error, FE_LOG_CATEGORY, __VA_ARGS__)

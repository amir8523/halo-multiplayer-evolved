// SPDX-License-Identifier: MIT
// ForgeEvolved: loader/version_proxy.cpp
//
// Minimal proxy for VERSION.dll.
//
// WHY THIS APPROACH
//
// HaloCampaignEvolved.exe statically imports VERSION.dll. Windows resolves that
// import from the executable's own directory before the system directory, so a
// version.dll placed next to the game binary is loaded automatically at process
// start, before the engine or Steam has initialized.
//
// The consequences for a non technical user are the whole point:
//   no injector, no launcher, no administrator rights, no antivirus prompt from a
//   process opening another process, and no separate step to remember. Copy two
//   files in, delete them to uninstall.
//
// CORRECTNESS REQUIREMENTS
//
//   1. Every export the game may call must be forwarded to the real DLL. Missing
//      one turns into a crash the moment the game reads a version resource. The
//      linker /export:X=path,@ordinal form forwards without a thunk, so the real
//      implementation is called with no cost and no chance of a signature mistake.
//
//   2. DllMain must do almost nothing. It runs under the loader lock, where
//      LoadLibrary of anything with its own dependencies can deadlock. So the mod
//      is loaded on a separate thread and DllMain returns immediately.
//
//   3. A failure to load the mod must leave the game fully playable. The proxy
//      still forwards every version API, so the worst case is a game with no mod
//      rather than a game that will not start.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdio>

// ---------------------------------------------------------------------------
// Export forwarding
// ---------------------------------------------------------------------------
//
// Forwarded to the copy in the system directory. Using the full path is
// deliberate: a bare "version.GetFileVersionInfoW" would resolve back to this
// proxy and recurse until the stack is gone.

#pragma comment(linker, "/export:GetFileVersionInfoA=C:\\Windows\\System32\\version.GetFileVersionInfoA,@1")
#pragma comment(linker, "/export:GetFileVersionInfoByHandle=C:\\Windows\\System32\\version.GetFileVersionInfoByHandle,@2")
#pragma comment(linker, "/export:GetFileVersionInfoExA=C:\\Windows\\System32\\version.GetFileVersionInfoExA,@3")
#pragma comment(linker, "/export:GetFileVersionInfoExW=C:\\Windows\\System32\\version.GetFileVersionInfoExW,@4")
#pragma comment(linker, "/export:GetFileVersionInfoSizeA=C:\\Windows\\System32\\version.GetFileVersionInfoSizeA,@5")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExA=C:\\Windows\\System32\\version.GetFileVersionInfoSizeExA,@6")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExW=C:\\Windows\\System32\\version.GetFileVersionInfoSizeExW,@7")
#pragma comment(linker, "/export:GetFileVersionInfoSizeW=C:\\Windows\\System32\\version.GetFileVersionInfoSizeW,@8")
#pragma comment(linker, "/export:GetFileVersionInfoW=C:\\Windows\\System32\\version.GetFileVersionInfoW,@9")
#pragma comment(linker, "/export:VerFindFileA=C:\\Windows\\System32\\version.VerFindFileA,@10")
#pragma comment(linker, "/export:VerFindFileW=C:\\Windows\\System32\\version.VerFindFileW,@11")
#pragma comment(linker, "/export:VerInstallFileA=C:\\Windows\\System32\\version.VerInstallFileA,@12")
#pragma comment(linker, "/export:VerInstallFileW=C:\\Windows\\System32\\version.VerInstallFileW,@13")
#pragma comment(linker, "/export:VerLanguageNameA=C:\\Windows\\System32\\version.VerLanguageNameA,@14")
#pragma comment(linker, "/export:VerLanguageNameW=C:\\Windows\\System32\\version.VerLanguageNameW,@15")
#pragma comment(linker, "/export:VerQueryValueA=C:\\Windows\\System32\\version.VerQueryValueA,@16")
#pragma comment(linker, "/export:VerQueryValueW=C:\\Windows\\System32\\version.VerQueryValueW,@17")

namespace {

constexpr const wchar_t* kModFileName  = L"ForgeEvolved.dll";
constexpr const wchar_t* kLoaderLogName = L"ForgeEvolved\\loader.log";

HMODULE g_mod_module = nullptr;

/// Writes one line to a loader log next to the game binary.
///
/// The mod's own logging is unavailable until it loads, so a load failure would
/// otherwise be invisible. This file is the first thing to check when a user
/// reports "the mod does nothing".
void LogLine(const wchar_t* format, ...) {
    wchar_t directory[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, directory, MAX_PATH) == 0) {
        return;
    }
    wchar_t* const last_slash = wcsrchr(directory, L'\\');
    if (last_slash == nullptr) {
        return;
    }
    *(last_slash + 1) = L'\0';

    wchar_t log_path[MAX_PATH] = {};
    if (swprintf_s(log_path, MAX_PATH, L"%s%s", directory, kLoaderLogName) < 0) {
        return;
    }

    // The subdirectory may not exist yet on a fresh install.
    wchar_t log_directory[MAX_PATH] = {};
    if (swprintf_s(log_directory, MAX_PATH, L"%sForgeEvolved", directory) >= 0) {
        ::CreateDirectoryW(log_directory, nullptr);
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, log_path, L"a, ccs=UTF-8") != 0 || file == nullptr) {
        return;
    }

    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    fwprintf(file, L"[%04d-%02d-%02d %02d:%02d:%02d] ", now.wYear, now.wMonth, now.wDay,
             now.wHour, now.wMinute, now.wSecond);

    va_list args;
    va_start(args, format);
    vfwprintf(file, format, args);
    va_end(args);

    fwprintf(file, L"\n");
    fclose(file);
}

/// Loads the mod. Runs on its own thread, off the loader lock.
DWORD WINAPI LoadModThread(LPVOID) {
    wchar_t module_path[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, module_path, MAX_PATH) == 0) {
        LogLine(L"GetModuleFileNameW failed with %lu", ::GetLastError());
        return 1;
    }
    wchar_t* const last_slash = wcsrchr(module_path, L'\\');
    if (last_slash == nullptr) {
        LogLine(L"the executable path has no directory component");
        return 1;
    }
    *(last_slash + 1) = L'\0';

    wchar_t mod_path[MAX_PATH] = {};
    if (swprintf_s(mod_path, MAX_PATH, L"%s%s", module_path, kModFileName) < 0) {
        LogLine(L"the mod path is too long");
        return 1;
    }

    // Apply a downloaded update before anything is loaded.
    //
    // This is the only moment the mod's DLL is not mapped into the process, and therefore
    // the only moment Windows will allow it to be replaced. The mod downloads a new build
    // while the game runs and leaves it alongside as a .pending file; here it simply
    // becomes the mod.
    //
    // The old build is kept as .backup rather than deleted, so a bad update can be undone by
    // hand without downloading anything, and the swap is skipped entirely if the replace
    // fails, which leaves the working build in place.
    wchar_t pending_path[MAX_PATH] = {};
    if (swprintf_s(pending_path, MAX_PATH, L"%s%s.pending", module_path, kModFileName) > 0 &&
        ::GetFileAttributesW(pending_path) != INVALID_FILE_ATTRIBUTES) {
        wchar_t backup_path[MAX_PATH] = {};
        if (swprintf_s(backup_path, MAX_PATH, L"%s%s.backup", module_path, kModFileName) > 0) {
            ::DeleteFileW(backup_path);
            if (::GetFileAttributesW(mod_path) != INVALID_FILE_ATTRIBUTES) {
                ::MoveFileW(mod_path, backup_path);
            }
            if (::MoveFileW(pending_path, mod_path) != FALSE) {
                LogLine(L"applied a downloaded update; the previous build is kept as %s.backup",
                        kModFileName);
            } else {
                // Put the working build back rather than leaving nothing to load.
                ::MoveFileW(backup_path, mod_path);
                ::DeleteFileW(pending_path);
                LogLine(L"a downloaded update could not be applied (%lu); keeping the current "
                        L"build",
                        ::GetLastError());
            }
        }
    }

    if (::GetFileAttributesW(mod_path) == INVALID_FILE_ATTRIBUTES) {
        LogLine(L"%s was not found next to the game executable; the game will run unmodded",
                kModFileName);
        return 1;
    }

    // The mod's directory is added to the search path so it can ship its own
    // dependencies without polluting the game folder.
    g_mod_module = ::LoadLibraryExW(mod_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (g_mod_module == nullptr) {
        const DWORD error = ::GetLastError();
        LogLine(L"LoadLibrary('%s') failed with %lu. A missing Visual C++ runtime is the usual "
                L"cause; install the latest x64 redistributable.",
                mod_path, error);
        return 1;
    }

    LogLine(L"loaded %s", kModFileName);
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            // No thread notifications: this DLL has no per thread state and the
            // game creates many threads.
            ::DisableThreadLibraryCalls(module);

            // Deliberately not LoadLibrary here. DllMain holds the loader lock,
            // and the mod links the C++ runtime, Steamworks and more, any of which
            // can deadlock if loaded under it.
            const HANDLE thread = ::CreateThread(nullptr, 0, &LoadModThread, nullptr, 0, nullptr);
            if (thread != nullptr) {
                ::CloseHandle(thread);
            } else {
                LogLine(L"CreateThread for the mod loader failed with %lu", ::GetLastError());
            }
            break;
        }

        case DLL_PROCESS_DETACH:
            // reserved is non null when the process is terminating, in which case
            // Windows is tearing everything down and unloading is both unnecessary
            // and unsafe.
            if (reserved == nullptr && g_mod_module != nullptr) {
                ::FreeLibrary(g_mod_module);
                g_mod_module = nullptr;
            }
            break;

        default:
            break;
    }
    return TRUE;
}

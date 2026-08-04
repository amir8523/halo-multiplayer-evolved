// SPDX-License-Identifier: MIT
// MultiplayerEvolved: tools/trigger/MPE_Trigger.cpp
//
// Sends a command to MultiplayerEvolved running inside the game.
//
// WHY THIS EXISTS
//
// The mod lives inside HaloCampaignEvolved.exe and there is no in game UI to drive it.
// Until there is, everything it can do is unreachable: the exports exist, and nothing
// calls them.
//
// This tool closes that gap. It writes a command string into the game's address space
// and starts a thread at the mod's MPE_Command export. That is the whole mechanism, and
// it is enough to drive every operation the mod exposes.
//
// HOW THE ADDRESS IS FOUND
//
// CreateRemoteThread needs the address of MPE_Command inside the game process, which is
// not the address it has here. Both are the same DLL, so the offset from module base to
// the export is identical in both processes:
//
//   1. Load our own copy of MultiplayerEvolved.dll locally, without running its DllMain, and
//      ask for the export. Subtracting the local base gives the offset.
//   2. Find the module base of MultiplayerEvolved.dll inside the game.
//   3. Remote address is remote base plus that offset.
//
// No pattern scanning and no hardcoded addresses: the loader already solved this.
//
// Usage:
//   MPE_Trigger.exe "ff status"
//   MPE_Trigger.exe "ff on"
//   MPE_Trigger.exe "diag"
//   MPE_Trigger.exe "globals cheat"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>

#include <cstdio>
#include <string>

namespace {

void Print(const std::string& text) {
    std::fputs(text.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

/// Finds the game process by executable name.
[[nodiscard]] DWORD FindGameProcess() {
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD found  = 0;

    if (::Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (::_wcsicmp(entry.szExeFile, L"HaloCampaignEvolved.exe") == 0) {
                found = entry.th32ProcessID;
                break;
            }
        } while (::Process32NextW(snapshot, &entry) != FALSE);
    }
    ::CloseHandle(snapshot);
    return found;
}

/// Finds a module's base address inside another process.
[[nodiscard]] std::uintptr_t FindRemoteModuleBase(DWORD process_id, const wchar_t* module_name) {
    // A 64 bit target needs the 32 bit list flag too on some Windows versions, and the
    // call can fail transiently while the target is loading modules, so it is retried.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const HANDLE snapshot = ::CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
        if (snapshot == INVALID_HANDLE_VALUE) {
            ::Sleep(250);
            continue;
        }

        MODULEENTRY32W entry{};
        entry.dwSize          = sizeof(entry);
        std::uintptr_t result = 0;

        if (::Module32FirstW(snapshot, &entry) != FALSE) {
            do {
                if (::_wcsicmp(entry.szModule, module_name) == 0) {
                    result = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                    break;
                }
            } while (::Module32NextW(snapshot, &entry) != FALSE);
        }
        ::CloseHandle(snapshot);
        if (result != 0) {
            return result;
        }
        ::Sleep(250);
    }
    return 0;
}

/// Offset of an export within our own copy of the DLL.
[[nodiscard]] std::uintptr_t FindExportOffset(const wchar_t* dll_path, const char* export_name,
                                              std::string& out_error) {
    // DONT_RESOLVE_DLL_REFERENCES maps the image without running DllMain, so loading the
    // mod here does not start a second copy of it.
    const HMODULE local = ::LoadLibraryExW(dll_path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (local == nullptr) {
        out_error = "could not map MultiplayerEvolved.dll locally, error " +
                    std::to_string(::GetLastError());
        return 0;
    }

    const FARPROC address = ::GetProcAddress(local, export_name);
    if (address == nullptr) {
        out_error = std::string("this build of MultiplayerEvolved.dll does not export ") + export_name;
        ::FreeLibrary(local);
        return 0;
    }

    const std::uintptr_t offset = reinterpret_cast<std::uintptr_t>(address) -
                                  reinterpret_cast<std::uintptr_t>(local);
    ::FreeLibrary(local);
    return offset;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        Print("usage: MPE_Trigger.exe \"<command>\"");
        Print("  ff status | ff on | ff off | diag | globals <substring> | watch");
        return 2;
    }

    // The command is ASCII; narrow it without a locale dependent conversion.
    std::string command;
    for (const wchar_t* c = argv[1]; *c != L'\0'; ++c) {
        command.push_back(*c < 128 ? static_cast<char>(*c) : '?');
    }

    const DWORD process_id = FindGameProcess();
    if (process_id == 0) {
        Print("the game is not running");
        return 1;
    }

    // The mod's own directory is where our copy of the DLL lives, next to this tool.
    wchar_t tool_path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, tool_path, MAX_PATH);
    if (wchar_t* slash = ::wcsrchr(tool_path, L'\\'); slash != nullptr) {
        *(slash + 1) = L'\0';
    }
    const std::wstring dll_path = std::wstring(tool_path) + L"MultiplayerEvolved.dll";

    std::string          error;
    const std::uintptr_t offset = FindExportOffset(dll_path.c_str(), "MPE_Command", error);
    if (offset == 0) {
        Print(error);
        return 1;
    }

    const std::uintptr_t remote_base = FindRemoteModuleBase(process_id, L"MultiplayerEvolved.dll");
    if (remote_base == 0) {
        Print("MultiplayerEvolved.dll is not loaded in the game; is the mod installed?");
        return 1;
    }

    const std::uintptr_t remote_function = remote_base + offset;

    const HANDLE process = ::OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
            PROCESS_QUERY_INFORMATION,
        FALSE, process_id);
    if (process == nullptr) {
        Print("could not open the game process, error " + std::to_string(::GetLastError()) +
              ". Try running this from an elevated prompt.");
        return 1;
    }

    // The command string has to live in the target's address space for the duration of
    // the call.
    const SIZE_T size = command.size() + 1;
    void* const  remote_text =
        ::VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote_text == nullptr) {
        Print("could not allocate memory in the game process");
        ::CloseHandle(process);
        return 1;
    }

    SIZE_T written = 0;
    if (::WriteProcessMemory(process, remote_text, command.c_str(), size, &written) == FALSE ||
        written != size) {
        Print("could not write the command into the game process");
        ::VirtualFreeEx(process, remote_text, 0, MEM_RELEASE);
        ::CloseHandle(process);
        return 1;
    }

    const HANDLE thread = ::CreateRemoteThread(
        process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_function),
        remote_text, 0, nullptr);
    if (thread == nullptr) {
        Print("CreateRemoteThread failed, error " + std::to_string(::GetLastError()));
        ::VirtualFreeEx(process, remote_text, 0, MEM_RELEASE);
        ::CloseHandle(process);
        return 1;
    }

    // A command that logs a lot can take a moment; a generous wait costs nothing here.
    const DWORD wait = ::WaitForSingleObject(thread, 30000);
    DWORD       result = 0;
    if (wait == WAIT_OBJECT_0) {
        ::GetExitCodeThread(thread, &result);
        Print("command '" + command + "' returned " + std::to_string(static_cast<int>(result)));
    } else {
        Print("the command did not finish within 30 s");
    }

    ::CloseHandle(thread);
    // Freed only after the thread has finished, so the string outlives its use.
    ::VirtualFreeEx(process, remote_text, 0, MEM_RELEASE);
    ::CloseHandle(process);

    Print("see MultiplayerEvolved\\MultiplayerEvolved.log for the output");
    return (wait == WAIT_OBJECT_0) ? 0 : 1;
}

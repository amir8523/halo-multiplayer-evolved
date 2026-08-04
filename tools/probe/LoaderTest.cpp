// SPDX-License-Identifier: MIT
// MultiplayerEvolved: tools/probe/LoaderTest.cpp
//
// Smoke test for the built mod DLL.
//
// Loads MultiplayerEvolved.dll exactly as the game's loader proxy does, waits for its
// startup thread to make progress, then reports what the exported API says and what
// the log recorded.
//
// This catches the whole class of failure that is otherwise only visible as "the
// game does not start": a crash during DLL initialization, a missing runtime
// dependency, a bad export, or a logging fault. It found one already, a CRT
// fastfail from writing narrow bytes into a wide oriented log stream.
//
// It is placed in the game's binaries directory when run, so the mod resolves its
// data directory and the shipped steam_api64.dll the same way it will in the game.
// The engine module is deliberately absent from this process, so the expected
// outcome is that symbol discovery reports the simulation module never appeared and
// the mod stays inert. That is the correct fail closed behaviour.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdio>
#include <string>

namespace {

using PFN_IsReady   = int (*)();
using PFN_Version   = const char* (*)();
using PFN_Phase     = const char* (*)();
using PFN_Host      = int (*)(const char*, const char*, const char*, int, int);
using PFN_LobbyTest = int (*)(int);

void Print(const std::string& text) {
    std::fputs(text.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

template <typename Fn>
[[nodiscard]] Fn Resolve(HMODULE module, const char* name) {
    return reinterpret_cast<Fn>(::GetProcAddress(module, name));
}

} // namespace

int main(int argc, char** argv) {
    const int wait_seconds = (argc > 1) ? std::atoi(argv[1]) : 12;

    Print("loading MultiplayerEvolved.dll ...");
    const HMODULE module =
        ::LoadLibraryExW(L"MultiplayerEvolved.dll", nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        Print("FAIL: LoadLibrary failed with " + std::to_string(::GetLastError()));
        Print("      A missing Visual C++ runtime is the usual cause, but this build links");
        Print("      the CRT statically, so look at the loader log instead.");
        return 1;
    }
    Print("loaded, module handle acquired");

    const auto version = Resolve<PFN_Version>(module, "MPE_Version");
    const auto ready   = Resolve<PFN_IsReady>(module, "MPE_IsReady");
    const auto phase   = Resolve<PFN_Phase>(module, "MPE_Phase");
    const auto host    = Resolve<PFN_Host>(module, "MPE_HostSession");

    if (version == nullptr || ready == nullptr || phase == nullptr || host == nullptr) {
        Print("FAIL: one or more exports did not resolve");
        return 1;
    }
    Print(std::string("MPE_Version() = ") + version());

    // The startup path polls for the engine module and for Steam on its own thread.
    // Sampled once a second so the progression is visible.
    for (int second = 1; second <= wait_seconds; ++second) {
        ::Sleep(1000);
        Print("t+" + std::to_string(second) + "s  MPE_IsReady=" + std::to_string(ready()) +
              "  MPE_Phase=" + phase());
    }

    // Hosting must be refused here: the engine binding cannot resolve in a process
    // that has no engine. A negative value is the negated mpe::ErrorCode, and
    // anything other than a refusal would mean the capability gate is not working.
    const int host_result = host("slayer", "levels/test/test", nullptr, 8, 1);
    Print("MPE_HostSession(...) = " + std::to_string(host_result) +
          (host_result < 0 ? "  (refused, which is correct without an engine)"
                           : "  UNEXPECTED: hosting was accepted"));

    // The lobby self test drives the Steam metadata plane end to end. It works
    // without the engine, which is the entire point: it isolates the networking half
    // from the engine half.
    if (const auto lobby_test = Resolve<PFN_LobbyTest>(module, "MPE_LobbySelfTest");
        lobby_test != nullptr) {
        Print("");
        Print("running lobby self test (creates a real Steam lobby) ...");
        const int lobby_result = lobby_test(20);
        Print("MPE_LobbySelfTest(20) = " + std::to_string(lobby_result) +
              (lobby_result == 0 ? "  PASSED" : "  FAILED, see the log"));
    } else {
        Print("MPE_LobbySelfTest not exported by this build");
    }

    Print("");
    Print("done. Inspect MultiplayerEvolved\\MultiplayerEvolved.log for the startup record.");

    // ExitProcess rather than returning: the startup thread may still be polling,
    // and the module's DllMain skips its teardown when the process is terminating,
    // which is the intended path.
    ::ExitProcess(host_result < 0 ? 0 : 1);
}

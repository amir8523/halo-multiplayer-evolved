# ForgeEvolved

Multiplayer and custom maps for Halo: Campaign Evolved.

Slayer and Capture the Flag over Steam with no port forwarding, plus a JSON map
format and an external editor.

## The idea in one paragraph

The game's gameplay does not run on Unreal. Unreal Engine 5 is the shell; the
simulation is `HaloSimulation_tag_release.dll`, a Reach or H4 era Blam engine whose
shipped binary still contains the multiplayer gametypes, a complete host and client
replication layer, Forge, the Megalo scripting VM, and the networked map variant
pipeline. What it lacks is a transport and a session driver that work on Steam,
because the shell binds networking to PlayFab Party and uses Steam only for
presence.

So ForgeEvolved does not reimplement any of that. It supplies the transport
(`ISteamNetworkingSockets` over the Steam Datagram Relay), the lobby and session
driver, and a map pipeline, and lets Blam remain authoritative for everything it
already knows how to do.

The full evidence and reasoning is in
[docs/00-ARCHITECTURE.md](docs/00-ARCHITECTURE.md).

## Status

Everything below marked "built and verified" has been compiled and run, not just
written.

| Component | State |
| --- | --- |
| Builds with zero third party dependencies | Verified |
| `ForgeEvolved.dll`, 12 exported `FE_*` entry points | Verified |
| `version.dll` proxy, all 17 version exports forwarded | Verified |
| Loads into the running game without crashing | **Verified in game** |
| Steam API binding to the game's own `steam_api64.dll` | **Verified in game** |
| Steam interfaces and callback ABI registration | **Verified in game** |
| Symbol discovery against the live relocated Blam module | **Verified in game**, 5/5 required |
| Blam descriptor records read and write | Memory access works; engine does not read them |
| **Steam lobby create, metadata, member data, leave** | **Verified against live Steam** |
| Relay transport, listen server over SDR | Initializes in game, no peer test yet |
| Wire protocol with role and phase authorization gate | Compiles, unit level only |
| Lobby state machine, roster, ready, synchronized launch | Compiles, not yet exercised |
| Map format, parser, canonical binary, hash identity | Compiles, not yet exercised |
| Blam command ABI (start a match) | **Not done.** See below |
| Frontend UI integration | **Not done.** No in game menu is hooked |
| Forge Studio | Architecture and core editing model only |

Startup inside a running `HaloCampaignEvolved.exe`, from `ForgeEvolved.log`:

```
[Mod]          game build: 2026.07.25.1112544.4-Rel-i343-Meteorite-2607-CU3
[Blam.Module]  attached to HaloSimulation_tag_release.dll at 0x7FFE312F0000
[Blam.Symbols] indexed 3649 record(s) across 3 table(s); 5/5 required
[Steam.Api]    net_sockets='SteamNetworkingSockets012' networking_ready=true
[Lobby.Steam]  steam matchmaking hooks registered for user <id>
[Net.Steam]    steam transport ready (virtual port 22701, relay warm up requested)
[Mod]          ForgeEvolved ready
```

Lobby verification, from `FE_LobbySelfTest` against live Steam:

```
lobby self test: creating a lobby
lobby self test: lobby 109775244801953286 created, owner=true
lobby self test:   fe.protocol = 1
lobby self test:   fe.build    = <game build>
lobby self test:   fe.host     = <steam id>
lobby self test:   member ready flag round trip: 1
lobby self test: PASSED
```

That covers CreateLobby, the LobbyCreated call result firing (which proves the
hand written CallResult ABI), metadata publish and read back, the member data path a
ready flag travels before any transport connection exists, and LeaveLobby.

Measured findings about the engine, including the command descriptor structure and the
writable versus read only split, are in
[docs/04-ENGINE-BINDING.md](docs/04-ENGINE-BINDING.md).

### What is actually playable right now

Nothing. The mod installs and loads, and refuses to host or join with a stated
reason. Two things are missing before a match can happen, and neither is more code
of the kind already here.

**1. The engine command binding.** Symbol discovery works and is self validating.
Run it yourself:

```bash
build\SymbolProbe.exe "..\Meteorite\Binaries\Win64\HaloSimulation_tag_release.dll"
```

It maps the module by hand, applies relocations, executes no engine code, and
reports three name tables it identified:

| Table | Address | Stride | Records | Layout | Holds |
| --- | --- | --- | --- | --- | --- |
| 0 | `.rdata 0x1808303E8` | `0x10` | 2173 | `{const char*, u32 id, u32 category}` | `network_session_*`, `forge_object_properties_*` |
| 1 | `.data 0x1809A35F8` | `0x18` | 1149 | `{const char*, u64 type, u64 value}` | `net_speculative_host_migration_disable` |
| 2 | `.data 0x1809A1738` | `0x18` | 327 | same | `enable_console_window` |

Tables 1 and 2 are the debug globals, and their value field at `+0x10` is a working
read and write surface for engine booleans.

What is missing is the *commands*. `net_load_and_use_map_variant`,
`net_build_map_variant`, `write_current_map_variant`,
`read_map_variant_and_make_current`, `net_simulation_set_stream_bandwidth` and
`console_command` all exist as strings at known addresses, but no pointer to them
exists in any static table. They are registered by code at runtime. Reaching them
needs RIP relative cross reference analysis over 7.9 MB of `.text` to find each
registration call site and the handler pointer passed with it, then the calling
convention for that handler. `PatternScanner::FindRipRelativeReferences` is the
starting point and is already written. This is disassembler and debugger work.

**2. No UI is hooked.** The mod exposes `FE_HostSession`, `FE_JoinSession`,
`FE_SetReady` and the rest, but nothing calls them. The game's own Multiplayer entry
drives its shipped co-op flow over PlayFab Party and has no connection to this mod.
Someone has to either hook the UE5 frontend or ship a separate overlay that calls
the `FE_*` API.

Until both are done, `InertEngineControl` is wired in and the mod is deliberately
inert. The game is unaffected.

## Building

Requires Visual Studio 2022 or newer, CMake 3.24 or newer, and the
[Steamworks SDK](https://partner.steamgames.com/downloads/list), which cannot be
fetched automatically because Valve's licence requires each developer to download it.

```bash
cmake -B build -S . -DFE_STEAMWORKS_SDK=C:/sdk/steamworks -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

To install straight into the game after each build:

```bash
cmake -B build -S . -DFE_STEAMWORKS_SDK=C:/sdk/steamworks -DFE_GAME_DIR="D:/SteamLibrary/steamapps/common/Halo Campaign Evolved"
cmake --build build --config Release --target install_to_game
```

## Installing

Copy `version.dll`, `ForgeEvolved.dll` and the `ForgeEvolved/` folder into:

```
<game>/Meteorite/Binaries/Win64/
```

That is the whole install. The game statically imports `VERSION.dll`, so the proxy
is loaded automatically at startup with no injector, no launcher and no
administrator rights. Delete the three items to uninstall.

Release packaging and the Nexus path are in
[docs/03-PACKAGING-AND-NEXUS.md](docs/03-PACKAGING-AND-NEXUS.md).

## Layout

```
src/
├── Core/      Result, logging, hashing, bounds checked serialization, build identity
├── Blam/      Module attach, pattern and string scanning, symbol discovery
├── Engine/    IEngineControl, the seam between the mod and the game
├── Net/       Transport interface, wire protocol, Steam relay transport
├── Lobby/     Platform facade, Steam matchmaking hooks, the lobby state machine
├── Map/       Map model, JSON and canonical binary, transactional injector
└── ModMain.cpp  Wiring, tick loop, public C API

loader/        The version.dll proxy
data/          Symbol descriptors and example maps, shipped with releases
docs/          Architecture, map format, packaging
tools/         Forge Studio
```

## Design rules

These are not style preferences. Each one exists because its absence causes a
specific class of failure.

**No hardcoded addresses, ever.** A game update shifts every address, and a
hardcoded offset turns a working mod into a crash on patch day. Everything is
discovered at runtime by anchoring on the engine's own name strings, which are
stable precisely because the engine depends on them.

**Fail closed.** Discovery either fully succeeds and validates, or the mod stays
loaded and completely inert with the reason in the log. A half resolved binding
that guesses at a missing entry is how mods corrupt saves.

**No exceptions.** The Blam DLL is built without exception support across its ABI
boundary and unwinding through engine frames corrupts its state. Every fallible
operation returns `Result` or `Expected<T>`, both `[[nodiscard]]`, so a dropped
failure is a compile warning. The JSON parser is the only thing that can throw and
its exceptions are contained at the call site.

**All remote input is hostile.** `ByteReader` cannot over-read. `DecodePacket`
rejects malformed frames. `IsMessageAcceptable` checks the message against the local
role and phase before the body is parsed, so a client cannot start matches on the
host. Map payloads are hash verified before being parsed.

**Every observer callback arrives on the tick thread.** Steam callbacks fire on the
game's thread, so both Steam integrations copy the event into a queue and nothing
else. `Poll` drains it on the tick thread. That is why `LobbyManager` contains no
locks, and a lock appearing in it means this contract has been broken below.

**Three interfaces, not two.** `ILobbyBackend`, `IPeerTransport` and
`IEngineControl` exist so a dedicated server swaps one implementation, a game patch
touches one implementation, and the whole lobby state machine can be tested with no
game and no Steam client.

## Contributing

Good first places to work, in rough order of how self contained they are:

1. **A symbol descriptor for a new game build.** Data only, no code. See
   `data/symbols/` and the instructions inside the existing descriptor.
2. **Forge Studio domain and application layers.** Pure C#, no UI dependency, fully
   testable. See `tools/ForgeStudio/README.md`.
3. **Map format fields.** The procedure is at the end of
   [docs/02-MAP-FORMAT.md](docs/02-MAP-FORMAT.md).
4. **A Vortex game extension.** Highest leverage packaging work there is; after it
   exists, installing is one click.
5. **A public lobby browser.** No new transport work needed; the lobby keys it would
   filter on are already published.

Match the surrounding code. Comments explain why a decision was made, not what a
line does.

## Licence

MIT. See LICENSE.

Not affiliated with Microsoft, 343 Industries or Valve. Halo is a trademark of
Microsoft.

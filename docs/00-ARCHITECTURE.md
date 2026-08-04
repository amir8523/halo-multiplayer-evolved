# MultiplayerEvolved architecture

## What the game actually is

Every design decision below follows from static analysis of the shipped build
`2026.07.25.1112544.4-Rel-i343-Meteorite-2607-CU3`. The findings matter because
they invert the obvious plan.

| Observation | Evidence in the install |
| --- | --- |
| Unreal Engine 5 is the shell, not the game | `Meteorite/` project layout, IoStore TOC version 8, `Engine/Plugins/Halo` |
| Gameplay runs on Blam | `Meteorite/Binaries/Win64/HaloSimulation_tag_release.dll`, single export `CreateBlamEngineShell` |
| The Blam build is Reach or H4 era, not H1 | `survival_game_engine`, `game_engine_sandbox_variant_block`, `saved_film_category_multiplayer`, `s_game_engine_ai_traits` |
| Classic gametypes are already present | `slayer`, `territories`, `oddball_effect`, `juggernaut`, `flood_infection`, `game_engine_team_options_block`, `slayer leader traits` |
| A complete session and replication layer is present | `network_session_class_system_link` / `_xbox_live` / `_offline`, `network_session_privacy_*`, `network_session_security_set_challenge_response`, `simulation_interpolation_*`, `simulation_priority_display_*`, `net_simulation_set_stream_bandwidth`, `net_speculative_host_migration_disable` |
| Forge is already present | `forge_main`, `forge_main_menu_tools`, `forge_main_menu_palettes`, `forge_object_properties_physics` / `_spawn_time` / `_team` / `_shape_radius`, `superforge` |
| Map variants already replicate | `net_build_map_variant`, `net_load_and_use_map_variant`, `net_verify_map_variant`, `write_current_map_variant`, `read_map_variant_and_make_current` |
| The Megalo scripting VM is present | `megalogamengine`, `megalo_debug`, `CS:Megalo Globals` |
| A command surface exists | `console_command`, `enable_console_window`, `CS:Telnet console` |
| The Blam layer is platform agnostic | Party and NAT strings (`join_failed_unable_to_connect_party_strict_nat`) with zero Steam or PlayFab strings inside the DLL |
| Networking is bound to PlayFab at the shell level | `PartyWin.dll` and `PlayFabMultiplayerWin.dll` imported by the executable only |
| Steam is present but only for presence | `SteamMatchMaking009`, `SteamFriends017`, `SteamUser023`, `SteamUtils010`, and no `SteamNetworkingSockets` interface string |
| No anti-cheat | No EAC or BattlEye files anywhere in the install |
| Loading is trivial | The executable statically imports `VERSION.dll` |

The consequence: **do not write a replication layer, gametype rules, or an object
injector.** All three already ship. Write the transport and the session driver
that the shipped systems are missing, and let Blam remain authoritative.

## Layering

```
                 ┌──────────────────────────────────────────┐
   UI / script → │  Public C API  (MPE_HostSession, ...)      │
                 └────────────────────┬─────────────────────┘
                                      │
                 ┌────────────────────▼─────────────────────┐
                 │            LobbyManager                   │
                 │  authority, roster, ready, countdown,     │
                 │  synchronized launch, map distribution    │
                 └───┬──────────────┬──────────────┬────────┘
                     │              │              │
        ┌────────────▼───┐  ┌───────▼────────┐  ┌──▼───────────────┐
        │ ILobbyBackend  │  │ IPeerTransport │  │ IEngineControl   │
        │ (metadata)     │  │ (data)         │  │ (game)           │
        └────────┬───────┘  └───────┬────────┘  └──┬───────────────┘
                 │                  │              │
     SteamMatchmakingHooks  SteamSocketsTransport   Blam binding
     ISteamMatchmaking      ISteamNetworkingSockets SymbolRegistry
     ISteamFriends          + Steam Datagram Relay  + debug table
```

Three interfaces, three reasons:

- **Dedicated servers.** Swap `SteamSocketsTransport` for an address bound
  transport. `LobbyManager` is untouched.
- **Patch resilience.** A game update changes only the Blam binding.
- **Testability.** The full lobby state machine runs headless against fakes with
  no game process and no Steam client.

## Network flow

### Two planes, deliberately separate

| | Metadata plane | Data plane |
| --- | --- | --- |
| Carrier | Steam lobby | `ISteamNetworkingSockets` over SDR |
| Rate | A few messages per minute | Up to the simulation tick rate |
| Carries | Who is here, mode, map name, map hash, host identity, ready flags | Handshake, roster, settings, map payload, engine datagrams |
| Exists when | From "Multiplayer" pressed until the session ends | From the first client connection onward |

A lobby exists before any connection does, and it is how a client learns the
host's identity. Conflating the two is the usual cause of lobbies that show
players who are not actually connected.

### Topology

Listen server. The host owns the authoritative Blam simulation. Clients connect
to the host and to nobody else, so a client has exactly one connection and the
host's uplink is the constraint the lobby manages. `SessionClass::SystemLink`
is selected in the engine, because that is its direct peer to peer path with no
platform account service in the loop, and it is the path our transport then
carries.

### Why SDR

`CreateListenSocketP2P` routes through Valve's relay network:

- No port forwarding, no UPnP, no router configuration.
- Neither peer learns the other's IP address.
- The NAT traversal problem the engine's own strings show it hits
  (`join_failed_unable_to_connect_party_strict_nat`) is absorbed by the relay.

Cost is added latency through the relay. `PeerStats::is_relayed` reports whether
a given connection is relayed, and Steam promotes to a direct route on its own
when one is available.

### Lanes

Four lanes, configured with `ConfigureConnectionLanes`, lower value sent first:

| Lane | Channel | Priority | Reason |
| --- | --- | --- | --- |
| 0 | Control | 0 | Handshake and keepalive must never queue |
| 1 | Lobby | 0 | Roster, settings, countdown, launch |
| 3 | Simulation | 5 | Gameplay, above bulk |
| 2 | MapTransfer | 20 | Multi megabyte, must never delay a launch |

Without lane separation a 2 MB map download sits ahead of a `LaunchNow` and the
match starts several seconds late on that peer.

### Connect sequence

```
Client                       Steam                        Host
  │                            │                            │
  │  accept invite (overlay)   │                            │
  │───────────────────────────>│                            │
  │  GameLobbyJoinRequested_t  │                            │
  │<───────────────────────────│                            │
  │  JoinLobby                 │                            │
  │───────────────────────────>│                            │
  │  LobbyEnter_t              │                            │
  │<───────────────────────────│                            │
  │                                                         │
  │  read fe.protocol, fe.build, fe.host from lobby data    │
  │  reject locally on mismatch, with an explanation        │
  │                                                         │
  │  ConnectP2P(host identity, virtual port 22701) ────────>│
  │                                       AcceptConnection  │
  │                                  ConfigureConnectionLanes
  │  HandshakeRequest (build, name, claimed identity) ─────>│
  │                        verify claimed == authenticated  │
  │                        verify game build matches        │
  │                        verify lobby not full, phase ok  │
  │<──────────────────────────── HandshakeAccept (slot, team)│
  │<──────────────────────────── MatchSettingsSync          │
  │<──────────────────────────── RosterUpdate               │
  │<──────────────────────────── MapManifest                │
  │  MapChunkRequest x N ─────────────────────────────────> │
  │<──────────────────────────── MapChunk x N               │
  │  verify SHA-256, parse, then MapTransferDone ─────────> │
  │  ReadyStateChange(true) ──────────────────────────────> │
  │<──────────────────────────── RosterUpdate (ready)       │
```

The client validates compatibility from lobby metadata **before** connecting, so
a mismatched build produces a sentence the player can act on rather than a failed
handshake.

### Launch sequence

Mirrors the original Combat Evolved transition, where the match becomes live on
every machine at the same simulation tick.

```
1. Host verifies every peer is ready and holds the selected map.
2. Host broadcasts LaunchCountdown once per second.
   Any regression cancels it with a reason everyone sees:
     a peer unreadies, a peer loses the map, a peer disconnects,
     the roster falls below two, the host changes the map.
3. At zero the host broadcasts LaunchNow carrying
     scenario, map content hash, shared random seed, host wall clock
   then flushes immediately and begins loading itself.
4. Every peer, host included, takes the identical path:
     ApplyMatchSettings -> LoadMapVariant -> BeginLoadScenario
   and reports LoadProgress upstream, unreliable and monotonic.
5. When every peer reports 1.0 the host broadcasts AllPeersLoaded,
   flushes, and calls LaunchMatch. Peers call LaunchMatch on receipt.
```

Step 4 is what makes the transition feel right. Without it, early loaders spawn
into an empty map while others are still on the loading screen.

### Authority model

The host is authoritative over everything a player can see. A client sends
requests and waits for the host's snapshot. Concretely:

- `ReadyStateChange` is a request. The host decides, and `RosterUpdate` is what
  changes local state on every machine.
- `RosterUpdate` is a full snapshot with a monotonic revision, not a delta. The
  roster is small and bounded, and a snapshot cannot desynchronize the way a
  missed delta can. Stale revisions are discarded.
- Chat is relayed by the host, which stamps the author from the authenticated
  connection rather than trusting the body. A client cannot speak as another
  player.

### Hostile input

Every byte on the wire may be hostile. Three structural defences, in order:

1. **`ByteReader` cannot over-read.** A failed read sets a sticky flag and leaves
   the output untouched, so a handler can issue a whole sequence of reads and
   check once at the end without risking a buffer overrun in between.
2. **`DecodePacket`** rejects short frames, bad magic, version mismatch, unknown
   types, oversized payloads, and any type arriving on the wrong channel.
3. **`IsMessageAcceptable(type, role, phase)`** is the authorization matrix. It
   runs *before* the body is parsed. A client sending `LaunchNow`, or any peer
   sending `ReadyStateChange` before completing the handshake, is disconnected for
   `ProtocolViolation`. Without this gate a client could start matches on the
   host.

Beyond the protocol: identity is cross checked against the transport's
authenticated identity, map payloads are SHA-256 verified before parsing, every
count on the wire is bounds checked before it drives a loop or a reserve, and
chat is stripped of control characters.

### Bandwidth

`kAssumedHostUplinkBytesPerSecond` is 512 KB/s, divided by peer count and floored
at 24 KB/s per peer, then applied through
`IEngineControl::SetSimulationBandwidth`, which drives the engine's own
`net_simulation_set_stream_bandwidth`. Recomputed whenever the roster changes.
Overshooting a home uplink produces loss that the engine then spends the match
recovering from, so the default is deliberately conservative.

### Host migration

Disabled for the duration of a match through `SetHostMigrationEnabled(false)`,
which drives `net_speculative_host_migration_disable`. Our listen server
designates the host explicitly; a speculative migration would hand authority to a
peer whose transport is not in host mode.

## Engine binding

`HaloSimulation_tag_release.dll` exports exactly one symbol, so everything else
is located by inspection. The project **never hardcodes an address**, because a
game update shifts every address and a hardcoded offset turns a working mod into
a crash on patch day.

Instead, discovery anchors on the engine's own debug name strings, which are
stable across patches precisely because the engine depends on them:

```
1. Locate a NUL terminated anchor literal, for example
   "net_load_and_use_map_variant", requiring a NUL on both sides so
   a substring cannot match.
2. Find pointer aligned locations in .data whose 8 byte value equals
   that literal's address. Each is a candidate record's name field.
3. For each candidate and each stride in {16, 24, 32, 40, 48, 56, 64},
   walk backward and forward requiring N consecutive records whose name
   field points at a plausible engine identifier.
4. Accept a table only when at least 8 consecutive records validate,
   then expand to its full extent and index every record by name.
5. Cross validate: every required symbol must be present in the
   discovered table, or discovery fails.
```

**Fail closed.** Discovery either fully succeeds and validates, or the mod stays
loaded and completely inert: `InertEngineControl` reports zero capabilities, so
`HostSession` and `JoinSession` refuse immediately with the reason, before a lobby
is created or another player is involved. The game remains fully playable. A half
resolved registry that guesses at a missing entry is how mods corrupt saves.

When discovery fails, `SymbolRegistry::BuildDiscoveryReport` emits the table
geometry, the first records, and a hex dump of one record annotated with which
fields point into the module and what strings they resolve to. That report is what
a contributor attaches to an issue, and `MPE_DumpDiagnostics()` writes it on
demand.

Supporting a new game patch is adding a JSON file under `data/symbols/`, named
after the build string. No code change.

## Map pipeline

```
Forge Studio          .fmap.json          MapVariantParser        MapVariant
(external editor) ──> (authoring) ──────> ParseJsonFile ────────> (validated model)
                                          strict, total,               │
                                          diagnostics by JSON path     │
                                                                       ▼
                                                          WriteCanonicalBinary
                                                          deterministic bytes
                                                                       │
                                                    ┌──────────────────┴─────────┐
                                                    ▼                            ▼
                                              SHA-256 identity            wire payload
                                              manifest, LaunchNow         chunked, CRC per
                                                                          chunk, hash at end
                                                                                 │
                                                                                 ▼
                                                              engine map variant loader
                                                              (net_load_and_use_map_variant)
                                                              or MapVariantInjector
```

Two representations exist for one reason: **map identity must be exact.** If peers
hashed formatted JSON, a trailing newline or a reordered key would make identical
maps appear different and block the launch. So JSON is the authoring format,
canonical binary is the identity and transfer format, and the hash is taken over
the binary. Determinism comes from a fixed field order, every collection sorted by
id, and floats emitted as IEEE-754 bit patterns.

Validation is semantic, not only structural. A syntactically perfect map can still
be unplayable: CTF with one flag stand, or a team with no spawns. Those are caught
at author time rather than by eight people who just sat through a loading screen.

`MapVariantInjector` is two phase and transactional. Phase 1 resolves every
palette key against the loaded scenario without placing anything, so an
unresolvable key fails with the world untouched. Phase 2 places objects and
records every handle; any failure despawns everything the apply created. A half
applied map is worse than no map, because the host would be playing a layout no
client can reproduce.

## Threading

One rule, enforced structurally: **every observer callback arrives on the mod tick
thread.**

Steam callbacks fire on whichever thread pumps `SteamAPI_RunCallbacks`, which
inside this process is the game's thread, not ours. So both
`SteamSocketsTransport` and `SteamMatchmakingHooks` do the minimum possible work
in a callback: copy the event into a mutex guarded queue. `Poll` drains that queue
on the tick thread and issues notifications from there. Received data is pulled,
not pushed, via `ReceiveMessagesOnPollGroup`, so it needs no queue at all.

The consequence is that `LobbyManager` contains no locks. A lock appearing in it
would be a sign this contract has been broken somewhere below.

`owns_callback_pump` defaults to false for exactly this reason. It is set true
only in a standalone dedicated server or a test harness that owns the Steam pipe.

## Error handling

The Blam DLL is compiled without exception support across its ABI boundary, and
unwinding through engine frames corrupts its state. So every fallible operation
returns `Result` or `Expected<T>` and nothing throws. The only library that can
throw is the JSON parser, and its exceptions are contained at the call site in
`MapVariantParser.cpp` and `SymbolRegistry.cpp` and converted to `Error`.

`Result` is `[[nodiscard]]`, so a dropped failure is a compile warning.

## Startup

The proxy loads us before the engine shell exists and before Steam initializes.
Nothing may assume either is ready, so startup is a bounded poll:

1. Open the log first, so any later failure is recorded.
2. Poll for `HaloSimulation_tag_release.dll`, up to 120 seconds.
3. Attach, discover the debug table, validate required symbols.
4. Poll for Steam to be logged on, then create the hooks and the transport.
5. Construct `LobbyManager` and start the 60 Hz tick loop.

Any step failing leaves the mod inert with the reason in the log and in every
refusal the UI receives.

## Loading vector

The executable statically imports `VERSION.dll`, and Windows resolves that from
the executable's directory before the system directory. A `version.dll` proxy
next to the game binary is therefore loaded automatically at process start.

No injector, no launcher, no administrator rights, no antivirus prompt from one
process opening another, and no step to remember. Two files in to install, delete
them to uninstall.

The proxy forwards all 17 version exports by full path (a bare forward would
resolve back to itself and recurse), and `DllMain` does nothing but spawn a
thread, because `LoadLibrary` under the loader lock can deadlock.

## What remains before a first playable build

Symbol discovery is complete and self validating. The one thing static analysis
cannot establish is the **call ABI of a debug table record**, meaning which field
offset holds the handler and what its signature is. That needs one debugger
session against a live process, and `BuildDiscoveryReport` prints exactly the hex
view needed to determine it.

Until that constant is confirmed, `InertEngineControl` is wired in and the mod
refuses to host or join with a clear reason. Everything above the engine binding
is complete and exercisable: the lobby state machine, the relay transport, the
protocol, the map pipeline.

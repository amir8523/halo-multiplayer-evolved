# Engine binding: what is known

Everything here was measured against
`HaloSimulation_tag_release.dll` from build
`2026.07.25.1112544.4-Rel-i343-Meteorite-2607-CU3`, reproducible with the two probe
tools. Addresses are given at the module's preferred base `0x180000000`. The module
loads at a relocated base in the real game (observed `0x7FFE312F0000`), and both
probes and the mod handle that.

Reproduce:

```bash
build\SymbolProbe.exe "..\Meteorite\Binaries\Win64\HaloSimulation_tag_release.dll"
build\XrefProbe.exe   "..\Meteorite\Binaries\Win64\HaloSimulation_tag_release.dll"
```

Neither tool executes a single instruction of engine code. Both map the PE by hand
and apply base relocations themselves, because a normal `LoadLibrary` of this module
runs its `DllMain`, which fails fast without its host shell and takes the tool with
it.

## Verified in the live game

Confirmed from `MultiplayerEvolved.log` with the mod loaded into a running
`HaloCampaignEvolved.exe`:

```
attached to HaloSimulation_tag_release.dll at 0x7FFE312F0000 (7 sections)
discovery indexed 3649 record(s) across 3 table(s); 5/5 required, 5/12 optional
steam binding: user='SteamUser023' friends='SteamFriends018'
               matchmaking='SteamMatchMaking009'
               net_sockets='SteamNetworkingSockets012'
               net_utils='SteamNetworkingUtils004' networking_ready=true
steam matchmaking hooks registered for user <id>
steam transport ready (virtual port 22701, relay warm up requested)
MultiplayerEvolved ready
```

Discovery against the relocated live image produced results identical to the offline
probe, which is the property that matters: the technique does not depend on a fixed
base.

## Name tables

Three tables, all found by anchoring on strings the engine itself depends on.

### Table 0: string ids

```
.rdata 0x1808303E8 .. 0x180838BB8   stride 0x10   2173 records

struct {
    const char*   name;   // +0x00
    std::uint32_t id;     // +0x08
    std::uint32_t group;  // +0x0C
};
```

Holds `network_session_class_*`, `network_session_privacy_*` and
`forge_object_properties_*`. Example: `network_session_class_system_link` has id
`0x5A` in group `0x20`.

These are identifiers, not commands. They name the values the session layer accepts,
which is what the lobby needs in order to speak about session class and privacy, but
they are not callable.

### Tables 1 and 2: debug globals

```
.data 0x1809A35F8 .. 0x1809AA1B0   stride 0x18   1149 records
.data 0x1809A1738 .. 0x1809A35E0   stride 0x18    327 records

struct {
    const char*   name;   // +0x00
    std::uint64_t type;   // +0x08   5 for every boolean observed
    std::uint64_t value;  // +0x10   inline, 0 in the file image
};
```

Confirmed members include `net_speculative_host_migration_disable`,
`enable_console_window`, `debug_projectiles`, `check_system_heap`,
`frontend_throttles_main_time` and about 1400 others.

The value is inline at `+0x10` rather than behind a pointer: the field is zero in the
file image and carries no base relocation, which a pointer to storage would. So this
is a working read and write surface for engine debug globals, addressable by name.

## The controllable surface, measured

`SymbolProbe` writes `build/all_symbols.txt`, the complete vocabulary the engine
exposes. 3649 records:

| Kind | Stride | Count | Writable |
| --- | --- | --- | --- |
| Debug globals | `0x18` | 1476 | Yes, value inline at `+0x10` |
| String ids | `0x10` | 2173 | No, they are UI labels |

This split is the single most important fact about what a mod can do to this build,
and it answers the multiplayer question definitively.

**Every multiplayer and Forge name is a string id.** Checked by grep over the dump:
`game_engine*`, `slayer*`, `variant*`, `variant_sandbox`, `megalo*`, `forge_main`,
`forge_main_menu_palettes`, `forge_object_properties_*`, `team_attacker`,
`team_defender`, `team_neutral`, `betray`, `kill_betrayal`, `boot_betrayer`. All of
them are stride `0x10`. They are localization tokens for a user interface that has
no content behind it, not switches.

**There is no friendly fire or player versus player damage global.** The only
writable records matching damage, combat, betrayal or team are:

```
debug_damage_player_inflicted_duration   type=6
debug_damage_player_inflicted_recent     type=5
respawn_players_into_friendly_vehicle    type=5
ai_render_recent_damage                  type=5
debug_player_damage                      type=5
debug_damage_networking                  type=5
... and 30 more, every one a debug_ or ai_render_ visualization toggle
```

Every one is a debug visualization or logging flag. None changes a damage rule. The
allegiance check that stops co-op players hurting each other is compiled logic in the
damage path, not a value, so no amount of writing to globals reaches it. Changing it
would mean patching instruction bytes, which is a different and far more fragile
undertaking than anything else in this project.

### CORRECTION: the globals are not a working control surface

An earlier version of this document, and statements made alongside it, claimed the
`cheat_*` family was writable and that this was a working capability. **That was
wrong.** The claim rested on inferring the record layout from its shape, and the
inference did not survive being tested.

`XrefProbe` was extended to decode every RIP relative encoding MSVC emits for
accessing a global, not just the `REX.W` forms it originally handled: plain 32 bit and
8 bit `mov` both directions, `movzx` and `movsx` byte and word loads, `cmp byte`,
`cmp dword`, and `mov` immediate stores. With that coverage it found:

```
cheat_deathless_player  record at 0x1809A92F8
    +0x00: 0 data accesses, 0 lea references
    +0x08: 0 data accesses, 0 lea references
    +0x10: 0 data accesses, 0 lea references
```

The same result for `cheat_omnipotent`, `cheat_medusa`, `debug_player_damage` and
`debug_damage`. Then the table bases themselves:

```
table 0x1808303E8 (.rdata, 2173 records)  0 lea references, 0 data accesses
table 0x1809A35F8 (.data,  1149 records)  0 lea references, 0 data accesses
table 0x1809A1738 (.data,   327 records)  0 lea references, 0 data accesses
```

Nothing in 7.9 MB of `.text` references any record field, or any table base.

Two conclusions follow:

1. **The field at `+0x10` is not storage the engine reads.** Writing to it changes no
   engine behaviour. `DebugGlobals::VerifyWritePath` passing proves only that the page
   is writable and that a value read back equals what was written. It proves nothing
   about the write mattering, and presenting it as verification of a control surface
   was a mistake.

2. **This machinery is most likely residual.** The interpretation that fits every
   measurement is that the engine was compiled with its full tag and debug
   descriptor data, while the subsystems that consume it are not wired up in this
   product. That single explanation covers all of it: the strings are present, the
   descriptors are present, nothing references them, `levels\multi\` is an empty
   search path, and no friendly fire rule exists.

`MPE_SetGlobal` is retained because reading and writing that memory is exactly what it
says it does, and because it is the tool a contributor would use to test this further.
It is no longer described as a way to enable cheats or change gameplay.

## Command descriptors

This is the structure the earlier resolver missed, and finding it is the main result
of the cross reference pass.

Six commands were traced. Each is an individual static descriptor in `.rdata`, not a
member of an array:

| Command | Descriptor |
| --- | --- |
| `write_current_map_variant` | `0x1807FF248` |
| `net_load_and_use_map_variant` | `0x1807FF298` |
| `read_map_variant_and_make_current` | `0x1807FF338` |
| `net_verify_map_variant` | `0x1807FF408` |
| `net_build_map_variant` | `0x1807FF458` |
| `net_simulation_set_stream_bandwidth` | `0x1807FE248` |

Every one has an identical shape, relative to the name field:

```
-0x08   0x0000000000000004      constant across all six
+0x00   const char* name
+0x08   0                       null in all six, most likely help text
+0x10   0x180350490             .text, a function start, IDENTICAL in all six
+0x18   0x1801B2430             .text, not a function start, IDENTICAL in all six
+0x20   0
+0x28   0
+0x30   packed u64, varies      0x900050002, 0x90001, 0x80001, 0x800080002
```

Three conclusions follow, and the third is the one that matters:

1. **These are not array members.** The gaps between descriptors are `0x50`, `0x170`
   and `0x210`, which share no common stride. They are separately emitted static
   objects, which is exactly why no pointer array contains them and why a
   stride based table walk could never find them.

2. **The name is not referenced from code.** `XrefProbe` found zero RIP relative
   references to any of the six strings in the 7.9 MB `.text`. The name pointer is
   baked into the static initializer, so nothing loads it with a `lea`.

3. **`+0x10` and `+0x18` are not the per command handlers.** They are byte for byte
   identical across six semantically unrelated commands. A per command function
   cannot be. They are shared behaviour, almost certainly a template instantiation
   or a common base, and the per command distinction lives in the packed integer at
   `+0x30` together with the `-0x20` slot.

## What `console_command` actually is

Not a command. It has zero pointer holders anywhere in the module, and is referenced
only by two `lea rdx, [rip+...]` sites at `0x1803294BD` and `0x180329977`, both
immediately calling `0x1801F8700`. Being passed in `rdx` to a shared callee alongside
other strings makes it a log or category label. Treating it as a dispatch entry point
was a wrong assumption, now corrected in the descriptor.

## What is still missing, precisely

To call a command by name three unknowns remain, and none can be resolved by reading
data:

1. **The calling convention of `0x180350490`.** Needs disassembly. Its parameters and
   whether it takes the descriptor, an argument list, or both is unknown.
2. **The meaning of the packed field at `+0x30`.** The values `0x900050002` and
   `0x80001` look like packed argument type and count descriptors, but that is a
   guess until the parser that reads them is disassembled.
3. **The registration and lookup path.** Static descriptors must be registered
   somewhere during initialization for a name lookup to work. That path is unknown,
   so there is no known way to ask the engine for a command by name.

Nothing in the shipped mod pretends otherwise. `InertEngineControl` is wired in and
refuses to host or join with a stated reason.

## The next concrete step

Disassemble `0x180350490` and `0x1801B2430`, at the relocated base, with a debugger
attached to the running game. `0x1801B2430` is the more interesting of the two,
because `.pdata` does not describe it as a function start, which usually means a jump
thunk or an interior label, and that shape often marks a dispatch helper.

Then find the descriptor registration by breaking on writes to a known descriptor
address, for example the `net_load_and_use_map_variant` descriptor at
preferred `0x1807FF298`, during engine bring up.

`PatternScanner::FindRipRelativeReferences` and the `.pdata` backed `FunctionTable` in
`tools/probe/XrefProbe.cpp` are the pieces to build on. Both already work.

## Honest scope estimate

The debug globals surface is usable now and is enough to drive boolean engine knobs
such as `net_speculative_host_migration_disable`.

Starting a multiplayer match is a different order of work. It needs the command ABI
above, plus the session construction path, plus a UI entry point, none of which is
reachable by static analysis alone. Treat it as a reverse engineering project
measured in weeks, not as remaining implementation of the existing code.

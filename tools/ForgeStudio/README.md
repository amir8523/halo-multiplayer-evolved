# Forge Studio

External map editor for ForgeEvolved. Authors `.fmap.json`, the format documented
in [../../docs/02-MAP-FORMAT.md](../../docs/02-MAP-FORMAT.md).

## Why external rather than in game

The engine ships its own Forge (`forge_main_menu_tools`, `forge_main_menu_palettes`,
`forge_object_properties_*`), and reaching it is the right long term goal because
its output replicates natively through `net_load_and_use_map_variant`. An external
editor is still worth building, and shipping first, for reasons the in game tool
cannot cover:

- It works before the engine binding is confirmed. A community can build a map
  library while the runtime work continues.
- Text output is diffable and reviewable. A map arrives as a pull request.
- Precision editing, bulk operations, symmetry mirroring and scripted generation
  are all far easier outside a gamepad driven UI.
- It runs on a second monitor while the game is open.

The two are complementary, not alternatives. Forge Studio writes JSON; the game
compiles JSON to the engine's native structures.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│ Presentation            Avalonia views, 3D viewport          │
│                         No domain logic, no file access      │
└───────────────────────────┬─────────────────────────────────┘
                            │ observes, dispatches commands
┌───────────────────────────▼─────────────────────────────────┐
│ Application             MapDocument                          │
│                         command stack, selection, dirty flag │
│                         validation debounce                  │
└───────────────────────────┬─────────────────────────────────┘
                            │ mutates
┌───────────────────────────▼─────────────────────────────────┐
│ Domain                  MapVariant, ObjectPlacement, ...     │
│                         Plain data. No UI, no IO.            │
└───────────────────────────┬─────────────────────────────────┘
                            │ serialized by
┌───────────────────────────▼─────────────────────────────────┐
│ Infrastructure          JsonMapSerializer, PaletteCatalog,   │
│                         ValidationBridge, FileWatcher        │
└─────────────────────────────────────────────────────────────┘
```

Dependencies point inward only. The domain has no reference to Avalonia, which is
what lets the whole editing model be unit tested without a UI and reused by a
command line map linter.

### Every mutation is a command

`IEditCommand` with `Apply` and `Revert`. Nothing mutates the document directly.
This is not gold plating: a map editor without reliable undo is unusable, and
retrofitting undo onto direct mutation never works. Consequences that fall out for
free:

- Undo and redo.
- Multi object edits as one atomic step, through `CompositeCommand`.
- A dirty flag that is exact rather than a guess.
- Drag operations that coalesce into one undo entry via `IsCoalescableWith`.

### Validation is the game's validation

`ValidationBridge` calls the same `fe::map::Validate` the game runs, through a
small C export from the mod DLL. Reimplementing the rules in C# would guarantee
they drift, and the failure mode of drift is a map that passes in the editor and
is rejected at launch.

Validation runs debounced after edits, and diagnostics are surfaced inline against
the offending object rather than in a list the author has to correlate by hand.

### Palette catalog

`PaletteCatalog` loads the set of valid `palette_key` values for a scenario from
`data/palettes/<scenario>.json`, exported from the game. The editor therefore
cannot author a key the game cannot resolve, which turns the most common authoring
mistake into an impossible one.

### Live preview

Optional. When the game is running with ForgeEvolved loaded, Forge Studio writes
the document to a scratch file and calls `FE_SelectMap`, which parses it and
applies it through `MapVariantInjector`. Iteration becomes save then look, with no
relaunch.

## Layout

```
tools/ForgeStudio/
├── ForgeStudio.sln
├── src/
│   ├── ForgeStudio.Domain/          MapVariant and friends, plain records
│   ├── ForgeStudio.Application/     MapDocument, IEditCommand, selection
│   ├── ForgeStudio.Infrastructure/  JsonMapSerializer, PaletteCatalog, bridge
│   └── ForgeStudio.Desktop/         Avalonia views and the viewport
└── tests/
    └── ForgeStudio.Tests/           Domain and command stack tests
```

## Toolchain

.NET 8 and Avalonia 11. Cross platform, so a map author does not need Windows even
though the game does. `System.Text.Json` with a source generated context, so
serialization needs no reflection and startup stays fast.

## Contributing

The domain and application layers are the place to start. Both are pure C# with no
UI dependency and full test coverage, so a change there is verifiable without
launching anything.

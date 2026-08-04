# The `.fmap.json` map format

Authoring format for MultiplayerEvolved custom maps. Reference implementation:
[`src/Map/MapVariantParser.cpp`](../src/Map/MapVariantParser.cpp). Working example:
[`data/maps/example_canyon.fmap.json`](../data/maps/example_canyon.fmap.json).

## Two representations

| | `.fmap.json` | Canonical binary |
| --- | --- | --- |
| Purpose | Authoring, review, hand editing | Identity and network transfer |
| Produced by | Forge Studio, a text editor, a script | `WriteCanonicalBinary` |
| Stable bytes | No | Yes, byte for byte |
| Hashed | Never | SHA-256, this is the map's identity |

Map identity has to be exact, because a host and every client must agree on
whether they hold the same layout before a match can start. Hashing formatted JSON
would make a trailing newline or a reordered key produce a different hash for an
identical map, and the launch would be blocked with no way for the author to
diagnose it. So JSON is parsed into a model, the model is written to canonical
binary, and the hash is taken over that.

Determinism in the binary comes from three rules: a fixed field order, every
collection sorted by `id`, and floats written as IEEE-754 bit patterns with no
decimal conversion.

## Root

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `schema_version` | integer | yes | Must be 1. A newer value is rejected with a message telling the author to update. |
| `name` | string | yes | Up to 64 characters. |
| `description` | string | no | Up to 512 characters. |
| `author_name` | string | no | Up to 64 characters. |
| `author_platform_id` | integer | no | Steam ID of the author. |
| `base_scenario` | string | yes | Scenario the layout is built on. Dictates which base geometry loads. |
| `supported_modes` | array of string | yes | At least one. A mode absent here is never offered for this map. |
| `objects` | array | no | Up to 640. |
| `spawns` | array | no | Up to 128. At least 2 are required to validate. |
| `objectives` | array | no | Up to 64. |
| `boundaries` | array | no | Up to 32. |

`base_scenario` is never chosen independently of the map. Selecting a map sets the
scenario, so the two cannot disagree.

Mode names: `slayer`, `team_slayer`, `capture_the_flag`, `oddball`,
`king_of_the_hill`, `territories`, `juggernaut`, `infection`.

## Common types

**Position.** Either `[x, y, z]` or `{"x": , "y": , "z": }`. Engine world units.
Each component must be finite and within plus or minus 100000. A value outside that
is a unit conversion mistake in the authoring tool, so it is rejected rather than
clamped.

**Rotation.** Either `yaw_degrees` as a number, or `rotation` as `[x, y, z, w]`.
`yaw_degrees` is the common case for weapons and scenery and is converted to a
quaternion on load. A quaternion that is not unit length is normalized with a
warning; a zero length quaternion is an error.

**Team.** Integer 0 to 7, or `255` for neutral. Omitting the field means neutral.

**Shape.** Mirrors the engine's own shape fields.

```json
{ "type": "none" }
{ "type": "sphere",   "radius": 12.0 }
{ "type": "cylinder", "radius": 12.0, "top": 6.0, "bottom": 2.0 }
{ "type": "box",      "width": 20.0, "depth": 14.0, "top": 6.0, "bottom": 2.0 }
```

Only the dimensions a given shape uses are read, so an unused field cannot
silently affect behaviour.

## `objects[]`

| Field | Type | Required | Default | Notes |
| --- | --- | --- | --- | --- |
| `id` | integer | yes | | Unique across the whole document. |
| `palette_key` | string | yes | | Stable content key, see below. |
| `position` | position | yes | | |
| `rotation` or `yaw_degrees` | | no | identity | |
| `scale` | number | no | 1.0 | 0.01 to 100. |
| `physics` | string | no | `normal` | `normal`, `fixed`, `phased`. |
| `team` | integer | no | neutral | |
| `spawn_time_seconds` | integer | no | 0 | 0 means the engine default. |
| `spawn_at_start` | boolean | no | true | |
| `respawn_count` | integer | no | -1 | Negative means infinite. |
| `label` | string | no | | Gametype label, up to 48 characters. |
| `user_data` | integer | no | 0 | |
| `shape` | shape | no | none | |

### `palette_key`

Lowercase segments separated by dots, for example `weapon.rocket_launcher`,
`vehicle.warthog`, `scenery.crate_large`, `equipment.overshield`.

The grammar is restricted (lowercase letters, digits, underscore, dot, no empty
segments, no leading digit or dot) so a key can be used as a lookup token without
escaping anywhere in the pipeline.

Keys are resolved to the engine's palette index at load time rather than being
stored as indices. That is deliberate: a game patch that renumbers palettes would
silently turn every stored index into the wrong object, whereas an unresolvable key
is a clear error before anything is placed.

## `spawns[]`

| Field | Type | Required | Default |
| --- | --- | --- | --- |
| `id` | integer | yes | |
| `position` | position | yes | |
| `yaw_degrees` | number | no | 0 |
| `team` | integer | no | neutral |
| `initial_only` | boolean | no | false |
| `label` | string | no | |

`initial_only` marks a spawn used only at match start. A neutral spawn counts
toward every team's spawn availability.

## `objectives[]`

| Field | Type | Required |
| --- | --- | --- |
| `id` | integer | yes |
| `kind` | string | yes |
| `position` | position | yes |
| `rotation` or `yaw_degrees` | | no |
| `team` | integer | no |
| `shape` | shape | no |
| `label` | string | no |

Kinds: `flag_stand`, `ball_spawn`, `hill_marker`, `territory_marker`.

## `boundaries[]`

| Field | Type | Required |
| --- | --- | --- |
| `id` | integer | yes |
| `kind` | string | yes |
| `name` | string | no |
| `min` | position | yes |
| `max` | position | yes |

Kinds: `playable`, `soft_kill`, `hard_kill`. `min` must be strictly less than `max`
on every axis. More than one `playable` volume produces a warning, since the engine
uses their union and the intent is ambiguous.

## Validation

Two layers, both total. Nothing reaches the engine until the whole document has
validated.

**Structural.** Types, ranges, enum spellings, string lengths, finite numbers,
collection ceilings, unique ids. Each finding names the exact JSON path, for
example `objects[17].palette_key`, so one parse attempt reports everything wrong
rather than making the author fix errors one at a time.

**Semantic.** Whether the map is actually playable:

| Rule | Applies to |
| --- | --- |
| At least 2 spawn points | Every map |
| Every team has at least one spawn | Any team mode in `supported_modes` |
| A `flag_stand` for team 0 and team 1 | `capture_the_flag` |
| At least one neutral `ball_spawn` | `oddball` |
| At least one neutral `hill_marker` | `king_of_the_hill` |
| At least 2 territory markers | `territories` |

These exist because their absence produces a match that cannot be finished, and
that failure only appears after everyone has sat through a loading screen.

Warnings do not block a load. Errors do. Forge Studio's publish check promotes
warnings to errors via `treat_warnings_as_errors`; the game does not, so a map with
cosmetic warnings still plays.

## Transfer

A host serializes the selected map once and announces a `MapManifest` carrying the
name, the SHA-256 hex, the total byte count, the chunk count and the base scenario.
The manifest is self validating: the chunk count must be exactly what the byte count
implies, so a peer supplied count cannot drive a receiver's loop past the end of its
buffer.

Clients request chunks of 16 KB on the low priority `MapTransfer` lane, so a large
download can never delay a launch message. Each chunk carries a CRC-32 checked
before the bytes are copied into the assembly buffer, giving cheap early rejection
of corruption. When the payload is complete the SHA-256 is verified against the
manifest, and only then is it parsed. A payload that hashes correctly but does not
deserialize is still unusable, and finding that out at assembly time is far better
than at launch.

The hash check is also the security boundary: it makes a host unable to hand a
client a different map than the one it announced.

## Adding a field

1. Add it to `MapVariant` in [`src/Map/MapVariant.h`](../src/Map/MapVariant.h).
2. Read it in `MapVariantParser.cpp`, with a range check and a diagnostic.
3. Write it in `WriteJson`, omitting it when it holds the default so existing files
   do not churn in a diff.
4. Append it to `WriteCanonicalBinary` and `ReadCanonicalBinary`, **at the end** of
   its record.
5. Bump `kCanonicalVersion`. Leave `kSchemaVersion` alone if older JSON still loads,
   which it will if the field is optional.
6. Add the field to this document and to the example map.

Appending rather than inserting keeps the change readable in a diff and keeps older
canonical payloads parseable up to the point where the new field begins.

# Packaging and releasing on Nexus Mods

The install has to work for someone who has never installed a mod. That is not a
nicety: adoption is what makes a multiplayer mod playable at all, because a
multiplayer mod nobody can install has nobody to play against.

Two supported paths, both of which come from the same build.

## Why installation is already easy

The executable statically imports `VERSION.dll`, and Windows resolves that from the
executable's own directory before the system directory. So a `version.dll` next to
the game binary is loaded automatically at process start.

That removes every step that normally goes wrong:

| Usual approach | Problem | MultiplayerEvolved |
| --- | --- | --- |
| Injector executable | Antivirus flags one process opening another | Not used |
| Custom launcher | Breaks Steam overlay, playtime, invites | Not used |
| Administrator rights | UAC prompt, users refuse | Not needed |
| Script extender bootstrap | Another dependency to install | Not needed |
| Replacing a game file | Steam's file verification reverts it | Nothing is replaced |

Install is a file copy. Uninstall is deleting those files.

## Release artifact

One archive, laid out so its root maps onto the game's `Meteorite/Binaries/Win64`:

```
MultiplayerEvolved-0.1.0.zip
├── version.dll                     the loader proxy
├── MultiplayerEvolved.dll                the mod
└── MultiplayerEvolved/
    ├── symbols/
    │   └── <game build>.json       one per supported game build
    ├── maps/
    │   └── example_canyon.fmap.json
    └── README.txt                  three lines, where the log is
```

The archive root is the install directory, which means the manual instructions are
one sentence and Vortex needs no special handling.

Ship the `.pdb` as a separate optional file. A crash report without symbols is
noise, and bundling it in the main archive doubles the download for something only
a handful of people need.

## Path 1: Vortex, the default for most users

Nexus's mod manager. It downloads, extracts and deploys with one click.

For Vortex to place files correctly the archive must be rooted at the deployment
directory, which the layout above already satisfies. Vortex deploys mod files
relative to the game's mod staging path; for a game it does not know natively, the
user selects the target once.

To make it fully automatic, publish a Vortex game extension. It is a small
JavaScript file declaring:

- Steam App ID `2806050`, which is how Vortex finds the install without asking.
- The mod path, `Meteorite/Binaries/Win64`.
- The executable to launch, `Meteorite/Binaries/Win64/HaloCampaignEvolved.exe`.
- A test that an archive is a MultiplayerEvolved mod, namely the presence of
  `MultiplayerEvolved.dll` or a `MultiplayerEvolved/` directory at the root.

The extension is published separately from the mod, and Vortex offers it to anyone
who adds the game. This is the single highest leverage piece of packaging work:
after it exists, installing is "Mod Manager Download" and nothing else.

## Path 2: The installer, for users who do not want a mod manager

A single small executable that:

1. Finds the game with no input. Read `InstallPath` from
   `HKEY_LOCAL_MACHINE\SOFTWARE\Wow6432Node\Valve\Steam`, parse
   `steamapps/libraryfolders.vdf` for every library, then look for
   `appmanifest_2806050.acf` in each. This covers the common case of the game
   living on a different drive from Steam, which a hardcoded Program Files path
   does not.
2. Falls back to a folder picker only when detection fails, validating the choice
   by checking that `Meteorite/Binaries/Win64/HaloCampaignEvolved.exe` exists.
3. Refuses to install while the game is running, since the DLL would be locked.
   Detect by process name and say so plainly.
4. Warns if a `version.dll` is already present that is not ours. Another mod may
   own that slot, and silently overwriting it breaks that mod. Ours is identifiable
   by its version resource.
5. Copies the files, then writes a manifest of exactly what it wrote.
6. Offers uninstall, which deletes only what the manifest lists.

Two failure modes to handle explicitly, because both generate most support
requests:

- **SmartScreen.** An unsigned executable downloaded from the internet shows "Windows
  protected your PC". Either sign the installer, or ship the plain archive as the
  primary download and treat the installer as optional. An Authenticode
  certificate is the real fix and is worth the cost once the mod has an audience.
- **Missing Visual C++ runtime.** The mod links the MSVC runtime. If it is absent,
  `LoadLibrary` fails and the loader log says so. Either link statically with `/MT`
  to remove the dependency entirely, or have the installer check for and install
  the redistributable. Static linking is simpler and costs a few hundred kilobytes.

## Verifying an install without launching the game

After install, `Meteorite/Binaries/Win64/MultiplayerEvolved/loader.log` should exist after
the first launch and contain `loaded MultiplayerEvolved.dll`. If the file is missing, the
proxy is not being loaded, which means it is in the wrong directory. If the file
exists and reports a `LoadLibrary` failure, the runtime is missing.

Putting that in the mod description saves more support time than anything else in
it. Most reports resolve to one of those two lines.

## The Nexus mod page

**Category.** Multiplayer, with Utilities as a secondary. Not Gameplay: people
filtering for multiplayer mods should find this.

**Requirements.** List the Visual C++ redistributable only if not linking
statically. Do not list the Steamworks SDK; that is a build dependency, not a user
one.

**Description.** Lead with what it does and what it needs, in that order. A
description that opens with architecture loses the reader. Suggested structure:

1. One sentence on what it is: classic multiplayer, Slayer and CTF, with custom
   map support.
2. What is needed: the game on Steam, a Steam friend to play with. State plainly
   that no port forwarding is required, because that is the question everyone asks.
3. Install: two steps.
4. How to play: press Multiplayer, invite through the Steam overlay, pick a mode
   and a map, everyone readies up.
5. Troubleshooting: the two log lines above.
6. Known limitations, honestly. A mod page that admits what does not work yet gets
   better bug reports and fewer angry comments.

**Versioning.** Semantic versioning, and treat the wire protocol as part of the
public contract. `kProtocolVersion` changing is a minor bump at minimum, and the
changelog must say "everyone in a lobby must be on this version". Peers already
reject a mismatch with a clear message, so the failure is graceful, but people need
to know why.

**Game updates.** A game patch changes the build string, so the shipped symbol
descriptor no longer matches by name. The mod falls back to its built in defaults
and revalidates against the running binary, so it often keeps working. When it does
not, it goes inert with the reason in the log rather than crashing.

That behaviour needs to be in the description, because the day after a game patch
is when the comments arrive. Ask affected users for their log and their game
version, which is what `MPE_DumpDiagnostics()` and the startup lines provide, then
ship a new descriptor JSON. That is a data-only release and can be same day.

## Release checklist

```
[ ] Release build, warnings clean
[ ] Version bumped in CMakeLists.txt
[ ] Symbol descriptor present for the current game build
[ ] Tested: host and join between two machines, not two accounts on one
[ ] Tested: install on a machine that has never had the mod
[ ] Tested: uninstall leaves no files behind
[ ] Tested: game still launches with the mod present but Steam offline
[ ] loader.log and MultiplayerEvolved.log both produced and readable
[ ] Archive root maps onto Meteorite/Binaries/Win64
[ ] PDB uploaded as a separate optional file
[ ] Changelog states whether the protocol version changed
```

The Steam offline test matters. Steam being unavailable is a normal condition, and
the mod must degrade to an unmodified single player game rather than preventing
launch.

## Playing together

Discovery is Steam friends and invitations. The host presses Multiplayer, invites
through the overlay, and lobby visibility is friends only by default. Friends list
"Join Game" also works, through rich presence.

A public lobby browser is a natural next step and needs no new transport work:
`ISteamMatchmaking::RequestLobbyList` with filters on the `fe.protocol` and
`fe.build` keys already published. Worth deferring until there is a population to
browse, because an empty browser reads as a broken mod.

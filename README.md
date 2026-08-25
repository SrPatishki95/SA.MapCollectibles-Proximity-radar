# SA.MapCollectibles

A GTA: San Andreas ASI mod that marks the game's five collectible sets on the
pause menu map and the in-game radar, in the style of the "Show Discovered
Extras" option from Vice City Stories.

The pause map's **Options** panel gains a **Collectibles** row that cycles
through the sets — Gang Tags, Snapshots, Horseshoes, Oysters, Stunt Jumps, or
All. The ones you have already collected are marked at full brightness. With the
optional finder enabled, the ones you have not are marked too: faded on the map,
and solid on the radar as you get near them, so the remaining collectibles can
be tracked down without a wiki open on a second monitor.

Built with [plugin-sdk](https://github.com/DK22Pac/plugin-sdk). Targets
**GTA SA 1.0 US (HOODLUM)** — every patch is byte-guarded and the mod disables
itself on any other build.

## Credits

- **matt1010** — collectible icons.
- [aap](https://github.com/aap/debugmenu) — `debugmenu_public.h`, included
  verbatim under its MIT licence (see `source/debugmenu_LICENSE.txt`).

## Install

Copy into your game's `scripts` folder, keeping the layout:

```
scripts/SA.MapCollectibles.asi
scripts/SA.MapCollectibles.ini
scripts/MapCollectibles/collectibles.txd
```

Requires an ASI loader (Silent's / Ultimate ASI Loader). A
[modloader](https://github.com/thelink2012/sa-modloader) subfolder works too —
the mod resolves its files relative to the `.asi`.

## Configuration

`SA.MapCollectibles.ini` is re-read while the game runs, so edits apply within a
couple of seconds without a restart.

| Key | Default | Meaning |
| --- | --- | --- |
| `Finder` | `1` | Also mark the collectibles you have **not** picked up yet. |
| `FinderRadarRange` | `250` | How close a collectible must be to appear on the radar, in game units. |
| `MapFadeAlpha` | `120` | How faded the not-yet-collected markers are on the pause map. |
| `IconSize` | `10` | Icon size, in thousandths of the screen width. |

If [debugmenu](https://github.com/aap/debugmenu) is installed, the same settings
appear live under a **Map Collectibles** page, along with **Save settings to
INI**. Debug menu changes are not overwritten by the file watcher.

Custom icons: replace `MapCollectibles/collectibles.txd`, keeping the texture
names `graffiti`, `snapshot`, `horseshoe`, `oyster` and `usj`.

## How it works

Notes from reverse engineering SA 1.0 US, in case they save someone else the
disassembly.

### The pause map Options panel is not a menu page

`aScreens[MENUPAGE_MAP]` holds a single entry — BACK. The Options panel is a
**CMenuSystem panel**, the same widget the clothes and mod shops use, built
inside `CMenuManager::AdditionalOptionInput` (`0x5773D0`). It is created with
`CreateNewMenu(MENU_TYPE_DEFAULT, "FEP_OPT", ...)` and filled with the GXT keys
`FED_BL1`–`FED_BL5`, with a second column holding `FEM_ON`/`FEM_OFF` built on the
stack.

Adding a row is mostly free: `InsertMenu` recomputes `m_nNumRows` from column 0,
and both the up/down bounds in `InputStandardMenu` and the background window
height in `DisplayStandardMenu` derive from it, so the panel grows on its own.
The one hardcoded bound is the post-accept highlight advance at `0x578B5D`.

Two things do need care. The vanilla toggle is
`*(&this->m_bPrefsShowLocations + acceptedRow) ^= 1`, so a sixth row would
clobber `m_bPrefsShowLegends` — the accept has to be intercepted before the game
sees it. And because a cycling row deliberately keeps the highlight in place,
`InputStandardMenu` never clears `m_nAcceptedRow`, so the mod has to clear it or
the option cycles every frame.

Row labels are GXT keys resolved by `CText::Get`, so the mod hooks the four call
sites inside `DisplayStandardMenu` and answers for its own keys. No GXT edits.

### Where the collectibles live

| Set | Source |
| --- | --- |
| Gang tags | `CTagManager::ms_tagDesc` at `0xA9A8C0`, 100 entries. Sprayed when the alpha byte exceeds `0xE4`. |
| Stunt jumps | `CStuntJumpManager`'s pool — all jumps are always present, each with a `done` flag. |
| Snapshots, horseshoes, oysters | Not in any IPL and not `CREATE_PICKUP`. They are spawned by three dedicated script opcodes — `0958`, `0959`, `095A` — so the 50 + 50 + 50 coordinates were extracted from `main.scm` into `CollectibleCoords.h`. |

Collected pickups are removed from `CPickups::aPickUps`, so "collected" for those
three sets means a known coordinate with no live pickup left on it.

### Drawing on the map

`CRadar`'s blip transform is not usable here. `TransformRadarPointToScreenSpace`
has a map branch keyed on `0xBA67A1`, but the constants it reads
(`0xBA67AC`/`B0`/`B4`) are not recomputed for the current pan and zoom, which
collapses every marker into a small offset cluster.

Instead the mod takes the map rect from whatever the game just drew. `PrintMap`
has two branches: a 12x12 grid of radar tiles covering the world from -3000 to
3000, and — for the overview map — a single whole-map sprite. Hooking one call in
each branch gives the exact screen rect either way, at any zoom, and without
depending on how other mods rescale the menu.

### Drawing on the radar

The HUD radar does not go through `CRadar::DrawBlips` — that has exactly one
caller, the pause map. The minimap is `CRadar::DrawMap`, and it sets the radar
origin near its end, so the standard blip chain is valid immediately after it.

`CRadar::LimitRadarPoint` is deliberately not used: it clamps rather than culls,
which pins every distant collectible to the rim of the radar. Markers are culled
against the radar circle instead, with enough margin that an icon is either drawn
whole or not at all.

## Build

Needs [plugin-sdk](https://github.com/DK22Pac/plugin-sdk) with the
`PLUGIN_SDK_DIR` environment variable pointing at it, and its `Plugin.lib` built
for SA.

```
msbuild SA.MapCollectibles.vcxproj /p:Configuration="Release GTA-SA" /p:Platform=Win32
```

## Licence

MIT — see `LICENSE`.

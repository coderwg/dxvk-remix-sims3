# Installing The Sims 3 under RTX Remix

What you need:

- Windows 10 or 11 and an NVIDIA RTX graphics card
- The Sims 3 from the EA App, Legacy Update 1.69 (the tested version; see [README.md](README.md))
- RTX Remix runtime 1.5.2

Paths starting with `bridge\` are in this repository.

`<game>` below is your game folder: `C:\Program Files\EA Games\The Sims 3` for a default EA App
install. Everything the game runs from is in `<game>\Game\Bin`.

Before you change anything in `<game>\Game\Bin`, copy the files you are about to replace somewhere
safe.

## 1. RTX Remix

Download RTX Remix 1.5.2 from the
[rtx-remix releases](https://github.com/NVIDIAGameWorks/rtx-remix/releases). Copy everything in it --
the `d3d9.dll` and the `.trex` folder among it -- into `<game>\Game\Bin`, next to `TS3W.exe`.

## 2. The hook

Both halves of the bridge come from this repository: the 32-bit client `d3d9.dll`, which holds the
hook and the #1028 fix, and the 64-bit server `NvRemixBridge.exe`.

**From a release:** the release's zip holds both, under `Game\Bin` as they go into your game folder,
and the `bridge\sims3` folder this guide refers to.

**Or build them.** You need Visual Studio 2017 or later (or its Build Tools; 2022 Build Tools were
used) with the C++ tools for x86 and x64, Python 3.9 or later, and Meson and Ninja
(`pip install meson ninja`).

The repository is NVIDIA's whole dxvk-remix; the bridge needs only three of its folders. To fetch
just those:

```
git clone --filter=blob:none --sparse -b sims3-hook https://github.com/coderwg/dxvk-remix-sims3.git
cd dxvk-remix-sims3
git sparse-checkout set bridge
git sparse-checkout add public
git sparse-checkout add --skip-checks submodules/Detours
git submodule update --init submodules/Detours
```

Then build:

```
bridge\_build_x86_release.cmd    -> bridge\_output\d3d9.dll
bridge\_build_x64_release.cmd    -> bridge\_output\.trex\NvRemixBridge.exe
```

Then:

- copy `d3d9.dll` over `<game>\Game\Bin\d3d9.dll`;
- copy `NvRemixBridge.exe` over `<game>\Game\Bin\.trex\NvRemixBridge.exe`.

## 3. Configuration

From `bridge\sims3\config\`:

| File | Goes to | What it does |
|---|---|---|
| `rtx.conf` | `<game>\Game\Bin\rtx.conf` | Remix settings the hook needs: the terrain baker, the scene scale, the interface textures, the mods on |
| `bridge.conf` | `<game>\Game\Bin\.trex\bridge.conf` | lets the hook send lights to Remix (`exposeRemixApi`) |
| `sims3hook.txt` | `<game>\Game\Bin\sims3hook.txt` | the hook's options, each explained in the file; the defaults are the file's values |

Remix rewrites `rtx.conf` whenever you save settings from its menu (Alt+X). That is fine: it keeps
these lines.

## 4. Draw distance (recommended)

`bridge\sims3\config\GraphicsRules.diff` lists the changes to `<game>\Game\Bin\GraphicsRules.sgr`; make them
by hand. They push the far plane, the fog and the trees' and Sims' detail distances out to 8000 units,
allow 16 lots in full detail, and enlarge the game's render targets. `rtx.conf`'s terrain baker
covers exactly that 8000.

To match, set `maxactivelots = 16` in `Documents\Electronic Arts\The Sims 3\Options.ini`.

## 5. The lamps

The lamps need two things: the light table and the lamp reporter.

**The light table** holds each lamp model's lights, from your own install, packs and mods:

```
python bridge\sims3\tools\lite_table.py "<game>" "<game>\Game\Bin\sims3lights.txt"
```

It reads the game's packages and your `Mods` folder, in about ten seconds. Run it again after
installing a pack or a mod that changes lights.

**The lamp reporter** is a script mod that tells the hook which lamps are near the camera and whether
they are on. Copy `bridge\sims3\scriptmod\lamps\Sims3RtxLamps.package` into
`Documents\Electronic Arts\The Sims 3\Mods\Packages`. If you have never used mods, set up the `Mods`
folder and its `Resource.cfg` as for any Sims 3 mod.

Use the package from the same release or commit as the hook, because the two must match.

Without the table or the reporter, the lamps stay dark; the sun and the moon still work.

## 6. Glass and water

Copy `bridge\sims3\remix-mod\Sims3Glass` and `bridge\sims3\remix-mod\Sims3Water` into
`<game>\Game\Bin\rtx-remix\mods\`.

**Optional: water ripples and bumpy glass.** These are made from the game's own textures:

1. Play once with a pool, the town's water and a bumpy glass door (a shower door) in view. The hook
   writes their textures to `<game>\Game\Bin\rtx-remix\logs\sims3-textures`.
2. Run:
   ```
   python bridge\sims3\remix-mod\make_textures.py --waves "<game>\Game\Bin\rtx-remix\logs\sims3-textures" --bumps "<game>\Game\Bin\rtx-remix\logs\sims3-textures"
   ```
3. Copy `bridge\sims3\remix-mod\Sims3Water` (now with its textures) and the new `bridge\sims3\remix-mod\Sims3GlassBumps`
   into the `mods` folder.

## 7. In-game settings

Tested with:

- windowed, 1920 x 1080;
- Sim detail and tree detail Very High;
- draw distance High;
- lighting and reflections at their highest;
- edge smoothing off (it does nothing under Remix).

## 8. First run

Start the game as usual. The Remix menu opens with Alt+X.

The hook writes to `<game>\Game\Bin\rtx-remix\logs\bridge32.log`: its lines start with
`Sims 3 camera hook:`, with a block of statistics every ten seconds and at exit. Look there first if
something is wrong.

## Turning it off

Set the environment variable `SIMS3_CAMERA_HOOK=0` to run the bridge without the hook.

To remove everything, delete from `<game>\Game\Bin`:

- `d3d9.dll`, `.trex`, `rtx.conf`, `sims3hook.txt`, `sims3lights.txt` and `rtx-remix`;
- the files you copied in; put back your saved copies, including `GraphicsRules.sgr`.

Then remove `Sims3RtxLamps.package` from `Mods\Packages`.

## Running the tests

```
bridge\sims3\test\run_tests.cmd "<game>"
```

The tests use the shader dumps the hook writes while the game runs
(`<game>\Game\Bin\rtx-remix\logs\sims3-shaders`). Without the game folder, the checks that need them
are skipped.

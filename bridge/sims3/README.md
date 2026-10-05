# The Sims 3 under RTX Remix

A compatibility hook that lets [NVIDIA RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix)
ray-trace The Sims 3. It lives in this fork of the RTX Remix bridge, inside the bridge's 32-bit
client (`d3d9.dll`), the part that sits in the game's process.

Unofficial: not made by or affiliated with NVIDIA or Electronic Arts.

To install it, see [INSTALL.md](INSTALL.md).

## Why the game needs it

RTX Remix rebuilds a game's scene from what the game hands Direct3D 9, so it works best with games
that use the old fixed-function pipeline. The Sims 3 draws everything with its own shaders:

- it gives Direct3D no camera; the view and projection sit in shader constants;
- it binds a surface's colour texture on whichever stage its shader reads, not the one Remix takes;
- it draws many things Remix must not trace: shadow maps, the water's reflection pass, glow, its own
  tone curve, the Sims' soft shadows;
- it lights everything in its shaders, so there are no lights for Remix to use.

Unhooked, Remix shows The Sims 3 without ray tracing. It also crashed when a world loaded:
[rtx-remix issue #1028](https://github.com/NVIDIAGameWorks/rtx-remix/issues/1028), fixed by the first
commit on this branch (`GetDepthStencilSurface` returns `D3DERR_NOTFOUND` when nothing is bound).

## What the hook does

Wherever it can, the hook reads what it needs from the game itself -- the shader bytecode, or the
game's own records -- rather than from lists made by hand.

- **Camera.** Read from the vertex shaders' constants each frame and handed to Remix; the reflection
  passes are told apart from the main view.
- **What Remix sees.** Each draw is judged by its shaders: the game's fakes (shadow maps, the sky cube,
  the water's reflection pass, glow, the tone curve, soft shadows, decal overlays) are left out; the
  rest is captured.
- **Textures.** The colour texture is found in the pixel shader's bytecode and moved to the stage
  Remix reads, with its texture coordinate and sampler states. Cut-outs (leaves, fences, hair) become
  alpha tests Remix applies.
- **Normals.** Where the game packs its normals elsewhere, a variant of the game's own vertex shader
  writes them out for Remix.
- **Terrain.** Painted by the game's own terrain shaders through Remix's terrain baker, each terrain
  draw tagged with one of three marker textures that `rtx.conf` names.
- **Walls.** Door and window openings cut from the game's own wall masks.
- **Lots in the distance.** The low-detail models split into the house and its ground; the ground
  baked as terrain; lit windows glow at night.
- **Trees** (SpeedTree). Leaves cut at each plant's fade as the game does; leaf cards fixed in the
  world, facing out from their tree; trees close to the camera drawn solid.
- **Glass, mirrors and water.** Given real materials by two Remix mods in this folder: clear glass,
  tinted car glass, the plumbob, mirrors, and pool, pond, sea and object water. Glass moves with its
  object.
- **Sun, moon and fog.** The game's own light read from its memory each frame and sent as the sun or
  the moon, told apart by the game's clock, with dusk and dawn as cross-fades; the game's own fog
  colour and distances sent to Remix's fog.
- **Lamps.** A small script mod in the game reports each lamp near the camera (which one, where,
  on or off); the hook lights it with the game's own light definitions, read from your install by a
  tool in this folder. Street lamps follow the game's night switch.

## What is in this folder

| Folder | What |
|---|---|
| `config/` | `rtx.conf` (Remix), `bridge.conf` (the bridge), `sims3hook.txt` (the hook's options), `GraphicsRules.diff` (the game's draw-distance edit) |
| `remix-mod/` | the Remix mods `Sims3Glass` and `Sims3Water`; `make_textures.py`, which makes their ripple and bump maps from your game |
| `scriptmod/` | `lamps/`: the lamp reporter script mod (source and package); `build_scriptmod.py` builds it. `mapviewhomelot/` is a parked experiment, not needed |
| `tools/` | `lite_table.py`: the lamps' light definitions from your install |
| `test/` | the unit test (`run_tests.cmd "<game folder>"`) |
| `docs/` | the issue #1028 write-ups, the bridge PR text, and the first design document (historical) |

The hook's code is in `src/client/`: `sims3_camera_hook.h` and the `sims3_*` files next to it.

This repository holds no game data. The light table, the ripple maps and the bump maps are made on
your PC from your own copy of the game.

## Tested with

- The Sims 3 from the EA App, Legacy Update 1.69.47.024017 (`TS3.exe`)
- RTX Remix runtime 1.5.2 (`remix-1.5.2+68edea01`)
- Windows 11, GeForce RTX 5090, driver 616.56

The hook recognises shaders by their bytecode, which is the same on every install of this game
version. Other game versions are untested.

## Credits and license

Built on NVIDIA's [RTX Remix bridge](https://github.com/NVIDIAGameWorks/bridge-remix) (MIT, see
`LICENSE-MIT`). The hook and everything in this folder are by coderwg, under the same MIT license.
`src/client/xxhash.h` is xxHash by Yann Collet (BSD 2-Clause). The Sims 3 is a trademark of
Electronic Arts.

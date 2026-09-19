## Describe the bug

The Sims 3 (32-bit, DX9, Shader Model 3.0) crashes deterministically when
loading any 3D scene under the RTX Remix bridge. Main menu and Alt+X dev UI
render correctly. After clicking **Play** → world (or loading a saved game),
`TS3.exe` null-derefs inside its own code.

**Crash signature** (identical across many reproductions, including in
rasterization-passthrough mode with `rtx.enableRaytracing = False`):

- Exception: `ACCESS_VIOLATION reading address 0x00000000`
- EIP: `0x00610329` (`TS3.exe + 0x00210329`)
- `eax = 0`, `ebp = 0` — characteristic of a vtable / function-pointer call
  through a null `this`

**Bridge is healthy at moment of crash** — `remix-dxvk.log` contains zero
`err:` entries. The last commands `TS3.exe` issued, per `bridge64.log`'s
post-mortem ring buffer:

```
IDirect3D9Ex_CheckDeviceFormat × 5
IDirect3D9Ex_GetAdapterMonitor × 1
```

i.e., the game was performing device-capability queries at the precise moment
of the crash; TS3.exe then died asynchronously in its own CPU-side code, not
while inside a D3D9 call.

**The bridge presents synthesised device-identification values to the game.**
Comparing Sims 3's own `DeviceConfig.log` with-vs-without Remix:

| Field | Without Remix | With Remix |
|---|---|---|
| Driver DLL | `nvldumd.dll` | `nvd3dum.dll` |
| Driver version | `32.0.15.9571` | `32767.65535.65535.65535` (sentinels) |
| Chipset Board | `205710de` | `00000000` |
| Chipset | `00a1` | `0000` |

These synthesised values are not exposed by any `dxvk.conf` / `rtx.conf` /
`user.conf` option, including `d3d9.customDeviceId` / `customVendorId` /
`customDeviceDesc`. They appear to be generated at a bridge layer below DXVK.

**Environment**

- GPU: NVIDIA GeForce RTX 5090, Driver 596.21.0
- OS: Windows 11 (10.0 Build 26220)
- RTX Remix runtime: `remix-1.4.2+cfb6edc9`
- RTX Remix bridge: `remix-main+cfb6edc9`
- Game: The Sims 3 (EA App, "Legacy Update", `TS3.exe` build `1.0.0.18`)

---

## How do you reproduce the bug

1. Install The Sims 3 (EA App version) with EA's "Legacy Update" applied.
2. Confirm the game launches and loads worlds normally **without** Remix.
3. Drop the latest RTX Remix release (`d3d9.dll`, `.trex\`) into `Game\Bin\`.
4. Launch the game. Main menu renders; Alt+X dev UI confirms the bridge has
   hooked the process.
5. Click **Play** → choose any world, **or** load any saved game.
6. Crash within ~90 seconds with the signature above.

Diagnostic files attached:

- `Game\Bin\rtx-remix\logs\bridge32.log`, `bridge64.log`, `remix-dxvk.log`
- `Documents\Electronic Arts\The Sims 3\xcpt *.txt` (text) and `*.mdmp`
  (binary minidump)
- `Documents\Electronic Arts\The Sims 3\DeviceConfig.log` (with-Remix and
  without-Remix versions show the synthetic-IDs comparison directly)

---

## What is the expected behavior

The host process should not silently null-deref before any rendering takes
place. If the game's loader is reading one or more of the synthesised
device-identification fields and failing on them, exposing those fields as
user-overridable — or passing them through unmodified — would give users a
route to a diagnosable failure mode rather than a silent crash.

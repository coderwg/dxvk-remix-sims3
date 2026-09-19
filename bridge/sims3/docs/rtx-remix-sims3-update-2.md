**Root cause found and fixed — a one-line bridge-client bug. Patch attached; The Sims 3
now loads and runs.**

Traced against a complete native apitrace (14.2 M calls), the runtime and bridge
source, the crash minidump's captured code page, and Remix **1.5.2** with `logAllCalls`.
Then fixed, rebuilt, and confirmed in-game.

### The bug — `GetDepthStencilSurface` returns `D3D_OK` with a NULL surface

`bridge-remix/src/client/d3d9_device.cpp`, `Direct3DDevice9Ex_LSS::GetDepthStencilSurface`:

```cpp
Direct3DSurface9_LSS* pLssDepthStencil = bridge_cast<...>(*m_state.depthStencil);
*ppZStencilSurface = pLssDepthStencil;                 // NULL when nothing is bound
if (pLssDepthStencil) { pLssDepthStencil->AddRef(); /* send message */ }
WAIT_FOR_OPTIONAL_SERVER_RESPONSE("GetDepthStencilSurface()", D3DERR_INVALIDCALL, currentUID);
```

`WAIT_FOR_OPTIONAL_SERVER_RESPONSE` (`src/util/util_bridgecommand.h`) returns **`D3D_OK`**
by default. D3D9 specifies **`D3DERR_NOTFOUND`** when no depth-stencil is bound, and the
DXVK runtime one layer down does exactly that (`d3d9_device.cpp`:
`if (m_state.depthStencil == nullptr) return D3DERR_NOTFOUND;`) — but the client
short-circuits before the server is consulted, and `D3DERR_NOTFOUND` appears nowhere in
the client.

**Why it kills Sims 3.** The game guards this call on the HRESULT, as D3D9 permits. The
minidump's captured code page (runtime bytes; the exe is DRM-encrypted on disk) shows
the crashing function is its `Clear` wrapper:

```
mov  eax,[ecx+0xA0]   ; IDirect3DDevice9 slot 40 = GetDepthStencilSurface
call eax
test eax,eax
jl   +0x3D            ; skip depth inspection if FAILED(hr)   <- correct per spec
mov  eax,[esp+0xC]    ; pDepthSurface
mov  ecx,[eax]        ; <- FAULT at TS3.exe+0x210329: NULL, but hr said D3D_OK
mov  eax,[ecx+0x30]   ; IDirect3DSurface9 slot 12 = GetDesc
call eax
```

Native d3d9 returns `NOTFOUND` → the `jl` skips → it clears `TARGET` only (the right thing
with no depth bound) → no crash. The bridge returns `OK` → the guard passes →
`GetDesc(NULL)` → `ACCESS_VIOLATION reading 0x00000000`, `eax = 0`. In the crash run this
call is the **only one of 3,112** that returns NULL — it is the last call before the crash.

### The fix

```cpp
    if (pLssDepthStencil) {
      ...
    } else {
      return D3DERR_NOTFOUND;   // out-pointer is already NULL; match D3D9 and the runtime
    }
```

Built client + server from `main` (`7dbbd371`) with this change and deployed both (they
must share `BRIDGE_VERSION`). **Result: the save loads into the world and runs.** In the
log, `GetDepthStencilSurface` returned NULL **137 times** and the game continued through
every one — 4,234 render-to-texture passes (it used to die at ~2,600), 5,462 frames, zero
errors, no crash dump. A second session with Advanced Rendering on and high quality
settings (the heavier render path) hit the same NULL return **966 times** over ~9 minutes
and 8,700 frames, again with zero errors and no crash. The trace at the first NULL return is exactly the predicted path:
`SetDepthStencilSurface → GetDepthStencilSurface (NOTFOUND) → Clear → BeginScene`.

Patch (`git format-patch`) attached. Happy to open it as a PR against bridge-remix.

### Additional observation — debugoptimized server asserts on a transient `StretchRect` failure

While running the **debugoptimized** flavour (the one with `logAllCalls` support), toggling
DLSS off in the Remix menu mid-session killed the bridge server:
`../src/server/main.cpp:962`, `assert(SUCCEEDED(hresult))` in
`case IDirect3DDevice9Ex_StretchRect` (minidump `NvRemixBridge.exe_*.dmp` in `.trex`, exception
`0x80000003`). The runtime rejected a `StretchRect` issued while the upscaler surfaces were
being reconfigured (a swap-chain resize accompanies the DLSS change) — a transient, legitimate
`D3DERR_INVALIDCALL`. The server has 97 such `assert(SUCCEEDED(...))` guards, one per
forwarded call, so in that flavour any transiently failing call is fatal. The release
flavour compiles them out and behaves correctly (call fails quietly, one frame's copy is
skipped). Not a client-patch issue; noted because the debugoptimized package is what
users reach for when asked to gather logs.

### Secondary — the synthetic adapter identifier (why the game binds a NULL depth at all)

`dxvk-remix/src/d3d9/d3d9_adapter.cpp` hardcodes `Revision = 0`, `SubSysId = 0`,
`DriverVersion.QuadPart = INT64_MAX` (→ `32767.65535.65535.65535`; inherited from
upstream dxvk; not overridable — only `customVendorId/DeviceId/Desc` are). This is the
*only* adapter data Sims 3 reads before building its render-target pool (it never calls
`GetDeviceCaps`), and it flips the game from **0** `CheckDeviceFormat` probes natively to
**6** under Remix (2 refused). The pool builds identically, but a later 512×512
Sim-portrait pass then fails its depth-surface lookup and binds NULL — which Defect 1 turned
fatal. With Defect 1 fixed the game tolerates this correctly; a pass-through option for
those three fields would still be worthwhile for games that key on them.

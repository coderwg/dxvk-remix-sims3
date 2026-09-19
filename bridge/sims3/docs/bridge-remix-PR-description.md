**Title:** client: return D3DERR_NOTFOUND from GetDepthStencilSurface when no depth-stencil is bound

Fixes NVIDIAGameWorks/rtx-remix#1028 (The Sims 3 deterministic null-deref on world load).

### Problem

`Direct3DDevice9Ex_LSS::GetDepthStencilSurface` writes a NULL out-pointer when no
depth-stencil surface is bound, then falls through to `WAIT_FOR_OPTIONAL_SERVER_RESPONSE`,
which returns `D3D_OK` unless `sendAllServerResponses` is set. D3D9 specifies
`D3DERR_NOTFOUND` in this case, and the DXVK runtime returns exactly that
(`d3d9_device.cpp`: `if (m_state.depthStencil == nullptr) return D3DERR_NOTFOUND;`) — but
the client short-circuits before the server is consulted, and `D3DERR_NOTFOUND` does not
appear anywhere in the client.

A game that guards this call on `FAILED(hr)` — which D3D9 permits — is told the NULL
pointer is valid and dereferences it. The Sims 3 does this in its `Clear` wrapper and
crashes with `ACCESS_VIOLATION reading 0x00000000` (`IDirect3DSurface9::GetDesc` on a null
`this`, `TS3.exe+0x210329`) on every world / save load.

### Fix

Return `D3DERR_NOTFOUND` on the unbound path. The out-pointer is already NULL, so callers
see the same result native d3d9 gives them.

### Verification

- Sims 3 (EA App "Legacy Update", 32-bit) with Remix 1.5.2 runtime: crashed on every
  load across 9 reproductions, bitwise-identical EIP, invariant under all user config.
- Config-only proof first: with `sendAllServerResponses = True` (which makes the same path
  return a failing HRESULT via the timeout fallback) the game no longer crashed — it sat
  alive in the wait, confirming the null path and that `GetDesc(NULL)` was the fault.
- With this patch (client + server rebuilt from `main` so `BRIDGE_VERSION` matches), the
  game loads into the world and runs. In the bridge log, `GetDepthStencilSurface` returned
  NULL 137 times and the game continued through every one; 4,234 render-to-texture passes
  vs. a previous crash point of ~2,600; 5,462 frames; zero errors; no crash dump.

`GetRenderTarget` has the same defect via a different route: when
`m_state.renderTargets[RenderTargetIndex]` is NULL it skips the `if` block and hits an
explicit `return S_OK;` with a NULL out-pointer, where the runtime returns
`D3DERR_NOTFOUND`. Index 0 is never unbound in practice so no game has been seen to hit
it, but it's the same one-line fix if you'd like it in this PR.

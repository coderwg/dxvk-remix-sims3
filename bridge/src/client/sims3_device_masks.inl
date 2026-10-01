// The Sims 3 camera hook, part of d3d9_device.cpp: the game's state put back for the draws that are
// not captured, and the masked writes the runtime would drop (emulated with blending, or a copy of
// the target and a restore quad). Included after sims3_device_terrain.inl; not a standalone header.

// Stage 0's colour stage as the hook sets it for captured draws: TEXTURE x TFACTOR (the tint).
inline constexpr D3DTEXTURESTAGESTATETYPE kSims3Tss[3] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2 };
inline constexpr DWORD kSims3TssOurs[3] = { D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_TFACTOR };
// The render states the masked-write emulation saves and restores.
inline constexpr D3DRENDERSTATETYPE kSims3FogRs[5] = { D3DRS_FOGENABLE, D3DRS_FOGTABLEMODE, D3DRS_FOGCOLOR, D3DRS_FOGSTART, D3DRS_FOGEND };   // the game's fog for the runtime (milestone 56)
inline constexpr D3DRENDERSTATETYPE kSims3MaskRs[10] = { D3DRS_COLORWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_BLENDFACTOR, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA };

// Before a draw that is not captured: whatever the hook set on the device for captured draws
// goes back to the game's own value, so the game's other draws (the compositor) see the game's state.
template<typename Dev>
void sims3RestoreGameState(Sims3Hook& h, Dev* dev) {
  if (!h.uvIndexHidden && !h.tssOurs && !h.factorOurs) return;
  h.ourState = true;
  if (h.uvIndexHidden) { h.uvIndexHidden = false; dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, h.gameTss0[3]); }
  for (int i = 0; i < 3; ++i) if (h.tssOurs & (1u << i)) dev->SetTextureStageState(0, kSims3Tss[i], h.gameTss0[i]);
  h.tssOurs = 0;
  if (h.factorOurs) { h.factorOurs = false; h.sentFactor = h.gameFactor; dev->SetRenderState(D3DRS_TEXTUREFACTOR, h.gameFactor); }
  h.ourState = false;
  ++h.restoreCount;
}

// The scratch copy of render target 0 for a blended partial-channel write: one per target size and format.
template<typename Dev>
int sims3ScratchFor(Sims3Hook& h, Dev* dev) {
  if (h.rt0W == 0 || h.rt0H == 0 || h.rt0Fmt == 0) return -1;
  for (uint32_t i = 0; i < h.scratchCount; ++i)
    if (h.scratch[i].w == h.rt0W && h.scratch[i].h == h.rt0H && h.scratch[i].fmt == h.rt0Fmt) return (int) i;
  if (h.scratchCount >= 4) { ++h.copyFailed; return -1; }
  IDirect3DTexture9* tex = nullptr;
  if (FAILED(dev->CreateTexture(h.rt0W, h.rt0H, 1, D3DUSAGE_RENDERTARGET, (D3DFORMAT) h.rt0Fmt, D3DPOOL_DEFAULT, &tex, nullptr)) || tex == nullptr) { ++h.copyFailed; return -1; }
  IDirect3DSurface9* surf = nullptr;
  if (FAILED(tex->GetSurfaceLevel(0, &surf)) || surf == nullptr) { tex->Release(); ++h.copyFailed; return -1; }
  Sims3Hook::Scratch& s = h.scratch[h.scratchCount];
  s.w = h.rt0W; s.h = h.rt0H; s.fmt = h.rt0Fmt; s.tex = tex; s.surf = surf;
  return (int) h.scratchCount++;
}

// Before such a draw: the target copied to the scratch. The draw itself then runs with a full mask.
template<typename Dev>
bool sims3CopyTarget(Sims3Hook& h, Dev* dev, DWORD mask) {
  const int si = sims3ScratchFor(h, dev);
  if (si < 0) return false;
  IDirect3DSurface9* rt = nullptr;
  if (FAILED(dev->GetRenderTarget(0, &rt)) || rt == nullptr) { ++h.copyFailed; return false; }
  const RECT r = { 0, 0, (LONG) h.rt0W, (LONG) h.rt0H };
  const HRESULT hr = dev->StretchRect(rt, &r, h.scratch[si].surf, &r, D3DTEXF_NONE);
  rt->Release();
  if (FAILED(hr)) { ++h.copyFailed; return false; }
  h.copyMask = (uint8_t) mask; h.copyScratch = si;
  return true;
}

// After it: the channels the game did not write go back from the copy. A pre-transformed quad over
// the whole target, fixed function, blended with a constant factor of 1 on those channels and 0 on
// the written ones. Everything the quad needs is set and put back through the device's own setters,
// with the draw hooks told to ignore the quad and the state tracking told the values are ours.
template<typename Dev>
void sims3RestoreChannels(Sims3Hook& h, Dev* dev) {
  const int si = h.copyScratch; h.copyScratch = -1;
  if (si < 0 || h.scratch[si].tex == nullptr) return;
  static const D3DRENDERSTATETYPE kRs[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_LIGHTING, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPING, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_BLENDFACTOR };
  static const D3DTEXTURESTAGESTATETYPE kTss[] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS };
  static const D3DSAMPLERSTATETYPE kSs[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
  constexpr int nRs = sizeof kRs / sizeof kRs[0], nTss = sizeof kTss / sizeof kTss[0], nSs = sizeof kSs / sizeof kSs[0];
  DWORD rs[nRs], tss[nTss], tss1[2], ss[nSs], fvf = 0;
  for (int i = 0; i < nRs; ++i) dev->GetRenderState(kRs[i], &rs[i]);
  for (int i = 0; i < nTss; ++i) dev->GetTextureStageState(0, kTss[i], &tss[i]);
  dev->GetTextureStageState(1, D3DTSS_COLOROP, &tss1[0]); dev->GetTextureStageState(1, D3DTSS_ALPHAOP, &tss1[1]);
  for (int i = 0; i < nSs; ++i) dev->GetSamplerState(0, kSs[i], &ss[i]);
  IDirect3DBaseTexture9* tex0 = nullptr; dev->GetTexture(0, &tex0);
  IDirect3DVertexShader9* vs = nullptr; dev->GetVertexShader(&vs);
  IDirect3DPixelShader9* ps = nullptr; dev->GetPixelShader(&ps);
  IDirect3DVertexDeclaration9* decl = nullptr; dev->GetVertexDeclaration(&decl);
  dev->GetFVF(&fvf);
  IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; dev->GetStreamSource(0, &vb, &vbOffset, &vbStride);

  h.ourDraw = true; h.ourState = true; h.inRemap = true; h.swappingVs = true;
  const DWORD m = h.copyMask;
  const DWORD factor = ((m & 8) ? 0u : 0xFF000000u) | ((m & 1) ? 0u : 0x00FF0000u) | ((m & 2) ? 0u : 0x0000FF00u) | ((m & 4) ? 0u : 0x000000FFu);
  dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE); dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE); dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); dev->SetRenderState(D3DRS_FOGENABLE, FALSE); dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE); dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0); dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE); dev->SetRenderState(D3DRS_CLIPPING, TRUE);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
  dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_BLENDFACTOR); dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVBLENDFACTOR); dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  dev->SetRenderState(D3DRS_BLENDFACTOR, factor);
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0); dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
  dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
  dev->SetTexture(0, h.scratch[si].tex);
  dev->SetVertexShader(nullptr); dev->SetPixelShader(nullptr);
  dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
  const float w = (float) h.rt0W, ht = (float) h.rt0H;
  struct V { float x, y, z, rhw, u, v; };
  const V q[4] = { { -0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f }, { w - 0.5f, -0.5f, 0.f, 1.f, 1.f, 0.f }, { -0.5f, ht - 0.5f, 0.f, 1.f, 0.f, 1.f }, { w - 0.5f, ht - 0.5f, 0.f, 1.f, 1.f, 1.f } };
  dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
  // the game's state back
  if (decl) dev->SetVertexDeclaration(decl); else dev->SetFVF(fvf);
  dev->SetVertexShader(vs); dev->SetPixelShader(ps);
  dev->SetTexture(0, tex0);
  if (vb) dev->SetStreamSource(0, vb, vbOffset, vbStride);
  for (int i = 0; i < nSs; ++i) dev->SetSamplerState(0, kSs[i], ss[i]);
  for (int i = 0; i < nTss; ++i) dev->SetTextureStageState(0, kTss[i], tss[i]);
  dev->SetTextureStageState(1, D3DTSS_COLOROP, tss1[0]); dev->SetTextureStageState(1, D3DTSS_ALPHAOP, tss1[1]);
  for (int i = 0; i < nRs; ++i) dev->SetRenderState(kRs[i], rs[i]);
  h.swappingVs = false; h.inRemap = false; h.ourState = false; h.ourDraw = false;
  if (decl) decl->Release();
  if (vs) vs->Release();
  if (ps) ps->Release();
  if (tex0) tex0->Release();
  if (vb) vb->Release();
}

// A draw that is not captured into an offscreen target with a partial colour write mask: the
// runtime drops any draw whose mask lacks R, G or B (the compositor's channel packing), so the
// mask is emulated with blending and the mask itself set to full; sims3EndDraw puts the states back.
template<typename Dev>
void sims3BeginMaskedWrite(Sims3Hook& h, Dev* dev, const DWORD* rs) {
  const DWORD cw = rs[D3DRS_COLORWRITEENABLE] & 0xF, bl = rs[D3DRS_ALPHABLENDENABLE] ? 1u : 0u;
  if (h.rtIsPrimary || (cw & 7) == 7) return;
  for (int i = 0; i < 10; ++i) h.maskSaved[i] = rs[kSims3MaskRs[i]];
  h.ourState = true;
  if (!bl) {
    // no blending: dest = src on the written channels, kept elsewhere = a constant blend factor of 1 / 0 per channel
    const DWORD f = ((cw & 8) ? 0xFF000000u : 0u) | ((cw & 1) ? 0x00FF0000u : 0u) | ((cw & 2) ? 0x0000FF00u : 0u) | ((cw & 4) ? 0x000000FFu : 0u);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE); dev->SetRenderState(D3DRS_BLENDFACTOR, f); dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_BLENDFACTOR); dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVBLENDFACTOR); dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD); dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xFu);
    h.maskEmu = 1; h.maskEmuBits = 0x7F; ++h.maskEmuA;
  } else if (cw == 8) {
    // alpha written with the game's blend, colour kept: the blend moves to the alpha channel, colour blends ZERO / ONE
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE); dev->SetRenderState(D3DRS_SRCBLENDALPHA, h.maskSaved[2]); dev->SetRenderState(D3DRS_DESTBLENDALPHA, h.maskSaved[3]); dev->SetRenderState(D3DRS_BLENDOPALPHA, h.maskSaved[4]); dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO); dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE); dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD); dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xFu);
    h.maskEmu = 2; h.maskEmuBits = 0x3DD; ++h.maskEmuB;
  } else if (sims3CopyTarget(h, dev, cw)) {
    // blending on with a partial colour mask: the target copied, the draw with a full mask, the rest put back after it
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xFu);
    h.maskEmu = 3; h.maskEmuBits = 0x1; ++h.maskEmuC;
  } else {
    ++h.maskEmuSkipped;
  }
  h.ourState = false;
  if (h.maskEmu && ((h.maskEmu != 3 && h.maskEmuLogged < 4) || (h.maskEmu == 3 && h.maskEmuCopyLogged < 8))) {
    if (h.maskEmu == 3) ++h.maskEmuCopyLogged; else ++h.maskEmuLogged;
    char msg[320];
    snprintf(msg, sizeof msg, "Sims 3 camera hook: masked write emulated at frame %u -> mask %lx into %ux%u, %s (blend was %lu: %lu/%lu op %lu), VS %016llx PS %016llx", h.frames + 1, (unsigned long) cw, (unsigned) h.rt0W, (unsigned) h.rt0H, h.maskEmu == 1 ? "constant blend factor" : h.maskEmu == 2 ? "separate alpha blend" : "copy, full mask, unwritten channels restored after the draw", (unsigned long) bl, (unsigned long) h.maskSaved[2], (unsigned long) h.maskSaved[3], (unsigned long) h.maskSaved[4], (unsigned long long) h.vsHash, (unsigned long long) h.psHash);
    Logger::info(msg);
  }
}

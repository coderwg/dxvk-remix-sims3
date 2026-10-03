// The Sims 3 camera hook, part of d3d9_device.cpp: the game's state put back for the draws that are
// not captured, and the masked writes the runtime would drop (emulated with blending, or a copy of
// the target and a restore quad). Included after sims3_device_terrain.inl; not a standalone header.

// Stage 0's colour stage as the hook sets it for captured draws: TEXTURE x TFACTOR (the tint).
inline constexpr D3DTEXTURESTAGESTATETYPE kSims3Tss[3] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2 };
inline constexpr DWORD kSims3TssOurs[3] = { D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_TFACTOR };
inline constexpr D3DRENDERSTATETYPE kSims3FogRs[5] = { D3DRS_FOGENABLE, D3DRS_FOGTABLEMODE, D3DRS_FOGCOLOR, D3DRS_FOGSTART, D3DRS_FOGEND };   // the game's fog for the runtime (milestone 56)

// Before a draw that is not captured: whatever the hook set on the device for captured draws
// goes back to the game's own value, so the game's other draws (the compositor) see the game's state.
template<typename Dev>
void sims3RestoreGameState(Sims3Hook& h, Dev* dev) {
  if (!h.uvIndexHidden && !h.tssOurs && !h.factorOurs) return;
  sims3cam::OwnCall ownCall(h.calls);
  if (h.uvIndexHidden) { h.uvIndexHidden = false; dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, h.gameTss0[3]); }
  for (int i = 0; i < 3; ++i) if (h.tssOurs & (1u << i)) dev->SetTextureStageState(0, kSims3Tss[i], h.gameTss0[i]);
  h.tssOurs = 0;
  if (h.factorOurs) { h.factorOurs = false; h.sentFactor = h.gameFactor; dev->SetRenderState(D3DRS_TEXTUREFACTOR, h.gameFactor); }
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
// the written ones. Its states held in a scope of their own, put back right after it (the draw's own,
// the full colour mask among them, still set); its vertex layout and stream put back by hand. The
// hook's own call: the draw hooks ignore the quad.
template<typename Dev>
void sims3RestoreChannels(Sims3Hook& h, Dev* dev) {
  const int si = h.copyScratch; h.copyScratch = -1;
  if (si < 0 || h.scratch[si].tex == nullptr) return;
  sims3cam::OwnCall ownCall(h.calls);
  IDirect3DVertexDeclaration9* decl = nullptr; dev->GetVertexDeclaration(&decl);
  DWORD fvf = 0; dev->GetFVF(&fvf);
  IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; dev->GetStreamSource(0, &vb, &vbOffset, &vbStride);
  sims3cam::HookCalls& c = h.calls;
  const sims3cam::HookCalls::Scope scope = c.open();
  const DWORD m = h.copyMask;
  const DWORD factor = ((m & 8) ? 0u : 0xFF000000u) | ((m & 1) ? 0u : 0x00FF0000u) | ((m & 2) ? 0u : 0x0000FF00u) | ((m & 4) ? 0u : 0x000000FFu);
  c.holdRs(dev, D3DRS_ZENABLE, D3DZB_FALSE); c.holdRs(dev, D3DRS_ZWRITEENABLE, FALSE); c.holdRs(dev, D3DRS_ALPHATESTENABLE, FALSE);
  c.holdRs(dev, D3DRS_CULLMODE, D3DCULL_NONE); c.holdRs(dev, D3DRS_FOGENABLE, FALSE); c.holdRs(dev, D3DRS_STENCILENABLE, FALSE);
  c.holdRs(dev, D3DRS_SCISSORTESTENABLE, FALSE); c.holdRs(dev, D3DRS_CLIPPLANEENABLE, 0); c.holdRs(dev, D3DRS_LIGHTING, FALSE);
  c.holdRs(dev, D3DRS_SRGBWRITEENABLE, FALSE); c.holdRs(dev, D3DRS_CLIPPING, TRUE);
  c.holdRs(dev, D3DRS_ALPHABLENDENABLE, TRUE); c.holdRs(dev, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
  c.holdRs(dev, D3DRS_SRCBLEND, D3DBLEND_BLENDFACTOR); c.holdRs(dev, D3DRS_DESTBLEND, D3DBLEND_INVBLENDFACTOR); c.holdRs(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD);
  c.holdRs(dev, D3DRS_BLENDFACTOR, factor);
  c.holdStage(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); c.holdStage(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  c.holdStage(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); c.holdStage(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  c.holdStage(dev, 0, D3DTSS_TEXCOORDINDEX, 0); c.holdStage(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
  c.holdStage(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE); c.holdStage(dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  c.holdSampler(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT); c.holdSampler(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); c.holdSampler(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  c.holdSampler(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); c.holdSampler(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); c.holdSampler(dev, 0, D3DSAMP_SRGBTEXTURE, FALSE);
  c.holdTexture(dev, 0, h.scratch[si].tex);
  c.holdVs(dev, nullptr); c.holdPs(dev, nullptr);
  dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
  const float w = (float) h.rt0W, ht = (float) h.rt0H;
  struct V { float x, y, z, rhw, u, v; };
  const V q[4] = { { -0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f }, { w - 0.5f, -0.5f, 0.f, 1.f, 1.f, 0.f }, { -0.5f, ht - 0.5f, 0.f, 1.f, 0.f, 1.f }, { w - 0.5f, ht - 0.5f, 0.f, 1.f, 1.f, 1.f } };
  dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
  // the game's state back
  c.close(dev, scope);
  if (decl) dev->SetVertexDeclaration(decl); else dev->SetFVF(fvf);
  if (vb) dev->SetStreamSource(0, vb, vbOffset, vbStride);
  if (decl) decl->Release();
  if (vb) vb->Release();
}

// A draw that is not captured into an offscreen target with a partial colour write mask: the
// runtime drops any draw whose mask lacks R, G or B (the compositor's channel packing), so the
// mask is emulated with blending and the mask itself set to full, held in the draw's scope (put back
// in sims3EndDraw).
template<typename Dev>
void sims3BeginMaskedWrite(Sims3Hook& h, Dev* dev, const DWORD* rs) {
  const DWORD cw = rs[D3DRS_COLORWRITEENABLE] & 0xF, bl = rs[D3DRS_ALPHABLENDENABLE] ? 1u : 0u;
  if (h.rtIsPrimary || (cw & 7) == 7) return;
  const DWORD src = rs[D3DRS_SRCBLEND], dst = rs[D3DRS_DESTBLEND], op = rs[D3DRS_BLENDOP];   // the game's blend (rs is the device's own, changed below)
  sims3cam::HookCalls& c = h.calls;
  int kind = 0;
  if (!bl) {
    // no blending: dest = src on the written channels, kept elsewhere = a constant blend factor of 1 / 0 per channel
    const DWORD f = ((cw & 8) ? 0xFF000000u : 0u) | ((cw & 1) ? 0x00FF0000u : 0u) | ((cw & 2) ? 0x0000FF00u : 0u) | ((cw & 4) ? 0x000000FFu : 0u);
    c.holdRs(dev, D3DRS_SEPARATEALPHABLENDENABLE, FALSE); c.holdRs(dev, D3DRS_BLENDFACTOR, f); c.holdRs(dev, D3DRS_SRCBLEND, D3DBLEND_BLENDFACTOR); c.holdRs(dev, D3DRS_DESTBLEND, D3DBLEND_INVBLENDFACTOR);
    c.holdRs(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD); c.holdRs(dev, D3DRS_ALPHABLENDENABLE, TRUE); c.holdRs(dev, D3DRS_COLORWRITEENABLE, 0xFu);
    kind = 1; ++h.maskEmuA;
  } else if (cw == 8) {
    // alpha written with the game's blend, colour kept: the blend moves to the alpha channel, colour blends ZERO / ONE
    c.holdRs(dev, D3DRS_SEPARATEALPHABLENDENABLE, TRUE); c.holdRs(dev, D3DRS_SRCBLENDALPHA, src); c.holdRs(dev, D3DRS_DESTBLENDALPHA, dst); c.holdRs(dev, D3DRS_BLENDOPALPHA, op);
    c.holdRs(dev, D3DRS_SRCBLEND, D3DBLEND_ZERO); c.holdRs(dev, D3DRS_DESTBLEND, D3DBLEND_ONE); c.holdRs(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD); c.holdRs(dev, D3DRS_COLORWRITEENABLE, 0xFu);
    kind = 2; ++h.maskEmuB;
  } else if (sims3CopyTarget(h, dev, cw)) {
    // blending on with a partial colour mask: the target copied, the draw with a full mask, the rest put back after it
    c.holdRs(dev, D3DRS_COLORWRITEENABLE, 0xFu);
    kind = 3; ++h.maskEmuC;
  } else {
    ++h.maskEmuSkipped;
  }
  if (kind && ((kind != 3 && h.maskEmuLogged < 4) || (kind == 3 && h.maskEmuCopyLogged < 8))) {
    if (kind == 3) ++h.maskEmuCopyLogged; else ++h.maskEmuLogged;
    char msg[320];
    snprintf(msg, sizeof msg, "Sims 3 camera hook: masked write emulated at frame %u -> mask %lx into %ux%u, %s (blend was %lu: %lu/%lu op %lu), VS %016llx PS %016llx", h.frames + 1, (unsigned long) cw, (unsigned) h.rt0W, (unsigned) h.rt0H, kind == 1 ? "constant blend factor" : kind == 2 ? "separate alpha blend" : "copy, full mask, unwritten channels restored after the draw", (unsigned long) bl, (unsigned long) src, (unsigned long) dst, (unsigned long) op, (unsigned long long) h.vsHash, (unsigned long long) h.psHash);
    Logger::info(msg);
  }
}

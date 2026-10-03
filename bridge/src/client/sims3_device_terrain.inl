// The Sims 3 camera hook, part of d3d9_device.cpp: the terrain: the marker textures, the pixel
// shader variants, the terrain draws and the lot composite's second pass. Included after
// sims3_device_capture.inl; not a standalone header.

// Whether rtx.conf (next to this DLL, read by the runtime at start) tags the markers: the
// terrain option naming both hashes and the hidden-instance option naming the layer marker's.
// hashes: [0] terrain, [1] the world's layer passes, [2] the lot composite's passes -- all three
// terrain, the last two hidden, the first NOT (a hidden lot copy hides the lot, see
// sims3BeginTerrainDraw).
inline bool sims3ConfTagsMarkers(const uint64_t* hashes) {
  char path[MAX_PATH] = {};
  HMODULE self = GetModuleHandleA("d3d9.dll");
  if (!self || !GetModuleFileNameA(self, path, MAX_PATH)) return false;
  char* slash = strrchr(path, '\\'); if (!slash) return false;
  snprintf(slash + 1, (size_t) (MAX_PATH - (slash + 1 - path)), "rtx.conf");
  FILE* f = fopen(path, "rb"); if (!f) return false;
  char hex[sims3cam::kTerrainMarkers][40];
  for (int i = 0; i < sims3cam::kTerrainMarkers; ++i) snprintf(hex[i], sizeof hex[i], "%016llx", (unsigned long long) hashes[i]);
  bool terrainOk = false, hiddenOk = false;
  char line[1024];
  while (fgets(line, sizeof line, f)) {
    for (char* p = line; *p; ++p) *p = (char) tolower((unsigned char) *p);
    const char* eq = strchr(line, '=');
    if (!eq) continue;
    if (strncmp(line, "rtx.terraintextures", 19) == 0) terrainOk = strstr(eq, hex[0]) != nullptr && strstr(eq, hex[1]) != nullptr && strstr(eq, hex[2]) != nullptr;
    else if (strncmp(line, "rtx.hideinstancetextures", 24) == 0) hiddenOk = strstr(eq, hex[1]) != nullptr && strstr(eq, hex[2]) != nullptr && strstr(eq, hex[0]) == nullptr;
  }
  fclose(f);
  return terrainOk && hiddenOk;
}

// The three marker textures, made once on the device and filled with their fixed content, and
// a check that rtx.conf tags their hashes (the hashes are constant: xxhash only confirms them).
template<typename Dev>
bool sims3EnsureMarkers(Sims3Hook& h, Dev* dev) {
  if (h.marker[0] && h.marker[1] && h.marker[2]) return true;
  if (h.markerFailed) return false;
  for (int kind = 0; kind < sims3cam::kTerrainMarkers; ++kind) {
    if (h.marker[kind]) continue;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(dev->CreateTexture(sims3cam::kTerrainMarkerSize, sims3cam::kTerrainMarkerSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) || tex == nullptr) {
      h.markerFailed = true; Logger::info("Sims 3 camera hook: terrain marker texture could not be created; terrain draws are captured as before"); return false;
    }
    uint32_t pixels[sims3cam::kTerrainMarkerSize * sims3cam::kTerrainMarkerSize];
    sims3cam::terrainMarkerPixels(kind, pixels);
    D3DLOCKED_RECT lr = {};
    if (FAILED(tex->LockRect(0, &lr, nullptr, 0)) || lr.pBits == nullptr) {
      tex->Release(); h.markerFailed = true; Logger::info("Sims 3 camera hook: terrain marker texture could not be written; terrain draws are captured as before"); return false;
    }
    for (uint32_t y = 0; y < sims3cam::kTerrainMarkerSize; ++y) memcpy((uint8_t*) lr.pBits + (size_t) y * lr.Pitch, pixels + y * sims3cam::kTerrainMarkerSize, sims3cam::kTerrainMarkerSize * 4);
    tex->UnlockRect(0);
    h.marker[kind] = tex;
    h.markerHash[kind] = (uint64_t) XXH3_64bits(pixels, sizeof pixels);   // the runtime hashes level 0's bytes, rows packed
  }
  char msg[560];
  {
    char terrain[96], hidden[64];
    snprintf(terrain, sizeof terrain, "0x%016llX, 0x%016llX, 0x%016llX", (unsigned long long) h.markerHash[0], (unsigned long long) h.markerHash[1], (unsigned long long) h.markerHash[2]);
    snprintf(hidden, sizeof hidden, "0x%016llX, 0x%016llX", (unsigned long long) h.markerHash[1], (unsigned long long) h.markerHash[2]);
    h.markersConfigSent = sims3ConfTagsMarkers(h.markerHash);
    if (h.markersConfigSent)
      snprintf(msg, sizeof msg, "Sims 3 camera hook: terrain markers created; rtx.conf tags them (rtx.terrainTextures = %s, rtx.hideInstanceTextures = %s; terrain marker dark red (visible), world layer-pass marker purple (hidden), lot composite marker black (hidden))", terrain, hidden);
    else
      snprintf(msg, sizeof msg, "Sims 3 camera hook: terrain markers created but rtx.conf does NOT tag them as required -> the lines must read \"rtx.terrainTextures = %s\" and \"rtx.hideInstanceTextures = %s\" (the dark red marker NOT hidden; or tag the three flat 32x32 textures as Terrain Texture and the purple and black ones as Hide Instance Texture in the menu, Save Settings); until then the ground shows the markers or lots vanish", terrain, hidden);
  }
  Logger::info(msg);
  return true;
}

// The pixel shader variant for a terrain draw: the game's shader with sampler 0 renumbered to
// the first free sampler (the game's stage-0 texture is bound there), and for the lit lot-area
// paint shaders the final lighting multiply replaced by the albedo. Made once per shader;
// freeStage receives the stage the variant reads the moved texture from.
template<typename Dev>
IDirect3DPixelShader9* sims3PsVariant(Sims3Hook& h, Dev* dev, IDirect3DPixelShader9* base, uint64_t hash, uint8_t alphaMode, int forced, int& freeStage) {
  for (uint32_t i = 0; i < h.psVariantCount; ++i)
    if (h.psVariants[i].base == base && h.psVariants[i].hash == hash && h.psVariants[i].alphaMode == alphaMode && h.psVariants[i].forced == (uint8_t) forced) { freeStage = h.psVariants[i].freeStage; return h.psVariants[i].variant; }
  if (h.psVariantCount >= Sims3Hook::kPsVariants) return nullptr;
  Sims3Hook::PsVariant& v = h.psVariants[h.psVariantCount++];
  v = { base, hash, alphaMode, (uint8_t) forced, nullptr, 0 };
  UINT size = 0;
  if (FAILED(base->GetFunction(nullptr, &size)) || size < 8) return nullptr;
  std::vector<DWORD> t(size / 4 + 1);
  if (FAILED(base->GetFunction(t.data(), &size))) return nullptr;
  t.resize(sims3cam::shaderTokenCount(t.data(), t.size()));
  if (t.size() < 2) return nullptr;
  // the stage layer 0 is read from: the shader's detail stage (milestone 17k, psDetailSampler),
  // else the first stage the shader does not declare (in-frame that stage reads nothing, runs
  // 78-82; the lot paint composite, the one shader without a detail read, is given a paint
  // layer's stage instead: forced, lotCompositeStage)
  const int maxSampler = sims3cam::psMaxSampler(t.data(), t.size());
  const int detail = sims3cam::psDetailSampler(t.data(), t.size());
  int free = 0;
  if (forced >= 1) free = forced;                                       // the composite's layer stage (milestone 17l)
  else if (detail >= 1) free = detail;
  else if (maxSampler >= 0 && maxSampler < 15) free = maxSampler + 1;
  if (free < 1 || free > 15) {
    if (h.psVariantLogged < 8) { ++h.psVariantLogged; char msg[192]; snprintf(msg, sizeof msg, "Sims 3 camera hook: no terrain variant for PS %016llx (samplers up to s%d, no detail read, none free below 16)", (unsigned long long) hash, maxSampler); Logger::info(msg); }
    return nullptr;
  }
  const uint32_t swapped = sims3cam::psSwapSamplers(t, 0, (uint32_t) free);
  bool unlit = false, alpha = false;
  const bool alphaOne = alphaMode == 1;
  if (sims3cam::wantsUnlitPatch(hash)) unlit = sims3cam::psUnlitOutput(t);
  if (alphaOne) alpha = sims3cam::psForceAlphaOne(t);
  IDirect3DPixelShader9* ps = nullptr;
  h.creatingVariant = true;
  const HRESULT hr = dev->CreatePixelShader(t.data(), &ps);
  h.creatingVariant = false;
  if (FAILED(hr) || ps == nullptr) return nullptr;
  v.variant = ps; v.freeStage = (uint8_t) free;
  ++h.psVariantsMade; if (unlit) ++h.psVariantsUnlit; if (alpha) ++h.psVariantsAlpha;
  freeStage = free;
  return ps;
}

// Puts the game's sampler states back on the held free stage (milestone 18i).
template<typename Dev>
void sims3TerrainStageRelease(Sims3Hook& h, Dev* dev) {
  if (h.tblockStage < 0) return;
  h.ourSampler = true;
  for (int i = 0; i < Sims3Hook::kSamplerCopies; ++i) if (h.tblockSet & (1u << i)) dev->SetSamplerState((DWORD) h.tblockStage, kSims3SamplerCopy[i], h.tblockSaved[i]);
  h.ourSampler = false;
  h.tblockStage = -1; h.tblockSet = 0;
}
// Ends the terrain block (milestone 18g): the game's sampler states back on the free stage and
// sRGB sampling back on where the hook turned it off, unless the game set them meanwhile.
template<typename Dev>
void sims3TerrainBlockEnd(Sims3Hook& h, Dev* dev) {
  if (!h.tblockActive) return;
  h.ourSampler = true;
  for (DWORD s = 0; s < 16; ++s) if (h.tblockSrgb & (1u << s)) dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, TRUE);
  h.ourSampler = false;
  sims3TerrainStageRelease(h, dev);
  h.tblockActive = false; h.tblockSrgb = 0; ++h.tblockFlushes;
}
// The paint composite's second pass in place (milestone 19): the game's own draw was pass 1
// (layer 4 blacked out); the same draw goes out again as pass 2, hidden, blended ONE / ONE,
// through the ordinary draw hooks as the hook's own re-issue.
template<typename Dev>
void sims3CompositeSecondPass(Sims3Hook& h, Dev* dev, bool indexed, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVertices, UINT start, UINT count) {
  h.compositeSecond = false;
  DWORD src = 0, dst = 0; dev->GetRenderState(D3DRS_SRCBLEND, &src); dev->GetRenderState(D3DRS_DESTBLEND, &dst);
  h.ourState = true; dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE); dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE); h.ourState = false;
  h.reissue = true; h.reissueKind = 2;
  if (indexed) dev->DrawIndexedPrimitive(type, baseVertex, minIndex, numVertices, start, count); else dev->DrawPrimitive(type, start, count);
  h.reissue = false; h.reissueKind = 0;
  h.ourState = true; dev->SetRenderState(D3DRS_SRCBLEND, src); dev->SetRenderState(D3DRS_DESTBLEND, dst); h.ourState = false;
}

// Ends the block only if the draw about to go out samples a stage the block holds: an
// uncaptured draw is rasterized with the device's sampler states, so a held stage it reads
// would sample wrongly, while a draw that binds nothing there is unaffected (the game's
// untextured lot overlays come between the terrain draws, milestone 18h).
template<typename Dev>
void sims3TerrainBlockEndIfUsed(Sims3Hook& h, Dev* dev) {
  if (!h.tblockActive) return;
  for (DWORD s = 0; s < 16; ++s) if (h.boundTex[s] != nullptr && ((h.tblockSrgb & (1u << s)) || (int) s == h.tblockStage)) { sims3TerrainBlockEnd(h, dev); return; }
  ++h.tblockKept;
}

// A terrain draw (kind 1 base, 2 layer pass, 3 a square's piece): the marker at stage 0, the game's
// stage-0 texture and its sampler states at the free stage, the pixel shader variant bound.
// Everything goes back in sims3EndDraw. Returns false when the draw has to be captured the ordinary way.
template<typename Dev>
bool sims3BeginTerrainDraw(Sims3Hook& h, Dev* dev, uint8_t kind) {
  if (!h.psBound || !sims3EnsureMarkers(h, dev)) return false;
  int freeStage = -1;
  DWORD alphaBlend = 0; dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
  const uint8_t alphaMode = sims3cam::terrainAlphaMode(h.vsTerrain, kind, alphaBlend);
  // the lot paint composite (milestone 17l, sims3cam::lotCompositeStage): its mask goes to a
  // paint layer's stage and the black marker takes that layer out; two passes, in place
  const bool composite = h.psHash == sims3cam::kLotCompositePs;
  const int pass = composite ? (h.reissue && h.reissueKind == 2 ? 2 : 1) : 0;
  if (composite && pass == 1 && !h.reissue) h.compositeSecond = true;   // the second pass follows in place (milestone 19)
  IDirect3DPixelShader9* variant = sims3PsVariant(h, dev, h.psBound, h.psHash, alphaMode, sims3cam::lotCompositeStage(pass), freeStage);
  if (!variant || freeStage < 1 || freeStage > 15) { ++h.terrainNoVariant; return false; }
  // which marker: base draws visible (0); every layer pass hidden -- the world's blended layers
  // (1), a lot's further chunk copies (1) and its composite passes (2). A lot's
  // re-submissions are also split in two (below), so the runtime's draw tracker never takes one
  // of them for the lot's own visible instance: with the same mesh, baked material and position
  // it did so whenever the camera moved (the terrain texture transform in the identity hash
  // changes each frame), and a hidden or blended draw landing on the lot's instance flickered or
  // blanked the whole lot (runs 85-91).
  const bool lotFamily = h.vsTerrain && h.vsTerrain->lotFamily;
  const int markerIdx = composite ? 2 : (kind == 2 || kind == 3) ? 1 : 0;   // kind 3: a square's piece that only paints (milestone 60), hidden
  // textures: the game's stage 0 moves to the free stage (with stage 0's sampler states), the marker takes stage 0
  h.freeStageRestore = h.boundTex[freeStage]; if (h.freeStageRestore) h.freeStageRestore->AddRef();
  h.terrainFreeStage = freeStage;
  h.remapRestore = h.boundTex[0]; if (h.remapRestore) h.remapRestore->AddRef();
  h.remapActive = true;
  h.inRemap = true;
  dev->SetTexture((DWORD) freeStage, h.boundTex[0]);
  dev->SetTexture(0, h.marker[markerIdx]);
  if (pass == 2) {   // the second composite pass: layers 1 and 2 black as well, so only layer 4 x mask.w is added
    for (int s = 0; s < 2; ++s) { h.extraRestore[s] = h.boundTex[s + 1]; if (h.extraRestore[s]) h.extraRestore[s]->AddRef(); dev->SetTexture((DWORD) (s + 1), h.marker[2]); }
    h.extraActive = true; ++h.compositePasses;
  }
  h.inRemap = false;
  // a lot's re-submission goes out as two half draws (the draw hooks split it; milestone 18b): the
  // runtime files a draw by its index data and counts, so a half can never be taken for the lot's
  // own visible instance whatever the camera does (the same mesh, material and position otherwise).
  // The composite's additive blend makes the runtime file its instance in the
  // "unordered" set that primary rays pass through, and that mark is never cleared; with the
  // same mesh and the same baked material as the lot's ground, the ground draw inherited that
  // instance whenever the camera moved and the tracker re-paired draws by geometry and position
  // (run 106, the white unselectable lots of lot mode 3). Halves sit in another geometry bucket.
  if (lotFamily && kind == 2) h.splitDraw = true;   // a hidden lot re-submission goes out as two halves (milestone 18b)
  // sampler states: the free stage takes stage 0's, and every used stage samples raw (non-sRGB)
  // texels -- the bake then holds sRGB-encoded texels, which the ray tracer gamma-corrects
  // itself (the game samples its layers as sRGB). Both are held across the terrain block
  // (milestone 18g) and only re-sent where the device's current value differs.
  if (!h.tblockActive) { h.tblockActive = true; h.tblockSrgb = 0; }
  if (h.tblockStage != freeStage) { sims3TerrainStageRelease(h, dev); h.tblockStage = freeStage; }   // another free stage: the previous one back to the game's states first
  h.ourSampler = true;
  for (int i = 0; i < Sims3Hook::kSamplerCopies; ++i) {
    DWORD v0 = 0, vf = 0;
    dev->GetSamplerState(0, kSims3SamplerCopy[i], &v0); dev->GetSamplerState((DWORD) freeStage, kSims3SamplerCopy[i], &vf);
    if (v0 == vf) continue;
    if (!(h.tblockSet & (1u << i))) { h.tblockSaved[i] = vf; h.tblockSet |= (uint8_t) (1u << i); }   // the game's value, saved once while the stage is held
    dev->SetSamplerState((DWORD) freeStage, kSims3SamplerCopy[i], v0); ++h.samplerCopies;
  }
  for (DWORD s = 0; s < 16; ++s) {
    if (h.boundTex[s] == nullptr && (int) s != freeStage) continue;
    DWORD v = 0; dev->GetSamplerState(s, D3DSAMP_SRGBTEXTURE, &v);
    if (v) { dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE); h.tblockSrgb |= (uint16_t) (1u << s); ++h.srgbOffs; }
  }
  h.ourSampler = false;
  // a draw that writes alpha 1 into the bake -- a base draw, a lot's opaque chunk copies -- needs
  // the full colour mask (the game masks alpha off)
  if (alphaMode != 0) {
    DWORD cw = 0xF; dev->GetRenderState(D3DRS_COLORWRITEENABLE, &cw);
    if ((cw & 0xFu) != 0xFu) { h.cwRestore = cw; h.cwOurs = true; h.ourState = true; dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xFu); h.ourState = false; }
  }
  // the pixel shader variant; the game's shader held until sims3EndDraw
  h.psRestore = h.psBound; h.psRestore->AddRef();
  h.swappingPs = true; dev->SetPixelShader(variant); h.swappingPs = false;
  if (kind == 2) ++h.terrainLayerDraws; else if (kind != 3) ++h.terrainBaseDraws;   // kind 3 pieces: mergePaintPieces
  return true;
}

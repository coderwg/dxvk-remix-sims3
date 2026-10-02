// The Sims 3 camera hook, part of d3d9_device.cpp: the device reset, and the hook around every draw
// of the game (sims3BeginDraw, sims3EndDraw). Included after sims3_device_walls.inl, last in the
// first anonymous namespace; not a standalone header.

// The device reset (and its destruction): every object the hook made or holds a reference to is
// released, the bound-shader and bound-texture facts are forgotten (the device state is back to
// defaults; the game's shaders themselves survive a Reset), and the runtime is back to default state.
inline void sims3OnReset(Sims3Hook& h) {
  for (auto& s : h.squares) if (s.merged) s.merged->Release();   // the squares' merged shapes (milestone 60)
  h.squares.clear(); h.mergePending = -1;
  for (auto& e : h.plates) {   // the low-detail lots' split, glow layer and its textures (milestones 69-71)
    if (e.house) e.house->Release(); if (e.plate) e.plate->Release(); if (e.glow) e.glow->Release(); if (e.glowVb) e.glowVb->Release();
  }
  h.plates.clear();
  for (auto& g : h.glowTexs) if (g.tex) g.tex->Release();
  h.glowTexs.clear();
  if (h.platePs) { h.platePs->Release(); h.platePs = nullptr; }
  h.platePsFailed = false;
  for (uint32_t i = 0; i < h.wallCacheCount; ++i) sims3ReleaseWallEntry(h.wallCache[i]);
  h.wallCacheCount = 0; for (auto& m : h.masks) m = Sims3Hook::MaskEntry(); h.maskUse = 0;
  for (auto& e : h.bufHashes) e = Sims3Hook::HashEntry(); h.bufHashUse = 0;
  h.vsWall = nullptr; h.wallLayout = sims3cam::WallLayout(); h.wallDeclId = 0;
  for (uint32_t i = 0; i < h.vsVariantCount; ++i) if (h.vsVariants[i].variant) h.vsVariants[i].variant->Release();
  h.vsVariantCount = 0;
  if (h.autoVsRestore) { h.autoVsRestore->Release(); h.autoVsRestore = nullptr; }
  if (h.remapRestore) { h.remapRestore->Release(); h.remapRestore = nullptr; }
  // the terrain baker's markers and pixel shader variants (milestone 17)
  for (uint32_t i = 0; i < h.psVariantCount; ++i) if (h.psVariants[i].variant) h.psVariants[i].variant->Release();
  h.psVariantCount = 0;
  if (h.psRestore) { h.psRestore->Release(); h.psRestore = nullptr; }
  if (h.freeStageRestore) { h.freeStageRestore->Release(); h.freeStageRestore = nullptr; }
  for (int i = 0; i < sims3cam::kTerrainMarkers; ++i) { if (h.marker[i]) h.marker[i]->Release(); h.marker[i] = nullptr; h.markerHash[i] = 0; }
  if (h.glassMarker) { h.glassMarker->Release(); h.glassMarker = nullptr; }
  for (int m = 0; m < sims3cam::kWaterMaterials; ++m) { if (h.waterMarkers[m]) h.waterMarkers[m]->Release(); h.waterMarkers[m] = nullptr; h.waterMarkerHashes[m] = 0; h.waterMarkerFailed[m] = false; }
  if (h.mirrorMarker) { h.mirrorMarker->Release(); h.mirrorMarker = nullptr; }
  h.mirrorMarkerHash = 0; h.mirrorMarkerFailed = false;
  if (h.carGlassMarker) { h.carGlassMarker->Release(); h.carGlassMarker = nullptr; }
  for (size_t i = 0; i < sims3cam::kGlassSurveyMax; ++i) { if (h.surveyMarkers[i]) h.surveyMarkers[i]->Release(); h.surveyMarkers[i] = nullptr; h.surveyHashes[i] = 0; h.surveyFailed[i] = false; }
  h.carGlassMarkerHash = 0; h.carGlassMarkerFailed = false;
  h.glassMarkerHash = 0; h.glassMarkerFailed = false; h.blendOurs = false; h.bumpHashes.clear();
  for (auto& g : h.glassSides) if (g.second.ib) g.second.ib->Release();
  h.glassSides.clear(); h.glassIb = nullptr; h.glassPrims = 0;
  h.markerFailed = false; h.markersConfigSent = false; h.terrainFreeStage = -1; h.tblockActive = false; h.tblockStage = -1; h.tblockSet = 0; h.tblockSrgb = 0; h.ourSampler = false; h.psBound = nullptr; h.vsTerrain = nullptr; h.lotFurtherCopy = false; h.swappingPs = false;
  h.cwOurs = false; h.atOurs = false; h.fogOurs = false;
  h.compositePass = 0; h.extraActive = false; h.splitDraw = false; h.ourConsts = false; h.reissue = false; h.reissueKind = 0; h.compositeSecond = false;
  for (int i = 0; i < 2; ++i) { if (h.extraRestore[i]) h.extraRestore[i]->Release(); h.extraRestore[i] = nullptr; }
  h.remapActive = false; h.maskEmu = 0; h.viewportOurs = false; h.vsSkyDome = false;
  h.vsBound = nullptr; h.vsTabled = false; h.vsNormal = nullptr; h.pendingPromote = 0; h.vsHash = 0;
  h.patch = nullptr; h.vsNeverCapture = 0; h.vsCapturedUv = false;
  h.lotCopies.clear();
  h.psAuto = nullptr; h.psAlbedoStage = -1; h.psTintReg = -1; h.psHash = 0;
  h.declIs3D = false; h.drawCaptured = false; h.autoCapturedUv = false;
  for (int i = 0; i < 16; ++i) { h.boundTex[i] = nullptr; h.boundColor2D[i] = false; h.boundKind[i] = 0; h.boundFmt[i] = 0; h.boundW[i] = h.boundH[i] = 0; }
  for (uint32_t i = 0; i < h.scratchCount; ++i) { if (h.scratch[i].surf) h.scratch[i].surf->Release(); if (h.scratch[i].tex) h.scratch[i].tex->Release(); h.scratch[i] = Sims3Hook::Scratch(); }
  h.scratchCount = 0; h.copyScratch = -1; h.ourDraw = false; h.rt0W = h.rt0H = 0; h.rt0Fmt = 0;
  h.held = sims3cam::Held(); h.cameraValid = false; h.camMirrored = false; h.cameraValidBeforeMirror = false; h.drawDropped = false; h.rtIsPrimary = true;
  h.frameCamSet = false;
  h.tssOurs = 0; h.uvIndexHidden = false; h.factorOurs = false; h.sentFactor = 0xFFFFFFFFu; h.gameFactor = 0xFFFFFFFFu;
  h.gameTss0[0] = D3DTOP_MODULATE; h.gameTss0[1] = D3DTA_TEXTURE; h.gameTss0[2] = D3DTA_CURRENT; h.gameTss0[3] = 0;
  h.gameXformSet[0] = h.gameXformSet[1] = false;
  h.sunSet = false; h.moonSet = false; h.terrainSunDraws = 0;
  for (uint32_t k = 0; k < h.lamps.n; ++k) { sims3cam::Lamp& Lr = h.lamps.lamps[k]; Lr.sent = false; Lr.api = Lr.api2 = Lr.api3 = nullptr; }   // the runtime's lights are gone with the device; the lamps are re-sent
  for (bool& r : h.rsSet) r = false;
  Logger::info("Sims 3 camera hook: device reset -> the hook's objects released, held state cleared");
}

// A wall piece's triangles against the frame's kept wall triangles of its vertex buffer (milestone 97):
// true when it is the back side of a zero-thickness wall; otherwise its triangles join them.
template<typename Dev>
bool sims3WallBackSide(Sims3Hook& h, Dev* dev) {
  IDirect3DVertexBuffer9* vb = nullptr; UINT off = 0, stride = 0;
  if (FAILED(dev->GetStreamSource(0, &vb, &off, &stride)) || !vb) return false;
  vb->Release();
  IDirect3DIndexBuffer9* ib = nullptr;
  if (FAILED(dev->GetIndices(&ib)) || !ib) return false;
  ib->Release();
  auto* lvb = bridge_cast<Direct3DVertexBuffer9_LSS*>(vb); auto* lib = bridge_cast<Direct3DIndexBuffer9_LSS*>(ib);
  if (!lvb || !lib || stride == 0 || h.wallLayout.posOff < 0) return false;
  const uint32_t vbId = (uint32_t) lvb->getId();
  struct { uint32_t vbId, vbVer, ibId, ibVer, off, stride; int32_t base; uint32_t start, prims, posOff; } pk = {
    vbId, lvb->sims3Version, (uint32_t) lib->getId(), lib->sims3Version, off, stride, h.drawBase, h.drawStart, h.drawPrims, (uint32_t) h.wallLayout.posOff };
  const uint64_t pieceKey = sims3cam::fnv1a64(&pk, sizeof pk);
  auto it = h.wallPieceTris.find(pieceKey);
  if (it == h.wallPieceTris.end()) {
    if (h.wallPieceTris.size() > 8192u) h.wallPieceTris.clear();
    std::vector<uint64_t> tris;
    const uint8_t* vd = lvb->sims3Data(); const uint8_t* id = lib->sims3Data();
    const bool ib32 = lib->getDesc().Format == D3DFMT_INDEX32;
    const size_t isz = ib32 ? 4u : 2u, n = (size_t) h.drawPrims * 3u;
    if (vd && id && ((size_t) h.drawStart + n) * isz <= lib->sims3Size()) {
      tris.reserve(h.drawPrims);
      for (size_t t = 0; t < n; t += 3) {
        int16_t p[3][4]; bool ok = true;
        for (int c = 0; c < 3 && ok; ++c) {
          const int64_t v = (int64_t) h.drawBase + (int64_t) sims3cam::readIndex(id, (size_t) h.drawStart + t + (size_t) c, ib32);
          const size_t at = (size_t) off + (size_t) v * stride + (size_t) h.wallLayout.posOff;
          if (v < 0 || at + 8u > lvb->sims3Size()) ok = false; else memcpy(p[c], vd + at, 8);   // SHORT4: x, full height, z, stub height
        }
        tris.push_back(ok ? sims3cam::wallTriKey(p[0], p[1], p[2]) : 0u);
      }
    }
    it = h.wallPieceTris.emplace(pieceKey, std::move(tris)).first;
  }
  auto& kept = h.wallFrameTris[vbId];
  uint32_t matched = 0;
  if (sims3cam::isWallBackSide(it->second, kept, matched)) {
    if (h.wallBackLogged < 12u) {
      ++h.wallBackLogged;
      Logger::info(format_string("Sims 3 camera hook: wall back side left out at frame %u -> VS %016llx PS %016llx, vertex buffer %u base %d: %u triangles, all on planes an earlier piece faces the other way",
                                 h.frames + 1, (unsigned long long) h.vsHash, (unsigned long long) h.psHash, vbId, (int) h.drawBase, (unsigned) h.drawPrims));
    }
    return true;
  }
  for (uint64_t k : it->second) if (k) kept.insert(k);
  return false;
}

// Bumpy glass (milestone 109): a named glass whose bump map bends the scene behind goes out as the
// clear glass with its own bump map -- the game's bump map itself at stage 0, whose runtime hash (XXH3
// of level 0 as the client keeps it, as the runtime hashes it: run 207) names its material in the
// Sims3GlassBumps mod, made by sims3/remix-mod/make_textures.py --bumps from this hook's dumps (game
// data). Returns that texture, or nullptr while the mod has none for it: the draw then goes out as
// the clear glass and the bump map is written once, as rtx-remix/logs/sims3-textures/bump_<hash>_<pixel
// shader>_s<stage>_<w>x<h>_<format>_k<slope scale x 1000>.raw.
template<typename Dev>
IDirect3DBaseTexture9* sims3GlassBump(Sims3Hook& h, Dev* dev, const sims3cam::NamedGlass& g) {
#if SIMS3_HAVE_XXHASH
  const int s = g.bumpStage;
  if (s < 0 || s >= 16 || h.boundKind[s] != 1 || !h.boundTex[s]) return nullptr;   // a 2D texture, not a render target
  auto* tex = bridge_cast<Direct3DTexture9_LSS*>(h.boundTex[s]);
  const uint8_t* data = tex ? tex->sims3Level0Data() : nullptr;
  if (!data) return nullptr;
  static char base[MAX_PATH] = {};   // the game's folder
  if (!base[0]) { GetModuleFileNameA(nullptr, base, MAX_PATH); if (char* p = strrchr(base, '\\')) *p = 0; }
  if (!h.bumpModRead) {
    h.bumpModRead = true;
    char path[MAX_PATH + 64]; snprintf(path, sizeof path, "%s\\rtx-remix\\mods\\Sims3GlassBumps\\mod.usda", base);
    std::string text; FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") == 0 && f) { char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n); fclose(f); }
    for (const uint64_t m : sims3cam::modMaterialHashes(text)) h.bumpMaterials.insert(m);
    Logger::info(format_string("Sims 3 camera hook: bumpy glass -- %u bump map materials in %s%s", (unsigned) h.bumpMaterials.size(), path, text.empty() ? " (not there)" : ""));
  }
  const D3DSURFACE_DESC d = tex->getLevelDesc(0);
  const size_t bytes = bridge_util::calcTotalSizeOfRect(d.Width, d.Height, d.Format);
  const uint64_t key = (uint64_t) tex->getId() << 32 | tex->sims3Level0Version();
  auto it = h.bumpHashes.find(key);
  if (it == h.bumpHashes.end()) {
    if (h.bumpHashes.size() > 4096u) h.bumpHashes.clear();
    it = h.bumpHashes.emplace(key, (uint64_t) XXH3_64bits(data, bytes)).first;
  }
  const uint64_t hash = it->second;
  if (h.bumpMaterials.count(hash)) { ++h.bumpDraws; return h.boundTex[s]; }
  ++h.bumpPending;
  if (h.bumpWritten.size() < 64u && h.bumpWritten.insert(hash).second) {
    float c[4] = { 1.f, 1.f, 1.f, 1.f };
    if (g.bumpScaleReg >= 0) dev->GetPixelShaderConstantF((UINT) g.bumpScaleReg, c, 1);
    char dir[MAX_PATH + 32]; snprintf(dir, sizeof dir, "%s\\rtx-remix\\logs\\sims3-textures", base);
    CreateDirectoryA(dir, nullptr);
    char fb[16], path[MAX_PATH + 160];
    snprintf(path, sizeof path, "%s\\bump_%016llX_%016llx_s%d_%ux%u_%s_k%d.raw", dir, (unsigned long long) hash, (unsigned long long) h.psHash, s,
             (unsigned) d.Width, (unsigned) d.Height, sims3FormatName(d.Format, fb, sizeof fb), (int) std::lround(c[0] * 1000.f));
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
      FILE* f = nullptr;
      if (fopen_s(&f, path, "wb") == 0 && f) {
        fwrite(data, 1, bytes, f); fclose(f);
        Logger::info(format_string("Sims 3 camera hook: bump map written -> %s", path));
      }
    }
  }
  return nullptr;
#else
  (void) h; (void) dev; (void) g; return nullptr;
#endif
}

// A glass draw's sheets with a back side (milestone 104, sims3cam::glassFrontTriangles): the kept
// triangles, as an index buffer of the hook's own made once per mesh (buffers, their versions, range)
// and sent in the game's place; none when nothing is left out or the mesh is not read -- indexed
// triangle lists with one POSITION only (the parked cars' VS a77613ea adds a second one to the first:
// POSITION alone is not their place).
template<typename Dev>
void sims3GlassOneSide(Sims3Hook& h, Dev* dev) {
  h.glassIb = nullptr; h.glassPrims = 0;
  if (!h.drawIndexed || h.drawType != D3DPT_TRIANGLELIST || !h.declElems || h.drawPrims == 0) return;
  const D3DVERTEXELEMENT9* pe = nullptr; bool second = false;
  for (const D3DVERTEXELEMENT9* e = h.declElems; e->Stream != 0xFF; ++e) if (e->Usage == D3DDECLUSAGE_POSITION) { if (e->UsageIndex == 0) pe = e; else second = true; }
  const uint32_t bytes = pe ? sims3cam::declTypeBytes((uint8_t) pe->Type) : 0u;
  if (!pe || second || bytes == 0) { ++h.glassSideSkipped; return; }
  IDirect3DVertexBuffer9* vb = nullptr; UINT off = 0, stride = 0;
  if (FAILED(dev->GetStreamSource(pe->Stream, &vb, &off, &stride)) || !vb) return;
  vb->Release();
  IDirect3DIndexBuffer9* ib = nullptr;
  if (FAILED(dev->GetIndices(&ib)) || !ib) return;
  ib->Release();
  auto* lvb = bridge_cast<Direct3DVertexBuffer9_LSS*>(vb); auto* lib = bridge_cast<Direct3DIndexBuffer9_LSS*>(ib);
  if (!lvb || !lib || stride == 0) return;
  struct { uint32_t vbId, vbVer, ibId, ibVer, off, stride; int32_t base; uint32_t start, prims, posOff, posType; } pk = {
    (uint32_t) lvb->getId(), lvb->sims3Version, (uint32_t) lib->getId(), lib->sims3Version, off, stride, h.drawBase, h.drawStart, h.drawPrims, (uint32_t) pe->Offset, (uint32_t) pe->Type };
  const uint64_t key = sims3cam::fnv1a64(&pk, sizeof pk);
  auto it = h.glassSides.find(key);
  if (it == h.glassSides.end()) {
    if (h.glassSides.size() >= 1024u) { for (auto& g : h.glassSides) if (g.second.ib) g.second.ib->Release(); h.glassSides.clear(); }
    Sims3Hook::GlassSide side; side.prims = h.drawPrims;
    const uint8_t* vd = lvb->sims3Data(); const uint8_t* id = lib->sims3Data();
    const bool ib32 = lib->getDesc().Format == D3DFMT_INDEX32;
    const size_t isz = ib32 ? 4u : 2u, n = (size_t) h.drawPrims * 3u;
    bool ok = vd && id && ((size_t) h.drawStart + n) * isz <= lib->sims3Size();
    std::vector<uint32_t> idx(ok ? n : 0); std::vector<float> pos(ok ? n * 3 : 0);
    for (size_t i = 0; ok && i < n; ++i) {
      idx[i] = sims3cam::readIndex(id, (size_t) h.drawStart + i, ib32);
      const int64_t v = (int64_t) h.drawBase + (int64_t) idx[i];
      const size_t at = (size_t) off + (size_t) v * stride + (size_t) pe->Offset;
      if (v < 0 || at + bytes > lvb->sims3Size()) { ok = false; break; }
      float f[4] = {}; sims3cam::declDecode((uint8_t) pe->Type, vd + at, f);
      if ((pe->Type == D3DDECLTYPE_SHORT4 || pe->Type == D3DDECLTYPE_SHORT4N) && f[3] != 0.f) for (int c = 0; c < 3; ++c) f[c] /= f[3];   // the game's shaders divide a packed position by its w
      memcpy(&pos[i * 3], f, 12);
    }
    if (ok) {
      std::vector<uint8_t> keep;
      side.dropped = sims3cam::glassFrontTriangles(pos.data(), h.drawPrims, keep);
      if (side.dropped && side.dropped < h.drawPrims) {
        std::vector<uint32_t> kept; kept.reserve((size_t) (h.drawPrims - side.dropped) * 3u);
        for (size_t t = 0; t < keep.size(); ++t) if (keep[t]) { kept.push_back(idx[t * 3]); kept.push_back(idx[t * 3 + 1]); kept.push_back(idx[t * 3 + 2]); }
        side.ib = sims3MakeIndexBuffer(dev, kept);
        side.prims = h.drawPrims - side.dropped;
      }
      if (side.dropped && h.glassSideLogged < 12u) {
        ++h.glassSideLogged;
        Logger::info(format_string("Sims 3 camera hook: glass with a back side at frame %u -> VS %016llx PS %016llx: %u of %u triangles face away on a plane the sheet already covers, left out%s",
                                   h.frames + 1, (unsigned long long) h.vsHash, (unsigned long long) h.psHash, side.dropped, h.drawPrims, side.ib ? "" : " -- not sent (no index buffer)"));
      }
    }
    it = h.glassSides.emplace(key, side).first;
  }
  if (it->second.ib) { h.glassIb = it->second.ib; h.glassPrims = it->second.prims; ++h.glassSideDraws; h.glassSideTris += it->second.dropped; }
}

// Before every draw of the game (not the hook's own restore quad). A captured draw -- the main
// camera held, see sims3ApplyForDraw -- gets: its albedo presented as stage 0 when the game bound
// a cube map / render target there (Remix would drop the draw), the vertex shader variant for
// the draw (promoted coordinate, world normal, c255 read), the raw texcoord set hidden for the
// families whose shader output is verified, its tint as the texture factor, and its shader's
// cut-out as an alpha test (milestones 67-68). Any other draw gets the game's own state back, and a masked write
// into an offscreen target its emulation. Returns whether the draw is captured.
template<typename Dev>
bool sims3BeginDraw(Sims3Hook& h, Dev* dev, const DWORD* rs, UINT freq0) {
  h.glassIb = nullptr; h.glassPrims = 0;
  if (h.mergePending >= 0 && !h.reissue) {   // a square's shape that was not sent after its piece (the piece took another way out): next piece then
    if ((size_t) h.mergePending < h.squares.size()) h.squares[(size_t) h.mergePending].mergedFrame = 0xFFFFFFFFu;
    h.mergePending = -1; ++h.mergeSkipped;
  }
  const bool want = sims3ApplyForDraw(h, dev, rs);
  h.drawCaptured = want;
  if (h.drawDropped) return false;                // a reflection pass's draw: the caller returns without drawing
  if (!want) {
    sims3TerrainBlockEndIfUsed(h, dev);      // an uncaptured draw follows the terrain block: ends it only if it samples a held stage (milestone 18h)
    sims3RestoreGameState(h, dev);
    sims3BeginMaskedWrite(h, dev, rs);
    return false;
  }
  // the neighbourhood view's lot picture (milestone 63): a second surface over the lot's ground, left out
  if (h.vsHash == sims3cam::kLotPictureVs) { h.drawDropped = true; h.drawCaptured = false; h.dropWhy = "lot picture"; ++h.lotPictureDropped; return false; }
  // a mirror's face (milestone 101, sims3cam::isReflectiveSheet): the mirror material below
  h.reflectiveSheet = h.psAuto && h.psAuto->valid && sims3cam::isReflectiveSheet(*h.psAuto, rs[D3DRS_STENCILENABLE]);
  // a glass the game draws twice, unblended then blended (milestone 104, NamedGlass::blendedPassOnly): the unblended pass left out
  if (const sims3cam::NamedGlass* g = sims3cam::namedGlass(h.psHash)) if (g->blendedPassOnly && !rs[D3DRS_ALPHABLENDENABLE]) {
    h.drawDropped = true; h.drawCaptured = false; h.dropWhy = "glass's unblended pass"; ++h.glassPassDropped; return false;
  }
  // a zero-thickness wall's back side (milestone 97, sims3cam::isWallBackSide): left out
  if (h.drawIndexed && h.drawType == D3DPT_TRIANGLELIST && h.wallLayout.valid && h.vsWall && h.vsWall->valid && sims3WallBackSide(h, dev)) {
    h.drawDropped = true; h.drawCaptured = false; h.dropWhy = "wall back side"; ++h.wallBackDropped; return false;
  }
  int k = -1;
  ++h.capturedDraws;
  if (rs[D3DRS_FOGENABLE] && (rs[D3DRS_FOGTABLEMODE] != D3DFOG_NONE || rs[D3DRS_FOGVERTEXMODE] != D3DFOG_NONE)) ++h.gameFogDraws;   // a fog state of the game's own (milestone 56: none expected)
  h.autoCapturedUv = false; h.pendingPromote = 0; h.drawGlass = false;
  // a terrain draw (milestone 17): handed to the runtime's terrain baker with the marker at
  // stage 0 and the game's pixel shader variant; no albedo stage, no vertex shader variant
  uint8_t terrainKind = sims3cam::terrainDrawKind(h.vsTerrain, rs[D3DRS_ALPHABLENDENABLE], h.lotFurtherCopy);
  const bool lotFamilyDraw = terrainKind != 0 && h.vsTerrain && h.vsTerrain->lotFamily;
  // the town ground's squares (milestones 60, 63; the detailed pieces and the coarse squares alike):
  // an opaque piece of a square whose merged shape is ready only paints (3); the shape follows it
  // (sims3MergedSquareDraw)
  if (terrainKind == 1 && !h.reissue && !lotFamilyDraw) terrainKind = sims3SquarePiece(h, dev);
  if (terrainKind != 0 && h.reissue) terrainKind = h.reissueKind ? h.reissueKind : 2;   // the hook's own re-issue: the composite's second pass (hidden), a square's merged shape (1)
  h.compositeSecond = false;
  // a lot's ground goes out in place (milestone 19): nothing overwrites its paint in the atlas (run 119's paint test)
  const bool terrain = terrainKind != 0 && sims3BeginTerrainDraw(h, dev, terrainKind);
  if (!terrain) sims3TerrainBlockEnd(h, dev);   // a captured non-terrain draw follows the terrain block (milestone 18g)
  // the game's fog for the runtime (milestone 56): on the frame's first base terrain draws (drawn in
  // every view; their pixel shaders are 3.0, which fixed-function fog leaves alone, so the bake is
  // untouched), as D3D9 linear fog; the game's states are put back after the draw (sims3EndDraw)
  if (terrain && terrainKind == 1 && h.fogReady && h.psMajor >= 3 && h.fogFrameDraws < 4u) {
    ++h.fogFrameDraws; ++h.fogDraws;
    DWORD start = 0, end = 0; std::memcpy(&start, &h.fogStart, 4); std::memcpy(&end, &h.fogEnd, 4);
    const DWORD ours[5] = { TRUE, D3DFOG_LINEAR, h.fogColour, start, end };
    for (int i = 0; i < 5; ++i) dev->GetRenderState(kSims3FogRs[i], &h.fogSaved[i]);
    h.ourState = true; for (int i = 0; i < 5; ++i) dev->SetRenderState(kSims3FogRs[i], ours[i]); h.ourState = false;
    h.fogOurs = true;
  }
  if (!terrain) {
    // glass, mirrors and water (milestones 80, 86, 99, 101, 105, 109): the material's marker at stage 0
    // and the draw's blending off; its material is the hook's Remix mods' glass, car glass, mirror or
    // the water's own (sims3cam::isGlassShader, namedGlass, isReflectiveSheet, waterMaterial); a bumpy
    // glass's own bump map in the marker's place once the Sims3GlassBumps mod names it (sims3GlassBump)
    const int waterMat = sims3cam::waterMaterial(h.psHash);
    const bool water = waterMat >= 0 && sims3EnsureWaterMarker(h, dev, waterMat);
    const sims3cam::NamedGlass* named = water ? nullptr : sims3cam::namedGlass(h.psHash);
    const bool mirror = !water && h.reflectiveSheet && sims3EnsureMirrorMarker(h, dev);
    const bool carGlass = !water && !h.reflectiveSheet && named && named->material == sims3cam::kCarGlass && sims3EnsureCarGlassMarker(h, dev);
    IDirect3DBaseTexture9* const bump = (!water && !h.reflectiveSheet && !carGlass && named && named->bumpStage >= 0 && h.psAuto && h.psAuto->valid) ? sims3GlassBump(h, dev, *named) : nullptr;
    const bool glass = !water && !h.reflectiveSheet && !carGlass && h.psAuto && (named || (rs[D3DRS_ALPHABLENDENABLE] && sims3cam::isGlassShader(*h.psAuto))) && (bump || sims3EnsureGlassMarker(h, dev));
    IDirect3DTexture9* const survey = (carGlass || glass) ? sims3SurveyMarker(h, dev) : nullptr;   // the glass survey (milestone 110; one run)
    if (water || mirror || carGlass || glass) {
      h.drawGlass = true;
      h.remapRestore = h.boundTex[0]; if (h.remapRestore) h.remapRestore->AddRef();   // held until sims3EndDraw, as for an albedo remap
      h.remapActive = true;
      h.inRemap = true; dev->SetTexture(0, survey ? survey : water ? h.waterMarkers[waterMat] : mirror ? h.mirrorMarker : carGlass ? h.carGlassMarker : bump ? bump : (IDirect3DBaseTexture9*) h.glassMarker); h.inRemap = false;
      // water (milestone 94): the sampler states of the game's first wave map (the one its TEXCOORD0
      // reads) on stage 0, where the runtime takes the material's -- the normal map tiles as the game's
      // waves do; back in sims3EndDraw with the albedo remap's
      if (water) for (int s = 0; s < 16; ++s) if (h.boundKind[s] == 1 && sims3cam::isWaveMapFormat(h.boundFmt[s])) { if (s > 0) h.remapSamplerSet = sims3SamplerStatesTo0(h, dev, (DWORD) s, h.remapSamplerSaved); break; }
      // bumpy glass (milestone 109): its bump map's sampler states on stage 0 and the bump map's coordinate
      // as the captured TEXCOORD0 (a promoted variant), so the runtime lays the bumps where the game does
      if (bump && !survey) {
        if (named->bumpStage > 0) h.remapSamplerSet = sims3SamplerStatesTo0(h, dev, (DWORD) named->bumpStage, h.remapSamplerSaved);
        sims3AutoTexcoord(h, named->bumpStage, -1, false);
      }
      h.blendSaved = rs[D3DRS_ALPHABLENDENABLE]; h.blendOurs = true;
      h.ourState = true; dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); h.ourState = false;
      if (water) ++h.waterDraws[waterMat]; else if (mirror) ++h.mirrorDraws; else if (carGlass) ++h.carGlassDraws; else ++h.glassDraws;
      if (carGlass || glass) sims3GlassOneSide(h, dev);   // a sheet's back side left out (milestone 104)
      bool seen = false; for (uint32_t i = 0; i < h.glassLogged; ++i) if (h.glassLoggedPs[i] == h.psHash) seen = true;
      if (!seen && h.glassLogged < 32) {
        h.glassLoggedPs[h.glassLogged++] = h.psHash; char msg[224];
        const char* what = water ? sims3cam::kWaterMaterial[waterMat].name : mirror ? "mirror" : carGlass ? "car glass" : bump ? "bumpy glass" : "glass";
        snprintf(msg, sizeof msg, "Sims 3 camera hook: %s at frame %u -> VS %016llx PS %016llx presented with %s%s%s, blending off", what, h.frames + 1, (unsigned long long) h.vsHash, (unsigned long long) h.psHash,
                 bump ? "its own bump map" : "the ", bump ? "" : what, bump ? "" : " marker");
        Logger::info(msg);
      }
    } else if (h.psAlbedoStage >= 0 && h.psAlbedoStage < 16 && h.boundTex[h.psAlbedoStage] != nullptr) {
      k = h.psAlbedoStage; ++h.overrideDraws;
      // on an untabled vertex shader (a Sim outfit's permutation, run 41) the coordinate the albedo
      // sampler reads, from the bytecode, decides the promotion and the captured UV
      if (!h.vsTabled && h.vsBound && h.psAuto && h.psAuto->valid) sims3AutoTexcoord(h, k, -1, true);
    } else {
      // an untabled pixel shader: the albedo from its bytecode (a promoted vertex-shader variant when needed)
      if (h.psAuto && h.psAuto->valid && h.vsBound) k = sims3AutoAlbedo(h);
      if (k < 0) { bool bound[16]; for (int i = 0; i < 16; ++i) bound[i] = h.boundTex[i] != nullptr; k = sims3cam::pickAlbedoStage(bound, h.boundColor2D); }
    }
    // a cut-out the runtime must see (milestones 67-68): the shader's texkill on its albedo's alpha,
    // a * alpha + b with the bound constants, as the D3D alpha test the runtime applies to the albedo
    if (k >= 0 && h.psAuto && h.psAuto->valid && h.psAuto->cutSampler == k && !rs[D3DRS_ALPHATESTENABLE] && !h.atOurs) {
      auto get = [&](uint32_t reg, uint32_t comp) -> float { float v[4] = {}; dev->GetPixelShaderConstantF(reg, v, 1); return v[comp & 3u]; };
      const float a = sims3cam::cutEval(h.psAuto->cutA, get), b = sims3cam::cutEval(h.psAuto->cutB, get);
      uint32_t ref = 0; const uint32_t func = sims3cam::cutAlphaTest(a, b, ref);
      if (func) {
        dev->GetRenderState(D3DRS_ALPHATESTENABLE, &h.atSaved[0]); dev->GetRenderState(D3DRS_ALPHAFUNC, &h.atSaved[1]); dev->GetRenderState(D3DRS_ALPHAREF, &h.atSaved[2]);
        h.atOurs = true; h.ourState = true;
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE); dev->SetRenderState(D3DRS_ALPHAFUNC, func); dev->SetRenderState(D3DRS_ALPHAREF, ref);
        h.ourState = false;
        ++h.alphaCutDraws;
      }
      bool seen = false; for (uint32_t i = 0; i < h.cutLogged; ++i) if (h.cutLoggedPs[i] == h.psHash) seen = true;
      if (!seen && h.cutLogged < 32) {
        h.cutLoggedPs[h.cutLogged++] = h.psHash; char msg[256];
        snprintf(msg, sizeof msg, "Sims 3 camera hook: cut-out of PS %016llx (VS %016llx): texkill on the alpha of s%d, %.3f * alpha + %.3f -> %s %u",
                 (unsigned long long) h.psHash, (unsigned long long) h.vsHash, k, a, b,
                 func == D3DCMP_GREATEREQUAL ? "alpha test >=" : func == D3DCMP_LESSEQUAL ? "alpha test <=" : func == D3DCMP_NEVER ? "never drawn" : "nothing discarded, no test", ref);
        Logger::info(msg);
      }
    }
    // the vertex shader variant for the draw: the promoted coordinate and/or the world normal as a
    // NORMAL output, and on a hardware-instanced draw (split per instance) a read of c255 for the tag
    sims3BindVariant(h, dev, h.pendingPromote, sims3NormalChoice(h), (freq0 & D3DSTREAMSOURCE_INDEXEDDATA) && (freq0 & 0x3FFFFFFFu) > 1u);
  }
  if (k >= 0 && k < 16) ++h.remapCount[k];
  sims3NoteCapture(h, k, rs, freq0);
  // texture coordinates: hide the raw input set from the runtime (stage 0 index -> 7) for the
  // families whose shader output is verified, so it samples with the captured TEXCOORD0
  const bool wantCaptured = h.vsCapturedUv || h.autoCapturedUv;
  h.ourState = true;
  if (wantCaptured != h.uvIndexHidden) {
    h.uvIndexHidden = wantCaptured;
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, h.uvIndexHidden ? 7u : h.gameTss0[3]);
    if (h.uvIndexHidden && !h.loggedCapturedUv) { h.loggedCapturedUv = true; Logger::info("Sims 3 camera hook: first draw sampling with the shader's captured texture coordinates (stage 0 texcoord index 7 hides the raw input set)"); }
  }
  // Create-A-Style tint: albedo x TEXTUREFACTOR (white when the shader has no tint); each of the
  // three stage-0 states the game has written since is set again
  for (int i = 0; i < 3; ++i) if (!(h.tssOurs & (1u << i))) { h.tssOurs |= (uint8_t) (1u << i); dev->SetTextureStageState(0, kSims3Tss[i], kSims3TssOurs[i]); }
  // the tint as the game's shader reads it: its constant on the device at this draw (milestone 79)
  float tint[4] = { 1.f, 1.f, 1.f, 1.f };
  if (h.psTintReg >= 0) dev->GetPixelShaderConstantF((UINT) h.psTintReg, tint, 1);
  const uint32_t factor = (h.psTintReg >= 0) ? sims3cam::packTint(tint) : 0xFFFFFFFFu;
  if (!h.factorOurs || factor != h.sentFactor) {
    h.sentFactor = factor; h.factorOurs = true;
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, factor);
  }
  if (h.psTintReg >= 0 && h.psTintReg < 32 && !(h.loggedTintRegs & (1u << h.psTintReg))) {
    h.loggedTintRegs |= 1u << h.psTintReg; char msg[224];
    snprintf(msg, sizeof msg, "Sims 3 camera hook: first tint from c%d (PS %016llx) forwarded as texture factor: %08X (from %.3f %.3f %.3f)",
             h.psTintReg, (unsigned long long) h.psHash, factor, tint[0], tint[1], tint[2]);
    Logger::info(msg);
  }
  h.ourState = false;
  // the sky dome: any 2D texture at stage 0 (the runtime drops a draw whose stage-0 texture has
  // no hash, and a cube map has none), and a depth-1 viewport, which makes the draw the sky
  if (h.vsSkyDome) {
    if (k < 0 && (h.boundKind[0] & 0x7F) != 1) for (int s = 1; s < 16; ++s) if ((h.boundKind[s] & 0x7F) == 1 && h.boundTex[s]) { k = s; break; }
    dev->GetViewport(&h.gameViewport);
    D3DVIEWPORT9 vp = h.gameViewport; vp.MinZ = 1.f; vp.MaxZ = 1.f;
    h.ourState = true; dev->SetViewport(&vp); h.ourState = false;
    h.viewportOurs = true; ++h.skyDraws;
    if (h.skyLogged < 3) {
      ++h.skyLogged; char fb[16]; char msg[288];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: sky dome draw presented as the runtime's sky at frame %u -> VS %016llx PS %016llx, viewport depth 1, stage 0 %s (%s %ux%u)", h.frames + 1, (unsigned long long) h.vsHash, (unsigned long long) h.psHash, k > 0 ? "presented from stage" : "as bound", k > 0 ? sims3FormatName(h.boundFmt[k], fb, sizeof fb) : sims3FormatName(h.boundFmt[0], fb, sizeof fb), (unsigned) h.boundW[k > 0 ? k : 0], (unsigned) h.boundH[k > 0 ? k : 0]);
      if (k > 0) { const size_t l = strlen(msg); snprintf(msg + l, sizeof msg - l, " s%d", k); }
      Logger::info(msg);
    }
  }
  if (k > 0) {
    if (!h.loggedRemap) { h.loggedRemap = true; char msg[192]; snprintf(msg, sizeof msg, "Sims 3 camera hook: first captured draw whose albedo is not at stage 0; presenting stage %d as the albedo for such draws", k); Logger::info(msg); }
    h.remapRestore = h.boundTex[0]; if (h.remapRestore) h.remapRestore->AddRef();   // held until sims3EndDraw: the remap drops the state's reference
    h.remapActive = true;
    h.inRemap = true; dev->SetTexture(0, h.boundTex[k]); h.inRemap = false;
    // its sampler states go with it (milestone 68); the game's stage 0 states come back in sims3EndDraw
    h.remapSamplerSet = sims3SamplerStatesTo0(h, dev, (DWORD) k, h.remapSamplerSaved);
    if (h.remapSamplerSet) {
      ++h.remapSamplerDraws; if (h.remapSamplerSet & (1u << 5)) ++h.remapSrgbDraws;
      bool seen = false; for (uint32_t i = 0; i < h.remapSamplerLogged; ++i) if (h.remapSamplerLoggedPs[i] == h.psHash) seen = true;
      if (!seen && h.remapSamplerLogged < 24) {
        h.remapSamplerLoggedPs[h.remapSamplerLogged++] = h.psHash;
        static const char* const kNames[Sims3Hook::kSamplerCopies] = { "addressU", "addressV", "mag", "min", "mip", "sRGB", "anisotropy" };
        char msg[512]; int n = snprintf(msg, sizeof msg, "Sims 3 camera hook: albedo at stage %d keeps its sampler states at stage 0 for VS %016llx PS %016llx:", k, (unsigned long long) h.vsHash, (unsigned long long) h.psHash);
        for (int i = 0; i < Sims3Hook::kSamplerCopies && n > 0 && n < (int) sizeof msg; ++i) {
          if (!(h.remapSamplerSet & (1u << i))) continue;
          DWORD vk = 0; dev->GetSamplerState(0, kSims3SamplerCopy[i], &vk);
          n += snprintf(msg + n, sizeof msg - n, " %s %lu (stage 0 had %lu)", kNames[i], (unsigned long) vk, (unsigned long) h.remapSamplerSaved[i]);
        }
        Logger::info(msg);
      }
    }
  }
  return true;
}

// After the draw's message has been queued: the game's stage-0 texture and vertex shader back,
// the masked-write emulation undone.
template<typename Dev>
void sims3EndDraw(Sims3Hook& h, Dev* dev) {
  if (h.fogOurs) { h.fogOurs = false; h.ourState = true; for (int i = 0; i < 5; ++i) dev->SetRenderState(kSims3FogRs[i], h.fogSaved[i]); h.ourState = false; }
  if (h.viewportOurs) { h.viewportOurs = false; h.ourState = true; dev->SetViewport(&h.gameViewport); h.ourState = false; }
  // a terrain draw's pixel shader variant, moved texture and sampler states (milestone 17)
  if (h.psRestore) {
    IDirect3DPixelShader9* ps = h.psRestore; h.psRestore = nullptr;
    h.swappingPs = true; dev->SetPixelShader(ps); h.swappingPs = false;
    ps->Release();
  }
  if (h.cwOurs) { h.cwOurs = false; h.ourState = true; dev->SetRenderState(D3DRS_COLORWRITEENABLE, h.cwRestore); h.ourState = false; }
  if (h.atOurs) {
    h.atOurs = false; h.ourState = true;
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, h.atSaved[0]); dev->SetRenderState(D3DRS_ALPHAFUNC, h.atSaved[1]); dev->SetRenderState(D3DRS_ALPHAREF, h.atSaved[2]);
    h.ourState = false;
  }
  if (h.blendOurs) { h.blendOurs = false; h.ourState = true; dev->SetRenderState(D3DRS_ALPHABLENDENABLE, h.blendSaved); h.ourState = false; }   // a glass draw's blending (milestone 80)
  if (h.terrainFreeStage >= 0) {
    h.inRemap = true; dev->SetTexture((DWORD) h.terrainFreeStage, h.freeStageRestore); h.inRemap = false;
    if (h.freeStageRestore) { h.freeStageRestore->Release(); h.freeStageRestore = nullptr; }
    h.terrainFreeStage = -1;
  }
  if (h.extraActive) {   // the composite's second pass: stages 1 and 2 back (milestone 17l)
    h.extraActive = false; h.inRemap = true;
    for (int s = 0; s < 2; ++s) { dev->SetTexture((DWORD) (s + 1), h.extraRestore[s]); if (h.extraRestore[s]) { h.extraRestore[s]->Release(); h.extraRestore[s] = nullptr; } }
    h.inRemap = false;
  }
  h.splitDraw = false;
  if (h.remapActive) {
    h.remapActive = false;
    h.inRemap = true; dev->SetTexture(0, h.remapRestore); h.inRemap = false;
    if (h.remapRestore) { h.remapRestore->Release(); h.remapRestore = nullptr; }
  }
  sims3SamplerStatesBack(h, dev, h.remapSamplerSaved, h.remapSamplerSet);   // the game's stage 0 sampler states back (milestone 68)
  h.remapSamplerSet = 0;
  if (h.autoVsRestore) {
    IDirect3DVertexShader9* base = h.autoVsRestore; h.autoVsRestore = nullptr;
    h.swappingVs = true; dev->SetVertexShader(base); h.swappingVs = false;
    base->Release();
  }
  if (h.maskEmu) {
    if (h.maskEmu == 3) sims3RestoreChannels(h, dev);
    h.ourState = true;
    for (int i = 0; i < 10; ++i) if (h.maskEmuBits & (1u << i)) dev->SetRenderState(kSims3MaskRs[i], h.maskSaved[i]);
    h.ourState = false; h.maskEmu = 0;
  }
}

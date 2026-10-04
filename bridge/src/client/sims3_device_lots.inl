// The Sims 3 camera hook, part of d3d9_device.cpp: the low-detail lots (milestones 69-71): the
// model's split into house, plate and glow layer, the window-only glow texture and the draw in
// place of the game's. Included after sims3_device_squares.inl; not a standalone header.

// The low-detail lot model's split (milestone 69): from the client's copies of its buffers, the
// house's triangles and the plate's top, each as an index buffer of the hook's own (the same vertex
// numbers, drawn with the game's base vertex). Kept per content; null when nothing can be split.
template<typename Dev>
Sims3Hook::PlateEntry* sims3LotPlateEntry(Sims3Hook& h, Dev* dev, Direct3DVertexBuffer9_LSS* vb, UINT off, UINT stride, Direct3DIndexBuffer9_LSS* ib,
                                          const D3DVERTEXELEMENT9* decl, INT base, UINT start, UINT prims, Direct3DTexture9_LSS* glowTex) {
  const uint8_t* vd = vb ? vb->sims3Data() : nullptr; const uint8_t* id = ib ? ib->sims3Data() : nullptr;
  if (!vd || !id || !decl || stride == 0) { ++h.plateFailed; return nullptr; }
  int posOff = -1, nrmOff = -1, uvOff = -1, nrmType = -1;
  for (uint32_t i = 0; i < 24 && decl[i].Stream != 0xFF; ++i) {
    if (decl[i].Stream != 0) continue;
    if (decl[i].Usage == D3DDECLUSAGE_TEXCOORD && decl[i].UsageIndex == 0 && decl[i].Type == D3DDECLTYPE_FLOAT2) uvOff = decl[i].Offset;
    if (decl[i].Usage == D3DDECLUSAGE_POSITION && decl[i].UsageIndex == 0 && decl[i].Type == D3DDECLTYPE_FLOAT3) posOff = decl[i].Offset;
    if (decl[i].Usage == D3DDECLUSAGE_NORMAL && decl[i].UsageIndex == 0 && (decl[i].Type == D3DDECLTYPE_D3DCOLOR || decl[i].Type == D3DDECLTYPE_UBYTE4N)) { nrmOff = decl[i].Offset; nrmType = decl[i].Type; }
  }
  if (posOff < 0 || nrmOff < 0) { ++h.plateFailed; return nullptr; }
  const uint32_t vbSize = vb->sims3Size(), ibSize = ib->sims3Size();
  const bool ib32 = ib->getDesc().Format == D3DFMT_INDEX32;
  // the buffers by id and write count: ids come from a counter that never repeats, so the pair names
  // the content without hashing it (some ninety models a frame in the neighbourhood view)
  struct { uint64_t vbId, ibId, glowId; uint32_t vbVersion, ibVersion, glowVersion, off, stride, ib32, posOff, nrmOff; int32_t base; uint32_t start, prims; } k;
  memset(&k, 0, sizeof k);
  k.vbId = (uint64_t) vb->getId(); k.ibId = (uint64_t) ib->getId(); k.vbVersion = vb->sims3Version; k.ibVersion = ib->sims3Version;
  if (glowTex) { k.glowId = (uint64_t) glowTex->getId(); k.glowVersion = glowTex->sims3Level0Version(); }
  k.off = off; k.stride = stride; k.ib32 = ib32 ? 1u : 0u; k.posOff = (uint32_t) posOff; k.nrmOff = (uint32_t) nrmOff; k.base = base; k.start = start; k.prims = prims;
  const uint64_t key = sims3cam::fnv1a64(&k, sizeof k);
  for (auto& e : h.plates) if (e.key == key) { e.lastFrame = h.frames; return e.ok ? &e : nullptr; }
  // a new model: split it (the oldest entry makes room past 256)
  if (h.plates.size() >= 256) {
    size_t oldest = 0; for (size_t i = 1; i < h.plates.size(); ++i) if (h.plates[i].lastFrame < h.plates[oldest].lastFrame) oldest = i;
    if (h.plates[oldest].house) h.plates[oldest].house->Release();
    if (h.plates[oldest].plate) h.plates[oldest].plate->Release();
    if (h.plates[oldest].glow) h.plates[oldest].glow->Release();
    if (h.plates[oldest].glowVb) h.plates[oldest].glowVb->Release();
    h.plates.erase(h.plates.begin() + (ptrdiff_t) oldest);
  }
  h.plates.emplace_back(); Sims3Hook::PlateEntry& e = h.plates.back(); e.key = key; e.lastFrame = h.frames;
  ++h.plateBuilds;
  const uint32_t isz = ib32 ? 4u : 2u, ibCount = ibSize / isz;
  std::vector<uint32_t> idx; idx.reserve((size_t) prims * 3u);
  uint32_t lo = 0xFFFFFFFFu, hi = 0;
  for (uint32_t t = 0; t < prims * 3u && start + t < ibCount; ++t) {
    uint32_t i;
    i = sims3cam::readIndex(id, (size_t) start + t, ib32);
    idx.push_back(i); if (i < lo) lo = i; if (i > hi) hi = i;
  }
  if (idx.size() < 3 || lo > hi) { ++h.plateFailed; return nullptr; }
  // the vertices the range reaches, as numbers from lo: position and class
  const uint32_t count = hi - lo + 1;
  std::vector<float> pos((size_t) count * 3u), uv((size_t) count * 2u); std::vector<uint8_t> cls(count);
  for (uint32_t v = 0; v < count; ++v) {
    const int64_t vtx = (int64_t) base + (int64_t) lo + (int64_t) v;
    const uint64_t at = (uint64_t) off + (uint64_t) (vtx < 0 ? 0 : vtx) * stride;
    if (vtx < 0 || at + (uint64_t) nrmOff + 4u > vbSize || at + (uint64_t) posOff + 12u > vbSize) { cls[v] = 0; continue; }
    memcpy(&pos[(size_t) v * 3u], vd + at + posOff, 12);
    cls[v] = vd[at + nrmOff + 3];
    if (uvOff >= 0 && at + (uint64_t) uvOff + 8u <= vbSize) memcpy(&uv[(size_t) v * 2u], vd + at + uvOff, 8);
  }
  std::vector<uint32_t> rel(idx.size()); for (size_t i = 0; i < idx.size(); ++i) rel[i] = idx[i] - lo;
  std::vector<uint32_t> house, plate; sims3cam::PlateSplitStats st;
  sims3cam::splitLotPlate(pos, cls, rel, house, plate, st);
  if (plate.empty()) ++h.plateNone;   // nothing to split: the model drawn as the game draws it
  // the windows that glow (milestone 70): the house's triangles over a bright texel of the glow atlas
  std::vector<uint32_t> glow;
  if (glowTex && uvOff >= 0) {
    const uint8_t* gd = glowTex->sims3Level0Data();
    const D3DSURFACE_DESC gdesc = glowTex->getLevelDesc(0);
    std::vector<uint8_t> level;
    if (gd && sims3cam::decodeMaxChannel((uint32_t) gdesc.Format, gd, bridge_util::calcTotalSizeOfRect(gdesc.Width, gdesc.Height, gdesc.Format), gdesc.Width, gdesc.Height, level))
      sims3cam::selectGlowTriangles(uv, house, level, gdesc.Width, gdesc.Height, sims3cam::kLotGlowThreshold, glow);
    if (!glow.empty()) ++h.glowModels;
  }
  if (plate.empty() && glow.empty()) return nullptr;
  auto make = [&](std::vector<uint32_t>& list, IDirect3DIndexBuffer9*& out, uint32_t& mn, uint32_t& num, uint32_t& np) -> bool {
    if (list.empty()) { np = 0; return true; }
    uint32_t a = 0xFFFFFFFFu, b = 0;
    for (uint32_t& v : list) { v += lo; if (v < a) a = v; if (v > b) b = v; }
    out = sims3MakeIndexBuffer(dev, list);
    if (!out) return false;
    mn = a; num = b - a + 1; np = (uint32_t) (list.size() / 3);
    return true;
  };
  if (!make(house, e.house, e.houseMin, e.houseNum, e.housePrims) || !make(plate, e.plate, e.plateMin, e.plateNum, e.platePrims)) { ++h.plateFailed; return nullptr; }
  // the glow layer (milestone 71): its triangles as vertices of their own, each moved kLotGlowLift
  // along its face normal (turned to the side of the vertex normal), drawn from 0 with indices 0..n-1
  if (!glow.empty()) {
    std::vector<uint8_t> bytes(glow.size() * (size_t) stride);
    std::vector<uint32_t> seq(glow.size());
    bool ok = true;
    for (size_t t = 0; t + 2 < glow.size() && ok; t += 3) {
      float p[9]; float vn[3] = { 0.f, 0.f, 0.f };
      for (int k = 0; k < 3; ++k) {
        const uint32_t v = glow[t + k];
        const uint64_t at = (uint64_t) off + (uint64_t) ((int64_t) base + (int64_t) lo + (int64_t) v) * stride;
        if (at + stride > vbSize) { ok = false; break; }
        memcpy(&bytes[(t + k) * stride], vd + at, stride);
        memcpy(&p[3 * k], vd + at + posOff, 12);
        const uint8_t* q = vd + at + nrmOff;
        const float nx = (nrmType == D3DDECLTYPE_D3DCOLOR ? q[2] : q[0]) / 127.5f - 1.f, ny = q[1] / 127.5f - 1.f, nz = (nrmType == D3DDECLTYPE_D3DCOLOR ? q[0] : q[2]) / 127.5f - 1.f;
        vn[0] += nx; vn[1] += ny; vn[2] += nz;
        seq[t + k] = (uint32_t) (t + k);
      }
      if (!ok) break;
      sims3cam::liftTriangle(p, vn, sims3cam::kLotGlowLift);
      for (int k = 0; k < 3; ++k) memcpy(&bytes[(t + k) * stride + posOff], &p[3 * k], 12);
    }
    if (ok) { e.glowVb = sims3MakeVertexBuffer(dev, bytes); e.glow = e.glowVb ? sims3MakeIndexBuffer(dev, seq) : nullptr; }
    if (e.glowVb && e.glow) { e.glowMin = 0; e.glowNum = (uint32_t) seq.size(); e.glowPrims = (uint32_t) (seq.size() / 3); e.glowStride = stride; }
    else { if (e.glowVb) { e.glowVb->Release(); e.glowVb = nullptr; } if (e.glow) { e.glow->Release(); e.glow = nullptr; } e.glowPrims = 0; }
  }
  e.ok = true;
  return &e;
}

// A low-detail lot's glow atlas with only its windows lit, one full-size level (milestone 71); kept per
// atlas (id and write count); null when it cannot be read or made (the game's atlas is used then).
template<typename Dev>
IDirect3DTexture9* sims3LotGlowTexture(Sims3Hook& h, Dev* dev, Direct3DTexture9_LSS* src) {
  if (!src) return nullptr;
  const uint64_t key = ((uint64_t) src->getId() << 32) ^ (uint64_t) src->sims3Level0Version();
  for (auto& g : h.glowTexs) if (g.key == key) { g.lastFrame = h.frames; return g.tex; }
  if (h.glowTexs.size() >= 256) {
    size_t oldest = 0; for (size_t i = 1; i < h.glowTexs.size(); ++i) if (h.glowTexs[i].lastFrame < h.glowTexs[oldest].lastFrame) oldest = i;
    if (h.glowTexs[oldest].tex) h.glowTexs[oldest].tex->Release();
    h.glowTexs.erase(h.glowTexs.begin() + (ptrdiff_t) oldest);
  }
  h.glowTexs.emplace_back(); Sims3Hook::GlowTex& g = h.glowTexs.back(); g.key = key; g.lastFrame = h.frames;
  const uint8_t* data = src->sims3Level0Data();
  const D3DSURFACE_DESC desc = src->getLevelDesc(0);
  std::vector<uint32_t> argb;
  if (!data || !sims3cam::decodeColour((uint32_t) desc.Format, data, bridge_util::calcTotalSizeOfRect(desc.Width, desc.Height, desc.Format), desc.Width, desc.Height, argb)) { ++h.glowTexFailed; return nullptr; }
  sims3cam::windowOnlyGlow(argb, sims3cam::kLotGlowThreshold);
  IDirect3DTexture9* tex = sims3MakeTexture(dev, argb.data(), desc.Width, desc.Height);
  if (!tex) { ++h.glowTexFailed; return nullptr; }
  g.tex = tex; ++h.glowTexMade;
  return tex;
}

// A low-detail lot model's draw (milestones 69-71), in place of the game's: its house (with the
// plate's sides) from the house index buffer; the plate's top as terrain -- the visible terrain marker
// at stage 0 and the hook's plate shader, no alpha test; and, while the game's switch c3.x is above 0,
// its glowing windows once more as light -- the window-only glow texture at stage 0 with s3's sampler
// states, blended ONE / ONE, the texture factor the switch, from the glow layer's own vertices 5 cm out
// from the wall. The game's bindings come back afterwards. False when the hook has nothing to change
// (the game's draw then goes out as it is); uid is the last message's.
template<typename Dev, typename St>
bool sims3LotModelDraw(Sims3Hook& h, Dev* dev, const St& st, INT base, UINT minIndex, UINT numVertices, UINT start, UINT prims, UID& uid) {
  if (!*st.streams[0] || !*st.indices || !*st.vertexDecl) return false;
  auto* vb0 = bridge_cast<Direct3DVertexBuffer9_LSS*>(*st.streams[0]);
  auto* ib = bridge_cast<Direct3DIndexBuffer9_LSS*>(*st.indices);
  auto* decl = bridge_cast<Direct3DVertexDeclaration9_LSS*>(*st.vertexDecl);
  const int gs = sims3cam::kLotGlowStage;
  Direct3DTexture9_LSS* glowTex = (h.psHash == sims3cam::kLotImpostorPs && h.boundTex[gs] && (h.boundKind[gs] & 0x7F) == 1) ? bridge_cast<Direct3DTexture9_LSS*>(h.boundTex[gs]) : nullptr;
  Sims3Hook::PlateEntry* e = sims3LotPlateEntry(h, dev, vb0, st.streamOffsets[0], st.streamStrides[0], ib, decl ? decl->sims3Elements() : nullptr, base, start, prims, glowTex);
  if (!e) return false;
  const bool wantSplit = e->platePrims != 0;
  if (wantSplit && !h.platePs && !h.platePsFailed) {
    IDirect3DPixelShader9* ps = nullptr;
    HRESULT hr;
    { sims3cam::OwnCall ownCall(h.calls); hr = dev->CreatePixelShader(sims3cam::kLotPlatePs, &ps); }
    if (FAILED(hr) || !ps) { h.platePsFailed = true; Logger::info("Sims 3 camera hook: the plate's pixel shader could not be created; low-detail lots drawn as one object"); }
    else h.platePs = ps;
  }
  const bool split = wantSplit && h.platePs && sims3EnsureMarkers(h, dev);
  const float glowScale = st.pixelConstants.fConsts[sims3cam::kLotGlowScaleReg].data[0];   // the game's switch, c3.x
  const bool glowOn = glowTex && e->glowPrims && glowScale > 0.f;
  if (!split && !glowOn) return false;
  auto send = [&](INT b, UINT mn, UINT nv, UINT s, UINT np) {
    ClientMessage c(Commands::IDirect3DDevice9Ex_DrawIndexedPrimitive, dev->getId());
    uid = c.get_uid();
    const D3DPRIMITIVETYPE type = D3DPT_TRIANGLELIST;
    c.send_many(type, b, mn, nv, s, np);
  };
  IDirect3DIndexBuffer9* gib = (IDirect3DIndexBuffer9*) ib; gib->AddRef();
  if (!split) send(base, minIndex, numVertices, start, prims);   // the model as the game draws it, then its glow
  if (split) {
    if (e->housePrims) { dev->SetIndices(e->house); send(base, e->houseMin, e->houseNum, 0u, e->housePrims); }
    const sims3cam::HookCalls::Scope scope = h.calls.open();   // the plate's states, put back right after it
    h.calls.holdTexture(dev, 0, h.marker[0]); h.calls.holdPs(dev, h.platePs); h.calls.holdRs(dev, D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetIndices(e->plate);
    send(base, e->plateMin, e->plateNum, 0u, e->platePrims);
    h.calls.close(dev, scope);
    ++h.plateDraws; h.plateTriangles += e->platePrims;
  }
  if (glowOn) {
    const sims3cam::HookCalls::Scope scope = h.calls.open();   // the glow's states, put back right after it
    IDirect3DTexture9* own = sims3LotGlowTexture(h, dev, glowTex);   // the windows only, one level (milestone 71)
    h.calls.holdTexture(dev, 0, own ? (IDirect3DBaseTexture9*) own : h.boundTex[gs]);
    sims3SamplerStatesTo0(h, dev, (DWORD) gs);
    const float g = glowScale > 1.f ? 1.f : glowScale;
    const DWORD grey = (DWORD) (g * 255.f + 0.5f);
    static constexpr D3DRENDERSTATETYPE kGlowRs[7] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_ALPHATESTENABLE, D3DRS_ZWRITEENABLE, D3DRS_TEXTUREFACTOR };
    const DWORD ours[7] = { TRUE, D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_ADD, FALSE, FALSE, 0xFF000000u | (grey << 16) | (grey << 8) | grey };
    for (int i = 0; i < 7; ++i) h.calls.holdRs(dev, kGlowRs[i], ours[i]);
    IDirect3DVertexBuffer9* gvb0 = (IDirect3DVertexBuffer9*) vb0; gvb0->AddRef();
    const UINT off0 = st.streamOffsets[0], st0 = st.streamStrides[0];
    dev->SetStreamSource(0, e->glowVb, 0, e->glowStride);
    dev->SetIndices(e->glow);
    send(0, e->glowMin, e->glowNum, 0u, e->glowPrims);
    dev->SetStreamSource(0, gvb0, off0, st0); gvb0->Release();
    h.calls.close(dev, scope);
    ++h.glowDraws; h.glowTriangles += e->glowPrims;
  }
  dev->SetIndices(gib); gib->Release();
  return true;
}

// The Sims 3 camera hook, part of d3d9_device.cpp: how a draw of the game goes to the runtime (milestone
// 153: one way out). Included after sims3_device_lots.inl; not a standalone header.

// A captured hardware-instanced draw (milestone 12) goes to the runtime once per instance (see
// Sims3Hook::drawCaptured), the instance streams' offsets stepping through their data and stream 0 at
// one instance for the duration; the game's settings come back afterwards. False when it is not one.
template<typename Dev, typename St, typename Send>
bool sims3SendInstances(Sims3Hook& h, Dev* dev, const St& st, Send&& send) {
  if (!(st.streamFreqs[0] & D3DSTREAMSOURCE_INDEXEDDATA)) return false;
  const uint32_t instances = st.streamFreqs[0] & 0x3FFFFFFFu; uint32_t instStreams = 0;
  for (uint32_t s = 1; s < caps::MaxStreams; ++s)
    if ((st.streamFreqs[s] & D3DSTREAMSOURCE_INSTANCEDATA) && *st.streams[s] != nullptr && st.streamStrides[s] > 0) instStreams |= 1u << s;
  if (instances <= 1 || instances > 1024 || instStreams == 0) return false;
  const UINT freq0 = st.streamFreqs[0];
  IDirect3DVertexBuffer9* vb[caps::MaxStreams] = {}; UINT off[caps::MaxStreams] = {}, stride[caps::MaxStreams] = {};
  for (uint32_t s = 1; s < caps::MaxStreams; ++s) if (instStreams & (1u << s)) {
    vb[s] = (IDirect3DVertexBuffer9*) bridge_cast<Direct3DVertexBuffer9_LSS*>(*st.streams[s]);
    vb[s]->AddRef();   // held across the re-bindings below (each one drops the state's reference first)
    off[s] = st.streamOffsets[s]; stride[s] = st.streamStrides[s];
  }
  // The runtime folds draws of one frame with the same material, geometry and vertex-shader hash
  // into one object, and the vertex-shader hash of a captured draw covers the bytecode AND the float
  // constants the shader can reach (d3d9_rtx_geometry.cpp). A per-instance value in c255 (never
  // uploaded by the game; the variant bound for the draw reads it, see appendConstantRead) keeps
  // each instance its own object.
  float savedTag[4]; memcpy(savedTag, &st.vertexConstants.fConsts[255], sizeof savedTag);
  dev->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 1u);
  for (uint32_t i = 0; i < instances; ++i) {
    for (uint32_t s = 1; s < caps::MaxStreams; ++s) if (instStreams & (1u << s)) dev->SetStreamSource(s, vb[s], off[s] + i * stride[s], stride[s]);
    const float tag[4] = { (float) (i + 1), 0.f, 0.f, 0.f };
    dev->SetVertexShaderConstantF(255, tag, 1);
    send();
  }
  dev->SetVertexShaderConstantF(255, savedTag, 1);
  for (uint32_t s = 1; s < caps::MaxStreams; ++s) if (instStreams & (1u << s)) { dev->SetStreamSource(s, vb[s], off[s], stride[s]); vb[s]->Release(); }
  dev->SetStreamSourceFreq(0, freq0);
  ++h.deinstancedDraws; h.deinstancedInstances += instances;
  return true;
}

// A captured wall draw gets its window and door openings cut into the geometry (milestone 13,
// sims3_walls.h). The pixel shader's mask test is evaluated on the client from the buffers' shadow
// copies and the mask atlas, and the cut triangles are drawn from the hook's own buffers; the game's
// bindings come back afterwards. A draw whose opening test cannot be evaluated -- a pixel shader the
// analyser does not know, walls C without its alpha test, a mask not readable on the client -- still
// loses the triangles its vertex shader hides, which never depended on the mask (milestone 77). False
// when it is not a wall draw or nothing of it changes (the game's draw then goes out as it is).
template<typename Dev, typename St>
bool sims3SendWallCut(Sims3Hook& h, Dev* dev, const St& st, D3DPRIMITIVETYPE type, INT base, UINT start, UINT prims, UID& uid) {
  if (type != D3DPT_TRIANGLELIST || !h.wallLayout.valid || !h.vsWall || !h.vsWall->valid
      || *st.streams[0] == nullptr || *st.streams[1] == nullptr || *st.indices == nullptr) return false;
  const sims3cam::PsAnalysis* ps = (h.psAuto && h.psAuto->valid && h.psAuto->maskSampler >= 0 && h.psAuto->maskSampler < 16) ? h.psAuto : nullptr;
  const DWORD* rs = st.renderStates.data();
  // the discard threshold: texkill at 0.5; walls C alpha-tests mask + z - 0.5 against the
  // reference; below 0: no opening test
  float thr = -1.f;
  if (ps && ps->maskKill) thr = 0.5f;
  else if (ps && ps->maskAlpha && rs[D3DRS_ALPHATESTENABLE] && (rs[D3DRS_ALPHAFUNC] == D3DCMP_GREATEREQUAL || rs[D3DRS_ALPHAFUNC] == D3DCMP_GREATER)) thr = 0.5f + (float) (rs[D3DRS_ALPHAREF] & 0xFFu) / 255.f;
  auto* vb0 = bridge_cast<Direct3DVertexBuffer9_LSS*>(*st.streams[0]);
  auto* vb1 = bridge_cast<Direct3DVertexBuffer9_LSS*>(*st.streams[1]);
  auto* ib = bridge_cast<Direct3DIndexBuffer9_LSS*>(*st.indices);
  const uint8_t* d0 = vb0->sims3Data(); const uint8_t* d1 = vb1->sims3Data(); const uint8_t* di = ib->sims3Data();
  if (!d0 || !d1 || !di) { ++h.wallSkipped; return false; }
  // the mask: the texture at the pixel shader's mask sampler (none bound: black everywhere)
  const int s = ps ? ps->maskSampler : -1;
  uint32_t maskId = 0, maskVer = 0, maskW = 0, maskH = 0, maskFmt = 0; uint64_t maskHash = 0; const uint8_t* mask = nullptr;
  if (thr >= 0.f && h.boundTex[s] != nullptr) {
    if ((h.boundKind[s] & 0x7F) == 1) {
      auto* tex = bridge_cast<Direct3DTexture9_LSS*>(h.boundTex[s]);
      const D3DSURFACE_DESC d = tex->getLevelDesc(0);
      maskId = (uint32_t) tex->getId(); maskVer = tex->sims3Level0Version(); maskW = d.Width; maskH = d.Height; maskFmt = (uint32_t) d.Format;
      mask = sims3WallMask(h, maskId, maskVer, maskFmt, d.Width, d.Height, tex->sims3Level0Data(), maskHash);
    }
    if (!mask) {
      thr = -1.f;
      if (h.wallSkipLogged < 4) {
        ++h.wallSkipLogged; char fb[16]; char m[288];
        snprintf(m, sizeof m, "Sims 3 camera hook: wall draw's openings not cut at frame %u -> the mask at stage %d (%s %ux%u) is not readable on the client (its hidden triangles still go), VS %016llx PS %016llx", h.frames + 1, s, sims3FormatName(maskFmt, fb, sizeof fb), maskW, maskH, (unsigned long long) h.vsHash, (unsigned long long) h.psHash);
        Logger::info(m);
      }
    }
  }
  if (thr < 0.f) { maskW = maskH = 0; maskHash = 0; ++h.wallNoOpeningTest; }
  sims3cam::WallCutInput in;
  in.layout = h.wallLayout;
  in.vb0 = d0; in.vb0Size = vb0->sims3Size(); in.offset0 = st.streamOffsets[0]; in.stride0 = st.streamStrides[0];
  in.vb1 = d1; in.vb1Size = vb1->sims3Size(); in.offset1 = st.streamOffsets[1]; in.stride1 = st.streamStrides[1];
  in.ib = di; in.ibSize = ib->sims3Size(); in.ib32 = ib->getDesc().Format == D3DFMT_INDEX32;
  in.baseVertex = base; in.startIndex = start; in.primCount = prims;
  in.mask = mask; in.maskW = maskW; in.maskH = maskH;
  float ck[4]; memcpy(ck, &st.vertexConstants.fConsts[h.vsWall->clampReg], sizeof ck);
  in.params.clampLo = ck[0]; in.params.clampHi = ck[1]; in.params.clampVis = ck[2]; in.params.kScale = h.vsWall->kScale; in.params.threshold = thr;
  // the key follows the buffers' and the mask's CONTENT (hashed once per upload), the draw range, the
  // declaration and the constants the cut depends on
  struct { uint64_t vb0Hash, vb1Hash, ibHash, maskHash; uint32_t off0, st0, off1, st1, ib32, declId; int32_t base; uint32_t start, prims; float lo, hi, vis, kScale, thr; } k;
  memset(&k, 0, sizeof k);
  k.vb0Hash = sims3ContentHash(h, (uint32_t) vb0->getId(), vb0->sims3Version, d0, in.vb0Size);
  k.vb1Hash = sims3ContentHash(h, (uint32_t) vb1->getId(), vb1->sims3Version, d1, in.vb1Size);
  k.ibHash = sims3ContentHash(h, (uint32_t) ib->getId(), ib->sims3Version, di, in.ibSize);
  k.maskHash = maskHash; k.off0 = in.offset0; k.st0 = in.stride0; k.off1 = in.offset1; k.st1 = in.stride1; k.ib32 = in.ib32 ? 1u : 0u; k.declId = h.wallDeclId;
  k.base = base; k.start = start; k.prims = prims; k.lo = ck[0]; k.hi = ck[1]; k.vis = ck[2]; k.kScale = in.params.kScale; k.thr = thr;
  const uint64_t key = sims3cam::fnv1a64(&k, sizeof k);
  sims3cam::WallCutStats cutStats; bool built = false;
  Sims3Hook::WallEntry* e = sims3WallEntry(h, dev, key, in, cutStats, built);
  ++h.wallDraws;
  if (!e || !e->changed) return false;
  ++h.wallCutDraws;
  // the game's buffers are held by a reference of the hook's own while its own are bound
  IDirect3DVertexBuffer9* gvb0 = (IDirect3DVertexBuffer9*) vb0; IDirect3DVertexBuffer9* gvb1 = (IDirect3DVertexBuffer9*) vb1; IDirect3DIndexBuffer9* gib = (IDirect3DIndexBuffer9*) ib;
  gvb0->AddRef(); gvb1->AddRef(); gib->AddRef();
  const UINT off0 = st.streamOffsets[0], off1 = st.streamOffsets[1], st0 = st.streamStrides[0], st1 = st.streamStrides[1];
  dev->SetStreamSource(0, e->vb0, 0, st0); dev->SetStreamSource(1, e->vb1, 0, st1); dev->SetIndices(e->ib);
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_DrawIndexedPrimitive, dev->getId());
    uid = c.get_uid();
    const INT base0 = 0; const UINT min0 = 0, start0 = 0, nv = e->vertexCount, np = e->triangleCount;
    c.send_many(type, base0, min0, nv, start0, np);
  }
  dev->SetStreamSource(0, gvb0, off0, st0); dev->SetStreamSource(1, gvb1, off1, st1); dev->SetIndices(gib);
  gvb0->Release(); gvb1->Release(); gib->Release();
  return true;
}

// The one way out of the hook for a draw of the game (milestone 153; before, the draw members chose
// among the ways in a chain of their own). The first that applies, in this order: a captured instanced
// draw once per instance; a captured wall draw with its openings cut; a captured low-detail lot model
// in its parts (milestones 69-71, sims3LotModelDraw); a glass sheet without its back side (milestone
// 104, h.glassIb from sims3BeginDraw); a SpeedTree draw once per group of plants cut alike (milestone
// 139, h.fadeSplit from sims3BeginDraw); a lot's hidden re-submission as two halves (milestone 17r,
// h.splitDraw from sims3BeginDraw). Every device call here is the hook's own; the game's bindings come
// back after each. True when the hook sent the draw; otherwise the draw member sends it as the game
// made it. uid: the last message's.
template<typename Dev, typename St>
bool sims3SendDraw(Sims3Hook& h, Dev* dev, const St& st, bool indexed, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT numVertices, UINT start, UINT prims, UID& uid) {
  sims3cam::OwnCall ownCall(h.calls);
  auto send = [&](UINT s, UINT np) {   // the game's draw call, over (part of) its range
    if (indexed) { ClientMessage c(Commands::IDirect3DDevice9Ex_DrawIndexedPrimitive, dev->getId()); uid = c.get_uid(); c.send_many(type, base, minIndex, numVertices, s, np); }
    else { ClientMessage c(Commands::IDirect3DDevice9Ex_DrawPrimitive, dev->getId()); uid = c.get_uid(); c.send_many(type, s, np); }
  };
  if (indexed && h.drawCaptured && sims3SendInstances(h, dev, st, [&]() { send(start, prims); })) return true;
  if (indexed && h.drawCaptured && sims3SendWallCut(h, dev, st, type, base, start, prims, uid)) return true;
  if (indexed && h.drawCaptured && h.vsHash == sims3cam::kLotImpostorVs && type == D3DPT_TRIANGLELIST
      && sims3LotModelDraw(h, dev, st, base, minIndex, numVertices, start, prims, uid)) return true;
  if (indexed && h.glassIb && type == D3DPT_TRIANGLELIST) {
    // the kept triangles from the hook's own index buffer; the game's index buffer back afterwards
    IDirect3DIndexBuffer9* gib = (IDirect3DIndexBuffer9*) bridge_cast<Direct3DIndexBuffer9_LSS*>(*st.indices);
    if (gib) gib->AddRef();
    dev->SetIndices(h.glassIb);
    send(0u, h.glassPrims);
    dev->SetIndices(gib); if (gib) gib->Release();
    h.glassIb = nullptr;
    return true;
  }
  if (indexed && h.fadeSplit) {
    // the other plants at scale 0 (collapsed onto their own place), the group's reference; the game's
    // constants come back afterwards (the reference with the rest of its alpha test, sims3EndDraw)
    const UINT regs = h.fadePlants * 3u;
    float saved[sims3cam::FadeGroups::kPlants * 3 * 4], block[sims3cam::FadeGroups::kPlants * 3 * 4];
    memcpy(saved, &st.vertexConstants.fConsts[0], regs * 4 * sizeof(float));
    for (uint32_t g = 0; g < h.fadeG.count; ++g) {
      memcpy(block, saved, regs * 4 * sizeof(float));
      for (uint32_t i = 0; i < h.fadePlants; ++i) if (!(h.fadeG.members[g] & (1u << i))) block[(h.fadeReg + 3u * i) * 4u] = 0.f;
      dev->SetVertexShaderConstantF(0, block, regs);
      sims3SetAlphaTest(h, dev, h.fadeG.func, h.fadeG.ref[g]);
      send(start, prims);
    }
    dev->SetVertexShaderConstantF(0, saved, regs);
    h.fadeSplit = false; ++h.fadeSplitDraws; h.fadeSplitParts += h.fadeG.count;
    return true;
  }
  if (h.splitDraw && type == D3DPT_TRIANGLELIST && prims >= 2) {
    // each half its own geometry to the runtime's draw tracker; together they bake the same triangles
    const UINT half = prims / 2;
    send(start, half); send(start + half * 3, prims - half);
    ++h.splitDraws;
    return true;
  }
  return false;
}

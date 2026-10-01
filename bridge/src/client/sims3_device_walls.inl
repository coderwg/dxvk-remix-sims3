// The Sims 3 camera hook, part of d3d9_device.cpp: the wall openings (milestone 13): the mask atlas
// decoded, the content hashes and the cut geometry's cache. Included after sims3_device_masks.inl;
// not a standalone header.

// ---- wall openings (milestone 13) ----------------------------------------------------------
// The mask atlas decoded to one byte per texel, cached per (texture, upload). Null when the
// texture's level 0 was never locked on the client or its format is not one the decoder reads.
inline const uint8_t* sims3WallMask(Sims3Hook& h, uint32_t texId, uint32_t version, uint32_t fmt, uint32_t w, uint32_t hgt, const uint8_t* data, uint64_t& contentHash) {
  for (auto& m : h.masks) if (m.used && m.texId == texId && m.version == version) { contentHash = m.contentHash; return m.ok ? m.red.data() : nullptr; }
  Sims3Hook::MaskEntry& m = h.masks[h.maskNext]; h.maskNext = (h.maskNext + 1) % 4;
  m.used = true; m.texId = texId; m.version = version; m.red.clear(); m.contentHash = 0;
  m.ok = data != nullptr && sims3cam::decodeMaskRed(fmt, data, sims3cam::maskBytes(fmt, w, hgt), w, hgt, m.red);
  if (m.ok) { ++h.wallMasksDecoded; m.contentHash = sims3cam::fnv1a64(m.red.data(), m.red.size()) ^ ((uint64_t) w << 48) ^ ((uint64_t) hgt << 32); }
  contentHash = m.contentHash;
  return m.ok ? m.red.data() : nullptr;
}

// The content hash of a buffer upload, computed once per (object, upload): the wall cut's cache
// key follows the bytes, so an identical re-upload rebuilds nothing.
inline uint64_t sims3ContentHash(Sims3Hook& h, uint32_t id, uint32_t version, const uint8_t* data, size_t size) {
  for (auto& e : h.bufHashes) if (e.used && e.id == id && e.version == version) return e.hash;
  Sims3Hook::HashEntry& e = h.bufHashes[h.bufHashNext]; h.bufHashNext = (h.bufHashNext + 1) % 64;
  e.used = true; e.id = id; e.version = version; e.hash = sims3cam::fnv1a64(data, size) ^ ((uint64_t) size << 40);
  return e.hash;
}

inline void sims3ReleaseWallEntry(Sims3Hook::WallEntry& e) {
  if (e.vb0) e.vb0->Release();
  if (e.vb1) e.vb1->Release();
  if (e.ib) e.ib->Release();
  e = Sims3Hook::WallEntry();
}

// The cut geometry of a wall draw: from the cache by key, else built now (its buffers created
// on the device). An entry with changed == false says the draw has no opening to cut.
template<typename Dev>
Sims3Hook::WallEntry* sims3WallEntry(Sims3Hook& h, Dev* dev, uint64_t key, const sims3cam::WallCutInput& in, sims3cam::WallCutStats& statsOut, bool& builtOut) {
  builtOut = false;
  for (uint32_t i = 0; i < h.wallCacheCount; ++i) if (h.wallCache[i].key == key) { h.wallCache[i].lastFrame = h.frames; return &h.wallCache[i]; }
  sims3cam::WallCutOutput res;
  const bool ok = sims3cam::cutWallOpenings(in, res);
  statsOut = res.stats; builtOut = true;
  Sims3Hook::WallEntry e = {};
  e.key = key; e.lastFrame = h.frames; e.changed = ok && res.changed;
  ++h.wallBuilt;
  if (e.changed) {
    h.wallCutTriangles += res.stats.cut; h.wallRemovedTriangles += res.stats.removed; h.wallHiddenTriangles += res.stats.hidden;
    e.vb0 = sims3MakeVertexBuffer(dev, res.vb0); e.vb1 = sims3MakeVertexBuffer(dev, res.vb1); e.ib = sims3MakeIndexBuffer(dev, res.ib);
    e.vertexCount = res.vertexCount; e.triangleCount = res.triangleCount;
    if (!e.vb0 || !e.vb1 || !e.ib) { sims3ReleaseWallEntry(e); e.key = key; e.lastFrame = h.frames; ++h.wallBuildFailed; }
  }
  uint32_t slot = h.wallCacheCount;
  if (slot >= Sims3Hook::kWallCacheSize) {
    slot = 0;
    for (uint32_t i = 1; i < Sims3Hook::kWallCacheSize; ++i) if (h.wallCache[i].lastFrame < h.wallCache[slot].lastFrame) slot = i;
    sims3ReleaseWallEntry(h.wallCache[slot]); ++h.wallEvicted;
  } else {
    ++h.wallCacheCount;
  }
  h.wallCache[slot] = e;
  return &h.wallCache[slot];
}

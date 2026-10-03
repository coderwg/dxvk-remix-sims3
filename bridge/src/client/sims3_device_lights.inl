// The Sims 3 camera hook, part of d3d9_device.cpp: the lamp reporter's block and the game's light
// record (searched for on threads of their own), the Remix API lights, the lamps and the lights of
// the sky. Included at file scope, before sims3_device_present.inl; not a standalone header.

// The Sims 3 camera hook (milestone 36): the lamp reporter's block. A script mod allocates it,
// marks its head and rewrites the lamps near the camera a few times a second; it sits somewhere
// in the process's private memory. It is searched for on a thread of its own (the search reads
// through the whole process, a second's work) and read every frame; every read of it is
// guarded, the memory being the game's.
static const uint8_t* sims3LampScanRegion(const uint8_t* base, size_t size) {
  __try {
    if (size < 64) return nullptr;
    const uint32_t* q = (const uint32_t*) base;
    const uint32_t* end = (const uint32_t*) (base + (size & ~(size_t) 3)) - 16;
    for (; q <= end; ++q)
      if (q[0] == sims3cam::kLampMagic0 && q[1] == sims3cam::kLampMagic1 && q[2] == sims3cam::kLampMagic2 && q[3] == (uint32_t) (uintptr_t) q) return (const uint8_t*) q;
  } __except (EXCEPTION_EXECUTE_HANDLER) { }
  return nullptr;
}
static void sims3LampScan() {
  // every committed region that can be written, whatever its kind or size (milestone 38: the game's
  // own allocator need not hand out plain private read-write pages)
  const uint8_t* a = (const uint8_t*) 0x10000; const uint8_t* found = nullptr;
  MEMORY_BASIC_INFORMATION mbi;
  uint32_t regions = 0; uint64_t bytes = 0;
  while (!found && VirtualQuery(a, &mbi, sizeof mbi) == sizeof mbi) {
    const DWORD prot = mbi.Protect & 0xFF;
    const bool writable = prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_WRITECOPY;
    if (mbi.State == MEM_COMMIT && writable && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
      ++regions; bytes += mbi.RegionSize;
      found = sims3LampScanRegion((const uint8_t*) mbi.BaseAddress, mbi.RegionSize);
    }
    const uint8_t* next = (const uint8_t*) mbi.BaseAddress + mbi.RegionSize;
    if (next <= a) break;
    a = next;
  }
  g_sims3LampScanRegions = regions; g_sims3LampScanMegabytes = (uint32_t) (bytes >> 20);
  if (found) g_sims3LampBlock = found;
  ++g_sims3LampScans;
  g_sims3LampScanBusy = false;
}
// The search for the game's own light in its memory (milestones 48, 49). Every committed writable
// region of the process is searched for the game's light record holding the direction and colour
// the terrain was handed (within 0.02: the light moves while the search runs), except the hook's
// own state and the bridge's shared memory (mapped regions: the command queue to the server holds
// copies of every upload). What is found is then confirmed against the terrain (in Present).
static void sims3LightScanRegion(const uint8_t* base, size_t size, uint32_t& count) {
  __try {
    if (size < 16) return;
    const float* q = (const float*) base;
    const float* end = (const float*) (base + (size & ~(size_t) 3)) - 8;
    const float* t = g_sims3LightTarget;
    for (; q <= end; ++q) {
      if (!sims3cam::lightRecord(q, t, t + 3, 0.02f)) continue;
      const uint8_t* at = (const uint8_t*) q;
      bool own = false;
      for (int k = 0; k < 2; ++k) if (at >= g_sims3LightExcludeFrom[k] && at < g_sims3LightExcludeTo[k]) own = true;
      if (own) continue;
      if (count < kSims3LightHitsMax) { g_sims3LightHitAt[count] = q; ++count; }
      else ++g_sims3LightHitOverflow;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static void sims3LightScan() {
  const uint8_t* a = (const uint8_t*) 0x10000;
  MEMORY_BASIC_INFORMATION mbi;
  uint32_t regions = 0, count = 0, mapped = 0; uint64_t bytes = 0;
  while (VirtualQuery(a, &mbi, sizeof mbi) == sizeof mbi) {
    const DWORD prot = mbi.Protect & 0xFF;
    const bool writable = prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_WRITECOPY;
    if (mbi.State == MEM_COMMIT && writable && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
      if (mbi.Type == MEM_MAPPED) ++mapped;
      else { ++regions; bytes += mbi.RegionSize; sims3LightScanRegion((const uint8_t*) mbi.BaseAddress, mbi.RegionSize, count); }
    }
    const uint8_t* next = (const uint8_t*) mbi.BaseAddress + mbi.RegionSize;
    if (next <= a) break;
    a = next;
  }
  g_sims3LightScanRegions = regions; g_sims3LightScanMegabytes = (uint32_t) (bytes >> 20); g_sims3LightSkippedMapped = mapped;
  g_sims3LightHitCount = count;
  g_sims3LightScanDone = true;
  g_sims3LightScanBusy = false;
}
// Floats read from a place found, or false when it cannot be read any more.
static bool sims3LightRead(const float* p, float* out, int n) {
  __try {
    for (int k = 0; k < n; ++k) out[k] = p[k];
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The game's light as its record holds it now; false when the record is not one any more.
static bool sims3ReadGameLight(const float* p, sims3cam::Sun& out) {
  float v[8];
  if (!sims3LightRead(p, v, 8) || !sims3cam::finiteFloats(v, 8) || !sims3cam::floatBits(v + 3, 0u) || !sims3cam::floatBits(v + 7, 0x3f800000u)) return false;
  return sims3cam::skyLightFrom(v + 4, v, out);
}
// What a place in memory belongs to: a module and the offset into it, or the kind of memory.
static void sims3DescribeAddress(const void* p, char* out, size_t cap) {
  HMODULE mod = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR) p, &mod) && mod) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(mod, path, MAX_PATH);
    const char* name = strrchr(path, '\\'); name = name ? name + 1 : path;
    snprintf(out, cap, "%s+0x%x", name, (unsigned) ((const uint8_t*) p - (const uint8_t*) mod));
    return;
  }
  MEMORY_BASIC_INFORMATION mbi = {};
  if (VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi)
    snprintf(out, cap, "%s memory, allocation %p + 0x%x", mbi.Type == MEM_PRIVATE ? "private" : (mbi.Type == MEM_MAPPED ? "mapped" : "image"), mbi.AllocationBase, (unsigned) ((const uint8_t*) p - (const uint8_t*) mbi.AllocationBase));
  else snprintf(out, cap, "unknown memory");
}

// The game's light record through the game's own pointers (milestone 127, sims3cam::kLightChains):
// every chain read with guarded reads, each end checked as a record, the address at least two agree
// on. The exe's build stamp decides once whether the chains apply at all.
static bool sims3ReadWord(uintptr_t at, uint32_t& out) {
  __try { out = *(const uint32_t*) at; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static uint32_t sims3ExeStamp() {
  __try {
    const uint8_t* img = (const uint8_t*) GetModuleHandleA(nullptr);
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*) (img + ((const IMAGE_DOS_HEADER*) img)->e_lfanew);
    return nt->FileHeader.TimeDateStamp;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static const float* sims3LightByChains(int& votes) {
  const uintptr_t img = (uintptr_t) GetModuleHandleA(nullptr);
  uintptr_t at[sims3cam::kLightChainCount] = {};
  for (int k = 0; k < sims3cam::kLightChainCount; ++k) {
    const sims3cam::LightChain& c = sims3cam::kLightChains[k];
    uint32_t obj = 0, ptr = 0; sims3cam::Sun s;
    if (sims3ReadWord(img + c.base, obj) && obj && sims3ReadWord((uintptr_t) obj + c.member, ptr) && ptr && sims3ReadGameLight((const float*) ((uintptr_t) ptr + c.offset), s))
      at[k] = (uintptr_t) ptr + c.offset;
  }
  return (const float*) sims3cam::lightChainVote(at, sims3cam::kLightChainCount, votes);
}

// The block read whole: its head checked, its records copied, and its sequence the same before and
// after (an odd one means the reporter is writing). 0 read, 1 being written, 2 not the block any more.
static int sims3LampRead(const uint8_t* block, float* floats, int32_t* ints, sims3cam::LampHead& head) {
  __try {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(block, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 2;
    uint8_t first[64]; memcpy(first, block, sizeof first);
    if (!sims3cam::lampReportHead(first, (uint32_t) (uintptr_t) block, head)) return 2;
    if (head.sequence & 1u) return 1;
    memcpy(floats, block + head.floatsAt, (size_t) (head.lamps + 1) * sims3cam::kLampFloats * sizeof(float));
    memcpy(ints, block + head.intsAt, (size_t) head.lamps * sims3cam::kLampInts * sizeof(int32_t));
    uint32_t after; memcpy(&after, block + 20, sizeof after);
    return after == head.sequence ? 0 : 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 2; }
}

// The Sims 3 camera hook (milestone 20b): the sun and the lamps as Remix API lights. An API
// light is immutable: a change destroys the old one and creates a new one under the same hash
// (the runtime's identity for it, kept for temporal stability); a handle is drawn every frame
// its light should exist. Radiance and size are explicit here, where the fixed-function path
// left them to the runtime's conversion (a 2-degree disc at intensity 1 per unit of colour for
// the sun; a 4-unit sphere for every lamp).
static const uint64_t kSims3SunHash = 0x53494D5333535541ull, kSims3MoonHash = 0x53494D53334D4F4Full, kSims3LampHash = 0x53494D53334C4D00ull;
static void* sims3ApiLightReplace(void* old, const remixapi_LightInfo& info) {
  if (old) remixapi::remixapi_DestroyLight((remixapi_LightHandle) old);
  remixapi_LightHandle h = nullptr;
  if (remixapi::remixapi_CreateLight(&info, &h) != REMIXAPI_ERROR_CODE_SUCCESS) return nullptr;
  return (void*) h;
}
static void* sims3ApiSun(void* old, const sims3cam::Sun& s, int body) {   // body: 0 the sun, 1 the moon
  remixapi_LightInfoDistantEXT d = {};
  d.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  d.direction.x = -s.dir[0]; d.direction.y = -s.dir[1]; d.direction.z = -s.dir[2];   // the direction the light travels
  d.angularDiameterDegrees = sims3cam::sunAngle();
  d.volumetricRadianceScale = 1.f;
  remixapi_LightInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO; info.pNext = &d; info.hash = body ? kSims3MoonHash : kSims3SunHash;
  const float k = sims3cam::sunRadiance();
  info.radiance.x = s.col[0] > 0.f ? s.col[0] * k : 0.f; info.radiance.y = s.col[1] > 0.f ? s.col[1] * k : 0.f; info.radiance.z = s.col[2] > 0.f ? s.col[2] * k : 0.f;
  return sims3ApiLightReplace(old, info);
}
// A sphere light, shaped to a cone when a direction and an angle from the axis are given (milestone 32).
static void* sims3ApiSphere(uint64_t hash, const float* pos, const float* col, const float* dir, float halfAngle) {
  remixapi_LightInfoSphereEXT sp = {};
  sp.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  sp.position.x = pos[0]; sp.position.y = pos[1]; sp.position.z = pos[2];
  sp.radius = sims3cam::lampRadius();
  sp.shaping_hasvalue = (dir != nullptr && halfAngle > 0.f && halfAngle < 179.5f) ? 1 : 0;
  if (sp.shaping_hasvalue) {
    sp.shaping_value.direction.x = dir[0]; sp.shaping_value.direction.y = dir[1]; sp.shaping_value.direction.z = dir[2];
    sp.shaping_value.coneAngleDegrees = halfAngle < 1.f ? 1.f : halfAngle;
    sp.shaping_value.coneSoftness = sims3cam::lampConeSoftness();
    sp.shaping_value.focusExponent = 0.f;
  }
  sp.volumetricRadianceScale = 1.f;
  remixapi_LightInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO; info.pNext = &sp; info.hash = hash;
  const float k = sims3cam::lampRadiance();
  info.radiance.x = col[0] > 0.f ? col[0] * k : 0.f; info.radiance.y = col[1] > 0.f ? col[1] * k : 0.f; info.radiance.z = col[2] > 0.f ? col[2] * k : 0.f;
  return sims3ApiLightReplace(nullptr, info);
}
// A tube: a cylinder light of the given length from the light's position along its axis.
static void* sims3ApiCylinder(uint64_t hash, const float* pos, const float* axis, float length, const float* col) {
  remixapi_LightInfoCylinderEXT cy = {};
  cy.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT;
  cy.position.x = pos[0] + axis[0] * length * 0.5f; cy.position.y = pos[1] + axis[1] * length * 0.5f; cy.position.z = pos[2] + axis[2] * length * 0.5f;
  cy.radius = sims3cam::lampRadius() * 0.3f;
  cy.axis.x = axis[0]; cy.axis.y = axis[1]; cy.axis.z = axis[2];
  cy.axisLength = length;
  cy.volumetricRadianceScale = 1.f;
  remixapi_LightInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO; info.pNext = &cy; info.hash = hash;
  const float k = sims3cam::lampRadiance();
  info.radiance.x = col[0] > 0.f ? col[0] * k : 0.f; info.radiance.y = col[1] > 0.f ? col[1] * k : 0.f; info.radiance.z = col[2] > 0.f ? col[2] * k : 0.f;
  return sims3ApiLightReplace(nullptr, info);
}
// A lamp's API lights, by its type (milestone 32): a point is a sphere; a spot a sphere shaped to its
// cone; a lamp shade its two cones and the light through the shade; a tube a cylinder. The lights it
// had are destroyed first. Returns the number of API calls made.
static uint32_t sims3ApiLamp(sims3cam::Lamp& L) {
  uint32_t calls = 0;
  void** hs[3] = { &L.api, &L.api2, &L.api3 };
  for (int q = 0; q < 3; ++q) if (*hs[q]) { remixapi::remixapi_DestroyLight((remixapi_LightHandle) *hs[q]); *hs[q] = nullptr; ++calls; }
  const uint64_t hash = kSims3LampHash ^ (uint64_t) L.id;   // the object the lamp is anchored to (milestone 21)
  const bool aimed = sims3cam::lampShapes() && sims3cam::len3(L.dir) > 0.5f;
  const float scale = sims3cam::lampConeScale();
  if (aimed && L.kind == 4 && L.angle > 0.f) { L.api = sims3ApiSphere(hash, L.pos, L.col, L.dir, L.angle * scale); ++calls; }
  else if (aimed && L.kind == 5) {
    const float back[3] = { -L.dir[0], -L.dir[1], -L.dir[2] };
    if (L.angle > 0.f) { L.api = sims3ApiSphere(hash, L.pos, L.col, L.dir, L.angle * scale); ++calls; }
    if (L.bottom > 0.f) { L.api2 = sims3ApiSphere(hash ^ (1ull << 36), L.pos, L.col, back, L.bottom * scale); ++calls; }
    const float g = sims3cam::lampShadeGlow();
    const float glow[3] = { L.col[0] * L.shade[0] * g, L.col[1] * L.shade[1] * g, L.col[2] * L.shade[2] * g };
    if (sims3cam::luminance(glow) > 1e-3f) { L.api3 = sims3ApiSphere(hash ^ (2ull << 36), L.pos, glow, nullptr, 0.f); ++calls; }
  }
  else if (aimed && L.kind == 6 && L.tube > 0.f) { L.api = sims3ApiCylinder(hash, L.pos, L.dir, L.tube, L.col); ++calls; }
  if (!L.api && !L.api2 && !L.api3) { L.api = sims3ApiSphere(hash, L.pos, L.col, nullptr, 0.f); ++calls; }
  return calls;
}

// The Sims 3 camera hook: the lamps. Every lamp the reporter names as lit, looked up in the light
// table by the keys of its object, placed by its object's transform, sent as Remix API lights.
static void sims3PresentLamps(Sims3Hook& h) {
  const bool api = GlobalOptions::getExposeRemixApi();
  // the lamp reporter's block: searched for on a thread of its own until found, read every frame
  const uint8_t* block = g_sims3LampBlock.load();
  if (!block && !g_sims3LampScanBusy.load() && (h.lampReportScanFrame == 0 || h.frames - h.lampReportScanFrame > (g_sims3LampScans.load() < 12u ? 600u : 3600u))) {
    h.lampReportScanFrame = h.frames ? h.frames : 1;
    g_sims3LampScanBusy = true;
    std::thread(sims3LampScan).detach();
  }
  h.lampReportLive = false;
  if (block) {
    const size_t floats = (size_t) (sims3cam::kLampCapacity + 1) * sims3cam::kLampFloats, ints = (size_t) sims3cam::kLampCapacity * sims3cam::kLampInts;
    if (h.lampRecords.size() < floats) { h.lampRecords.resize(floats); h.lampRecordsNext.resize(floats); h.lampInts.resize(ints); h.lampIntsNext.resize(ints); }
    sims3cam::LampHead head = {};
    const int r = sims3LampRead(block, h.lampRecordsNext.data(), h.lampIntsNext.data(), head);
    if (r == 0) { h.lampRecords.swap(h.lampRecordsNext); h.lampInts.swap(h.lampIntsNext); h.lampReported = head.lamps; h.lampReportWorld = head.world; ++h.lampReportReads; h.lampReportFails = 0; }
    else if (r == 1) ++h.lampReportStale;   // being written: the last whole reading stands
    else if (++h.lampReportFails > 300u) { g_sims3LampBlock = nullptr; h.lampReported = 0; h.lampReportWorld = false; h.lampReportFails = 0; Logger::info("Sims 3 camera hook: the lamp reporter's block is gone (or is an older version's); searching again"); }
    h.lampReportLive = h.lampReportReads > 0 && h.lampReportWorld;
    if (h.lampReportLive && !h.lampReportAnnounced) {
      h.lampReportAnnounced = true;
      const float* f0 = h.lampRecords.data();
      char msg[340];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: the lamp reporter found at %p after %u searches, frame %u: %u lamps within %.0f units of the camera target (%.1f, %.1f, %.1f); the game's levels dim %.2f, normal %.2f, bright %.2f; the light table: %u models, %u objects",
               (const void*) block, g_sims3LampScans.load(), h.frames, h.lampReported, f0[3], f0[0], f0[1], f0[2], f0[4], f0[5], f0[6],
               (unsigned) sims3cam::liteTable().models.size(), (unsigned) sims3cam::liteTable().objects.size());
      Logger::info(msg);
    }
  }
  // the game's clock; the world lights (a street lamp's) are reported on around the clock (run 151).
  // Their light is scaled by the game's own night switch (milestones 54, 55: the fade the game
  // shows its lamp glow on the ground by, kept next to its light record, run 163; it takes about
  // eight game minutes, run 164); until the record is found they are on while the game's word for
  // night holds
  h.clock = h.lampReportLive ? sims3cam::clockFromRecord(h.lampRecords.data()) : sims3cam::GameClock {};
  float worldFade = -1.f;   // the game's night switch, 0..1; -1 not read
  if (h.lightState == 3 && !h.nightSwitchBad) {
    float sw = -1.f;
    if (sims3LightRead(h.lightPlaces[h.lightUse].p - 28, &sw, 1)) sims3cam::nightSwitchValue(&sw, &worldFade);
  }
  const bool bySwitch = worldFade >= 0.f;
  if (!bySwitch) worldFade = h.clock.known && h.clock.night ? 1.f : 0.f;
  if (!sims3cam::lampWorldLights()) worldFade = 0.f;
  const bool worldLights = worldFade > 0.f;
  h.worldFade = worldFade; h.worldBySwitch = bySwitch;
  if (h.clock.known && (!h.clockSaid || h.clock.night != h.clockNight) && h.clockLogged < 80u) {
    h.clockSaid = true; h.clockNight = h.clock.night; ++h.clockLogged;
    char msg[200];
    snprintf(msg, sizeof msg, "Sims 3 camera hook: the game's clock at frame %u: %.2f h, %s (sunrise %.2f, sunset %.2f)",
             h.frames, h.clock.hour, h.clock.night ? "NIGHT" : "DAY", h.clock.sunrise, h.clock.sunset);
    Logger::info(msg);
  }
  {
    const int street = (worldFade <= 0.f ? 0 : (worldFade < 1.f ? 1 : 2)) + (bySwitch ? 3 : 0);
    if ((h.clock.known || bySwitch) && street != h.streetSaid && h.streetLogged < 120u) {
      h.streetSaid = street; ++h.streetLogged;
      char msg[280];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: the street lamps are %s (%.3f) from frame %u, clock %.2f h, by %s",
               !sims3cam::lampWorldLights() ? "dark (lampWorldLights 0)" : (worldFade <= 0.f ? "dark" : (worldFade < 1.f ? "FADING" : "LIT")), worldFade, h.frames, h.clock.known ? h.clock.hour : -1.f,
               bySwitch ? "the game's own night switch" : "the game's word for night (its night switch not found yet)");
      Logger::info(msg);
    }
  }
  // the lit lamps that have a definition, the nearest to the camera's target first
  h.lamps.begin();
  h.lampsOn = h.lampsUndefined = h.lampsBeyondBudget = h.lampsWorldDark = 0;
  const bool mark = h.markDump == 2;
  if (h.lampReportLive) {
    const sims3cam::LiteTable& table = sims3cam::liteTable();
    const float* frame = h.lampRecords.data();
    struct Pick { uint32_t k; float d2; const sims3cam::LiteModel* m; };
    static std::vector<Pick> picks; picks.clear();
    for (uint32_t k = 0; k < h.lampReported; ++k) {
      const float* rec = frame + (size_t) (k + 1) * sims3cam::kLampFloats;
      if (rec[8] <= 0.5f) continue;
      ++h.lampsOn;
      const int32_t* id = h.lampInts.data() + (size_t) k * sims3cam::kLampInts;
      const uint64_t modelKey = sims3cam::lampId64(id + 2), objectKey = sims3cam::lampId64(id + 5);
      int how = 0;
      const sims3cam::LiteModel* m = table.find(modelKey, objectKey, &how);
      if (!m) {
        ++h.lampsUndefined;
        bool said = false;
        for (uint32_t q = 0; q < h.lampUndefinedCount && !said; ++q) said = h.lampUndefinedKeys[q] == modelKey;
        if (!said && h.lampUndefinedCount < 64u) {   // a lit lamp the light table has no definition for, once per model
          h.lampUndefinedKeys[h.lampUndefinedCount++] = modelKey;
          char msg[300];
          snprintf(msg, sizeof msg, "Sims 3 camera hook: a lit lamp at (%.1f, %.1f, %.1f) has no definition in the light table: catalog model key %08x:%016llx, resource key %08x:%016llx (its model has no lamp light, or the table is older than the game's packages)",
                   rec[0], rec[1], rec[2], (unsigned) id[4], (unsigned long long) modelKey, (unsigned) id[7], (unsigned long long) objectKey);
          Logger::info(msg);
        }
        continue;
      }
      if (how > 0 && !(h.lampKeyHow & (1u << how))) {
        h.lampKeyHow |= 1u << how;
        char msg[300];
        snprintf(msg, sizeof msg, "Sims 3 camera hook: a lamp's definition found by %s: catalog model key %08x:%016llx, resource key %08x:%016llx -> model %016llx",
                 how == 1 ? "its catalog model key naming the model" : (how == 2 ? "its resource key naming the object" : "its catalog model key naming the object"),
                 (unsigned) id[4], (unsigned long long) modelKey, (unsigned) id[7], (unsigned long long) objectKey, (unsigned long long) m->inst);
        Logger::info(msg);
      }
      uint32_t usable = 0;
      for (uint8_t li = 0; li < m->n; ++li) if (m->lights[li].type != 11 || worldLights) ++usable;
      if (!usable) { ++h.lampsWorldDark; continue; }   // world lights alone, and it is day
      const float d[3] = { rec[0] - frame[0], rec[1] - frame[1], rec[2] - frame[2] };
      picks.push_back({ k, d[0]*d[0] + d[1]*d[1] + d[2]*d[2], m });
    }
    const uint32_t budget = sims3cam::lampMax();
    if (picks.size() > budget) {
      std::nth_element(picks.begin(), picks.begin() + budget, picks.end(), [](const Pick& a, const Pick& b) { return a.d2 < b.d2; });
      h.lampsBeyondBudget = (uint32_t) picks.size() - budget;
      picks.resize(budget);
    }
    uint32_t marked = 0;
    for (const Pick& pick : picks) {
      const float* rec = frame + (size_t) (pick.k + 1) * sims3cam::kLampFloats;
      const float* rows = rec + 12;
      const int32_t* id = h.lampInts.data() + (size_t) pick.k * sims3cam::kLampInts;
      const uint64_t object = sims3cam::lampId64(id);
      for (uint8_t li = 0; li < pick.m->n; ++li) {
        const sims3cam::LiteLight& def = pick.m->lights[li];
        if (def.type == 11 && !worldLights) continue;
        bool fresh = false;
        sims3cam::Lamp* L = h.lamps.name(object, li, fresh);
        if (!L) break;
        sims3cam::worldPoint(rows, def.pos, L->pos);
        // the shape: the definition's direction points from the lit side back to the light, so the
        // light travels the other way; carried to the world by the object's turn
        float back[3]; const bool aimed = sims3cam::worldDir(rows, def.at, back);
        for (int q = 0; q < 3; ++q) L->dir[q] = -back[q];
        const bool shaded = def.type == 5 || (def.type == 11 && aimed);
        L->kind = shaded ? (uint8_t) 5 : (def.type == 11 ? (uint8_t) 3 : def.type);
        L->angle = (def.type == 4 || shaded) ? def.d[0] : 0.f;
        L->bottom = shaded ? def.d[2] : 0.f;
        for (int q = 0; q < 3; ++q) L->shade[q] = shaded ? def.d[3 + q] : 0.f;
        L->tube = def.type == 6 ? def.d[0] : 0.f;
        const sims3cam::LampWord w = sims3cam::lampWordFromRecord(rec, def.col, def.intensity, frame[5]);
        for (int q = 0; q < 3; ++q) L->col[q] = def.type == 11 ? w.col[q] * worldFade : w.col[q];   // a world light, by the game's night switch
        if ((fresh && h.lampWordsLogged < 600u) || (mark && marked < 60u)) {
          if (fresh) ++h.lampWordsLogged;
          if (mark) ++marked;
          char msg[420];
          snprintf(msg, sizeof msg, "Sims 3 camera hook: lamp %s, frame %u: object %016llx (model %016llx) at (%.2f, %.2f, %.2f), light %u of %u, type %u, at (%.2f, %.2f, %.2f), travelling (%.2f, %.2f, %.2f); colour preset %d (%.2f, %.2f, %.2f), level x%.2f (intensity %.2f, dimmer %.2f), floor %d; sent colour %.2f, %.2f, %.2f",
                   fresh ? "LIT" : "at the mark", h.frames, (unsigned long long) object, (unsigned long long) pick.m->inst, rec[0], rec[1], rec[2], (unsigned) li + 1u, (unsigned) pick.m->n, (unsigned) def.type,
                   L->pos[0], L->pos[1], L->pos[2], L->dir[0], L->dir[1], L->dir[2], (int) (rec[9] + 0.5f), rec[3], rec[4], rec[5], w.level, rec[6], rec[7], (int) rec[11], w.col[0], w.col[1], w.col[2]);
          Logger::info(msg);
        }
      }
    }
  }
  for (uint32_t k = 0; k < h.lamps.n; ++k) {   // the lights no longer named: put out
    const sims3cam::Lamp& L = h.lamps.lamps[k];
    if (L.seen || h.lampWordsLogged >= 600u) continue;
    ++h.lampWordsLogged;
    char msg[200];
    snprintf(msg, sizeof msg, "Sims 3 camera hook: lamp put out, frame %u: object %016llx, light %u, at (%.2f, %.2f, %.2f)", h.frames, (unsigned long long) L.object, (unsigned) L.light + 1u, L.pos[0], L.pos[1], L.pos[2]);
    Logger::info(msg);
  }
  h.lamps.end();
  if (mark) {
    char msg[300];
    snprintf(msg, sizeof msg, "Sims 3 camera hook: lamps at the mark, frame %u: %u reported, %u lit, %u of those without a definition, %u of world lights alone and dark by day, %u beyond the budget of %u; %u lights held; clock %.2f h, %s",
             h.frames, h.lampReported, h.lampsOn, h.lampsUndefined, h.lampsWorldDark, h.lampsBeyondBudget, sims3cam::lampMax(), h.lamps.n, h.clock.hour, !h.clock.known ? "unknown" : (h.clock.night ? "night" : "day"));
    Logger::info(msg);
  }
  for (uint32_t k = 0; k < h.lamps.nGone; ++k) {   // the lights of the entries put out this frame
    if (h.lamps.gone[k]) { remixapi::remixapi_DestroyLight((remixapi_LightHandle) h.lamps.gone[k]); ++h.apiLightCalls; ++h.lampEvents; }
  }
  for (uint32_t k = 0; k < h.lamps.n && api; ++k) {
    sims3cam::Lamp& L = h.lamps.lamps[k];
    const bool changed = !L.sent || sims3cam::lampDist(L.sentPos, L.pos) > 0.05f ||
                         std::fabs(L.sentCol[0] - L.col[0]) > 0.02f || std::fabs(L.sentCol[1] - L.col[1]) > 0.02f || std::fabs(L.sentCol[2] - L.col[2]) > 0.02f ||
                         sims3cam::dot3(L.sentDir, L.dir) < 0.999f * sims3cam::dot3(L.dir, L.dir);   // the object turned
    if (changed) {
      h.apiLightCalls += sims3ApiLamp(L);
      for (int q = 0; q < 3; ++q) { L.sentPos[q] = L.pos[q]; L.sentCol[q] = L.col[q]; L.sentDir[q] = L.dir[q]; }
      L.sent = true; ++h.lampEvents;
      if (L.kind < 32 && !(h.loggedShapes & (1u << L.kind))) {   // the first light of each kind, with its shape (milestone 32)
        h.loggedShapes |= 1u << L.kind;
        char msg[360];
        snprintf(msg, sizeof msg, "Sims 3 camera hook: first light of kind %u (3 sphere, 4 cone, 5 two cones and a shade, 6 cylinder) forwarded at (%.1f, %.1f, %.1f): its light travels (%.2f, %.2f, %.2f), cone %.0f degrees from the axis, opposite cone %.0f, shade light %.2f,%.2f,%.2f, tube %.2f; shapes %s, cone scale %.2f; API lights: main %s, opposite %s, shade %s",
                 (unsigned) L.kind, L.pos[0], L.pos[1], L.pos[2], L.dir[0], L.dir[1], L.dir[2], L.angle, L.bottom, L.shade[0], L.shade[1], L.shade[2], L.tube,
                 sims3cam::lampShapes() ? "on" : "off", sims3cam::lampConeScale(), L.api ? "yes" : "no", L.api2 ? "yes" : "no", L.api3 ? "yes" : "no");
        Logger::info(msg);
      }
    }
    if (L.api) remixapi::remixapi_DrawLightInstance((remixapi_LightHandle) L.api);   // every frame the lamp is on
    if (L.api2) remixapi::remixapi_DrawLightInstance((remixapi_LightHandle) L.api2);
    if (L.api3) remixapi::remixapi_DrawLightInstance((remixapi_LightHandle) L.api3);
  }
}

// The Sims 3 camera hook: the lights of the sky. The game's one directional light, as the lit
// terrain shaders were handed it in this frame, is the sun's or the moon's by the game's clock;
// the moon's is scaled by the user's share, and at dusk and at dawn the two are cross-faded in
// equal parts (sims3cam::SkyLights, milestones 42 to 46). A frame without a lit terrain draw
// keeps the lights the runtime holds.
template<typename Dev>
void sims3PresentSky(Sims3Hook& h, Dev* dev) {
  static const char* const kBody[2] = { "sun", "moon" };
  sims3cam::Sun game = {};
  bool fresh = false;
  if (h.terrainSunDraws > 0) {
    fresh = sims3cam::skyLightFrom(h.terrainSunCol, h.terrainSunDir, game);
    if (!fresh && ++h.sunRefused <= 20u) {
      char msg[260];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: sun: the terrain's constants at frame %u are not a light's and are left aside: c0 %.3f, %.3f, %.3f, c1 %.3f, %.3f, %.3f (%u draws)",
               h.frames, h.terrainSunCol[0], h.terrainSunCol[1], h.terrainSunCol[2], h.terrainSunDir[0], h.terrainSunDir[1], h.terrainSunDir[2], h.terrainSunDraws);
      Logger::info(msg);
    }
  }
  const bool readTerrain = fresh;
  // The game's own light in its memory (milestone 49). Two seconds after a world is live, on the
  // lot, a search finds the records holding what the terrain is handed (sims3cam::lightRecord);
  // five seconds of agreement with the terrain confirm one; it is then the sky's light in every
  // view (milestone 51), checked against the terrain whenever the lot shows it, and searched for
  // again when it stops agreeing or a world is loaded anew.
  if (h.lampReportLive) ++h.lightLiveFrames;
  else { h.lightLiveFrames = 0; h.lightFromChains = false; if (h.lightState != 1) { h.lightState = 0; h.lightUse = -1; } }
  // The game's light through its own pointers (milestone 127): for the TS3.exe build the chains were
  // found in, from the first frame of a live world, in every view, with neither the terrain nor the
  // search; at least two chains must agree on a record. Any other build, or chains whose record stops
  // agreeing with the terrain, leave it to the search below.
  if (h.lightChainBuild < 0) {
    const uint32_t stamp = sims3ExeStamp();
    h.lightChainBuild = stamp == sims3cam::kLightChainStamp ? 1 : 0;
    char msg[200];
    snprintf(msg, sizeof msg, "Sims 3 camera hook: the game's exe build stamp %08x: %s", stamp, h.lightChainBuild ? "its light read through the game's own pointers (milestone 127)" : "not the build the pointers were found in -- its light by the search");
    Logger::info(msg);
  }
  if (h.lampReportLive && h.lightChainBuild == 1 && !h.lightChainBad) {
    int votes = 0;
    const float* p = sims3LightByChains(votes);
    if (p && !(h.lightState == 3 && h.lightFromChains && h.lightPlaces[0].p == p)) {
      h.lightPlaces[0] = Sims3Hook::LightPlace(); h.lightPlaces[0].p = p; h.lightPlaceN = 1; h.lightUse = 0; h.lightState = 3; h.lightDisagree = 0; h.lightFromChains = true;
      ++h.lightChainSets;
      if (h.lightChainSets <= 8u) {
        char msg[220];
        snprintf(msg, sizeof msg, "Sims 3 camera hook: the game's own light through its pointers at %p (%d of %d chains agree) at frame %u: the sky's light from now on, in every view",
                 (const void*) p, votes, sims3cam::kLightChainCount, h.frames);
        Logger::info(msg);
      }
    }
  }
  if ((h.lightState == 0 || (h.lightState == 4 && h.frames - h.lightRetryFrame > 600u)) && readTerrain && h.clock.known && h.lightLiveFrames > 120u && !g_sims3LightScanBusy.load()) {
    h.lightState = 1; ++h.lightSearches;
    for (int q = 0; q < 3; ++q) { g_sims3LightTarget[q] = h.terrainSunDir[q]; g_sims3LightTarget[3 + q] = h.terrainSunCol[q]; }
    g_sims3LightExcludeFrom[0] = (const uint8_t*) &g_sims3; g_sims3LightExcludeTo[0] = (const uint8_t*) (&g_sims3 + 1);
    g_sims3LightExcludeFrom[1] = (const uint8_t*) dev; g_sims3LightExcludeTo[1] = (const uint8_t*) (dev + 1);
    g_sims3LightHitOverflow = 0; g_sims3LightScanDone = false; g_sims3LightScanBusy = true;
    std::thread(sims3LightScan).detach();
  }
  if (h.lightState == 1 && g_sims3LightScanDone.load()) {
    h.lightPlaceN = g_sims3LightHitCount.load();
    for (uint32_t k = 0; k < h.lightPlaceN; ++k) { h.lightPlaces[k].p = g_sims3LightHitAt[k]; h.lightPlaces[k].checked = h.lightPlaces[k].matched = 0; }
    h.lightConfirmFrames = 0;
    if (h.lightPlaceN) h.lightState = 2; else { h.lightState = 4; h.lightRetryFrame = h.frames; }
    char msg[300];
    snprintf(msg, sizeof msg, "Sims 3 camera hook: the game's own light: search %u done at frame %u, clock %.2f h: %u records holding the terrain's light (%u more not kept) in %u regions, %u MB",
             h.lightSearches, h.frames, h.clock.known ? h.clock.hour : -1.f, h.lightPlaceN, g_sims3LightHitOverflow.load(), g_sims3LightScanRegions.load(), g_sims3LightScanMegabytes.load());
    Logger::info(msg);
  }
  if (h.lightState == 2 && readTerrain) {
    ++h.lightConfirmFrames;
    for (uint32_t k = 0; k < h.lightPlaceN; ++k) {
      float v[8];
      ++h.lightPlaces[k].checked;
      if (sims3LightRead(h.lightPlaces[k].p, v, 8) && sims3cam::lightRecord(v, h.terrainSunDir, h.terrainSunCol, 0.001f)) ++h.lightPlaces[k].matched;
    }
    if (h.lightConfirmFrames >= 300u) {
      uint32_t confirmed = 0; h.lightUse = -1;
      for (uint32_t k = 0; k < h.lightPlaceN; ++k)
        if (h.lightPlaces[k].checked && h.lightPlaces[k].matched * 10u >= h.lightPlaces[k].checked * 9u) { ++confirmed; if (h.lightUse < 0) h.lightUse = (int) k; }
      char msg[360];
      if (h.lightUse >= 0) {
        h.lightState = 3; h.lightDisagree = 0;
        const Sims3Hook::LightPlace& L = h.lightPlaces[h.lightUse];
        char where[160]; sims3DescribeAddress(L.p, where, sizeof where);
        snprintf(msg, sizeof msg, "Sims 3 camera hook: the game's own light FOUND at %p (%s): it agreed with the terrain in %u of %u frames (%u of %u records agreed); the sky's light from now on, in every view",
                 (const void*) L.p, where, L.matched, L.checked, confirmed, h.lightPlaceN);
      } else {
        h.lightState = 4; h.lightRetryFrame = h.frames;
        snprintf(msg, sizeof msg, "Sims 3 camera hook: the game's own light: none of the %u records agreed with the terrain for five seconds; searched for again in ten seconds", h.lightPlaceN);
      }
      Logger::info(msg);
    }
  }
  if (h.lightState == 3 && readTerrain) {
    float v[8];
    const bool agree = sims3LightRead(h.lightPlaces[h.lightUse].p, v, 8) && sims3cam::lightRecord(v, h.terrainSunDir, h.terrainSunCol, 0.002f);
    if (agree) h.lightDisagree = 0;
    else if (++h.lightDisagree > 120u) {
      h.lightState = 0; h.lightUse = -1;
      const bool chains = h.lightFromChains;
      if (chains) { h.lightChainBad = true; h.lightFromChains = false; }   // the pointers are not trusted again this session
      char msg[220];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: the game's own light%s stopped agreeing with the terrain at frame %u: searched for again", chains ? " (through its pointers)" : "", h.frames);
      Logger::info(msg);
    }
  }
  // The game's night switch, next to its light record (run 163: 28 floats before it), is handed
  // to the lit terrain as c7.x: checked whenever the terrain is drawn; if they ever disagree for
  // two seconds the street lamps go back to the game's word for night.
  if (h.lightState == 3 && readTerrain && !h.nightSwitchBad) {
    float sw = -1.f;
    uint32_t want; memcpy(&want, &h.terrainNightSwitch, 4);
    if (sims3LightRead(h.lightPlaces[h.lightUse].p - 28, &sw, 1) && sims3cam::floatBits(&sw, want)) h.nightSwitchDisagree = 0;
    else if (++h.nightSwitchDisagree > 120u) {
      h.nightSwitchBad = true;
      char msg[260];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: the game's night switch next to its light record (%.3f) does not agree with the terrain's (%.3f): the street lamps follow the game's word for night instead",
               sw, h.terrainNightSwitch);
      Logger::info(msg);
    }
  }
  // The game's fog next to its light record (run 163: its colour 64 floats before it, its range 20
  // before it) is handed to the lit terrain as c2 and c4: checked in the same way; if they ever
  // disagree for two seconds the fog is left off for the session.
  if (sims3cam::fogFromGame() && h.lightState == 3 && readTerrain && !h.fogBad) {
    const float* rec = h.lightPlaces[h.lightUse].p;
    float fog[8] = {};
    const bool same = sims3LightRead(rec - 64, fog, 4) && sims3LightRead(rec - 20, fog + 4, 4) &&
                      std::memcmp(fog, h.terrainFog, 3 * sizeof(float)) == 0 && std::memcmp(fog + 4, h.terrainFog + 4, 4 * sizeof(float)) == 0;
    if (same) h.fogDisagree = 0;
    else if (++h.fogDisagree > 120u) {
      h.fogBad = true; h.fogReady = false;
      char msg[320];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: the game's fog next to its light record (colour %.3f %.3f %.3f, c4 %.6f %.4f) does not agree with the terrain's (%.3f %.3f %.3f, %.6f %.4f): the fog is left off",
               fog[0], fog[1], fog[2], fog[4], fog[5], h.terrainFog[0], h.terrainFog[1], h.terrainFog[2], h.terrainFog[4], h.terrainFog[5]);
      Logger::info(msg);
    }
  }
  // the sky's light: the game's own record once found, in every view; until then the lit terrain;
  // without either the last light holds
  bool fromGame = false;
  if (!fresh) ++h.framesNoTerrainSun;
  if (h.lightState == 3) {
    sims3cam::Sun rec = {};
    if (sims3ReadGameLight(h.lightPlaces[h.lightUse].p, rec)) { game = rec; fresh = true; fromGame = true; ++h.framesFromGame; }
  }
  {
    // where the light comes from, logged when it has changed for half a second
    const int src = fromGame ? 1 : (readTerrain ? 0 : 2);
    if (src == h.skySrcCand) ++h.skySrcFrames; else { h.skySrcCand = src; h.skySrcFrames = 1; }
    if (h.skySrcFrames == 30u && src != h.skySrcLogged && h.skySrcLogs < 100u) {
      ++h.skySrcLogs; h.skySrcLogged = src;
      static const char* const kSrc[3] = { "the lit terrain (the game's light record not found yet)", "the game's own light in its memory", "nowhere: held at its last value (no lit terrain drawn, the game's light record not found yet)" };
      char msg[320];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: the sky's light comes from %s since frame %u, clock %.2f h",
               kSrc[src], h.frames - 29u, h.clock.known ? h.clock.hour : -1.f);
      Logger::info(msg);
    }
  }
  if (fresh) {
    // the dawn eased (milestone 43): the game's sunrise is steep, so for dawnMinutes after sunrise the sun's light is scaled up from nothing
    sims3cam::Sun eased = game;
    const float ease = sims3cam::clockMoonTime(h.clock) ? 1.f : sims3cam::dawnEase(h.clock, sims3cam::dawnHours());
    if (ease < 1.f) for (int q = 0; q < 3; ++q) eased.col[q] *= ease;
    const int what = h.sky.step(eased, sims3cam::clockMoonTime(h.clock), sims3cam::moonShare(), sims3cam::duskFade(h.clock, sims3cam::duskHours()), ease, sims3cam::clockDusk(h.clock), sims3cam::duskLevel());
    h.skySet = true; h.gameLight = game;
    if (!h.loggedSun) {
      h.loggedSun = true; char msg[400];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: the lights of the sky are the terrain's light: first taken at frame %u from %u terrain draws, colour %.3f, %.3f, %.3f toward %.3f, %.3f, %.3f; the moon's share of the game's moonlight %.2f (moonLight); the dawn eased over %.0f minutes (dawnMinutes); the sun's afterglow from %.3f over %.0f minutes (duskLevel, duskMinutes)",
               h.frames, h.terrainSunDraws, game.col[0], game.col[1], game.col[2], game.dir[0], game.dir[1], game.dir[2], sims3cam::moonShare(), sims3cam::dawnHours() * 60.f, sims3cam::duskLevel(), sims3cam::duskHours() * 60.f);
      Logger::info(msg);
    }
    if (what != 0 && h.twilightLogged < 60u) {
      ++h.twilightLogged; char msg[400];
      if (what == 1)
        snprintf(msg, sizeof msg, "Sims 3 camera hook: dusk at frame %u, clock %.2f h: the sun's AFTERGLOW begins at colour %.3f, %.3f, %.3f (luminance %.3f) toward %.3f, %.3f, %.3f, held until the moon comes, then fading over %.0f minutes as the moon rises",
                 h.frames, h.clock.hour, h.sky.glow.col[0], h.sky.glow.col[1], h.sky.glow.col[2], sims3cam::luminance(h.sky.glow.col), h.sky.glow.dir[0], h.sky.glow.dir[1], h.sky.glow.dir[2], sims3cam::duskHours() * 60.f);
      else
        snprintf(msg, sizeof msg, "Sims 3 camera hook: dusk at frame %u, clock %.2f h: the sun's afterglow ENDS; the game's light is the %s's at %.3f",
                 h.frames, h.clock.hour, kBody[h.sky.body], sims3cam::luminance(game.col));
      Logger::info(msg);
    }
    for (int b = 0; b < 2; ++b) {
      if (!h.sky.showing[b]) continue;
      sims3cam::Sun& sent = b ? h.moon : h.sun; bool& set = b ? h.moonSet : h.sunSet; void*& api = b ? h.moonApi : h.sunApi;
      if (set && sims3cam::sameSun(h.sky.shown[b], sent)) continue;
      if (GlobalOptions::getExposeRemixApi()) {   // milestones 20b, 23: the API or nothing
        api = sims3ApiSun(api, h.sky.shown[b], b); ++h.apiLightCalls;
        if (!h.loggedApiLights) {
          h.loggedApiLights = true; char msg[260];
          snprintf(msg, sizeof msg, "Sims 3 camera hook: the sun and the moon go out as Remix API distant lights (angular diameter %.2f degrees, radiance x%.2f per unit of colour), the lamps as sphere lights (radius %.2f, radiance x%.1f)",
                   sims3cam::sunAngle(), sims3cam::sunRadiance(), sims3cam::lampRadius(), sims3cam::lampRadiance());
          Logger::info(msg);
        }
      } else if (!h.apiLightsWarned) {
        h.apiLightsWarned = true;
        Logger::warn("Sims 3 camera hook: no lights: the Remix API is off (exposeRemixApi = True in .trex\\bridge.conf turns it on for the bridge server)");
      }
      sent = h.sky.shown[b]; set = true;
      ++h.sunChanges;
    }
    // the trace: every step of a tenth in the sky's light (or 0.02 near the dark), of five degrees, the afterglow's beginning and end, and every two seconds through the dawn's ease
    const float lum = h.sky.level();
    const sims3cam::Sun& lead = h.sky.shown[h.sky.body];
    const bool moved = h.sunLogLum < 0.f || std::fabs(lum - h.sunLogLum) > (h.sunLogLum > 0.2f ? 0.1f * h.sunLogLum : 0.02f) || sims3cam::dot3(lead.dir, h.sunLogDir) < 0.996f || what != 0 || (ease < 1.f && h.frames - h.sunLogFrame >= 120u);
    if (moved && h.sunLogged < 500) {
      ++h.sunLogged; h.sunLogLum = lum; h.sunLogFrame = h.frames; for (int q = 0; q < 3; ++q) h.sunLogDir[q] = lead.dir[q];
      const sims3cam::Sun& s0 = h.sky.shown[0]; const sims3cam::Sun& s1 = h.sky.shown[1];
      char msg[700];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: sky at frame %u, clock %.2f h%s: the game's light%s is the %s's, colour %.3f, %.3f, %.3f (luminance %.3f), the dawn's ease x%.2f; SUN %s colour %.3f, %.3f, %.3f toward %.3f, %.3f, %.3f; MOON %s colour %.3f, %.3f, %.3f toward %.3f, %.3f, %.3f; the light of the sky %.3f, the afterglow x%.2f; sky level %.3f",
               h.frames, h.clock.known ? h.clock.hour : -1.f, !h.clock.known ? " (unknown)" : (h.clock.night ? " night" : " day"), fromGame ? "" : (readTerrain ? " (from the terrain: the game's record not found yet)" : ""), kBody[h.sky.body], game.col[0], game.col[1], game.col[2], sims3cam::luminance(game.col), ease,
               !h.sky.showing[0] ? "none," : (h.sky.glowing ? "its afterglow," : "as the game's,"), h.sky.showing[0] ? s0.col[0] : 0.f, h.sky.showing[0] ? s0.col[1] : 0.f, h.sky.showing[0] ? s0.col[2] : 0.f, s0.dir[0], s0.dir[1], s0.dir[2],
               !h.sky.showing[1] ? "none," : (h.sky.body == 0 ? "kept past sunrise," : "the game's by its share, at least its floor,"), h.sky.showing[1] ? s1.col[0] : 0.f, h.sky.showing[1] ? s1.col[1] : 0.f, h.sky.showing[1] ? s1.col[2] : 0.f, s1.dir[0], s1.dir[1], s1.dir[2],
               lum, h.sky.glowFade, h.skyLevel);
      Logger::info(msg);
    }
  }
  h.terrainSunDrawsLast = h.terrainSunDraws; h.terrainSunDraws = 0;
  const bool haveSky = h.skySet;   // for the night below
  if (h.sunApi && h.sunSet && h.sky.showing[0] && sims3cam::luminance(h.sun.col) > 0.001f) remixapi::remixapi_DrawLightInstance((remixapi_LightHandle) h.sunApi);   // every frame it gives light (milestone 20b)
  if (h.moonApi && h.moonSet && h.sky.showing[1] && sims3cam::luminance(h.moon.col) > 0.001f) remixapi::remixapi_DrawLightInstance((remixapi_LightHandle) h.moonApi);
  // The sky and the exposure (milestones 20d, 52, 54). The runtime's sky brightness stays 1: the
  // sky that lights the scene is the one the game draws, painted with the game's own sky colours
  // at every hour and in every weather (run 163 found them next to the light record), so there is
  // nothing to map. The ceiling of the auto-exposure follows the light of the sky as sent -- the
  // sun's and the moon's together, relative to the game's full daylight -- so that it does not
  // brighten the night back up (its default range reaches +5 EV).
  if (sims3cam::exposureFromLight()) {
    if (GlobalOptions::getExposeRemixApi()) {
      if (h.skyBrightnessSent != 1.f) {
        remixapi::remixapi_SetConfigVariable("rtx.skyBrightness", "1.000");
        h.skyBrightnessSent = 1.f;
        Logger::info("Sims 3 camera hook: sky brightness 1: the sky the game draws lights the scene as it is");
      }
      if (haveSky) {
        h.skyLevel = h.sky.level();
        const float b = h.skyLevel > 1.f ? 1.f : h.skyLevel;
        const float ev = sims3cam::nightEvMax() + (sims3cam::dayEvMax() - sims3cam::nightEvMax()) * b;
        // a change of a twentieth (at least 0.01 EV) is sent
        const float stepEv = 0.05f * std::fabs(h.evMaxSent) > 0.01f ? 0.05f * std::fabs(h.evMaxSent) : 0.01f;
        const bool due = h.evMaxSent < -98.f || (std::fabs(ev - h.evMaxSent) > stepEv && h.frames - h.skySendFrame >= 10);
        if (due) {
          char val[32];
          snprintf(val, sizeof val, "%.2f", ev); remixapi::remixapi_SetConfigVariable("rtx.autoExposure.evMaxValue", val);
          const bool step = h.skyLoggedB < 0.f || std::fabs(b - h.skyLoggedB) > 0.05f;   // against the last one logged
          h.evMaxSent = ev; h.skySendFrame = h.frames; ++h.skySends;
          if (step && h.skyBrightLogged < 200) {
            h.skyLoggedB = b;
            ++h.skyBrightLogged; char msg[200];
            snprintf(msg, sizeof msg, "Sims 3 camera hook: exposure ceiling %.2f EV sent at frame %u (the light of the sky %.3f; clock %.2f h)",
                     ev, h.frames, h.skyLevel, h.clock.known ? h.clock.hour : -1.f);
            Logger::info(msg);
          }
        }
      }
    } else if (!h.skyApiWarned) {
      h.skyApiWarned = true;
      Logger::warn("Sims 3 camera hook: the exposure is not driven: the Remix API is off (exposeRemixApi = True in .trex\\bridge.conf turns it on for the bridge server)");
    }
  }
  // The game's fog (milestone 56): its colour (the terrain's c2) and its range (c4), read from beside
  // its light record in every view, go to the runtime as D3D9 linear fog on the next frame's first
  // base terrain draws (sims3BeginDraw); the runtime lays it over the ray-traced picture (its
  // composite fog, used while rtx.volumetrics.enable is off). The colour goes in linear light, its
  // hue as the D3DCOLOR and its brightness as rtx.fogColorScale, lit as the scene is lit (milestone
  // 57: x the light of the sky as sent over the game's own, so the night's fog is as dim as the
  // night's moon; run 166 saw a blue veil with the game's full fog colour against a 2 % moon). The
  // fog waits until its first brightness has been sent.
  h.fogReady = false; h.fogFrameDraws = 0;
  if (sims3cam::fogFromGame() && h.lightState == 3 && !h.fogBad && !GlobalOptions::getExposeRemixApi()) {
    if (!h.fogApiWarned) { h.fogApiWarned = true; Logger::warn("Sims 3 camera hook: the game's fog is not sent: the Remix API is off (its brightness goes through it)"); }
  } else if (sims3cam::fogFromGame() && h.lightState == 3 && !h.fogBad) {
    const float* rec = h.lightPlaces[h.lightUse].p;
    float c2[4] = {}, c4[4] = {};
    if (sims3LightRead(rec - 64, c2, 4) && sims3LightRead(rec - 20, c4, 4) &&
        sims3cam::fogColourFromGame(c2, &h.fogColour, &h.fogBright) && sims3cam::fogRangeFromGame(c4, &h.fogStart, &h.fogEnd)) {
      h.fogCurve = c4[3];
      h.fogShare = sims3cam::fogLightShare(h.skySet ? h.sky.level() : 0.f, sims3cam::luminance(h.gameLight.col));
      const float scale = sims3cam::fogColourScale() * h.fogBright * h.fogShare;
      const bool sentBefore = h.fogScaleNow >= 0.f;
      const float stepScale = 0.05f * h.fogScaleNow > 1.0e-6f ? 0.05f * h.fogScaleNow : 1.0e-6f;   // a twentieth, as the exposure
      if (!sentBefore || (std::fabs(scale - h.fogScaleNow) > stepScale && h.frames - h.fogScaleFrame >= 10u)) {
        char val[32];
        snprintf(val, sizeof val, "%.6f", scale);
        remixapi::remixapi_SetConfigVariable("rtx.fogColorScale", val);
        h.fogScaleNow = scale; h.fogScaleFrame = h.frames; ++h.fogScaleSends;
        const bool step = h.fogScaleLogged < 0.f || std::fabs(scale - h.fogScaleLogged) > 0.2f * h.fogScaleLogged;
        if (step && h.fogScaleLines < 200u) {
          h.fogScaleLogged = scale; ++h.fogScaleLines;
          char msg[300];
          snprintf(msg, sizeof msg, "Sims 3 camera hook: fog brightness %.6f sent at frame %u (the fog colour's brightest channel %.4f x the light sent over the game's %.3f x fogColourScale %.2f; clock %.2f h)",
                   scale, h.frames, h.fogBright, h.fogShare, sims3cam::fogColourScale(), h.clock.known ? h.clock.hour : -1.f);
          Logger::info(msg);
        }
      }
      h.fogReady = sentBefore;
      int moved = 0;
      for (int k = 0; k < 3; ++k) {
        const int a = (int) ((h.fogColour >> (8 * k)) & 0xFFu), b = (int) ((h.fogLoggedColour >> (8 * k)) & 0xFFu);
        if ((a > b ? a - b : b - a) > moved) moved = a > b ? a - b : b - a;
      }
      // a line when the hue moves 8 of 255 (run 167: at 2 the dawn's drift used up the cap by 5.7 h), the
      // end a fifth or the curve a quarter (the zoom moves both), and at every mark whatever the cap
      const bool step = h.fogLoggedEnd < 0.f || moved > 8 || std::fabs(h.fogEnd - h.fogLoggedEnd) > 0.2f * h.fogLoggedEnd || std::fabs(h.fogCurve - h.fogLoggedCurve) > 0.25f;
      if ((step && h.fogLogged < 200u) || h.markDump == 2) {
        h.fogLoggedColour = h.fogColour; h.fogLoggedEnd = h.fogEnd; h.fogLoggedCurve = h.fogCurve; ++h.fogLogged;
        char msg[340];
        snprintf(msg, sizeof msg, "Sims 3 camera hook: the game's fog at frame %u, clock %.2f h: colour %.3f %.3f %.3f, from %.0f to %.0f (curve %.2f) -> the runtime's fog colour %u %u %u of 255 (linear hue), from %.0f to %.0f",
                 h.frames, h.clock.known ? h.clock.hour : -1.f, c2[0], c2[1], c2[2], (1.f - c4[1]) / c4[0], -c4[1] / c4[0], c4[3],
                 (unsigned) ((h.fogColour >> 16) & 0xFFu), (unsigned) ((h.fogColour >> 8) & 0xFFu), (unsigned) (h.fogColour & 0xFFu), h.fogStart, h.fogEnd);
        Logger::info(msg);
      }
    }
  }
}

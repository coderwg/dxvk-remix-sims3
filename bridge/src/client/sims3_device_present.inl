// The Sims 3 camera hook, part of d3d9_device.cpp: the statistics lines and the frame's end
// (sims3OnPresent, called by Present). Included at file scope, after sims3_device_lights.inl and
// before Present; not a standalone header.

// The Sims 3 camera hook: the statistics line, plus the per-shader capture table since the last
// table when asked. Also printed once at shutdown (sims3LogFinalStats), so a session that ends
// before the next periodic line still reports what happened in the lot.
static void sims3LogStats(bool withTable) {
  const Sims3Hook& h = g_sims3;
  char msg[640];
  int n = snprintf(msg, sizeof msg, "Sims 3 camera hook: after %u frames -- capture: %u draws, albedo from the table %u, from bytecode %u (%u variants made, %u draws without a candidate), tabled albedo with the coordinate from bytecode %u, %u invisible draws skipped, %u draws with the variant table full; presented stage:",
                   h.frames, h.capturedDraws, h.overrideDraws, h.autoDraws, h.autoVariantsMade, h.autoNoAlbedo, h.autoTexcoordDraws, h.invisibleDrawsSkipped, h.vsVariantsFull);
  for (int k = 0; k < 16 && n > 0 && n < (int) sizeof msg - 16; ++k)
    if (h.remapCount[k]) n += snprintf(msg + n, sizeof msg - n, " s%d=%u", k, h.remapCount[k]);
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   normals: %u variants, %u draws with the shader's normal, %u with the input hidden, vs_2_0 rewritten %u (failed %u); instanced draws split %u (%u instances)",
           h.normalVariantsMade, h.normalDraws, h.normalHiddenDraws, h.vsConverted, h.vsConvertFailed, h.deinstancedDraws, h.deinstancedInstances);
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   state: the game's put back %u times; masked writes emulated %u + %u + %u copied (skipped %u, copy failed %u)",
           h.restoreCount, h.maskEmuA, h.maskEmuB, h.maskEmuC, h.maskEmuSkipped, h.copyFailed);
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   lights: %u lamp lights held (lit %u times, put out %u times, %u light calls)", h.lamps.n, h.lamps.lit, h.lamps.out, h.lampEvents);
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   sky: the game's light is the %s's, luminance %.3f; sun %s luminance %.3f toward %.3f, %.3f, %.3f; moon %s luminance %.3f toward %.3f, %.3f, %.3f (share %.2f); the sun's afterglow %s; %u light updates, %u terrain draws in the last frame, %u frames without one, %u refused",
           h.sky.body ? "moon" : "sun", sims3cam::luminance(h.gameLight.col),
           h.sky.showing[0] ? (h.sky.glowing ? "its afterglow," : "sent,") : "none,", h.sky.showing[0] ? sims3cam::luminance(h.sky.shown[0].col) : 0.f, h.sky.shown[0].dir[0], h.sky.shown[0].dir[1], h.sky.shown[0].dir[2],
           h.sky.showing[1] ? (h.sky.body == 0 ? "kept," : "sent,") : "none,", h.sky.showing[1] ? sims3cam::luminance(h.sky.shown[1].col) : 0.f, h.sky.shown[1].dir[0], h.sky.shown[1].dir[1], h.sky.shown[1].dir[2], sims3cam::moonShare(),
           h.sky.glowing ? format_string("at x%.2f", h.sky.glowFade).c_str() : "none", h.sunChanges, h.terrainSunDrawsLast, h.framesNoTerrainSun, h.sunRefused);
  Logger::info(msg);
  {
    const uint64_t nowTick = GetTickCount64();
    if (g_sims3.statsTick != 0 && nowTick > g_sims3.statsTick && h.frames > g_sims3.statsFrame) {
      snprintf(msg, sizeof msg, "Sims 3 camera hook:   frame rate: %.1f frames a second over the last %u frames",
               (double) (h.frames - g_sims3.statsFrame) * 1000.0 / (double) (nowTick - g_sims3.statsTick), h.frames - g_sims3.statsFrame);
      Logger::info(msg);
    }
    g_sims3.statsTick = nowTick; g_sims3.statsFrame = h.frames;
  }
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   exposure: the light of the sky %.3f, exposure ceiling %.2f EV sent %u times; sky brightness %s%s",
           h.skyLevel, h.evMaxSent, h.skySends, h.skyBrightnessSent == 1.f ? "1 (the game's own sky)" : "not set", GlobalOptions::getExposeRemixApi() ? "" : " (Remix API off: nothing sent)");
  Logger::info(msg);
  uint32_t lampHandles = 0;
  for (uint32_t k = 0; k < h.lamps.n; ++k) { const sims3cam::Lamp& Lh = h.lamps.lamps[k]; lampHandles += (Lh.api ? 1u : 0u) + (Lh.api2 ? 1u : 0u) + (Lh.api3 ? 1u : 0u); }
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   clock: %s %.2f h, sunrise %.2f, sunset %.2f; the street lamps at %.3f by %s; %u lit lamps of world lights alone left dark",
           !h.clock.known ? "unknown," : (h.clock.night ? "night," : "day,"), h.clock.hour, h.clock.sunrise, h.clock.sunset, h.worldFade,
           h.worldBySwitch ? "the game's night switch" : "the game's word for night", h.lampsWorldDark);
  Logger::info(msg);
  {
    static const char* const kLightState[5] = { "not searched yet", "searching", "confirming against the terrain", "found", "none found (searched again later)" };
    snprintf(msg, sizeof msg, "Sims 3 camera hook:   the game's own light: %s%s; %u searches, %u records at the last, %u frames taken from it",
             kLightState[h.lightState < 0 || h.lightState > 4 ? 0 : h.lightState], h.lightState == 3 ? format_string(" at %p", (const void*) h.lightPlaces[h.lightUse].p).c_str() : "",
             h.lightSearches, h.lightPlaceN, h.framesFromGame);
  }
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   fog: %s; the runtime's fog hue %u %u %u of 255 at brightness %.6f (the colour's %.4f x the light sent over the game's %.3f; %u sends) from %.0f to %.0f (the game's curve %.2f), on %u terrain draws; %u captured draws carried a fog state of the game's own",
           !sims3cam::fogFromGame() ? "off (fogFromGame 0)" : (h.fogBad ? "OFF: the game's fog did not agree with the terrain's" : (!GlobalOptions::getExposeRemixApi() ? "OFF: the Remix API is off" : (h.fogReady ? "the game's own, from beside its light record" : "waiting for the game's light record"))),
           (unsigned) ((h.fogColour >> 16) & 0xFFu), (unsigned) ((h.fogColour >> 8) & 0xFFu), (unsigned) (h.fogColour & 0xFFu), h.fogScaleNow, h.fogBright, h.fogShare, h.fogScaleSends, h.fogStart, h.fogEnd, h.fogCurve, h.fogDraws, h.gameFogDraws);
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   lamp reporter: %s; %u lamps reported, %u lit, %u of those without a definition in the light table, %u beyond the budget of %u; %u readings (%u while it was writing), %u searches (the last through %u regions, %u MB)",
           h.lampReportLive ? "live" : (g_sims3LampBlock.load() ? "found, no world loaded" : "NOT FOUND: no lamp gives light (the script mod Sims3RtxLamps.package is not in Mods\\Packages, is an older version, or no world has loaded yet)"),
           h.lampReported, h.lampsOn, h.lampsUndefined, h.lampsBeyondBudget, sims3cam::lampMax(), h.lampReportReads, h.lampReportStale, g_sims3LampScans.load(), g_sims3LampScanRegions.load(), g_sims3LampScanMegabytes.load());
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   api lights: %s; %u calls, sun %s, %u lamp handles; the game's light table: %s",
           GlobalOptions::getExposeRemixApi() ? "on" : "off (no lights: exposeRemixApi is not set)", h.apiLightCalls, (h.sunApi || h.moonApi) ? (h.sunApi && h.moonApi ? "and moon live" : "or moon live") : "and moon none", lampHandles,
           !sims3cam::liteTable().models.empty() ? format_string("%u models and %u objects read from sims3lights.txt", (unsigned) sims3cam::liteTable().models.size(), (unsigned) sims3cam::liteTable().objects.size()).c_str()
             : (sims3cam::liteTable().lines ? "sims3lights.txt is of an older format (no lamps; run sims3/tools/lite_table.py)" : "no sims3lights.txt next to the DLL (no lamps; run sims3/tools/lite_table.py)"));
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   walls: %u draws, openings cut in %u (%u triangles cut, %u removed, %u hidden dropped; %u geometries built, %u evicted, %u build failures, %u refused; %u without an opening test, %u skipped with no client copy; %u masks decoded)",
           h.wallDraws, h.wallCutDraws, h.wallCutTriangles, h.wallRemovedTriangles, h.wallHiddenTriangles, h.wallBuilt, h.wallEvicted, h.wallBuildFailed, h.wallRefused, h.wallNoOpeningTest, h.wallSkipped, h.wallMasksDecoded);
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   reflections: %u mirrored camera uploads; %u draws dropped in %u frames (%u of them by the stencil mirror's render states alone)",
           h.mirroredUploads, h.reflectionDrops, h.reflectionFrames, h.reflectionDropsByStates);
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   terrain: %u base draws and %u layer passes (%u lot chunk copies) for the baker, %u without a variant; %u pixel shader variants (%u unlit, %u with alpha forced to 1), %u sampler states copied, sRGB sampling turned off %u times, both held across %u terrain blocks (%u holds cancelled by the game, %u uncaptured draws let through); markers %s; %u lot chunk copies dropped",
           h.terrainBaseDraws, h.terrainLayerDraws, h.terrainLotCopyDraws, h.terrainNoVariant, h.psVariantsMade, h.psVariantsUnlit, h.psVariantsAlpha, h.samplerCopies, h.srgbOffs, h.tblockFlushes, h.tblockCancelled, h.tblockKept, h.markersConfigSent ? "tagged in rtx.conf" : (h.marker[0] ? "made, NOT tagged in rtx.conf" : "not made yet"), h.lotCopyDrops);
  Logger::info(msg);
  {
    uint32_t ready = 0; uint64_t kept = 0, skirts = 0, flat = 0;
    for (const auto& s : h.squares) if (s.ready) { ++ready; kept += s.kept; skirts += s.skirts; flat += s.flat; }
    snprintf(msg, sizeof msg, "Sims 3 camera hook:   terrain squares (design B): %u squares known, %u with a merged shape (%llu triangles kept, %llu skirt and %llu flat ones left out); %u merged draws, %u pieces painting only, %u pieces traced as before (no shape yet), %u shapes built (%u failed), %u skipped, %u released",
             (unsigned) h.squares.size(), ready, (unsigned long long) kept, (unsigned long long) skirts, (unsigned long long) flat,
             h.mergedDraws, h.mergePaintPieces, h.mergeFallbackPieces, h.mergeBuilds, h.mergeBuildFailed, h.mergeSkipped, h.mergeEvicted);
    Logger::info(msg);
    snprintf(msg, sizeof msg, "Sims 3 camera hook:   the neighbourhood view's lot picture (milestone 63): %u draws left out", h.lotPictureDropped);
    Logger::info(msg);
    snprintf(msg, sizeof msg, "Sims 3 camera hook:   cut-outs (milestones 67-68): %u pixel shaders read with a cut-out on a sampler's alpha; %u captured draws given it as an alpha test",
             h.cutShaders, h.alphaCutDraws);
    Logger::info(msg);
    snprintf(msg, sizeof msg, "Sims 3 camera hook:   glass (milestones 80-82): %u draws presented with the glass marker (hash 0x%016llX%s); left out: %u repeats of a pane already sent in the frame, %u planar-reflection surface passes",
             h.glassDraws, (unsigned long long) h.glassMarkerHash, h.glassMarker ? "" : ", not created yet", h.glassRepeats, h.mirrorSurfaceDropped);
    Logger::info(msg);
    snprintf(msg, sizeof msg, "Sims 3 camera hook:   albedo sampler states (milestone 68): %u draws moved an albedo to stage 0 with sampler states differing from stage 0's, %u of them its sRGB flag",
             h.remapSamplerDraws, h.remapSrgbDraws);
    Logger::info(msg);
    snprintf(msg, sizeof msg, "Sims 3 camera hook:   low-detail lots' ground (milestone 69): %s; %u model draws split (%u plate triangles baked as terrain), %u models split, %u with no plate, %u could not be read",
             sims3cam::terrainLotPlate() ? "the plate's top as terrain (terrainLotPlate 1)" : "one object (terrainLotPlate 0)", h.plateDraws, h.plateTriangles, h.plateBuilds - h.plateNone - h.plateFailed, h.plateNone, h.plateFailed);
    Logger::info(msg);
    snprintf(msg, sizeof msg, "Sims 3 camera hook:   low-detail lots' window glow (milestones 70-71): %s; %u glow passes (%u triangles, %.0f cm out from the wall), %u models with glowing windows, %u window-only glow textures (%u could not be made)",
             sims3cam::lotGlow() ? "on (lotGlow 1)" : "off (lotGlow 0)", h.glowDraws, h.glowTriangles, sims3cam::kLotGlowLift * 100.f, h.glowModels, h.glowTexMade, h.glowTexFailed);
    Logger::info(msg);
  }
  MEMORYSTATUSEX ms = {}; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   lot paint: %u composite second passes, %u lot re-submissions split in two; client copies of surfaces %u MB, address space in use %u of %u MB",
           h.compositePasses, h.splitDraws, (unsigned) (Direct3DSurface9_LSS::sims3ShadowBytes() >> 20), (unsigned) ((ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20), (unsigned) (ms.ullTotalVirtual >> 20));
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   cameras: transforms sent to the runtime %u times; sky dome draws presented as the sky %u",
           h.transformSends, h.skyDraws);
  Logger::info(msg);
  if (!withTable) return;
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   captured draws per shader pair since the last table (%d pairs%s; textures as first seen, render states as last seen):", h.shaderStatCount, h.shaderStatCount >= Sims3Hook::kShaderStats ? ", table full" : "");
  Logger::info(msg);
  for (int i = 0; i < h.shaderStatCount; ++i) {
    const auto& s = h.shaderStats[i];
    const char* vsName = "-";
    if (const sims3cam::ShaderPatch* p = sims3cam::findShaderPatch(s.vs)) vsName = p->name;
    else if (const sims3cam::TexcoordPromote* t = sims3cam::findTexcoordPromote(s.vs)) vsName = t->name;
    const char* psName = "-";
    if (const sims3cam::AlbedoStage* a = sims3cam::findAlbedoStage(s.ps)) psName = a->name;
    int n2 = snprintf(msg, sizeof msg, "Sims 3 camera hook:   %6u draws  VS %016llx [%s]  PS %016llx [%s]  stage %d  rs: cull %u blend %u %u/%u z %u/%u atest %u cw %x st %u  tex:",
                      s.draws, (unsigned long long) s.vs, vsName, (unsigned long long) s.ps, psName, s.stage,
                      (unsigned) s.cull, (unsigned) s.blend, (unsigned) s.src, (unsigned) s.dst, (unsigned) s.zw, (unsigned) s.zf, (unsigned) s.atest, (unsigned) s.cw, (unsigned) s.stencil);
    if (s.instDraws && n2 > 0 && n2 < (int) sizeof msg - 48) n2 += snprintf(msg + n2, sizeof msg - n2, " instanced %u (up to %u per draw)", s.instDraws, (unsigned) s.instMax);
    for (int t = 0; t < 8 && n2 > 0 && n2 < (int) sizeof msg - 48; ++t) {
      if (!s.kind[t]) continue;
      char fb[16];
      if ((s.kind[t] & 0x7f) == 1)
        n2 += snprintf(msg + n2, sizeof msg - n2, " s%d=%s %ux%u%s", t, sims3FormatName(s.fmt[t], fb, sizeof fb), (unsigned) s.w[t], (unsigned) s.h[t], (s.kind[t] & 0x80) ? "(RT)" : "");
      else
        n2 += snprintf(msg + n2, sizeof msg - n2, " s%d=%s", t, (s.kind[t] & 0x7f) == 2 ? "CUBE" : "VOLUME");
    }
    Logger::info(msg);
  }
  g_sims3.shaderStatCount = 0;
}

// Called from the client's shutdown path (d3d9_lss.cpp) so the last stretch of the session is reported.
void sims3LogFinalStats() {
  if (sims3cam::enabled() && g_sims3.frames > 0) sims3LogStats(true);
}

// The frame's end, from Present: the periodic diagnostics (the summary line every 600 frames --
// short sessions still get one from inside the lot -- and the per-shader capture table every
// 3600), the frame's bookkeeping and the mark key, then the lamps and the lights of the sky.
template<typename Dev>
void sims3OnPresent(Sims3Hook& h, Dev* dev) {
  ++h.frames;
  if (h.frames == 300 || h.frames % 600 == 0) {
    sims3LogStats(h.frames == 300 || h.frames % 3600 == 0);
  }
  h.frameCamSet = false;
  h.glassSent.clear();   // the glass draws of the frame (milestone 82)
  h.lotCopies.clear();   // the lot meshes drawn this frame (milestone 16)
  sims3SquaresFrameEnd(h, dev);   // the squares' shapes for the next frame (milestone 60)
  sims3TerrainBlockEnd(h, dev);   // the frame is over: the game's sampler states back (milestone 18g)
  {
    const bool f9 = ((GetAsyncKeyState(VK_F9) | GetAsyncKeyState(sims3cam::markKey()) | GetAsyncKeyState(VK_OEM_3)) & 0x8000) != 0;   // F9, the configured key (sims3hook.txt markKey) or backtick
    if (f9 && !h.f9Down) { h.markDump = 2; h.diagLines = 0; }   // the lit lamps and the fog, logged once; the diagnostic's draws of two frames
    else if (h.markDump) --h.markDump;
    h.f9Down = f9;
  }
  sims3PresentLamps(h);
  sims3PresentSky(h, dev);
}

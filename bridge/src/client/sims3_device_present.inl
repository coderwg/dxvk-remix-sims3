// The Sims 3 camera hook, part of d3d9_device.cpp: the statistics lines and the frame's end
// (sims3OnPresent, called by Present). Included at file scope, after sims3_device_lights.inl and
// before Present; not a standalone header.

// The Sims 3 camera hook (milestone 147): the statistics, one line per system, every 600 frames and
// once at shutdown (sims3LogFinalStats), so a session that ends before the next one still reports.
static void sims3LogStats() {
  const Sims3Hook& h = g_sims3;
  char msg[900];
  float fps = 0.f;
  {
    const uint64_t nowTick = GetTickCount64();
    if (g_sims3.statsTick != 0 && nowTick > g_sims3.statsTick && h.frames > g_sims3.statsFrame)
      fps = (float) ((double) (h.frames - g_sims3.statsFrame) * 1000.0 / (double) (nowTick - g_sims3.statsTick));
    g_sims3.statsTick = nowTick; g_sims3.statsFrame = h.frames;
  }
  int n = snprintf(msg, sizeof msg, "Sims 3 camera hook: after %u frames (%.1f a second) -- capture: %u draws, albedo from the table %u, from bytecode %u (%u variants made, %u draws without a candidate), tabled albedo with the coordinate from bytecode %u, %u invisible draws skipped, %u draws with the variant table full; presented stage:",
                   h.frames, fps, h.capturedDraws, h.overrideDraws, h.autoDraws, h.autoVariantsMade, h.autoNoAlbedo, h.autoTexcoordDraws, h.invisibleDrawsSkipped, h.vsVariantsFull);
  for (int k = 0; k < 16 && n > 0 && n < (int) sizeof msg - 16; ++k)
    if (h.remapCount[k]) n += snprintf(msg + n, sizeof msg - n, " s%d=%u", k, h.remapCount[k]);
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   draws: %u normal variants, %u draws with the shader's normal, %u with the input hidden, vs_2_0 rewritten %u (failed %u); instanced draws split %u (%u instances); the game's state put back %u times; masked writes emulated %u + %u + %u copied (skipped %u, copy failed %u); %u state changes not made (the hook's undo log full)",
           h.normalVariantsMade, h.normalDraws, h.normalHiddenDraws, h.vsConverted, h.vsConvertFailed, h.deinstancedDraws, h.deinstancedInstances,
           h.restoreCount, h.maskEmuA, h.maskEmuB, h.maskEmuC, h.maskEmuSkipped, h.copyFailed, h.calls.full);
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   cameras: transforms sent %u times, %u uploads without their eye taken as the camera continued, %u sky dome draws; dropped: %u shadow map, %u sky cube, %u water reflection, %u reflection-pass draws in %u frames (%u mirrored uploads), %u of the game's own fakes, %u world surfaces left out, %u blended copies",
           h.transformSends, h.continuedUploads, h.skyDraws, h.unshownDrops[sims3cam::kShadowMapPass], h.unshownDrops[sims3cam::kSkyCubePass], h.unshownDrops[sims3cam::kWaterReflectionPass],
           h.reflectionDrops, h.reflectionFrames, h.mirroredUploads, h.dropped[sims3cam::kGameFake], h.dropped[sims3cam::kLeftOut], h.dropped[sims3cam::kBlendedCopy]);
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   sky: the game's light record %s (%u frames lit by it); clock %s %.2f h (sunrise %.2f, sunset %.2f); the game's light is the %s's, luminance %.3f; sun %s %.3f toward %.3f, %.3f, %.3f; moon %s %.3f toward %.3f, %.3f, %.3f; afterglow %s; %u light updates; the street lamps at %.3f by the game's night switch",
           h.lightRec ? "reached" : "NOT reached", h.framesFromGame, !h.clock.known ? "unknown," : (h.clock.night ? "night," : "day,"), h.clock.hour, h.clock.sunrise, h.clock.sunset,
           h.sky.body ? "moon" : "sun", sims3cam::luminance(h.gameLight.col),
           h.sky.showing[0] ? (h.sky.glowing ? "its afterglow" : "sent") : "none", h.sky.showing[0] ? sims3cam::luminance(h.sky.shown[0].col) : 0.f, h.sky.shown[0].dir[0], h.sky.shown[0].dir[1], h.sky.shown[0].dir[2],
           h.sky.showing[1] ? (h.sky.body == 0 ? "kept" : "sent") : "none", h.sky.showing[1] ? sims3cam::luminance(h.sky.shown[1].col) : 0.f, h.sky.shown[1].dir[0], h.sky.shown[1].dir[1], h.sky.shown[1].dir[2],
           h.sky.glowing ? format_string("at x%.2f", h.sky.glowFade).c_str() : "none", h.sunChanges, h.worldFade);
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   fog: %s; the runtime's fog hue %u %u %u of 255 at brightness %.6f (the colour's %.4f x the light sent over the game's %.3f; %u sends) from %.0f to %.0f (the game's curve %.2f), on %u terrain draws",
           !GlobalOptions::getExposeRemixApi() ? "OFF: the Remix API is off" : (h.fogReady ? "the game's own" : "waiting for the game's light record"),
           (unsigned) ((h.fogColour >> 16) & 0xFFu), (unsigned) ((h.fogColour >> 8) & 0xFFu), (unsigned) (h.fogColour & 0xFFu), h.fogScaleNow, h.fogBright, h.fogShare, h.fogScaleSends, h.fogStart, h.fogEnd, h.fogCurve, h.fogDraws);
  Logger::info(msg);
  uint32_t lampHandles = 0;
  for (uint32_t k = 0; k < h.lamps.n; ++k) { const sims3cam::Lamp& Lh = h.lamps.lamps[k]; lampHandles += (Lh.api ? 1u : 0u) + (Lh.api2 ? 1u : 0u) + (Lh.api3 ? 1u : 0u); }
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   lamps: the reporter %s; %u reported, %u lit (%u without a definition, %u of world lights alone left dark, %u beyond the budget); %u lights held (lit %u times, put out %u times); API lights %s, %u calls, %u lamp handles; the light table %s",
           h.lampReportLive ? "live" : (g_sims3LampBlock.load() ? "found, no world loaded" : "NOT FOUND: no lamp gives light (the script mod Sims3RtxLamps.package is not in Mods\\Packages, is an older version, or no world has loaded yet)"),
           h.lampReported, h.lampsOn, h.lampsUndefined, h.lampsWorldDark, h.lampsBeyondBudget, h.lamps.n, h.lamps.lit, h.lamps.out,
           GlobalOptions::getExposeRemixApi() ? "on" : "OFF (exposeRemixApi is not set: no lights)", h.apiLightCalls, lampHandles,
           !sims3cam::liteTable().models.empty() ? format_string("%u models", (unsigned) sims3cam::liteTable().models.size()).c_str()
             : (sims3cam::liteTable().lines ? "of an older format (no lamps; run sims3/tools/lite_table.py)" : "missing next to the DLL (no lamps; run sims3/tools/lite_table.py)"));
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   walls: %u draws, openings cut in %u (%u triangles cut, %u removed, %u hidden dropped; %u geometries built, %u evicted, %u build failures, %u refused; %u without an opening test, %u skipped with no client copy; %u masks decoded)",
           h.wallDraws, h.wallCutDraws, h.wallCutTriangles, h.wallRemovedTriangles, h.wallHiddenTriangles, h.wallBuilt, h.wallCache.evicted, h.wallBuildFailed, h.wallRefused, h.wallNoOpeningTest, h.wallSkipped, h.wallMasksDecoded);
  Logger::info(msg);
  {
    uint32_t ready = 0; uint64_t kept = 0, skirts = 0, flat = 0;
    for (const auto& s : h.squares) if (s.ready) { ++ready; kept += s.kept; skirts += s.skirts; flat += s.flat; }
    snprintf(msg, sizeof msg, "Sims 3 camera hook:   terrain: %u base draws and %u layer passes (%u lot chunk copies) for the baker, %u left out (no markers or shader variant), %u pixel shader variants; markers %s; squares: %u known, %u with a merged shape (%llu triangles kept, %llu skirt and %llu flat ones left out), %u merged draws, %u pieces painting only, %u traced as before, %u shapes built (%u failed), %u skipped, %u released",
             h.terrainBaseDraws, h.terrainLayerDraws, h.terrainLotCopyDraws, h.terrainLeftOut, h.psVariantsMade,
             h.markersConfigSent ? "tagged in rtx.conf" : (h.marker[0] ? "made, NOT tagged in rtx.conf" : "not made yet"),
             (unsigned) h.squares.size(), ready, (unsigned long long) kept, (unsigned long long) skirts, (unsigned long long) flat,
             h.mergedDraws, h.mergePaintPieces, h.mergeFallbackPieces, h.mergeBuilds, h.mergeBuildFailed, h.mergeSkipped, h.squares.evicted);
    Logger::info(msg);
  }
  MEMORYSTATUSEX ms = {}; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   lots: %u composite second passes, %u re-submissions split in two; low-detail models: %u draws split (%u plate triangles baked as terrain), %u split, %u with no plate, %u could not be read, %u glow passes (%u triangles) on %u models with glowing windows, %u glow textures (%u could not be made); client copies of surfaces %u MB, address space in use %u of %u MB",
           h.compositePasses, h.splitDraws, h.plateDraws, h.plateTriangles, h.plateBuilds - h.plateNone - h.plateFailed, h.plateNone, h.plateFailed,
           h.glowDraws, h.glowTriangles, h.glowModels, h.glowTexMade, h.glowTexFailed,
           (unsigned) (Direct3DSurface9_LSS::sims3ShadowBytes() >> 20), (unsigned) ((ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20), (unsigned) (ms.ullTotalVirtual >> 20));
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   cut-outs: %u pixel shaders read with a cut-out, %u draws given it as an alpha test; trees: %u fade shaders, %u draws given the test on the alpha, %u plant draws sent as %u (%u not split), %u leaf draws faced outward (%u whose constants were not the camera's axes), %u near-camera draws solid (%u left as the game's)",
           h.cutShaders, h.alphaCutDraws, h.fadeShaders, h.fadeTestDraws, h.fadeSplitDraws, h.fadeSplitParts, h.fadeSplitUnused, h.cardDraws, h.cardNotCamera, h.solidFadeDraws, h.solidFadeLeft);
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   glass: %u clear, %u car, %u plumbob, %u mirror; water: %u pool, %u pond, %u sea, %u object; bumpy glass %u draws with their own bump map (%u materials%s, %u still without one); %u placed by their object's WORLD transform (%u not placed); %u sent without their back side (%llu triangles left out, %u meshes not read)",
           h.glassDraws[sims3cam::kClearGlass], h.glassDraws[sims3cam::kCarGlass], h.glassDraws[sims3cam::kPlumbob], h.mirrorDraws,
           h.waterDraws[sims3cam::kWaterPool], h.waterDraws[sims3cam::kWaterPond], h.waterDraws[sims3cam::kWaterSea], h.waterDraws[sims3cam::kWaterObject],
           h.bumpDraws, (unsigned) h.bumpMaterials.size(), h.bumpModRead ? "" : ", not read yet", h.bumpPending,
           h.worldDraws, h.worldFailed, h.glassSideDraws, (unsigned long long) h.glassSideTris, h.glassSideSkipped);
  Logger::info(msg);
}

// The captured draws per shader pair of the marked frame (milestone 147: only at the mark; the
// table is cleared when the key goes down and printed at the end of that frame).
static void sims3LogShaderTable() {
  const Sims3Hook& h = g_sims3;
  char msg[640];
  snprintf(msg, sizeof msg, "Sims 3 camera hook: the marked frame's captured draws per shader pair (%d pairs%s; render states as last seen):", h.shaderStatCount, h.shaderStatCount >= Sims3Hook::kShaderStats ? ", table full" : "");
  Logger::info(msg);
  for (int i = 0; i < h.shaderStatCount; ++i) {
    const auto& s = h.shaderStats[i];
    const char* vsName = "-";
    if (const sims3cam::ShaderPatch* p = sims3cam::findShaderPatch(s.vs)) vsName = p->name;
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
}

// The options in force, once at the start (milestones 147, 155): every key of sims3cam::kHookOptions with
// its value (the file's, or the default, within its range), as sims3hook.txt writes them.
static void sims3LogOptions() {
  std::string msg = "Sims 3 camera hook: options --";
  for (int i = 0; i < sims3cam::kOptions; ++i) msg += format_string("%s %s %d", i ? "," : "", sims3cam::kHookOptions[i].key, sims3cam::optionInt(i));
  Logger::info(msg);
}

// Called from the client's shutdown path (d3d9_lss.cpp) so the last stretch of the session is reported.
void sims3LogFinalStats() {
  if (sims3cam::enabled() && g_sims3.frames > 0) { sims3LogStats(); sims3LogNearTreeCheck(); }   // the milestone-162 check
}

// The frame's end, from Present: the options once, the statistics every 600 frames (short sessions
// still get one from inside the lot), the marked frame's shader table, the frame's bookkeeping and the
// mark key, then the lamps and the lights of the sky.
template<typename Dev>
void sims3OnPresent(Sims3Hook& h, Dev* dev) {
  ++h.frames;
  if (h.frames == 1) sims3LogOptions();
  if (h.frames == 300 || h.frames % 600 == 0) sims3LogStats();
  if (h.markFrame) sims3LogShaderTable();   // the frame that ends here was the marked one
  h.frameCamSet = false;
  h.lotCopies.clear();   // the lot meshes drawn this frame (milestone 16)
  h.wallFrameTris.clear();   // the wall triangles kept this frame (milestone 97)
  sims3SquaresFrameEnd(h, dev);   // the squares' shapes for the next frame (milestone 60)
  sims3TerrainBlockEnd(h, dev);   // the frame is over: the game's sampler states back (milestone 18g)
  {
    const bool down = (GetAsyncKeyState(sims3cam::markKey()) & 0x8000) != 0;   // the mark key (sims3hook.txt markKey)
    h.markFrame = down && !h.markDown;   // the frame after the key: the lit lamps and the fog, logged once; the frame's glass (milestone 113)
    if (h.markFrame) { h.markGlassCount = 0; h.shaderStatCount = 0; }
    h.markDown = down;
  }
  sims3PresentLamps(h);
  sims3PresentSky(h);
}

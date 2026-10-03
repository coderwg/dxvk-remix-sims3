// The Sims 3 camera hook, part of d3d9_device.cpp: the hook's state (Sims3Hook, g_sims3), the lamp
// and light searches' globals, the forward declarations and the hook's own buffers. Included first,
// inside that file's first anonymous namespace; not a standalone header.

// State of the Sims 3 camera hook (see sims3_camera_hook.h). The constant-upload hook
// maintains the verified main camera; binding a declaration/FVF maintains declIs3D; each
// draw decides which transforms the runtime should hold and sends them only on change.
struct Sims3Hook {
  sims3cam::Held held;                 // what the runtime currently holds
  sims3cam::Camera cam = {};           // last verified main camera
  bool cameraValid = false;            // false until verified, and after a reflection-pass camera
  bool declIs3D = false;               // bound vertex layout has a 3-component POSITION
  const sims3cam::ShaderPatch* patch = nullptr;   // constant patch rule of the bound vertex shader
  uint8_t vsNeverCapture = 0;          // bound vertex shader's draws left to rasterization: 1 all of them, 2 the alpha-blended ones (neverCaptureMode)
  // The sky dome (runs 70-72): its draws are captured with a depth-1 viewport, which the runtime
  // takes as "this draw is the sky", and its first 2D texture presented at stage 0 (the cube map
  // there has no hash and would drop the draw).
  bool vsSkyDome = false;              // the bound vertex shader is a sky dome's (isSkyDomeShader)
  bool viewportOurs = false; D3DVIEWPORT9 gameViewport = {};   // the game's viewport while the depth-1 one is set
  uint32_t skyDraws = 0, skyLogged = 0;
  uint32_t frames = 0;                 // Present count, for periodic diagnostics
  uint32_t capturedDraws = 0, overrideDraws = 0, remapCount[16] = {};   // per-stage remap statistics
  bool rtIsPrimary = true;             // render target 0 is backbuffer-sized (the 3D pass)
  IDirect3DBaseTexture9* boundTex[16] = {};   // textures the game bound, per stage
  bool boundColor2D[16] = {};          // ...and whether each is a 2D colour texture (not a render target)
  uint32_t boundFmt[16] = {};          // D3DFORMAT per stage (diagnostics)
  uint16_t boundW[16] = {}, boundH[16] = {};
  uint8_t boundKind[16] = {};          // 0 none, 1 2D, 2 cube, 3 volume; +0x80 render target
  bool inRemap = false;                // our own SetTexture calls must not update the tracking above
  IDirect3DBaseTexture9* remapRestore = nullptr;
  bool remapActive = false;
  // The albedo's own sampler states, moved to stage 0 with it (milestone 68): the runtime reads
  // stage 0's -- the sRGB flag picks the decode, the addressing and filters the sampling.
  DWORD remapSamplerSaved[7] = {}; uint8_t remapSamplerSet = 0;
  uint32_t remapSamplerDraws = 0, remapSrgbDraws = 0, remapSamplerLogged = 0;
  uint64_t remapSamplerLoggedPs[24] = {};
  uint32_t cutShaders = 0, cutLogged = 0; uint64_t cutLoggedPs[32] = {};   // pixel shaders analysed with a cut-out; their first draws logged (milestone 68)
  bool loggedMain = false, loggedOther = false, loggedDraw3D = false, loggedDraw2D = false, loggedRemap = false;
  uint32_t loggedPatches = 0;          // bit per rule index, so each patch is announced once
  int psAlbedoStage = -1;              // bound pixel shader's known diffuse stage, or -1
  int psTintReg = -1;                  // bound pixel shader's tint constant register, or -1
  uint32_t sentFactor = 0xFFFFFFFFu;   // D3DRS_TEXTUREFACTOR the runtime currently holds
  uint8_t tssOurs = 0;                 // stage 0's COLOROP / COLORARG1 / COLORARG2 (bits 0..2) currently hold the hook's TFACTOR modulation
  uint32_t loggedTintRegs = 0;         // tint registers whose first forwarded value was logged
  // the sun (milestone 41): the directional light the lit terrain shaders were handed, c0 and c1 at the last such draw
  float terrainSunCol[3] = {}, terrainSunDir[3] = {}; uint32_t terrainSunDraws = 0, terrainSunDrawsLast = 0;
  float terrainNightSwitch = 0.f;      // the lit terrain's c7.x: the game's night switch as handed to the terrain (milestone 54)
  uint32_t nightSwitchDisagree = 0, streetLogged = 0; int streetSaid = -1; bool nightSwitchBad = false;   // the night switch next to the light record, checked against it
  float terrainFog[8] = {};            // the lit terrain's c2 and c4: the game's fog as handed to the terrain (milestone 56)
  bool fogReady = false, fogOurs = false, fogBad = false, fogApiWarned = false; DWORD fogSaved[5] = {};   // the game's fog for the runtime (milestone 56)
  float fogBright = 0.f, fogShare = 0.f, fogScaleNow = -1.f, fogScaleLogged = -1.f; uint32_t fogScaleFrame = 0, fogScaleSends = 0;   // its brightness, lit as the scene is lit (milestone 57)
  uint32_t fogColour = 0, fogLoggedColour = 0, fogFrameDraws = 0, fogDraws = 0, gameFogDraws = 0, fogLogged = 0, fogDisagree = 0;
  float fogStart = 0.f, fogEnd = 0.f, fogCurve = 1.f, fogLoggedEnd = -1.f, fogLoggedCurve = -1.f; uint32_t fogScaleLines = 0;   // the fog's two log lines, each with its own cap (milestone 58)
  sims3cam::Sun sun = {}, moon = {};   // the two lights of the sky as the runtime holds them
  bool sunSet = false, moonSet = false, skySet = false, loggedSun = false;
  sims3cam::SkyLights sky;             // the game's one light as the sun's and the moon's, with the sun's afterglow at dusk (milestones 42, 44)
  sims3cam::Sun gameLight = {};        // the game's light as last handed over
  uint32_t skySrcFrames = 0, skySrcLogs = 0; int skySrcCand = -1, skySrcLogged = -1;   // where the sky's light comes from, for the log
  // the game's own light in its memory (milestone 49): the records a search found, how well each agreed with the terrain, the one in use
  struct LightPlace { const float* p; uint32_t checked, matched; };
  LightPlace lightPlaces[64] = {}; uint32_t lightPlaceN = 0, lightConfirmFrames = 0, lightDisagree = 0, lightLiveFrames = 0, lightRetryFrame = 0, lightSearches = 0, framesFromGame = 0;
  int lightState = 0, lightUse = -1;   // 0 not searched, 1 searching, 2 confirming against the terrain, 3 found, 4 none found (searched again later)
  void* moonApi = nullptr; uint32_t twilightLogged = 0; float skyLoggedB = -1.f;
  uint64_t statsTick = 0; uint32_t statsFrame = 0;   // the last statistics, for the frame rate
  uint32_t sunChanges = 0, sunLogged = 0, framesNoTerrainSun = 0, sunRefused = 0, sunLogFrame = 0; float sunLogLum = -1.f, sunLogDir[3] = {};   // the sun trace (milestone 19d)
  bool rsSet[256] = {};                              // render states the game has set at least once (the array's initial values are not trusted)
  uint32_t invisibleDrawsSkipped = 0, invisibleLogged = 0;   // captured draws whose render states make them invisible in-game (colour writes off, ...)
  bool vsCapturedUv = false;           // bound vertex shader's draws sample with its captured TEXCOORD0 output
  bool uvIndexHidden = false;          // stage 0's D3DTSS_TEXCOORDINDEX currently points at the unused set 7
  bool loggedCapturedUv = false;
  uint64_t vsHash = 0, psHash = 0;     // bound shaders' bytecode hashes (0 = none), for the per-shader capture table
  struct ShaderStat { uint64_t vs, ps; uint32_t draws; int stage; uint8_t kind[8]; uint32_t fmt[8]; uint16_t w[8], h[8];
                      uint8_t cull, blend, src, dst, zw, zf, atest, cw, stencil;   // the render states seen at the last draw of the pair
                      uint32_t instDraws; uint16_t instMax; };               // hardware-instanced draws of the pair, and the largest instance count
  static constexpr int kShaderStats = 96;
  ShaderStat shaderStats[kShaderStats] = {};   // captured draws per (vertex shader, pixel shader) since the last table (further pairs are not recorded)
  int shaderStatCount = 0;
  uint32_t loggedShapes = 0;   // light kinds whose first shaped lamp has been logged (milestone 32)
  // the lamp reporter's block (milestones 36, 39): this frame's records, as read
  std::vector<float> lampRecords, lampRecordsNext; std::vector<int32_t> lampInts, lampIntsNext;
  uint32_t lampReported = 0, lampReportReads = 0, lampReportStale = 0, lampReportFails = 0, lampReportScanFrame = 0, lampWordsLogged = 0;
  uint32_t lampsOn = 0, lampsUndefined = 0, lampsBeyondBudget = 0, lampKeyHow = 0;   // this frame: reported lit, of those without a definition, of those beyond lampMax; the ways a definition was found so far
  uint64_t lampUndefinedKeys[64] = {}; uint32_t lampUndefinedCount = 0;   // the models without a definition the log has named
  bool lampReportLive = false, lampReportWorld = false, lampReportAnnounced = false;
  // the game's clock (milestone 40), and the world lights it keeps dark by day
  sims3cam::GameClock clock = {}; bool clockSaid = false, clockNight = false; uint32_t clockLogged = 0, lampsWorldDark = 0;
  float worldFade = 0.f; bool worldBySwitch = false;   // the street lamps' fade, 0..1, and whether it is the game's night switch (milestone 55)
  uint32_t markDump = 0;               // frames left to log after the mark key
  sims3cam::Lamps lamps;               // the game's own lamps, forwarded as Remix API lights
  uint32_t lampEvents = 0;             // API light creations and destructions made for lamps
  // night from the sun (milestone 20d): the sun's luminance smoothed, the day reference, what was sent
  float skyLevel = -1.f, skyBrightnessSent = -1.f, evMaxSent = -99.f; uint32_t skySends = 0, skySendFrame = 0, skyBrightLogged = 0; bool skyApiWarned = false;
  // lights through the Remix API (milestone 20b): the handles of the sun and of each lamp slot (remixapi_LightHandle, declared later in this file)
  void* sunApi = nullptr; uint32_t apiLightCalls = 0; bool loggedApiLights = false, apiLightsWarned = false;   // the lamps' handles live in the solver's lamps
  // untabled pixel shaders (milestone 7): the albedo chosen from the bytecode at draw time,
  // on a promoted variant of the game's vertex shader when its coordinate is not TEXCOORD0
  const sims3cam::PsAnalysis* psAuto = nullptr;   // bound pixel shader's sampler analysis when it has no table entry
  uint8_t psMajor = 0;                            // the bound pixel shader's major version (milestone 56)
  IDirect3DVertexShader9* vsBound = nullptr;      // the vertex shader the game bound (for the variant swap)
  bool vsTabled = false;                          // it has a captured-UV or never-capture entry: no auto coordinate, no variants
  struct VsVariant { IDirect3DVertexShader9* base; uint64_t hash; uint8_t texcoord; uint8_t normalOut; bool constRead; IDirect3DVertexShader9* variant; };
  static constexpr uint32_t kVsVariants = 512;
  VsVariant vsVariants[kVsVariants] = {};         // one per (shader, coordinate, normal, c255 read); keyed by pointer AND hash (an address may be reused); variant null = could not be made
  uint32_t vsVariantCount = 0, vsVariantsFull = 0;   // ...and the draws that found the table full
  const sims3cam::VsNormalInfo* vsNormal = nullptr;   // the bound (game) vertex shader's normal facts
  uint8_t pendingPromote = 0;                     // the coordinate to promote for this draw (0 = none), decided before the variant is bound
  uint32_t normalVariantsMade = 0, normalDraws = 0, normalHiddenDraws = 0, normalLogged = 0, normalShaderLogged = 0;
  uint32_t vsConverted = 0, vsConvertFailed = 0;   // vs_2_x shaders rewritten as vs_3_0 for a NORMAL output (milestone 11b)
  // Hardware-instanced draws (milestone 12): the runtime's vertex capture keeps one slot per
  // vertex, so every instance of an instanced draw overwrites the same slots and the captured
  // triangles mix vertices of different instances (the fence slivers). A captured instanced
  // draw is therefore issued once per instance, the instance stream offset stepping through
  // the instance data, with the frequency of stream 0 set to one instance for the duration.
  bool drawCaptured = false;                      // this draw is captured (set by the draw macro)
  uint32_t deinstancedDraws = 0, deinstancedInstances = 0, deinstanceLogged = 0;
  bool creatingVariant = false;                   // our own CreateVertexShader call: no tables, no dump
  bool swappingVs = false;                        // our own SetVertexShader call: the bound-shader facts stay the game's
  IDirect3DVertexShader9* autoVsRestore = nullptr; // the game's shader to re-bind after a swapped draw
  bool autoCapturedUv = false;                    // this draw samples with the captured TEXCOORD0 by the auto decision
  uint32_t autoDraws = 0, autoVariantsMade = 0, autoNoAlbedo = 0, autoLogged = 0;
  uint32_t autoTexcoordDraws = 0;                 // tabled pixel shader on an untabled vertex shader: coordinate from the bytecode
  // The hook's state changes used to stay on the device after a captured draw -- stage 0's
  // texture-coordinate index at 7, its colour stage rewired to TEXTURE x TFACTOR, the texture factor,
  // View/Projection forced to identity -- and the game's other draws inherited them. The game's own
  // values are tracked here and put back before any draw that is not captured.
  bool ourState = false;                          // our own SetTextureStageState / SetRenderState / SetTransform calls
  DWORD gameTss0[4] = { D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_CURRENT, 0 };   // stage 0: COLOROP, COLORARG1, COLORARG2, TEXCOORDINDEX (D3D defaults until the game sets them)
  DWORD gameFactor = 0xFFFFFFFFu;                 // D3DRS_TEXTUREFACTOR
  bool factorOurs = false;                        // the runtime holds our factor (else the game's)
  uint32_t restoreCount = 0;
  // The compositor packs the Sim textures' channels with partial colour write masks, and the runtime
  // (with ray tracing on) drops any draw whose mask lacks R, G or B before it asks whether the target
  // is an offscreen texture; such draws are emulated with blending and a full mask.
  uint8_t maskEmu = 0; DWORD maskSaved[10] = {}; uint16_t maskEmuBits = 0; uint32_t maskEmuA = 0, maskEmuB = 0, maskEmuSkipped = 0, maskEmuLogged = 0;
  // Blended writes to a subset of the colour channels (the compositor's per-channel pattern masks:
  // colour write 1 / 2 / 4 with blending on, run 50) have no single-pass equivalent with a full mask,
  // so the target is copied first, the draw runs with a full mask and the game's blend, and the
  // channels the game did not write go back from the copy with a constant-factor blend.
  bool ourDraw = false;                           // the restore quad is being drawn (the draw hooks ignore it)
  struct Scratch { uint16_t w = 0, h = 0; uint32_t fmt = 0; IDirect3DTexture9* tex = nullptr; IDirect3DSurface9* surf = nullptr; };
  static constexpr uint32_t kScratch = 4;
  Scratch scratch[kScratch]; uint32_t scratchCount = 0;
  uint16_t rt0W = 0, rt0H = 0; uint32_t rt0Fmt = 0;   // render target 0 as the game set it
  uint8_t copyMask = 0; int copyScratch = -1; uint32_t maskEmuC = 0, maskEmuCopyLogged = 0, copyFailed = 0;
  // Wall openings (milestone 13): the windows and doors the wall pixel shaders cut with a mask
  // are cut into the triangles on the client (sims3_walls.h) and the cut geometry, in the
  // hook's own buffers, is drawn instead of the game's. One entry per (buffers, uploads, range,
  // mask, constants); the least recently used entry goes when the cache is full.
  sims3cam::WallLayout wallLayout;                // the bound vertex declaration's wall elements (valid when it has them)
  uint32_t wallDeclId = 0;                        // ...and the declaration's id (cache key)
  const sims3cam::WallVsInfo* vsWall = nullptr;   // the bound (game) vertex shader's wall facts
  struct WallEntry { uint64_t key; uint32_t lastFrame; bool changed; IDirect3DVertexBuffer9* vb0; IDirect3DVertexBuffer9* vb1; IDirect3DIndexBuffer9* ib; uint32_t vertexCount, triangleCount; };
  static constexpr uint32_t kWallCacheSize = 256;
  WallEntry wallCache[kWallCacheSize] = {}; uint32_t wallCacheCount = 0;
  // the mask atlases decoded, per (texture, upload), and the content hashes of the wall buffers, per
  // (object, upload): the cache key follows the bytes. Sized for the lots of a neighbourhood (16 in
  // full detail since 2026-10-01; four mask slots had every lookup decode again, run 183: 5,777 decodes);
  // the least recently used entry goes (milestone 77).
  struct MaskEntry { bool used = false, ok = false; uint32_t texId = 0, version = 0, lastUse = 0; uint64_t contentHash = 0; std::vector<uint8_t> red; };
  static constexpr uint32_t kMasks = 32, kBufHashes = 256;
  MaskEntry masks[kMasks]; uint32_t maskUse = 0;
  struct HashEntry { bool used; uint32_t id, version, lastUse; uint64_t hash; };
  HashEntry bufHashes[kBufHashes] = {}; uint32_t bufHashUse = 0;
  uint32_t wallDraws = 0, wallCutDraws = 0, wallCutTriangles = 0, wallRemovedTriangles = 0, wallHiddenTriangles = 0, wallBuilt = 0, wallEvicted = 0, wallBuildFailed = 0, wallRefused = 0, wallSkipped = 0, wallNoOpeningTest = 0, wallLogged = 0, wallSkipLogged = 0, wallHiddenLogged = 0, wallMasksDecoded = 0;
  // Reflection passes (runs 66-68): the ray tracer renders reflections itself, so the 3D draws of
  // every reflection pass -- the sea/pool pass, a wall mirror's stencil pass -- are dropped on the
  // client: those under a mirrored camera upload, and those carrying the stencil mirror's render
  // states (draws whose own constants never reach the classifier).
  bool camMirrored = false;                       // the last classified camera upload was a reflection's
  bool cameraValidBeforeMirror = false;           // ...and whether the main camera held before it (milestone 85)
  bool drawDropped = false;                       // this draw is a reflection pass's (set by sims3BeginDraw; the draw returns at once)
  uint32_t mirrorPassEnds = 0;   // mirrored passes ended by a draw culling clockwise (milestone 85)
  uint32_t mirroredUploads = 0, reflectionDrops = 0, reflectionDropsByStates = 0, reflectionFrames = 0, reflectionFrame = 0xFFFFFFFFu, reflectionLogged = 0;
  // The lot terrain drawn once per world chunk (milestone 16, sims3cam::LotCopies): the World
  // rows c4..c6 as last uploaded, the lot meshes drawn this frame, and the copies dropped.
  float rows4to6[12] = {};
  sims3cam::LotCopies lotCopies;
  uint32_t lotCopyDrops = 0, lotCopyLogged = 0;
  bool lotFurtherCopy = false;                    // this draw is a lot mesh drawn again for a further world chunk
  // Terrain paint through the runtime's terrain baker (milestone 17; see kTerrainShaders in
  // sims3_camera_hook.h): a terrain draw gets a marker texture at stage 0, the game's stage-0
  // texture at a free stage, and a pixel shader variant reading it there.
  const sims3cam::TerrainShader* vsTerrain = nullptr;   // the bound (game) vertex shader is a terrain shader
  IDirect3DPixelShader9* psBound = nullptr;       // the pixel shader the game bound (for the variant swap)
  bool swappingPs = false;                        // our own SetPixelShader call: the bound-shader facts stay the game's
  IDirect3DPixelShader9* psRestore = nullptr;     // the game's pixel shader to re-bind after a terrain draw
  int terrainFreeStage = -1;                      // the stage the game's stage-0 texture was moved to for this draw, or -1
  IDirect3DBaseTexture9* freeStageRestore = nullptr;   // ...and what that stage held
  static constexpr int kSamplerCopies = 7;
  // The terrain block (milestone 18g): the free stage's copied sampler states and the stages'
  // sRGB flags stay as the terrain draws want them across consecutive terrain draws and go back
  // to the game's values at the first other draw or at Present
  // (before: copied and put back for every draw, ~1,400 bridge commands a frame). A game
  // SetSamplerState on a held state cancels that state's restore: the game's value is current.
  bool tblockActive = false; int tblockStage = -1;   // the one free stage whose copied sampler states are held (released on a switch: another terrain draw may read that stage as its own layer, run 116)
  uint8_t tblockSet = 0; DWORD tblockSaved[kSamplerCopies] = {};   // the held copies and the game's values
  uint16_t tblockSrgb = 0;                            // stages whose SRGBTEXTURE the hook holds off
  bool ourSampler = false;                            // our own SetSamplerState calls: no cancelling
  uint32_t tblockFlushes = 0, tblockCancelled = 0, tblockKept = 0;   // ...and the uncaptured draws that touched no held stage
  // The game's own paint composite draw just went out as pass 1; pass 2 follows in place (the
  // draw member re-issues it hidden, blended ONE / ONE; milestone 19).
  bool compositeSecond = false;
  IDirect3DTexture9* marker[sims3cam::kTerrainMarkers] = {};   // the terrain / layer-pass / composite marker textures (terrainMarkerPixels)
  uint64_t markerHash[sims3cam::kTerrainMarkers] = {};         // their level-0 hashes as the runtime computes them (0 = not computed)
  bool markerFailed = false, markersConfigSent = false;
  // glass (milestone 80): the glass marker, and the draw's blending the hook switched off
  IDirect3DTexture9* glassMarkers[sims3cam::kGlassMaterials] = {}; uint64_t glassMarkerHashes[sims3cam::kGlassMaterials] = {};   // per sims3cam::kGlassMaterial
  bool glassMarkerFailed[sims3cam::kGlassMaterials] = {}; uint32_t glassDraws[sims3cam::kGlassMaterials] = {};
  // the water materials' markers (milestones 86, 99), per sims3cam::kWaterMaterial
  IDirect3DTexture9* waterMarkers[sims3cam::kWaterMaterials] = {}; uint64_t waterMarkerHashes[sims3cam::kWaterMaterials] = {};
  bool waterMarkerFailed[sims3cam::kWaterMaterials] = {}; uint32_t waterDraws[sims3cam::kWaterMaterials] = {};
  // bumpy glass (milestone 109, sims3GlassBump): its bump maps' runtime hashes (by texture id and level-0
  // version), the hashes the Sims3GlassBumps mod has a material for (read once), the ones seen without
  std::unordered_map<uint64_t, uint64_t> bumpHashes; std::unordered_set<uint64_t> bumpMaterials, bumpWritten;
  bool bumpModRead = false; uint32_t bumpDraws = 0, bumpPending = 0;
  // the glass survey (milestone 110; one run): a flat marker per sims3cam::kGlassSurvey entry
  IDirect3DTexture9* surveyMarkers[sims3cam::kGlassSurveyMax] = {}; uint64_t surveyHashes[sims3cam::kGlassSurveyMax] = {};
  bool surveyFailed[sims3cam::kGlassSurveyMax] = {}; uint32_t surveyDraws = 0;
  uint64_t markGlassPs[32] = {}; uint32_t markGlassCount = 0;   // the glass shaders named at the mark (milestone 113, with the survey)
  uint32_t waveDumpedIds[32] = {}; uint32_t waveDumped = 0;   // the game's wave maps looked at (milestone 93): the mod's ripple maps' source
  // a zero-thickness wall's back side (milestone 97): each wall piece's triangle keys (cached by buffers,
  // versions and range), and this frame's kept wall triangles per vertex buffer
  std::unordered_map<uint64_t, std::vector<uint64_t>> wallPieceTris;
  std::unordered_map<uint32_t, std::unordered_set<uint64_t>> wallFrameTris;
  uint32_t wallBackDropped = 0, wallBackLogged = 0;
  DWORD blendSaved = 0; bool blendOurs = false;
  uint32_t glassLogged = 0; uint64_t glassLoggedPs[32] = {};
  IDirect3DTexture9* mirrorMarker = nullptr; uint64_t mirrorMarkerHash = 0; bool mirrorMarkerFailed = false; uint32_t mirrorDraws = 0;   // mirrors (milestone 101)
  bool drawGlass = false;               // this draw went out as glass
  bool reflectiveSheet = false;         // this draw is a reflective sheet: a mirror's face (milestone 101)
  const char* dropWhy = "";             // why this draw was left out (the mark key's dump)
  // a glass sheet's back side (milestone 104, sims3GlassOneSide): per mesh, the kept triangles as an
  // index buffer of the hook's own (D3DPOOL_DEFAULT, released at a device reset); this draw's, sent in
  // the game's place (d3d9_device.cpp)
  struct GlassSide { IDirect3DIndexBuffer9* ib = nullptr; uint32_t prims = 0, dropped = 0; };
  std::unordered_map<uint64_t, GlassSide> glassSides;
  IDirect3DIndexBuffer9* glassIb = nullptr; uint32_t glassPrims = 0;
  uint32_t glassSideDraws = 0, glassSideSkipped = 0, glassSideLogged = 0, glassPassDropped = 0; uint64_t glassSideTris = 0;
  const D3DVERTEXELEMENT9* declElems = nullptr;   // the bound declaration's elements (its POSITION, for the back side; its inputs, for the place)
  // glass carries its object's place (milestone 119, sims3GlassWorld): the vertex shaders' bytecode by
  // hash, whether this draw has the hook's WORLD transform, counts
  std::unordered_map<uint64_t, std::vector<DWORD>> vsTokens;
  bool worldOurs = false;
  uint32_t worldDraws = 0, worldFailed = 0, worldLogged = 0;
  // the lot paint composite's two passes (milestone 17l, sims3cam::lotCompositeStage): the pass
  // being issued (0 = the game's own draw, pass 1; 2 = the hook's second pass, milestone 19), and
  // pass 2's black marker at stages 1 and 2 (what they held, put back in sims3EndDraw)
  uint8_t compositePass = 0;
  IDirect3DBaseTexture9* extraRestore[2] = {}; bool extraActive = false;
  uint32_t compositePasses = 0;
  // a lot's re-submissions issued as two half draws (milestone 17r): the runtime's draw tracker
  // files a draw by the hash of its index data and counts, so a half never shares the lot's
  // visible instance (the lift of milestone 17p changed nothing the tracker looks at)
  bool splitDraw = false; uint32_t splitDraws = 0;
  // The hook's own re-issue of a draw (the paint composite's second pass, milestone 19), and the
  // terrain kind it is given.
  bool reissue = false; uint8_t reissueKind = 0;
  bool f9Down = false;                            // the mark key held (milestone 17y)
  bool ourConsts = false;                         // our own SetVertexShaderConstantF calls: no camera classification, no tracking
  struct PsVariant { IDirect3DPixelShader9* base; uint64_t hash; uint8_t alphaMode; uint8_t forced; IDirect3DPixelShader9* variant; uint8_t freeStage; };
  static constexpr uint32_t kPsVariants = 64;
  PsVariant psVariants[kPsVariants] = {}; uint32_t psVariantCount = 0;   // one per (shader, hash, alpha mode); variant null = could not be made
  // The bake's alpha is the terrain's opacity to the ray tracer (0 = the surface is not there), so a
  // base draw writes alpha 1 (terrainAlphaMode) with a full
  // colour mask; and every stage samples raw (non-sRGB) so the bake holds sRGB-encoded texels,
  // which the ray tracer gamma-corrects itself.
  DWORD cwRestore = 0; bool cwOurs = false;      // the game's COLORWRITEENABLE while the hook's full mask is set
  DWORD atSaved[3] = {}; bool atOurs = false;    // the game's ALPHATESTENABLE / ALPHAFUNC / ALPHAREF while the hook's alpha test is set (a cut-out, milestones 67-68)
  uint32_t terrainBaseDraws = 0, terrainLayerDraws = 0, terrainLotCopyDraws = 0, terrainNoVariant = 0, terrainLogged = 0, psVariantsMade = 0, psVariantsUnlit = 0, psVariantsAlpha = 0, psVariantLogged = 0, samplerCopies = 0, srgbOffs = 0;
  // the town ground's squares (milestone 60, design B): per square (one vertex buffer), the
  // opaque pieces the merged shape is made of and the shape's own index buffer
  struct Square {
    IDirect3DVertexBuffer9* vb = nullptr; uint32_t vbId = 0, vbVersion = 0; UINT offset = 0, stride = 0; uint16_t posOffset = 0;
    IDirect3DIndexBuffer9* ib = nullptr; uint32_t ibId = 0, ibVersion = 0; bool ib32 = false; INT base = 0;
    std::vector<uint64_t> ranges, frameRanges;   // the shape's pieces (sorted) and this frame's: start << 32 | triangle count
    uint32_t frameSeen = 0xFFFFFFFFu, mergedFrame = 0xFFFFFFFFu, lastFrame = 0;
    uint32_t builtVbVersion = 0, builtIbId = 0, builtIbVersion = 0; INT builtBase = 0;
    IDirect3DIndexBuffer9* merged = nullptr; uint32_t mergedPrims = 0, minIndex = 0, numVertices = 0; bool ready = false;
    uint32_t kept = 0, skirts = 0, flat = 0;
  };
  uint32_t lotPictureDropped = 0;   // the neighbourhood view's lot picture draws left out (milestone 63)
  uint32_t effectsLeftOut = 0;      // effect draws left out of the ray tracing (milestone 114, sims3cam::leftOutPs)
  uint32_t alphaCutDraws = 0;       // captured draws given their shader's cut-out as an alpha test (milestones 67-68)
  // The low-detail lots' ground plates as terrain (milestone 69): per model draw (by the buffers'
  // content and the draw range) the house's and the plate's own index buffers.
  struct PlateEntry { uint64_t key = 0; IDirect3DIndexBuffer9* house = nullptr; IDirect3DIndexBuffer9* plate = nullptr; IDirect3DIndexBuffer9* glow = nullptr; IDirect3DVertexBuffer9* glowVb = nullptr; uint32_t houseMin = 0, houseNum = 0, housePrims = 0, plateMin = 0, plateNum = 0, platePrims = 0, glowMin = 0, glowNum = 0, glowPrims = 0, glowStride = 0, lastFrame = 0; bool ok = false; };
  std::vector<PlateEntry> plates;
  IDirect3DPixelShader9* platePs = nullptr; bool platePsFailed = false;
  uint32_t plateDraws = 0, plateTriangles = 0, plateBuilds = 0, plateNone = 0, plateFailed = 0, plateLogged = 0;
  uint32_t glowDraws = 0, glowTriangles = 0, glowModels = 0;   // the low-detail lots' window glow passes (milestone 70)
  struct GlowTex { uint64_t key = 0; IDirect3DTexture9* tex = nullptr; uint32_t lastFrame = 0; };   // window-only copies of the glow atlases (milestone 71)
  std::vector<GlowTex> glowTexs; uint32_t glowTexMade = 0, glowTexFailed = 0;
  std::vector<Square> squares; int mergePending = -1;
  uint32_t mergedDraws = 0, mergePaintPieces = 0, mergeFallbackPieces = 0, mergeBuilds = 0, mergeBuildFailed = 0, mergeLogged = 0, mergeEvicted = 0, mergeSkipped = 0;
  bool drawIndexed = false; D3DPRIMITIVETYPE drawType = D3DPT_TRIANGLELIST; INT drawBase = 0; UINT drawStart = 0, drawPrims = 0;   // the indexed draw call's arguments, for the squares
  bool frameCamSet = false;   // the frame's first main camera is in cam; it holds for the frame (milestone 121)
  uint32_t transformSends = 0;
  // a measure (milestones 120-121): captured draws before the frame's main camera upload (placed with the
  // previous frame's camera) and the frames they fell in
  uint32_t staleCameraDraws = 0, staleCameraFrames = 0, staleCameraFrame = 0xFFFFFFFFu;
} g_sims3;

// the lamp reporter's block (milestone 36): where it was found, and the search for it
std::atomic<const uint8_t*> g_sims3LampBlock { nullptr };
std::atomic<bool> g_sims3LampScanBusy { false };
std::atomic<uint32_t> g_sims3LampScans { 0 };
std::atomic<uint32_t> g_sims3LampScanRegions { 0 }, g_sims3LampScanMegabytes { 0 };   // what the last search read through

// the search for the game's own light in its memory (milestone 49): the direction and colour the
// terrain was handed when the search began, the hook's own memory not to be counted, what was found
constexpr uint32_t kSims3LightHitsMax = 64;
std::atomic<bool> g_sims3LightScanBusy { false }, g_sims3LightScanDone { false };
float g_sims3LightTarget[6] = {};
const uint8_t* g_sims3LightExcludeFrom[2] = {}; const uint8_t* g_sims3LightExcludeTo[2] = {};
const float* g_sims3LightHitAt[kSims3LightHitsMax] = {};
std::atomic<uint32_t> g_sims3LightHitCount { 0 }, g_sims3LightHitOverflow { 0 }, g_sims3LightScanRegions { 0 }, g_sims3LightScanMegabytes { 0 }, g_sims3LightSkippedMapped { 0 };

template<typename Dev> void sims3TerrainBlockEnd(Sims3Hook& h, Dev* dev);   // defined with the terrain path (milestone 18g)
template<typename Dev> uint8_t sims3SquarePiece(Sims3Hook& h, Dev* dev);    // defined with the squares (milestone 60)

// The hook's facts about a bound object, from the object (milestone 17w; defined in
// sims3_device_notes.inl, after the vertex shader and declaration headers are included). The
// setters call them.
void sims3NoteVertexShader(Sims3Hook& h, IDirect3DVertexShader9* pShader);
void sims3NotePixelShader(Sims3Hook& h, IDirect3DPixelShader9* pShader);
void sims3NoteTexture(Sims3Hook& h, DWORD Stage, IDirect3DBaseTexture9* pTexture);
void sims3NoteDecl(Sims3Hook& h, IDirect3DVertexDeclaration9* pDecl);

// A vertex buffer of the hook's own with the given bytes (D3DPOOL_DEFAULT, released at a device
// reset; the walls' cut geometry, the low-detail lots' glow layer).
template<typename Dev>
IDirect3DVertexBuffer9* sims3MakeVertexBuffer(Dev* dev, const std::vector<uint8_t>& bytes) {
  IDirect3DVertexBuffer9* vb = nullptr;
  if (bytes.empty() || FAILED(dev->CreateVertexBuffer((UINT) bytes.size(), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, nullptr)) || !vb) return nullptr;
  void* p = nullptr;
  if (FAILED(vb->Lock(0, 0, &p, 0)) || !p) { vb->Release(); return nullptr; }
  memcpy(p, bytes.data(), bytes.size());
  vb->Unlock();
  return vb;
}
// An index buffer of the hook's own (D3DPOOL_DEFAULT, released at a device reset): 16-bit when every
// index fits, else 32-bit (the walls' cut geometry, the squares' shapes, the low-detail lots' parts).
template<typename Dev, typename Index>
IDirect3DIndexBuffer9* sims3MakeIndexBuffer(Dev* dev, const std::vector<Index>& indices) {
  if (indices.empty()) return nullptr;
  uint32_t maxIndex = 0; for (const Index i : indices) if ((uint32_t) i > maxIndex) maxIndex = (uint32_t) i;
  const bool wide = maxIndex >= 0xFFFFu;
  const size_t bytes = indices.size() * (wide ? 4u : 2u);
  IDirect3DIndexBuffer9* ib = nullptr;
  if (FAILED(dev->CreateIndexBuffer((UINT) bytes, D3DUSAGE_WRITEONLY, wide ? D3DFMT_INDEX32 : D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, nullptr)) || !ib) return nullptr;
  void* p = nullptr;
  if (FAILED(ib->Lock(0, 0, &p, 0)) || !p) { ib->Release(); return nullptr; }
  if (wide) { uint32_t* q = (uint32_t*) p; for (size_t i = 0; i < indices.size(); ++i) q[i] = (uint32_t) indices[i]; }
  else { uint16_t* q = (uint16_t*) p; for (size_t i = 0; i < indices.size(); ++i) q[i] = (uint16_t) indices[i]; }
  ib->Unlock();
  return ib;
}

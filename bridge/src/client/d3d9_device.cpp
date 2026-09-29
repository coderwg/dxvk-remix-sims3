/*
 * Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include "pch.h"
#include "d3d9_device.h"
#include "d3d9_lss.h"
#include "d3d9_util.h"
#include "d3d9_surfacebuffer_helper.h"
#include "d3d9_cubetexture.h"
#include "d3d9_indexbuffer.h"
#include "d3d9_pixelshader.h"
#include "d3d9_query.h"
#include "d3d9_surface.h"
#include "d3d9_swapchain.h"
#include "d3d9_texture.h"
#include "sims3_camera_hook.h"
#include "sims3_walls.h"
#include <atomic>
#include <thread>
// The Sims 3 camera hook (milestone 17): the runtime identifies textures by the XXH3 of their
// level-0 bytes; with xxhash.h next to this file the hook computes its marker textures' hashes
// and sets the runtime's terrain options itself, otherwise the markers are tagged in the menu.
#if __has_include("xxhash.h")
#define XXH_INLINE_ALL
#include "xxhash.h"
#define SIMS3_HAVE_XXHASH 1
#else
#define SIMS3_HAVE_XXHASH 0
#endif

namespace {

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
    bool loggedMain = false, loggedOther = false, loggedDraw3D = false, loggedDraw2D = false, loggedRemap = false;
    uint32_t loggedPatches = 0;          // bit per rule index, so each patch is announced once
    const sims3cam::AlbedoStage* psRig = nullptr;   // bound pixel shader carries the 4-light rig at c0..c7
    int psAlbedoStage = -1;              // bound pixel shader's known diffuse stage, or -1
    int psTintReg = -1;                  // bound pixel shader's tint constant register, or -1
    float tint[3] = { 1.f, 1.f, 1.f };   // last tint constant uploaded for a tinted pixel shader
    uint32_t sentFactor = 0xFFFFFFFFu;   // D3DRS_TEXTUREFACTOR the runtime currently holds
    uint8_t tssOurs = 0;                 // stage 0's COLOROP / COLORARG1 / COLORARG2 (bits 0..2) currently hold the hook's TFACTOR modulation
    bool loggedTint = false;
    sims3cam::SunVoter voter;            // this frame's votes for the sun
    sims3cam::SunVote sun = {};          // the sun currently forwarded as light 0
    bool sunSet = false;
    uint32_t sunChanges = 0, sunLogged = 0, framesNoSunVote = 0, sunDarkHeld = 0; float sunLogLum = -1.f, sunLogDir[3] = {};   // the sun trace (milestone 19d)
    sims3cam::SunVote sunCand = {};      // a different vote winner, waiting out the hysteresis
    uint32_t sunCandFrames = 0;
    int vsShadowReg = -1;                // bound vertex shader's shadow view-projection rows (kShadowSources), or -1
    float shadowDir[3] = {};             // light axis from the last shadow rows seen...
    bool shadowDirValid = false;
    uint32_t shadowDirFrame = 0;         // ...and the frame it was seen in
    bool loggedShadowSun = false;
    uint32_t sunDarkFrames = 0;          // frames the matching candidate has been much darker than the sun held
    uint32_t framesNoShadow = 0;         // frames without shadow rows (statistics)
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
    int vsWorldReg = -1;                 // bound vertex shader's World rows (kWorldRegs), or -1
    float objWorld[3] = {};              // World translation of the bound object shader...
    float objWorldRows[12] = {};         // ...and its three World rows (milestone 22: the game's lights carried to the world)
    float objRowsAfter[8] = {}; bool objRowsAfterValid = false;   // the two rows after World (c15, c16 on the main object shader: the light map projection, milestone 26)
    // the lot's light maps as the lamps' switch (milestones 27-29): every A8R8G8B8 texture the object-shader draws carry
    // at s2 (with the projection rows they upload after World), decoded on the client at each new version; a lamp is
    // judged through the map its own draws carry
    struct LightMapEntry { IDirect3DBaseTexture9* tex; uint32_t version, judged, W, H, lastFrame, draws; float rows[8]; bool rowsValid, shared, valid; float origin[3]; std::vector<uint8_t> data;
                           std::vector<uint8_t> prev; bool hasPrev, fresh; uint32_t lastCheck; };   // milestone 33: the version before, to tell a switch from the daylight's drift
    LightMapEntry lightMaps[4] = {};
    uint32_t lightMapDecodes = 0, lightMapVerdicts = 0, lightMapFails = 0, lightMapSwitches = 0, lightMapSwitchesLogged = 0, lightMapOutside = 0;
    uint64_t ibHash[512] = {};   // the hash per cached index buffer (milestone 29: the mark dump names meshes the light table does not know)
    uint32_t loggedShapes = 0;   // light types whose first shaped lamp has been logged (milestone 32)
    uint32_t lampFlipsLogged = 0;   // lamps' switches by the maps written to the log (milestone 33, bounded)
    // the lamp reporter's block (milestone 36): this frame's records, as read
    std::vector<float> lampRecords; uint32_t lampReported = 0, lampReportSeq = 0, lampReportReads = 0, lampReportStale = 0, lampReportFails = 0, lampReportMatched = 0, lampReportScanFrame = 0, lampWordsLogged = 0;
    bool lampReportLive = false, lampReportWorld = false, lampReportAnnounced = false;
    uint32_t lampNightFrames = 0; bool lampNight = false;   // consecutive frames of a sun level under the night level with the maps live; night once it has lasted (milestone 34)
    bool objWorldValid = false;          // ...and whether it has been uploaded since the shader was bound
    float rig[32] = {};                  // c0..c7 of the bound rig pixel shader (directions, colours)
    float psConst[64] = {}; uint16_t psConstMask = 0;   // c0..c15 of the bound rig pixel shader as uploaded, and which registers were (milestone 24 diagnostic)
    uint32_t markDump = 0, markDumpLogged = 0, markRigLogged = 0;
    uint64_t markMeshes[80] = {}; uint32_t markMeshCount = 0;   // the unknown meshes already named at this mark (milestone 30: one line each)          // frames left to log the lamp objects' constants after the mark key
    bool rigValid = false;
    sims3cam::LampSolver lamps;          // lamps voted by the rigs' rays, forwarded as API sphere lights (or fixed-function lights 1..7)
    uint32_t lampEvents = 0;             // SetLight/LightEnable calls made for lamps
    bool loggedLamp = false;
    // night from the sun (milestone 20d): the sun's luminance smoothed, the day reference, what was sent
    float skyLevel = -1.f, skyDayRef = 0.f, skyBrightnessSent = -1.f, evMaxSent = -99.f; uint32_t skySends = 0, skySendFrame = 0, skyBrightLogged = 0, skyNoCandFrames = 0; bool skyApiWarned = false;
    // lights through the Remix API (milestone 20b): the handles of the sun and of each lamp slot (remixapi_LightHandle, declared later in this file)
    void* sunApi = nullptr; uint32_t apiLightCalls = 0; bool loggedApiLights = false, apiLightsWarned = false;   // the lamps' handles live in the solver's lamps
    uint32_t lampEventsLogged = 0;       // lamp creations and drops written to the log (milestone 21c, bounded)
    // the object meshes identified by their index buffer (milestone 22): per index buffer id, the model in the
    // game's light table (-1 = none) once the buffer has been hashed
    uint32_t ibModelId[512] = {}; int ibModel[512] = {}; uint32_t ibModelCount = 0, liteLogged = 0, liteMatches = 0;
    // untabled pixel shaders (milestone 7): the albedo chosen from the bytecode at draw time,
    // on a promoted variant of the game's vertex shader when its coordinate is not TEXCOORD0
    const sims3cam::PsAnalysis* psAuto = nullptr;   // bound pixel shader's sampler analysis when it has no table entry
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
    D3DMATRIX gameXform[2] = {};                    // the game's own View / Projection, if it ever set them
    bool gameXformSet[2] = {};
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
    struct MaskEntry { bool used = false, ok = false; uint32_t texId = 0, version = 0; uint64_t contentHash = 0; std::vector<uint8_t> red; };
    static constexpr uint32_t kMasks = 4, kBufHashes = 64;
    MaskEntry masks[kMasks]; uint32_t maskNext = 0;   // the mask atlases decoded, per (texture, upload)
    struct HashEntry { bool used; uint32_t id, version; uint64_t hash; };
    HashEntry bufHashes[kBufHashes] = {}; uint32_t bufHashNext = 0;   // content hashes of the wall buffers, per (object, upload): the cache key follows the bytes
    uint32_t wallDraws = 0, wallCutDraws = 0, wallCutTriangles = 0, wallRemovedTriangles = 0, wallHiddenTriangles = 0, wallBuilt = 0, wallEvicted = 0, wallBuildFailed = 0, wallSkipped = 0, wallLogged = 0, wallSkipLogged = 0, wallHiddenLogged = 0, wallMasksDecoded = 0;
    // Reflection passes (runs 66-68): the ray tracer renders reflections itself, so the 3D draws of
    // every reflection pass -- the sea/pool pass, a wall mirror's stencil pass -- are dropped on the
    // client: those under a mirrored camera upload, and those carrying the stencil mirror's render
    // states (draws whose own constants never reach the classifier).
    bool camMirrored = false;                       // the last classified camera upload was a reflection's
    bool drawDropped = false;                       // this draw is a reflection pass's (set by sims3BeginDraw; the draw returns at once)
    uint32_t mirroredUploads = 0, reflectionDrops = 0, reflectionDropsByStates = 0, reflectionFrames = 0, reflectionFrame = 0xFFFFFFFFu, reflectionLogged = 0;
    bool loggedMirrorCam = false;
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
    // to the game's values at the first other draw, at the end of a replay burst or at Present
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
    // Diagnostic (milestone 19c, run 119's paint-stroke corruption): the lot paint textures as seen
    // at the game's own composite draw -- the mask at stage 0 and the layers at stages 1-4 -- with
    // the client's write count (LockableBuffer::sims3Version) and the mask's content hash; a change
    // between two draws of the same lot is logged, so a corrupted frame can be matched to what the
    // game uploaded just before.
    struct LotPaintRec { IDirect3DBaseTexture9* mask; uint32_t version; uint64_t hash; uint16_t w, h; IDirect3DBaseTexture9* layer[4]; uint32_t layerVersion[4]; };
    static constexpr uint32_t kLotPaintRecs = 64;
    LotPaintRec lotPaint[kLotPaintRecs] = {}; uint32_t lotPaintCount = 0, lotPaintChanges = 0, lotPaintLogged = 0, lotPaintReplLogged = 0;
    IDirect3DTexture9* marker[sims3cam::kTerrainMarkers] = {};   // the terrain / layer-pass / composite marker textures (terrainMarkerPixels)
    uint64_t markerHash[sims3cam::kTerrainMarkers] = {};         // their level-0 hashes as the runtime computes them (0 = not computed)
    bool markerFailed = false, markersConfigSent = false;
    // the lot paint composite's two passes (milestone 17l, sims3cam::lotCompositeStage): the pass
    // being issued by the replay (0 = the game's own draw, treated as pass 1), and pass 2's black
    // marker at stages 1 and 2 (what they held, put back in sims3EndDraw)
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
    // The sun (milestone 17x): a shadow-row direction far from the sun has to persist before it moves it.
    uint32_t sunJumpFrames = 0;
    // A rolling trace of every draw and event of the last ~300 frames (milestone 17y), written to
    // the log when the mark key is pressed (sims3hook.txt ringTrace = 1; off by default).
    static constexpr uint32_t kRingLines = 131072, kRingLine = 80;
    char ring[kRingLines][kRingLine]; uint32_t ringHead = 0, ringCount = 0, ringDumps = 0; bool f9Down = false;
    uint8_t lastKind = 0;                           // the terrain kind sims3BeginDraw gave the draw being issued
    uint32_t fCaptured = 0, fUncaptured = 0, fDropped = 0, fHeld = 0, fReplayed = 0, fTerrainWorld = 0, fTerrainLot = 0, fCamAdopt = 0, fCamAlt = 0, fCamMirror = 0;
    bool ourConsts = false;                         // our own SetVertexShaderConstantF calls: no camera classification, no tracking
    uint32_t frameDraws = 0;                        // the draw index within the frame (the ring trace)
    struct PsVariant { IDirect3DPixelShader9* base; uint64_t hash; uint8_t alphaMode; uint8_t forced; IDirect3DPixelShader9* variant; uint8_t freeStage; bool unlit; };
    static constexpr uint32_t kPsVariants = 64;
    PsVariant psVariants[kPsVariants] = {}; uint32_t psVariantCount = 0;   // one per (shader, hash, alpha mode); variant null = could not be made
    // The bake's alpha is the terrain's opacity to the ray tracer (0 = the surface is not there), so a
    // base draw writes alpha (1, or its coverage under an alpha test: terrainAlphaMode) with a full
    // colour mask; and every stage samples raw (non-sRGB) so the bake holds sRGB-encoded texels,
    // which the ray tracer gamma-corrects itself.
    DWORD cwRestore = 0; bool cwOurs = false;      // the game's COLORWRITEENABLE while the hook's full mask is set
    DWORD atSaved[3] = {}; bool atOurs = false;    // the game's ALPHATESTENABLE / ALPHAFUNC / ALPHAREF while the coverage test is set
    IDirect3DSurface9* rt0 = nullptr;               // render target 0 as the game set it (pointer only, no reference held)
    uint32_t terrainBaseDraws = 0, terrainLayerDraws = 0, terrainLotCopyDraws = 0, terrainNoVariant = 0, terrainLogged = 0, psVariantsMade = 0, psVariantsUnlit = 0, psVariantsAlpha = 0, psVariantLogged = 0, samplerCopies = 0, srgbOffs = 0;
    // Camera variety within a frame (run 69: the far view broken at a horizon tilt): the frame's
    // first main camera, and every main camera upload that differs from it in lens or position,
    // with the vertex shader it came with.
    sims3cam::Camera frameCam; bool frameCamSet = false;
    uint32_t frameMainUploads = 0, frameAltUploads = 0, framesWithAlt = 0, altLogged = 0, altUploadsTotal = 0;
    uint32_t transformSends = 0, transformSendsProj = 0;   // ...of which the projection changed
    struct AltCam { float fovY, nearZ, farZ, aspect, p33, p43, pos[3], fwdY; uint64_t vs; uint32_t count; bool first; };
    AltCam frameAlts[4] = {}; uint32_t frameAltCount = 0;
  } g_sims3;

  // the lamp reporter's block (milestone 36): where it was found, and the search for it
  std::atomic<const uint8_t*> g_sims3LampBlock { nullptr };
  std::atomic<bool> g_sims3LampScanBusy { false };
  std::atomic<uint32_t> g_sims3LampScans { 0 };

  // The Sims 3 camera hook (milestone 29): an object-shader draw notes the A8R8G8B8 texture it carries at s2 -- one of
  // the lot's light maps -- with its projection rows (y-free: a top-down map) and whether a second object has carried
  // it (a shared map, not one object's own texture). Returns the map's slot, or -1.
  static int sims3LightMapNote(Sims3Hook& h) {
    IDirect3DBaseTexture9* tex = h.boundTex[2];
    if (!h.psRig || h.psRig->stage != 3) return -1;   // only the object shaders whose diffuse sits at s3 sample the light map at s2 (milestone 30: the 3-light variant's s2 is its diffuse)
    if (!tex || (h.boundKind[2] & 0x7F) != 1 || h.boundFmt[2] != (uint32_t) D3DFMT_A8R8G8B8) return -1;
    const float* r = h.objRowsAfter;
    const bool rowsOk = h.objRowsAfterValid && r[1] == 0.f && r[5] == 0.f && (r[0] != 0.f || r[2] != 0.f) && (r[4] != 0.f || r[6] != 0.f);
    int slot = -1;
    for (int k = 0; k < 4 && slot < 0; ++k) if (h.lightMaps[k].tex == tex) slot = k;
    if (slot < 0) {   // a texture not noted before: an empty slot, else the one seen longest ago
      for (int k = 0; k < 4; ++k) {
        if (h.lightMaps[k].tex == nullptr) { slot = k; break; }
        if (slot < 0 || h.lightMaps[k].lastFrame < h.lightMaps[slot].lastFrame) slot = k;
      }
      Sims3Hook::LightMapEntry& e = h.lightMaps[slot];
      e.tex = tex; e.version = e.judged = 0xFFFFFFFFu; e.W = e.H = 0; e.draws = 0; e.rowsValid = e.shared = e.valid = false; e.data.clear();
      e.prev.clear(); e.hasPrev = e.fresh = false; e.lastCheck = 0;
      for (uint32_t k = 0; k < h.lamps.nLamps; ++k) { h.lamps.lamps[k].ownInit &= (uint8_t) ~(1u << slot); h.lamps.lamps[k].own[slot] = 0.f; }   // another map in this slot: the lamps read it afresh
      for (int q = 0; q < 3; ++q) e.origin[q] = h.objWorld[q];
    }
    Sims3Hook::LightMapEntry& e = h.lightMaps[slot];
    e.lastFrame = h.frames; ++e.draws;
    if (!e.shared && sims3cam::lampDist(e.origin, h.objWorld) > 0.5f) e.shared = true;
    if (rowsOk) { memcpy(e.rows, r, sizeof e.rows); e.rowsValid = true; }
    return slot;
  }

  template<typename Dev> void sims3TerrainBlockEnd(Sims3Hook& h, Dev* dev);   // defined with the terrain path (milestone 18g)

  // The hook's facts about a bound object, from the object (milestone 17w; defined after the
  // vertex shader and declaration headers are included). The setters call them, and so does
  // vertex shader and declaration headers are included). The setters call them.
  void sims3NoteVertexShader(Sims3Hook& h, IDirect3DVertexShader9* pShader);
  void sims3NotePixelShader(Sims3Hook& h, IDirect3DPixelShader9* pShader);
  void sims3NoteTexture(Sims3Hook& h, DWORD Stage, IDirect3DBaseTexture9* pTexture);
  void sims3NoteDecl(Sims3Hook& h, IDirect3DVertexDeclaration9* pDecl);

  // Returns whether this draw is captured with the main camera, and holds the runtime's transforms
  // accordingly: the main camera for a captured draw, else whatever the runtime had before the hook.
  template<typename Dev>
  bool sims3ApplyForDraw(Sims3Hook& h, Dev* dev, const DWORD* rs) {
    static const D3DMATRIX kIdentity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    const DWORD zEnable = rs[D3DRS_ZENABLE], stencil = rs[D3DRS_STENCILENABLE], cull = rs[D3DRS_CULLMODE];
    // a reflection pass's draw (runs 66-68): dropped before it reaches the runtime; the ray tracer
    // renders reflections itself. Nothing is changed on the device for it.
    h.drawDropped = sims3cam::isReflectionDraw(h.camMirrored, h.declIs3D, zEnable, stencil, cull);
    if (h.drawDropped) {
      ++h.reflectionDrops;
      if (!h.camMirrored) ++h.reflectionDropsByStates;
      if (h.reflectionFrame != h.frames) { h.reflectionFrame = h.frames; ++h.reflectionFrames; }
      if (h.reflectionLogged < 6) {
        ++h.reflectionLogged; char msg[240];
        snprintf(msg, sizeof msg, "Sims 3 camera hook: reflection-pass draw dropped at frame %u -> VS %016llx PS %016llx (camera mirrored %d, stencil %lu, cull %lu, %s target)", h.frames + 1, (unsigned long long) h.vsHash, (unsigned long long) h.psHash, (int) h.camMirrored, (unsigned long) stencil, (unsigned long) cull, h.rtIsPrimary ? "primary" : "offscreen");
        Logger::info(msg);
      }
      return false;
    }
    const bool is3D = sims3cam::drawIs3D(h.cameraValid, h.declIs3D, zEnable, h.rtIsPrimary);
    // the lot terrain drawn again for another world chunk (milestone 16): the frame's first draw of
    // the mesh was the whole lot already. With the terrain markers in place (milestone 17) the copy
    // is baked as a hidden layer pass, so its chunk's paint reaches the terrain texture; without
    // them it is dropped like a reflection pass's draw.
    h.lotFurtherCopy = false;
    if (is3D && h.vsHash == sims3cam::kLotTerrainVs) {
      IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0;
      if (SUCCEEDED(dev->GetStreamSource(0, &vb, &vbOffset, &vbStride)) && vb) {
        const uint64_t key = sims3cam::lotTerrainKey((uint64_t) (uintptr_t) vb, h.rows4to6);
        vb->Release();
        if (h.lotCopies.seen(key)) {
          if (h.marker[1] != nullptr && !h.markerFailed) {
            h.lotFurtherCopy = true; ++h.terrainLotCopyDraws;
          } else {
            h.drawDropped = true;
            ++h.lotCopyDrops;
            if (h.lotCopyLogged < 4) {
              ++h.lotCopyLogged; char msg[240];
              snprintf(msg, sizeof msg, "Sims 3 camera hook: lot terrain drawn again for another world chunk at frame %u -> dropped (mesh %p, World translation %.1f, %.1f, %.1f)", h.frames + 1, (void*) vb, h.rows4to6[3], h.rows4to6[7], h.rows4to6[11]);
              Logger::info(msg);
            }
            return false;
          }
        }
      }
    }
    const bool want = is3D && !sims3cam::neverCaptureDraw(h.vsNeverCapture, rs[D3DRS_ALPHABLENDENABLE]);
    if (want) {
      if (h.held.kind != sims3cam::Kind::Main || !sims3cam::similarMatrix(h.held.view, h.cam.view, 1e-5f) || !sims3cam::similarMatrix(h.held.proj, h.cam.proj, 1e-5f)) {
        h.held.kind = sims3cam::Kind::Main; h.held.view = h.cam.view; h.held.proj = h.cam.proj;
        if (!h.loggedDraw3D) { h.loggedDraw3D = true; Logger::info("Sims 3 camera hook: first 3D draw with the main camera (depth test on, 3-component position, primary target)"); }
        ++h.transformSends;
        if (h.held.kind == sims3cam::Kind::Main && !sims3cam::similarMatrix(h.held.proj, h.cam.proj, 1e-5f)) ++h.transformSendsProj;
        h.ourState = true;
        dev->SetTransform(D3DTS_VIEW, &h.cam.view);
        dev->SetTransform(D3DTS_PROJECTION, &h.cam.proj);
        h.ourState = false;
      }
    } else if (h.held.kind != sims3cam::Kind::None) {
      // back to what the runtime held before the hook: the game's own matrices when it set any, else identity
      h.held.kind = sims3cam::Kind::None;
      if (!h.loggedDraw2D) {
        h.loggedDraw2D = true;
        char msg[160];
        snprintf(msg, sizeof msg, "Sims 3 camera hook: first draw without the main camera (cameraValid=%d, declIs3D=%d, zEnable=%lu, primaryRT=%d)", (int) h.cameraValid, (int) h.declIs3D, (unsigned long) zEnable, (int) h.rtIsPrimary);
        Logger::info(msg);
      }
      h.ourState = true;
      dev->SetTransform(D3DTS_VIEW, h.gameXformSet[0] ? &h.gameXform[0] : &kIdentity);
      dev->SetTransform(D3DTS_PROJECTION, h.gameXformSet[1] ? &h.gameXform[1] : &kIdentity);
      h.ourState = false;
    }
    return want;
  }

  // Diagnostics: count captured draws per (vertex shader, pixel shader) pair.
  inline void sims3NoteStates(Sims3Hook::ShaderStat& s, const DWORD* rs) {
    s.cull = (uint8_t) rs[D3DRS_CULLMODE]; s.blend = (uint8_t) (rs[D3DRS_ALPHABLENDENABLE] ? 1 : 0);
    s.src = (uint8_t) rs[D3DRS_SRCBLEND]; s.dst = (uint8_t) rs[D3DRS_DESTBLEND];
    s.zw = (uint8_t) (rs[D3DRS_ZWRITEENABLE] ? 1 : 0); s.zf = (uint8_t) rs[D3DRS_ZFUNC];
    s.atest = (uint8_t) (rs[D3DRS_ALPHATESTENABLE] ? 1 : 0); s.cw = (uint8_t) (rs[D3DRS_COLORWRITEENABLE] & 0xF);
    s.stencil = (uint8_t) (rs[D3DRS_STENCILENABLE] ? 1 : 0);
  }
  inline void sims3NoteInstancing(Sims3Hook::ShaderStat& s, UINT freq0) {
    if (freq0 & D3DSTREAMSOURCE_INDEXEDDATA) {
      const uint32_t n = freq0 & 0x3FFFFFFFu;
      if (n > 1) { ++s.instDraws; if (n > s.instMax) s.instMax = (uint16_t) (n > 0xFFFF ? 0xFFFF : n); }
    }
  }
  inline void sims3NoteCapture(Sims3Hook& h, int stage, const DWORD* rs, UINT freq0) {
    for (int i = 0; i < h.shaderStatCount; ++i) {
      if (h.shaderStats[i].vs == h.vsHash && h.shaderStats[i].ps == h.psHash) { ++h.shaderStats[i].draws; h.shaderStats[i].stage = stage; sims3NoteStates(h.shaderStats[i], rs); sims3NoteInstancing(h.shaderStats[i], freq0); return; }
    }
    if (h.shaderStatCount < Sims3Hook::kShaderStats) {
      Sims3Hook::ShaderStat& s = h.shaderStats[h.shaderStatCount++];
      s.vs = h.vsHash; s.ps = h.psHash; s.draws = 1u; s.stage = stage; s.instDraws = 0; s.instMax = 0;
      for (int i = 0; i < 8; ++i) { s.kind[i] = h.boundKind[i]; s.fmt[i] = h.boundFmt[i]; s.w[i] = h.boundW[i]; s.h[i] = h.boundH[i]; }
      sims3NoteStates(s, rs);
      sims3NoteInstancing(s, freq0);
    }
  }

  // Diagnostics: a D3DFORMAT as text (a FOURCC as its letters, else the enum's known names).
  inline const char* sims3FormatName(uint32_t f, char* buf, size_t n) {
    if (f >= 0x20202020u) {
      buf[0] = (char) (f & 0xff); buf[1] = (char) ((f >> 8) & 0xff); buf[2] = (char) ((f >> 16) & 0xff); buf[3] = (char) ((f >> 24) & 0xff); buf[4] = 0;
      return buf;
    }
    switch (f) {
      case 21: return "A8R8G8B8"; case 22: return "X8R8G8B8"; case 23: return "R5G6B5"; case 25: return "A1R5G5B5"; case 26: return "A4R4G4B4";
      case 28: return "A8"; case 32: return "A8B8G8R8"; case 50: return "L8"; case 51: return "A8L8"; case 60: return "V8U8"; case 63: return "Q8W8V8U8";
      case 71: return "D32"; case 75: return "D24S8"; case 77: return "D24X8"; case 80: return "D16"; case 111: return "R16F"; case 112: return "G16R16F";
      case 113: return "A16B16G16R16F"; case 114: return "R32F"; case 115: return "G32R32F"; case 116: return "A32B32G32R32F";
      default: snprintf(buf, n, "fmt%u", f); return buf;
    }
  }

  // Diagnostics: keep the bytecode of every shader the game creates (once per hash) next to
  // the bridge logs, so the in-game shader set can be disassembled and tabled offline.
  // <exe dir>\rtx-remix\logs\sims3-shaders, created on first use.
  inline const char* sims3DumpDir() {
    static char base[MAX_PATH] = {};
    if (!base[0]) {
      GetModuleFileNameA(nullptr, base, MAX_PATH);
      char* p = strrchr(base, '\\'); if (p) *p = 0;
      strncat_s(base, "\\rtx-remix\\logs\\sims3-shaders", _TRUNCATE);
      CreateDirectoryA(base, nullptr);
    }
    return base;
  }
  inline void sims3DumpShader(const char* kind, uint64_t hash, const DWORD* tokens, size_t count) {
    if (!tokens || count == 0) return;
    char path[MAX_PATH + 64];
    snprintf(path, sizeof path, "%s\\%s_%016llx.bin", sims3DumpDir(), kind, (unsigned long long) hash);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return;
    fwrite(tokens, sizeof(DWORD), count, f);
    fclose(f);
  }

  // The coordinate for the albedo at stage k (milestone 7): tc as chosen by chooseAutoAlbedo, or
  // (tc < 0) the coordinate input the pixel shader's sampler k reads, from the bytecode analysis --
  // for a TABLED pixel shader on an untabled vertex shader (run 41: a Sim outfit's permutation,
  // flat grey from the raw SHORT2 input). A coordinate other than TEXCOORD0 asks for a promoted
  // variant of the vertex shader for the draw (h.pendingPromote, bound by sims3BindVariant); either
  // way the captured TEXCOORD0 is then what the pixel shader samples with (h.autoCapturedUv).
  inline void sims3AutoTexcoord(Sims3Hook& h, int k, int tc, bool tabledPs) {
    if (tc < 0) {
      const sims3cam::PsSamplerUse& u = h.psAuto->samplers[k];
      if (!u.read || u.dependent || u.projective || u.cube || u.texcoord < 0) return;
      tc = u.texcoord;
      ++h.autoTexcoordDraws;
    }
    // the shader's own coordinate output is what its pixel shader samples with; a promotion is
    // bound by sims3BindVariant, which clears autoCapturedUv again if the variant cannot be made
    h.autoCapturedUv = true;
    if (tc > 0 && !h.vsTabled) h.pendingPromote = (uint8_t) tc;
    if (h.autoLogged < 24) {
      ++h.autoLogged;
      char fb[16], msg[256];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: auto %s -> VS %016llx PS %016llx: stage %d (%s %ux%u) at TEXCOORD%d%s", tabledPs ? "coordinate (tabled albedo, untabled vertex shader)" : "albedo",
               (unsigned long long) h.vsHash, (unsigned long long) h.psHash, k,
               sims3FormatName(h.boundFmt[k], fb, sizeof fb), (unsigned) h.boundW[k], (unsigned) h.boundH[k], tc,
               (tc > 0 && !h.vsTabled) ? ", promoted variant for the draw" : (tc > 0 ? ", vertex shader tabled" : ", captured TEXCOORD0"));
      Logger::info(msg);
    }
  }

  // The normal the runtime should shade with, for the bound shaders (milestone 11): the register
  // of the vertex shader's world-normal output (chooseNormalTexcoord; the pixel shader's own use
  // of a coordinate as a normal breaks ties), 0xFF to hide the packed input normal instead
  // (vs_2_x, or no such output), 0xFE when the shader has no normal input at all.
  inline uint8_t sims3NormalChoice(const Sims3Hook& h) {
    if (!h.vsNormal || !h.vsNormal->valid || !h.vsNormal->hasNormalInput) return 0xFE;
    const uint16_t ps = (h.psAuto && h.psAuto->valid) ? h.psAuto->normalTexcoords : (uint16_t) 0;
    const int tc = sims3cam::chooseNormalTexcoord(*h.vsNormal, ps);
    if (tc >= 0) return h.vsNormal->outReg[tc];   // a vs_2_x candidate is an oT#, which convertVs2To3 keeps as o#
    return 0xFF;
  }

  // The variant of the bound vertex shader for this draw -- the promoted coordinate, the normal
  // output and/or the c255 read -- made once per (shader, coordinate, normal, read) through the
  // device and re-bound to the game's shader after the draw (sims3EndDraw). The game's shader is
  // held by a reference of the hook's own until then: binding the variant drops the device
  // state's reference to it.
  template<typename Dev>
  void sims3BindVariant(Sims3Hook& h, Dev* dev, uint8_t tc, uint8_t normalOut, bool constRead) {
    if (!h.vsBound || (tc == 0 && normalOut == 0xFE && !constRead)) return;
    Sims3Hook::VsVariant* v = nullptr;
    for (uint32_t i = 0; i < h.vsVariantCount; ++i)
      if (h.vsVariants[i].base == h.vsBound && h.vsVariants[i].hash == h.vsHash && h.vsVariants[i].texcoord == tc && h.vsVariants[i].normalOut == normalOut && h.vsVariants[i].constRead == constRead) { v = &h.vsVariants[i]; break; }
    if (!v) {
      if (h.vsVariantCount >= Sims3Hook::kVsVariants) { ++h.vsVariantsFull; }
      else {
        v = &h.vsVariants[h.vsVariantCount++];
        *v = { h.vsBound, h.vsHash, tc, normalOut, constRead, nullptr };
        UINT size = 0;
        if (SUCCEEDED(h.vsBound->GetFunction(nullptr, &size)) && size >= 8) {
          std::vector<DWORD> t(size / 4 + 1);
          if (SUCCEEDED(h.vsBound->GetFunction(t.data(), &size))) {
            t.resize(sims3cam::shaderTokenCount(t.data(), t.size()));
            bool good = t.size() >= 2, hidden = false, converted = false; uint32_t made = 0xFEu;
            // a vs_2_x shader cannot declare a NORMAL output: rewritten as vs_3_0 first (the
            // promotion then takes the vs_3_0 path on it)
            if (good && normalOut < 0xFE && h.vsNormal && h.vsNormal->version < 3) { converted = sims3cam::convertVs2To3(t); if (converted) ++h.vsConverted; else ++h.vsConvertFailed; }
            if (good && tc > 0) good = sims3cam::promoteTexcoord(t.data(), t.size(), tc) > 0;
            if (good && normalOut != 0xFE && h.vsNormal) {
              if (normalOut != 0xFF) made = sims3cam::makeNormalVariant(t, normalOut);
              if (normalOut == 0xFF || made == 0xFFu) { hidden = sims3cam::hideNormalInput(t, *h.vsNormal); good = hidden; made = 0xFEu; }
            }
            // a split instanced draw: the per-instance tag in c255 must be within the constants the
            // runtime hashes, which a dead read of c255 guarantees (see appendConstantRead)
            if (good && constRead) good = sims3cam::appendConstantRead(t, 255);
            if (good) {
              IDirect3DVertexShader9* shader = nullptr;
              h.creatingVariant = true;
              const HRESULT hr = dev->CreateVertexShader(t.data(), &shader);
              h.creatingVariant = false;
              if (SUCCEEDED(hr) && shader) {
                v->variant = shader; ++h.autoVariantsMade;
                if (made != 0xFEu || hidden) ++h.normalVariantsMade;
                if (h.normalLogged < 16) {
                  ++h.normalLogged;
                  char what[96], msg[224];
                  if (made != 0xFEu) snprintf(what, sizeof what, "world normal (o%u) repeated into NORMAL output o%u", (unsigned) normalOut, (unsigned) made);
                  else if (hidden) snprintf(what, sizeof what, "packed normal input hidden from the capture (triangle normals)");
                  else snprintf(what, sizeof what, "no normal change");
                  snprintf(msg, sizeof msg, "Sims 3 camera hook: shader variant for VS %016llx -> %s%s%s%s", (unsigned long long) h.vsHash, converted ? "vs_2_0 rewritten as vs_3_0, " : "", tc > 0 ? "texcoord promoted, " : "", what, constRead ? ", c255 read for the per-instance tag" : "");
                  Logger::info(msg);
                }
              }
            }
          }
        }
      }
    }
    if (v && v->variant) {
      h.autoVsRestore = h.vsBound; h.autoVsRestore->AddRef();
      h.swappingVs = true; dev->SetVertexShader(v->variant); h.swappingVs = false;
      if (v->normalOut < 0xFE) ++h.normalDraws; else if (v->normalOut == 0xFF) ++h.normalHiddenDraws;
    } else if (tc > 0) {
      h.autoCapturedUv = false;   // no promoted variant: the draw samples as declared
    }
  }

  // The sampler states a terrain draw's moved stage-0 texture keeps at its new stage (milestone 17).
  inline constexpr D3DSAMPLERSTATETYPE kSims3SamplerCopy[Sims3Hook::kSamplerCopies] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE, D3DSAMP_MAXANISOTROPY };
  // The rolling trace (milestone 17y): one short line per draw or event, oldest overwritten.
  inline void sims3RingPush(Sims3Hook& h, const char* text) {
    if (!sims3cam::ringTrace()) return;
    snprintf(h.ring[h.ringHead], Sims3Hook::kRingLine, "f%u %s", h.frames + 1, text);
    h.ringHead = (h.ringHead + 1) % Sims3Hook::kRingLines;
    if (h.ringCount < Sims3Hook::kRingLines) ++h.ringCount;
  }
  inline void sims3RingDump(Sims3Hook& h, const char* why) {
    char msg[200];
    snprintf(msg, sizeof msg, "Sims 3 camera hook: ==== ring trace dump %u (%s) at frame %u: %u lines, oldest first; draw flags: C captured, U not captured, D dropped, H held, R replayed; v camera valid, m mirrored; z depth test, b blend; T terrain shader, x split; rt id + p primary / o offscreen ====", h.ringDumps + 1, why, h.frames, h.ringCount);
    Logger::info(msg);
    const uint32_t start = (h.ringHead + Sims3Hook::kRingLines - h.ringCount) % Sims3Hook::kRingLines;
    for (uint32_t i = 0; i < h.ringCount; ++i) Logger::info(h.ring[(start + i) % Sims3Hook::kRingLines]);
    Logger::info("Sims 3 camera hook: ==== ring trace dump end ====");
    h.ringCount = 0; h.ringHead = 0; ++h.ringDumps;
  }
  inline void sims3RingDraw(Sims3Hook& h, const DWORD* rs) {
    const char what = h.reissue ? 'R' : h.drawDropped ? 'D' : h.drawCaptured ? 'C' : 'U';
    char t[80];
    snprintf(t, sizeof t, "d%u %c%c%c%c %08x/%08x k%u%s%s rt%04x%c", h.frameDraws, what, h.camMirrored ? 'm' : h.cameraValid ? 'v' : '-', rs[D3DRS_ZENABLE] ? 'z' : '-', rs[D3DRS_ALPHABLENDENABLE] ? 'b' : '-', (unsigned) (h.vsHash >> 32), (unsigned) (h.psHash >> 32), (unsigned) h.lastKind, h.vsTerrain ? "T" : "", h.splitDraw ? "x" : "", (unsigned) ((((uintptr_t) h.rt0) >> 4) & 0xFFFFu), h.rtIsPrimary ? 'p' : 'o');
    sims3RingPush(h, t);
    switch (what) { case 'R': ++h.fReplayed; break; case 'D': ++h.fDropped; break; case 'H': ++h.fHeld; break; case 'C': ++h.fCaptured; break; default: ++h.fUncaptured; break; }
    if (h.lastKind != 0 && h.vsTerrain && !h.reissue) { if (h.vsTerrain->lotFamily) ++h.fTerrainLot; else ++h.fTerrainWorld; }
  }

  // Whether rtx.conf (next to this DLL, read by the runtime at start) tags the markers: the
  // terrain option naming both hashes and the hidden-instance option naming the layer marker's.
  // (The bridge's Remix API channel cannot be used for this: the server never initialises its
  // API interface -- exposeRemixApi is off and its API version 0.5.1 predates the runtime's
  // 0.6.4 -- so RemixApi_SetConfigVariable called a null pointer and took the server down, run 74.)
  // hashes: [0] terrain, [1] the world's layer passes, [2] the lot composite's passes -- all three
  // terrain, the last two hidden, the first NOT (a hidden lot copy or replay hides the lot, see
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

  // The two marker textures, made once on the device and filled with their fixed content, and
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
#if SIMS3_HAVE_XXHASH
      h.markerHash[kind] = (uint64_t) XXH3_64bits(pixels, sizeof pixels);   // the runtime hashes level 0's bytes, rows packed
#endif
    }
    char msg[560];
    if (h.markerHash[0] && h.markerHash[1] && h.markerHash[2]) {
      char terrain[96], hidden[64];
      snprintf(terrain, sizeof terrain, "0x%016llX, 0x%016llX, 0x%016llX", (unsigned long long) h.markerHash[0], (unsigned long long) h.markerHash[1], (unsigned long long) h.markerHash[2]);
      snprintf(hidden, sizeof hidden, "0x%016llX, 0x%016llX", (unsigned long long) h.markerHash[1], (unsigned long long) h.markerHash[2]);
      h.markersConfigSent = sims3ConfTagsMarkers(h.markerHash);
      if (h.markersConfigSent)
        snprintf(msg, sizeof msg, "Sims 3 camera hook: terrain markers created; rtx.conf tags them (rtx.terrainTextures = %s, rtx.hideInstanceTextures = %s; terrain marker mid grey (visible), world layer-pass marker purple (hidden), lot composite marker black (hidden))", terrain, hidden);
      else
        snprintf(msg, sizeof msg, "Sims 3 camera hook: terrain markers created but rtx.conf does NOT tag them as required -> the lines must read \"rtx.terrainTextures = %s\" and \"rtx.hideInstanceTextures = %s\" (the grey marker NOT hidden; or tag the three flat 32x32 textures as Terrain Texture and the purple and black ones as Hide Instance Texture in the menu, Save Settings); until then the ground shows the markers or lots vanish", terrain, hidden);
    } else {
      snprintf(msg, sizeof msg, "Sims 3 camera hook: terrain markers created without hashes (no xxhash.h in the build) -> in the runtime menu tag the three flat 32x32 textures (mid grey, purple, black) as Terrain Texture, and the purple and black ones as Hide Instance Texture, then Save Settings");
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
    v = { base, hash, alphaMode, (uint8_t) forced, nullptr, 0, false };
    UINT size = 0;
    if (FAILED(base->GetFunction(nullptr, &size)) || size < 8) return nullptr;
    std::vector<DWORD> t(size / 4 + 1);
    if (FAILED(base->GetFunction(t.data(), &size))) return nullptr;
    t.resize(sims3cam::shaderTokenCount(t.data(), t.size()));
    if (t.size() < 2) return nullptr;
    // the stage layer 0 is read from: the shader's detail stage (milestone 17k, psDetailSampler),
    // else the first stage the shader does not declare (in-frame that stage reads nothing, runs
    // 78-82; the draws without a detail read are only replayed at Present, where it works)
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
    v.variant = ps; v.freeStage = (uint8_t) free; v.unlit = unlit;
    ++h.psVariantsMade; if (unlit) ++h.psVariantsUnlit; if (alpha) ++h.psVariantsAlpha;
    if (h.psVariantLogged < 12) {
      ++h.psVariantLogged; char msg[460];
      int lightMapStage = -1; for (int s = 0; s < 16; ++s) if (h.boundTex[s] && h.boundFmt[s] == 50u && h.boundW[s] == 16 && h.boundH[s] == 16) { lightMapStage = s; break; }
      snprintf(msg, sizeof msg, "Sims 3 camera hook: terrain variant for PS %016llx (%s) -> stage 0 read from s%d (%s; %u tokens renumbered)%s%s%s", (unsigned long long) hash, alphaMode == 0 ? (forced ? (forced == 4 ? "composite pass 1" : "composite pass 2") : "blended layer passes") : alphaMode == 1 ? "opaque draws, alpha 1" : "base draws, coverage alpha", free, forced >= 1 ? "a paint layer's stage; the black marker takes that layer out" : free == detail ? "the shader's detail stage; the marker's grey stands in for the detail" : "a stage the shader does not declare: reads nothing in-frame", swapped, unlit ? ", lighting and fog removed (albedo output)" : sims3cam::wantsUnlitPatch(hash) ? ", lighting NOT removed (final instruction not the expected mad)" : "", alphaOne ? (alpha ? ", alpha forced to 1" : ", alpha NOT forced (no free constant)") : alphaMode == 2 ? ", alpha = the shader's coverage (baked with an alpha test)" : "", (h.boundFmt[free] == 50u && h.boundW[free] == 16 && h.boundH[free] == 16) ? "; the swapped stage holds the game's 16x16 light map, whose doubled sample becomes the marker's grey = 1" : lightMapStage < 0 ? "; no 16x16 light map bound" : "; NOTE: the 16x16 light map is bound at another stage, so that map is baked");
      Logger::info(msg);
    }
    freeStage = free;
    return ps;
  }

  // A terrain draw (kind 1 base, 2 layer pass): the marker at stage 0, the game's stage-0 texture
  // and its sampler states at the free stage, the pixel shader variant bound. Everything goes
  // back in sims3EndDraw. Returns false when the draw has to be captured the ordinary way.
  // Ends the terrain block (milestone 18g): the game's sampler states back on the free stage and
  // sRGB sampling back on where the hook turned it off, unless the game set them meanwhile.
  // The lot paint textures at the game's composite draw (milestone 19c): any change since the
  // last draw with the same mask -- a new object, a further write, a different content hash --
  // is logged with the frame, bounded.
  inline void sims3LotPaintDiag(Sims3Hook& h) {
    IDirect3DBaseTexture9* mask = h.boundTex[0];
    if (mask == nullptr || (h.boundKind[0] & 0x7F) != 1) return;
    auto* const tex = bridge_cast<Direct3DTexture9_LSS*>(mask);
    if (tex == nullptr) return;
    Sims3Hook::LotPaintRec now = {};
    now.mask = mask; now.version = tex->sims3Level0Version(); now.hash = tex->sims3Level0Hash(); now.w = h.boundW[0]; now.h = h.boundH[0];
    for (int s = 0; s < 4; ++s) {
      now.layer[s] = h.boundTex[s + 1];
      auto* const lt = (now.layer[s] && (h.boundKind[s + 1] & 0x7F) == 1) ? bridge_cast<Direct3DTexture9_LSS*>(now.layer[s]) : nullptr;
      now.layerVersion[s] = lt ? lt->sims3Level0Version() : 0;
    }
    Sims3Hook::LotPaintRec* rec = nullptr;
    for (uint32_t i = 0; i < h.lotPaintCount; ++i) if (h.lotPaint[i].mask == mask) { rec = &h.lotPaint[i]; break; }
    if (rec == nullptr) {
      if (h.lotPaintCount < Sims3Hook::kLotPaintRecs) rec = &h.lotPaint[h.lotPaintCount++];
      else return;                   // table full: not tracked
      *rec = now; rec->version = ~0u;   // a first sight logs as a change below
    }
    char what[200] = {}; size_t n = 0; bool rare = false;   // rare: a composite first seen, a layer object replaced (milestone 19h)
    if (rec->version == ~0u) { n += (size_t) snprintf(what + n, sizeof what - n, " composite first seen"); rare = true; }
    else {
      if (now.version != rec->version) n += (size_t) snprintf(what + n, sizeof what - n, " mask written (%u -> %u)", rec->version, now.version);
      if (now.hash != rec->hash) n += (size_t) snprintf(what + n, sizeof what - n, " mask content changed");
      for (int s = 0; s < 4 && n < sizeof what - 40; ++s) {
        if (now.layer[s] != rec->layer[s]) { n += (size_t) snprintf(what + n, sizeof what - n, " layer %d replaced", s + 1); rare = true; }
        else if (now.layerVersion[s] != rec->layerVersion[s]) n += (size_t) snprintf(what + n, sizeof what - n, " layer %d written (%u -> %u)", s + 1, rec->layerVersion[s], now.layerVersion[s]);
      }
    }
    if (n == 0) return;
    *rec = now; ++h.lotPaintChanges;
    if (h.lotPaintLogged < 80 || (rare && h.lotPaintReplLogged < 300)) {   // the rare events past the cap (milestone 19h)
      ++h.lotPaintLogged; if (rare) ++h.lotPaintReplLogged; char msg[460];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: lot paint textures at frame %u ->%s; mask %p %ux%u version %u hash %016llx; layers %p v%u, %p v%u, %p v%u, %p v%u",
               h.frames + 1, what, (void*) now.mask, (unsigned) now.w, (unsigned) now.h, now.version, (unsigned long long) now.hash,
               (void*) now.layer[0], now.layerVersion[0], (void*) now.layer[1], now.layerVersion[1], (void*) now.layer[2], now.layerVersion[2], (void*) now.layer[3], now.layerVersion[3]);
      Logger::info(msg);
    }
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
  template<typename Dev>
  void sims3TerrainBlockEnd(Sims3Hook& h, Dev* dev) {
    if (!h.tblockActive) return;
    h.ourSampler = true;
    for (DWORD s = 0; s < 16; ++s) if (h.tblockSrgb & (1u << s)) dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, TRUE);
    h.ourSampler = false;
    sims3TerrainStageRelease(h, dev);
    h.ourSampler = true;
    h.ourSampler = false;
    h.tblockActive = false; h.tblockSrgb = 0; ++h.tblockFlushes;
  }
  // The paint composite's second pass in place (milestone 19): the game's own draw was pass 1
  // (layer 4 blacked out); the same draw goes out again as pass 2, hidden, blended ONE / ONE,
  // through the ordinary draw hooks with the re-issue flags the replay used.
  template<typename Dev>
  void sims3CompositeSecondPass(Sims3Hook& h, Dev* dev, bool indexed, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVertices, UINT start, UINT count) {
    h.compositeSecond = false;
    DWORD src = 0, dst = 0; dev->GetRenderState(D3DRS_SRCBLEND, &src); dev->GetRenderState(D3DRS_DESTBLEND, &dst);
    h.ourState = true; dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE); dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE); h.ourState = false;
    h.reissue = true; h.reissueKind = 2; h.compositePass = 2;
    if (indexed) dev->DrawIndexedPrimitive(type, baseVertex, minIndex, numVertices, start, count); else dev->DrawPrimitive(type, start, count);
    h.reissue = false; h.reissueKind = 0; h.compositePass = 0;
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

  template<typename Dev>
  bool sims3BeginTerrainDraw(Sims3Hook& h, Dev* dev, uint8_t kind) {
    if (!h.psBound || !sims3EnsureMarkers(h, dev)) return false;
    int freeStage = -1;
    DWORD alphaBlend = 0; dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
    const uint8_t alphaMode = sims3cam::terrainAlphaMode(h.vsTerrain, kind, alphaBlend);
    // the lot paint composite (milestone 17l, sims3cam::lotCompositeStage): its mask goes to a
    // paint layer's stage and the black marker takes that layer out; two passes in the replay
    const bool composite = h.psHash == sims3cam::kLotCompositePs;
    const int pass = composite ? (h.compositePass ? (int) h.compositePass : 1) : 0;
    if (composite && pass == 1 && !h.reissue) { h.compositeSecond = true; sims3LotPaintDiag(h); }   // the second pass follows in place (milestone 19)
    IDirect3DPixelShader9* variant = sims3PsVariant(h, dev, h.psBound, h.psHash, alphaMode, sims3cam::lotCompositeStage(pass), freeStage);
    if (!variant || freeStage < 1 || freeStage > 15) { ++h.terrainNoVariant; return false; }
    // which marker: base draws visible (0); every layer pass hidden -- the world's blended layers
    // (1), a lot's further chunk copies and replays (1) and its composite passes (2). A lot's
    // re-submissions are also split in two (below), so the runtime's draw tracker never takes one
    // of them for the lot's own visible instance: with the same mesh, baked material and position
    // it did so whenever the camera moved (the terrain texture transform in the identity hash
    // changes each frame), and a hidden or blended draw landing on the lot's instance flickered or
    // blanked the whole lot (runs 85-91).
    const bool lotFamily = h.vsTerrain && h.vsTerrain->lotFamily;
    const int markerIdx = composite ? 2 : kind == 2 ? 1 : 0;
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
    // a lot's re-submission goes out as two half draws (the draw hooks split it): the runtime
    // files a draw by its index data and counts, so a half can never be taken for the lot's own
    // visible instance whatever the camera does (the same mesh, material and position otherwise)
    // A lot's re-submissions and its paint composite go out as two half draws in every mode
    // (milestone 18b): the composite's additive blend makes the runtime file its instance in the
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
    // a draw that writes its alpha (1, or its coverage) into the bake -- a base draw, a lot's
    // opaque chunk copies and replays -- needs the full colour mask (the game masks alpha off);
    // with the coverage alpha an alpha test drops the unpainted texels, so they leave earlier bakes alone
    if (alphaMode != 0) {
      DWORD cw = 0xF; dev->GetRenderState(D3DRS_COLORWRITEENABLE, &cw);
      if ((cw & 0xFu) != 0xFu) { h.cwRestore = cw; h.cwOurs = true; h.ourState = true; dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xFu); h.ourState = false; }
      if (alphaMode == 2) {
        dev->GetRenderState(D3DRS_ALPHATESTENABLE, &h.atSaved[0]); dev->GetRenderState(D3DRS_ALPHAFUNC, &h.atSaved[1]); dev->GetRenderState(D3DRS_ALPHAREF, &h.atSaved[2]);
        h.atOurs = true; h.ourState = true;
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE); dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER); dev->SetRenderState(D3DRS_ALPHAREF, 0);
        h.ourState = false;
      }
    }
    // the pixel shader variant; the game's shader held until sims3EndDraw
    h.psRestore = h.psBound; h.psRestore->AddRef();
    h.swappingPs = true; dev->SetPixelShader(variant); h.swappingPs = false;
    if (kind == 2) ++h.terrainLayerDraws; else ++h.terrainBaseDraws;
    if (h.terrainLogged < 8) {
      ++h.terrainLogged; char fb[16]; char msg[320];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: terrain draw for the baker at frame %u -> VS %016llx PS %016llx, %s, game's stage 0 (%s %ux%u) read from s%d, marker at stage 0%s", h.frames + 1, (unsigned long long) h.vsHash, (unsigned long long) h.psHash, kind == 2 ? (h.splitDraw ? (h.lotFurtherCopy ? "lot chunk copy (hidden, two halves)" : "lot re-submission (hidden, two halves)") : "layer pass (hidden)") : "base terrain", (h.boundKind[0] & 0x7F) == 2 ? "CUBE" : sims3FormatName(h.boundFmt[0], fb, sizeof fb), (unsigned) h.boundW[0], (unsigned) h.boundH[0], freeStage, h.markersConfigSent ? "" : " (markers untagged: nothing is baked until they are tagged in the menu)");
      Logger::info(msg);
    }
    return true;
  }

  // Stage 0's colour stage as the hook sets it for captured draws: TEXTURE x TFACTOR (the tint).
  inline constexpr D3DTEXTURESTAGESTATETYPE kSims3Tss[3] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2 };
  inline constexpr DWORD kSims3TssOurs[3] = { D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_TFACTOR };
  // The render states the masked-write emulation saves and restores.
  inline constexpr D3DRENDERSTATETYPE kSims3MaskRs[10] = { D3DRS_COLORWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_BLENDFACTOR, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA };

  // Before a draw that is not captured: whatever the hook set on the device for captured draws
  // goes back to the game's own value, so the game's other draws (the compositor) see the game's state.
  template<typename Dev>
  void sims3RestoreGameState(Sims3Hook& h, Dev* dev) {
    if (!h.uvIndexHidden && !h.tssOurs && !h.factorOurs) return;
    h.ourState = true;
    if (h.uvIndexHidden) { h.uvIndexHidden = false; dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, h.gameTss0[3]); }
    for (int i = 0; i < 3; ++i) if (h.tssOurs & (1u << i)) dev->SetTextureStageState(0, kSims3Tss[i], h.gameTss0[i]);
    h.tssOurs = 0;
    if (h.factorOurs) { h.factorOurs = false; h.sentFactor = h.gameFactor; dev->SetRenderState(D3DRS_TEXTUREFACTOR, h.gameFactor); }
    h.ourState = false;
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
  // the written ones. Everything the quad needs is set and put back through the device's own setters,
  // with the draw hooks told to ignore the quad and the state tracking told the values are ours.
  template<typename Dev>
  void sims3RestoreChannels(Sims3Hook& h, Dev* dev) {
    const int si = h.copyScratch; h.copyScratch = -1;
    if (si < 0 || h.scratch[si].tex == nullptr) return;
    static const D3DRENDERSTATETYPE kRs[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_LIGHTING, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPING, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_BLENDFACTOR };
    static const D3DTEXTURESTAGESTATETYPE kTss[] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS };
    static const D3DSAMPLERSTATETYPE kSs[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
    constexpr int nRs = sizeof kRs / sizeof kRs[0], nTss = sizeof kTss / sizeof kTss[0], nSs = sizeof kSs / sizeof kSs[0];
    DWORD rs[nRs], tss[nTss], tss1[2], ss[nSs], fvf = 0;
    for (int i = 0; i < nRs; ++i) dev->GetRenderState(kRs[i], &rs[i]);
    for (int i = 0; i < nTss; ++i) dev->GetTextureStageState(0, kTss[i], &tss[i]);
    dev->GetTextureStageState(1, D3DTSS_COLOROP, &tss1[0]); dev->GetTextureStageState(1, D3DTSS_ALPHAOP, &tss1[1]);
    for (int i = 0; i < nSs; ++i) dev->GetSamplerState(0, kSs[i], &ss[i]);
    IDirect3DBaseTexture9* tex0 = nullptr; dev->GetTexture(0, &tex0);
    IDirect3DVertexShader9* vs = nullptr; dev->GetVertexShader(&vs);
    IDirect3DPixelShader9* ps = nullptr; dev->GetPixelShader(&ps);
    IDirect3DVertexDeclaration9* decl = nullptr; dev->GetVertexDeclaration(&decl);
    dev->GetFVF(&fvf);
    IDirect3DVertexBuffer9* vb = nullptr; UINT vbOffset = 0, vbStride = 0; dev->GetStreamSource(0, &vb, &vbOffset, &vbStride);

    h.ourDraw = true; h.ourState = true; h.inRemap = true; h.swappingVs = true;
    const DWORD m = h.copyMask;
    const DWORD factor = ((m & 8) ? 0u : 0xFF000000u) | ((m & 1) ? 0u : 0x00FF0000u) | ((m & 2) ? 0u : 0x0000FF00u) | ((m & 4) ? 0u : 0x000000FFu);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE); dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE); dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); dev->SetRenderState(D3DRS_FOGENABLE, FALSE); dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE); dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0); dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE); dev->SetRenderState(D3DRS_CLIPPING, TRUE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_BLENDFACTOR); dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVBLENDFACTOR); dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_BLENDFACTOR, factor);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0); dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetTexture(0, h.scratch[si].tex);
    dev->SetVertexShader(nullptr); dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    const float w = (float) h.rt0W, ht = (float) h.rt0H;
    struct V { float x, y, z, rhw, u, v; };
    const V q[4] = { { -0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f }, { w - 0.5f, -0.5f, 0.f, 1.f, 1.f, 0.f }, { -0.5f, ht - 0.5f, 0.f, 1.f, 0.f, 1.f }, { w - 0.5f, ht - 0.5f, 0.f, 1.f, 1.f, 1.f } };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    // the game's state back
    if (decl) dev->SetVertexDeclaration(decl); else dev->SetFVF(fvf);
    dev->SetVertexShader(vs); dev->SetPixelShader(ps);
    dev->SetTexture(0, tex0);
    if (vb) dev->SetStreamSource(0, vb, vbOffset, vbStride);
    for (int i = 0; i < nSs; ++i) dev->SetSamplerState(0, kSs[i], ss[i]);
    for (int i = 0; i < nTss; ++i) dev->SetTextureStageState(0, kTss[i], tss[i]);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, tss1[0]); dev->SetTextureStageState(1, D3DTSS_ALPHAOP, tss1[1]);
    for (int i = 0; i < nRs; ++i) dev->SetRenderState(kRs[i], rs[i]);
    h.swappingVs = false; h.inRemap = false; h.ourState = false; h.ourDraw = false;
    if (decl) decl->Release();
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (tex0) tex0->Release();
    if (vb) vb->Release();
  }

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
  template<typename Dev>
  IDirect3DIndexBuffer9* sims3MakeIndexBuffer(Dev* dev, const std::vector<uint16_t>& indices) {
    IDirect3DIndexBuffer9* ib = nullptr;
    if (indices.empty() || FAILED(dev->CreateIndexBuffer((UINT) (indices.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, nullptr)) || !ib) return nullptr;
    void* p = nullptr;
    if (FAILED(ib->Lock(0, 0, &p, 0)) || !p) { ib->Release(); return nullptr; }
    memcpy(p, indices.data(), indices.size() * 2);
    ib->Unlock();
    return ib;
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

  // The device reset (and its destruction): every object the hook made or holds a reference to is
  // released, the bound-shader and bound-texture facts are forgotten (the device state is back to
  // defaults; the game's shaders themselves survive a Reset), and the runtime is back to default state.
  inline void sims3OnReset(Sims3Hook& h) {
    for (uint32_t i = 0; i < h.wallCacheCount; ++i) sims3ReleaseWallEntry(h.wallCache[i]);
    h.wallCacheCount = 0; for (auto& m : h.masks) m = Sims3Hook::MaskEntry(); h.maskNext = 0;
    for (auto& e : h.bufHashes) e = Sims3Hook::HashEntry(); h.bufHashNext = 0;
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
    h.markerFailed = false; h.markersConfigSent = false; h.terrainFreeStage = -1; h.tblockActive = false; h.tblockStage = -1; h.tblockSet = 0; h.tblockSrgb = 0; h.ourSampler = false; h.psBound = nullptr; h.vsTerrain = nullptr; h.lotFurtherCopy = false; h.swappingPs = false;
    h.cwOurs = false; h.atOurs = false;
    h.compositePass = 0; h.extraActive = false; h.splitDraw = false; h.ourConsts = false; h.reissue = false; h.reissueKind = 0; h.compositeSecond = false;
    for (int i = 0; i < 2; ++i) { if (h.extraRestore[i]) h.extraRestore[i]->Release(); h.extraRestore[i] = nullptr; }
    h.rt0 = nullptr;
    h.remapActive = false; h.maskEmu = 0; h.viewportOurs = false; h.vsSkyDome = false;
    h.vsBound = nullptr; h.vsTabled = false; h.vsNormal = nullptr; h.pendingPromote = 0; h.vsHash = 0;
    h.patch = nullptr; h.vsNeverCapture = 0; h.vsCapturedUv = false; h.vsWorldReg = -1; h.vsShadowReg = -1; h.objWorldValid = false;
    h.lotCopies.clear();
    h.psAuto = nullptr; h.psRig = nullptr; h.psAlbedoStage = -1; h.psTintReg = -1; h.psHash = 0; h.rigValid = false; h.psConstMask = 0;
    h.declIs3D = false; h.drawCaptured = false; h.autoCapturedUv = false;
    for (int i = 0; i < 16; ++i) { h.boundTex[i] = nullptr; h.boundColor2D[i] = false; h.boundKind[i] = 0; h.boundFmt[i] = 0; h.boundW[i] = h.boundH[i] = 0; }
    for (uint32_t i = 0; i < h.scratchCount; ++i) { if (h.scratch[i].surf) h.scratch[i].surf->Release(); if (h.scratch[i].tex) h.scratch[i].tex->Release(); h.scratch[i] = Sims3Hook::Scratch(); }
    h.scratchCount = 0; h.copyScratch = -1; h.ourDraw = false; h.rt0W = h.rt0H = 0; h.rt0Fmt = 0;
    h.held = sims3cam::Held(); h.cameraValid = false; h.camMirrored = false; h.drawDropped = false; h.rtIsPrimary = true;
    h.frameCamSet = false; h.frameMainUploads = 0; h.frameAltUploads = 0; h.frameAltCount = 0;
    h.tssOurs = 0; h.uvIndexHidden = false; h.factorOurs = false; h.sentFactor = 0xFFFFFFFFu; h.gameFactor = 0xFFFFFFFFu;
    h.gameTss0[0] = D3DTOP_MODULATE; h.gameTss0[1] = D3DTA_TEXTURE; h.gameTss0[2] = D3DTA_CURRENT; h.gameTss0[3] = 0;
    h.gameXformSet[0] = h.gameXformSet[1] = false;
    h.voter.clear(); h.sunSet = false; h.sunCandFrames = 0; h.shadowDirValid = false;
    for (uint32_t k = 0; k < h.lamps.nLamps; ++k) { h.lamps.lamps[k].sent = false; h.lamps.lamps[k].api = nullptr; }   // the runtime's lights are gone with the device; the lamps are re-sent
    for (bool& r : h.rsSet) r = false;
    Logger::info("Sims 3 camera hook: device reset -> the hook's objects released, held state cleared");
  }

  // An untabled pixel shader's draw: the albedo stage from its bytecode and what is bound
  // (chooseAutoAlbedo), then its coordinate. Returns the stage to present, or -1 when the
  // bytecode names no candidate.
  inline int sims3AutoAlbedo(Sims3Hook& h) {
    int k = -1, tc = -1;
    if (!sims3cam::chooseAutoAlbedo(*h.psAuto, h.boundColor2D, h.boundFmt, h.boundW, h.boundH, k, tc)) { ++h.autoNoAlbedo; return -1; }
    ++h.autoDraws;
    sims3AutoTexcoord(h, k, tc, false);
    return k;
  }

  // A draw that is not captured into an offscreen target with a partial colour write mask: the
  // runtime drops any draw whose mask lacks R, G or B (the compositor's channel packing), so the
  // mask is emulated with blending and the mask itself set to full; sims3EndDraw puts the states back.
  template<typename Dev>
  void sims3BeginMaskedWrite(Sims3Hook& h, Dev* dev, const DWORD* rs) {
    const DWORD cw = rs[D3DRS_COLORWRITEENABLE] & 0xF, bl = rs[D3DRS_ALPHABLENDENABLE] ? 1u : 0u;
    if (h.rtIsPrimary || (cw & 7) == 7) return;
    for (int i = 0; i < 10; ++i) h.maskSaved[i] = rs[kSims3MaskRs[i]];
    h.ourState = true;
    if (!bl) {
      // no blending: dest = src on the written channels, kept elsewhere = a constant blend factor of 1 / 0 per channel
      const DWORD f = ((cw & 8) ? 0xFF000000u : 0u) | ((cw & 1) ? 0x00FF0000u : 0u) | ((cw & 2) ? 0x0000FF00u : 0u) | ((cw & 4) ? 0x000000FFu : 0u);
      dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE); dev->SetRenderState(D3DRS_BLENDFACTOR, f); dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_BLENDFACTOR); dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVBLENDFACTOR); dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD); dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xFu);
      h.maskEmu = 1; h.maskEmuBits = 0x7F; ++h.maskEmuA;
    } else if (cw == 8) {
      // alpha written with the game's blend, colour kept: the blend moves to the alpha channel, colour blends ZERO / ONE
      dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE); dev->SetRenderState(D3DRS_SRCBLENDALPHA, h.maskSaved[2]); dev->SetRenderState(D3DRS_DESTBLENDALPHA, h.maskSaved[3]); dev->SetRenderState(D3DRS_BLENDOPALPHA, h.maskSaved[4]); dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO); dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE); dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD); dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xFu);
      h.maskEmu = 2; h.maskEmuBits = 0x3DD; ++h.maskEmuB;
    } else if (sims3CopyTarget(h, dev, cw)) {
      // blending on with a partial colour mask: the target copied, the draw with a full mask, the rest put back after it
      dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xFu);
      h.maskEmu = 3; h.maskEmuBits = 0x1; ++h.maskEmuC;
    } else {
      ++h.maskEmuSkipped;
    }
    h.ourState = false;
    if (h.maskEmu && ((h.maskEmu != 3 && h.maskEmuLogged < 4) || (h.maskEmu == 3 && h.maskEmuCopyLogged < 8))) {
      if (h.maskEmu == 3) ++h.maskEmuCopyLogged; else ++h.maskEmuLogged;
      char msg[320];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: masked write emulated at frame %u -> mask %lx into %ux%u, %s (blend was %lu: %lu/%lu op %lu), VS %016llx PS %016llx", h.frames + 1, (unsigned long) cw, (unsigned) h.rt0W, (unsigned) h.rt0H, h.maskEmu == 1 ? "constant blend factor" : h.maskEmu == 2 ? "separate alpha blend" : "copy, full mask, unwritten channels restored after the draw", (unsigned long) bl, (unsigned long) h.maskSaved[2], (unsigned long) h.maskSaved[3], (unsigned long) h.maskSaved[4], (unsigned long long) h.vsHash, (unsigned long long) h.psHash);
      Logger::info(msg);
    }
  }

  // Before every draw of the game (not the hook's own restore quad). A captured draw -- the main
  // camera held, see sims3ApplyForDraw -- gets: its albedo presented as stage 0 when the game bound
  // a cube map / render target there (Remix would drop the draw), the vertex shader variant for
  // the draw (promoted coordinate, world normal, c255 read), the raw texcoord set hidden for the
  // families whose shader output is verified, its tint as the texture factor, and its light rig
  // fed to the lamp solver. Any other draw gets the game's own state back, and a masked write
  // into an offscreen target its emulation. Returns whether the draw is captured.
  template<typename Dev>
  bool sims3BeginDraw(Sims3Hook& h, Dev* dev, const DWORD* rs, UINT freq0) {
    if (!h.reissue) ++h.frameDraws;
    h.lastKind = 0;
    const bool want = sims3ApplyForDraw(h, dev, rs);
    h.drawCaptured = want;
    if (h.drawDropped) return false;                // a reflection pass's draw: the caller returns without drawing
    if (!want) {
      sims3TerrainBlockEndIfUsed(h, dev);      // an uncaptured draw follows the terrain block: ends it only if it samples a held stage (milestone 18h)
      sims3RestoreGameState(h, dev);
      sims3BeginMaskedWrite(h, dev, rs);
      return false;
    }
    int k = -1;
    ++h.capturedDraws;
    h.autoCapturedUv = false; h.pendingPromote = 0;
    // a terrain draw (milestone 17): handed to the runtime's terrain baker with the marker at
    // stage 0 and the game's pixel shader variant; no albedo stage, no vertex shader variant
    uint8_t terrainKind = sims3cam::terrainEnabled() ? sims3cam::terrainDrawKind(h.vsTerrain, rs[D3DRS_ALPHABLENDENABLE], h.lotFurtherCopy) : (uint8_t) 0;
    const bool lotFamilyDraw = terrainKind != 0 && h.vsTerrain && h.vsTerrain->lotFamily;
    if (terrainKind != 0 && h.reissue) terrainKind = h.reissueKind ? h.reissueKind : 2;   // the hook's own re-issue (the composite's second pass): hidden
    h.lastKind = terrainKind;
    h.compositeSecond = false;
    // a lot's ground goes out in place (milestone 19): nothing overwrites its paint in the atlas (run 119's paint test)
    const bool terrain = terrainKind != 0 && sims3BeginTerrainDraw(h, dev, terrainKind);
    if (!terrain) sims3TerrainBlockEnd(h, dev);   // a captured non-terrain draw follows the terrain block (milestone 18g)
    if (!terrain) {
      if (h.psAlbedoStage >= 0 && h.psAlbedoStage < 16 && h.boundTex[h.psAlbedoStage] != nullptr) {
        k = h.psAlbedoStage; ++h.overrideDraws;
        // on an untabled vertex shader (a Sim outfit's permutation, run 41) the coordinate the albedo
        // sampler reads, from the bytecode, decides the promotion and the captured UV
        if (!h.vsTabled && h.vsBound && h.psAuto && h.psAuto->valid) sims3AutoTexcoord(h, k, -1, true);
      } else {
        // an untabled pixel shader: the albedo from its bytecode (a promoted vertex-shader variant when needed)
        if (h.psAuto && h.psAuto->valid && h.vsBound) k = sims3AutoAlbedo(h);
        if (k < 0) { bool bound[16]; for (int i = 0; i < 16; ++i) bound[i] = h.boundTex[i] != nullptr; k = sims3cam::pickAlbedoStage(bound, h.boundColor2D); }
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
    if (h.psRig && h.objWorldValid) { sims3LightMapNote(h); if (h.rigValid) h.lamps.add(h.objWorld, h.rig, h.rig + 16, h.sunSet ? h.sun.dir : nullptr); }
    // Create-A-Style tint: albedo x TEXTUREFACTOR (white when the shader has no tint); each of the
    // three stage-0 states the game has written since is set again
    for (int i = 0; i < 3; ++i) if (!(h.tssOurs & (1u << i))) { h.tssOurs |= (uint8_t) (1u << i); dev->SetTextureStageState(0, kSims3Tss[i], kSims3TssOurs[i]); }
    const uint32_t factor = (h.psTintReg >= 0) ? sims3cam::packTint(h.tint) : 0xFFFFFFFFu;
    if (!h.factorOurs || factor != h.sentFactor) {
      h.sentFactor = factor; h.factorOurs = true;
      dev->SetRenderState(D3DRS_TEXTUREFACTOR, factor);
      if (!h.loggedTint && factor != 0xFFFFFFFFu) { h.loggedTint = true; char msg[160]; snprintf(msg, sizeof msg, "Sims 3 camera hook: first non-white tint forwarded as texture factor: %08X", factor); Logger::info(msg); }
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
    }
    return true;
  }

  // After the draw's message has been queued: the game's stage-0 texture and vertex shader back,
  // the masked-write emulation undone.
  template<typename Dev>
  void sims3EndDraw(Sims3Hook& h, Dev* dev) {
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
}

// Around every draw of the game (the hook's own restore quad is left alone). A reflection pass's
// draw returns here, never sent (nothing was changed on the device for it).
#define SIMS3_BEGIN_DRAW() \
  if (sims3cam::enabled() && !g_sims3.ourDraw) { sims3BeginDraw(g_sims3, this, m_state.renderStates.data(), m_state.streamFreqs[0]); sims3RingDraw(g_sims3, m_state.renderStates.data()); if (g_sims3.drawDropped) return D3D_OK; }
#define SIMS3_END_DRAW() if (sims3cam::enabled() && !g_sims3.ourDraw) sims3EndDraw(g_sims3, this)
#include "d3d9_vertexbuffer.h"
#include "d3d9_vertexdeclaration.h"
#include "d3d9_vertexshader.h"
#include "d3d9_volumetexture.h"
#include "shadow_map.h"
#include "client_options.h"
#include "swapchain_map.h"
#include "config/global_options.h"
#include "remix_api.h"
#include "window.h"

#include "util_bridge_assert.h"
#include "util_semaphore.h"

#include <wingdi.h>
#include <assert.h>

#define GET_PRES_PARAM() (m_pSwapchain->getPresentationParameters())

namespace {
  // The hook's facts about the bound objects (milestone 17w): what the setters recorded inline before.
  void sims3NoteVertexShader(Sims3Hook& h, IDirect3DVertexShader9* pShader) {
    auto* const pLssVertexShader = bridge_cast<Direct3DVertexShader9_LSS*>(pShader);
    h.patch = pLssVertexShader ? pLssVertexShader->sims3Patch : nullptr;
    h.vsNeverCapture = pLssVertexShader ? pLssVertexShader->sims3NeverCapture : 0;
    h.vsSkyDome = pLssVertexShader ? pLssVertexShader->sims3SkyDome : false;
    h.vsHash = pLssVertexShader ? pLssVertexShader->sims3Hash : 0;
    h.vsWorldReg = pLssVertexShader ? sims3cam::findWorldReg(pLssVertexShader->sims3Hash) : -1;
    h.objWorldValid = false;
    h.vsShadowReg = pLssVertexShader ? sims3cam::findShadowSource(pLssVertexShader->sims3Hash) : -1;
    h.vsCapturedUv = pLssVertexShader ? sims3cam::useCapturedUv(pLssVertexShader->sims3Hash) : false;
    h.vsBound = pShader;
    h.vsNormal = pLssVertexShader ? &pLssVertexShader->sims3Normal : nullptr;
    h.vsWall = pLssVertexShader ? &pLssVertexShader->sims3Wall : nullptr;
    h.vsTerrain = pLssVertexShader ? sims3cam::findTerrainShader(pLssVertexShader->sims3Hash) : nullptr;
    h.vsTabled = pLssVertexShader && (h.vsCapturedUv || h.vsNeverCapture == 1);   // a blended-only entry still gets its opaque draws' variants
  }
  void sims3NotePixelShader(Sims3Hook& h, IDirect3DPixelShader9* pShader) {
    Direct3DPixelShader9_LSS* pLssPixelShader = bridge_cast<Direct3DPixelShader9_LSS*>(pShader);
    h.psBound = pShader;
    h.psRig = pLssPixelShader ? pLssPixelShader->sims3LightRig : nullptr;
    h.psAlbedoStage = pLssPixelShader ? pLssPixelShader->sims3AlbedoStage : -1;
    h.psTintReg = pLssPixelShader ? pLssPixelShader->sims3TintReg : -1;
    h.psHash = pLssPixelShader ? pLssPixelShader->sims3Hash : 0;
    h.psAuto = pLssPixelShader ? &pLssPixelShader->sims3Auto : nullptr;
  }
  void sims3NoteTexture(Sims3Hook& h, DWORD Stage, IDirect3DBaseTexture9* pTexture) {
    h.boundTex[Stage] = pTexture;
    bool color2D = false;
    uint8_t kind = 0; uint32_t fmt = 0; uint16_t w = 0, hgt = 0;
    if (pTexture != nullptr) {
      const D3DRESOURCETYPE type = pTexture->GetType();
      if (type == D3DRTYPE_TEXTURE) {
        const D3DSURFACE_DESC d = bridge_cast<Direct3DTexture9_LSS*>(pTexture)->getLevelDesc(0);
        color2D = (d.Usage & D3DUSAGE_RENDERTARGET) == 0 && sims3cam::isColorFormat(d.Format);
        kind = (uint8_t) (1 | ((d.Usage & D3DUSAGE_RENDERTARGET) ? 0x80 : 0));
        fmt = (uint32_t) d.Format; w = (uint16_t) d.Width; hgt = (uint16_t) d.Height;
      } else if (type == D3DRTYPE_CUBETEXTURE) {
        kind = 2;
      } else if (type == D3DRTYPE_VOLUMETEXTURE) {
        kind = 3;
      }
    }
    h.boundColor2D[Stage] = color2D;
    h.boundKind[Stage] = kind; h.boundFmt[Stage] = fmt; h.boundW[Stage] = w; h.boundH[Stage] = hgt;
  }
  void sims3NoteDecl(Sims3Hook& h, IDirect3DVertexDeclaration9* pDecl) {
    auto* const pLssVtxDecl = bridge_cast<Direct3DVertexDeclaration9_LSS*>(pDecl);
    const UID id = (pLssVtxDecl) ? (UID) pLssVtxDecl->getId() : 0;
    h.declIs3D = pLssVtxDecl ? sims3cam::positionIs3D(pLssVtxDecl->sims3Elements()) : false;
    h.wallLayout = sims3cam::WallLayout(); h.wallDeclId = (uint32_t) id;
    if (pLssVtxDecl) sims3cam::wallLayoutFromDecl(pLssVtxDecl->sims3Elements(), h.wallLayout);
  }
}

#define SetShaderConst(func, StartRegister, pConstantData, Count, size, currentUID) \
  { \
    ClientMessage c(Commands::IDirect3DDevice9Ex_##func, getId()); \
    currentUID = c.get_uid(); \
    c.send_many(StartRegister, Count); \
    c.send_data(size, (void*)pConstantData); \
  }

extern NamedSemaphore* gpPresent;
extern std::mutex gSwapChainMapMutex;
extern SwapChainMap gSwapChainMap;

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::onDestroy() {
  // At this point the underlying d3d9 device's refcount should be 0 and device released
  assert(getRef<D3DRefCounted::Ref::Object>() == 0 &&
         "Destroying an LSS device object with underlying D3D9 object refcount > 0!");
   ClientMessage c { Commands::IDirect3DDevice9Ex_Destroy, getId() };
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::releaseInternalObjects(bool resetState) {
  // Take references first so that the device won't be
  // destroyed unintentionally and to prevent releaseInternalObjects()
  // recursion.
  const auto implicitRefCnt = m_implicitRefCnt; // m_implicitRefCnt will invalidate on destroy
  for (uint32_t n = 0; n < implicitRefCnt; n++) {
    D3DBase::AddRef();
  }

  // The Sims 3 camera hook: its own objects (shader variants, scratch targets, cut wall geometry)
  // and references go first, on Reset and on the device's destruction alike.
  if (sims3cam::enabled()) sims3OnReset(g_sims3);

  destroyImplicitObjects();

  if (resetState) {
    for (auto& texture : m_state.textures) {
      texture.reset(nullptr);
    }

    for (auto& rt : m_state.renderTargets) {
      rt.reset(nullptr);
    }

    for (auto& st : m_state.streams) {
      st.reset(nullptr);
    }

    m_state.indices.reset(nullptr);
    m_state.depthStencil.reset(nullptr);
    m_state.vertexShader.reset(nullptr);
    m_state.pixelShader.reset(nullptr);
    m_state.vertexDecl.reset(nullptr);
  }

  for (uint32_t n = 0; n < implicitRefCnt; n++) {
    D3DBase::Release();
  }
}

/*
 * Direct3DDevice9 Interface Implementation
 */
template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::QueryInterface(REFIID riid, LPVOID* ppvObj) {
  ZoneScoped;
  LogFunctionCall();
  if (ppvObj == nullptr) {
    return E_POINTER;
  }

  *ppvObj = nullptr;

  if (riid == __uuidof(IUnknown)
    || riid == __uuidof(IDirect3DDevice9)
    || (m_ex && riid == __uuidof(IDirect3DDevice9Ex))) {
    *ppvObj = bridge_cast<IDirect3DDevice9Ex*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

template<bool EnableSync>
ULONG Direct3DDevice9Ex_LSS<EnableSync>::AddRef() {
  ZoneScoped;
  LogFunctionCall();
  // Let the server control it's own device lifetime completely - no push
  return D3DBase::AddRef() - m_implicitRefCnt;
}

template<bool EnableSync>
ULONG Direct3DDevice9Ex_LSS<EnableSync>::Release() {
  ZoneScoped;
  LogFunctionCall();

  const ULONG cnt = D3DBase::Release();
  const bool bDestroy = !m_bIsDestroying && (cnt == m_implicitRefCnt);
  if (bDestroy) {
    m_bIsDestroying = true;
    // Device is about to be destroyed - release internal objects.
    releaseInternalObjects();
    return 0;
  }

  if (cnt > m_implicitRefCnt) {
    return cnt - m_implicitRefCnt;
  } else {
    return 0;
  }
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::TestCooperativeLevel() {
  ZoneScoped;
  LogFunctionCall();
  // This returns failure on uniqueness change - so ignore it for now, seems benign.
  // TODO: Return device removed when server dies
  return D3D_OK;
}

template<bool EnableSync>
UINT Direct3DDevice9Ex_LSS<EnableSync>::GetAvailableTextureMem() {
  ZoneScoped;
  LogFunctionCall();
  
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetAvailableTextureMem, getId());
    currentUID = c.get_uid();
  }
  WAIT_FOR_SERVER_RESPONSE("GetAvailableTextureMem()", 0, currentUID);
  // Available memory in MB
  UINT mem = (UINT) DeviceBridge::get_data();
  DeviceBridge::pop_front();

  return mem;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::EvictManagedResources() {
  ZoneScoped;
  LogFunctionCall();

  UID currentUID = 0;
  // Send command to server and wait for response
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_EvictManagedResources, getId());
    currentUID = c.get_uid();
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("EvictManagedResources()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetDirect3D(IDirect3D9** ppD3D9) {
  ZoneScoped;
  LogFunctionCall();

  if (ppD3D9 == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      (*ppD3D9) = m_pDirect3D;
      m_pDirect3D->AddRef();
    }
    if (GlobalOptions::getSendReadOnlyCalls()) {
      ClientMessage { Commands::IDirect3DDevice9Ex_GetDirect3D, getId() };
    }
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::internalGetDeviceCaps(D3DCAPS9* pCaps) {
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetDeviceCaps, getId());
    currentUID = c.get_uid();
  }

  WAIT_FOR_SERVER_RESPONSE("GetDeviceCaps()", D3DERR_INVALIDCALL, currentUID);

  HRESULT hresult = DeviceBridge::get_data();
  if (SUCCEEDED(hresult)) {
    uint32_t len = DeviceBridge::copy_data(*pCaps);
    if (len != sizeof(D3DCAPS9) && len != 0) {
      Logger::err("GetDeviceCaps() failed due to issue with data returned from server.");
      hresult = D3DERR_INVALIDCALL;
    }
  }
  DeviceBridge::pop_front();

  return hresult;
}


template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetDeviceCaps(D3DCAPS9* pCaps) {
  ZoneScoped;
  LogFunctionCall();

  if (pCaps == NULL)
    return D3DERR_INVALIDCALL;

  *pCaps = m_caps;

  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) {
  ZoneScoped;
  LogFunctionCall();

  if (pMode == NULL)
    return D3DERR_INVALIDCALL;

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetDisplayMode, getId());
    currentUID = c.get_uid();
    c.send_data(iSwapChain);
  }
  WAIT_FOR_SERVER_RESPONSE("GetDisplayMode()", D3DERR_INVALIDCALL, currentUID);

  HRESULT hresult = DeviceBridge::get_data();
  if (SUCCEEDED(hresult)) {
    uint32_t len = DeviceBridge::copy_data(*pMode);
    if (len != sizeof(D3DDISPLAYMODE) && len != 0) {
      Logger::err("GetDisplayMode() failed due to issue with data returned from server.");
      hresult = D3DERR_INVALIDCALL;
    }
  }
  DeviceBridge::pop_front();
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParameters) {
  ZoneScoped;
  LogFunctionCall();

  if (pParameters == NULL)
    return D3DERR_INVALIDCALL;

  *pParameters = m_createParams;
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) {
  ZoneScoped;
  LogFunctionCall();

  if (pCursorBitmap == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  const auto pLssSurface = bridge_cast<Direct3DSurface9_LSS*>(pCursorBitmap);
  if (pLssSurface) {
    UID currentUID = 0;
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetCursorProperties, getId());
      currentUID = c.get_uid();
      c.send_many(XHotSpot, YHotSpot, pLssSurface->getId());
    }
    WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetCursorProperties()", D3DERR_INVALIDCALL, currentUID);
  }
  return S_OK;
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::SetCursorPosition(int X, int Y, DWORD Flags) {
  ZoneScoped;
  LogFunctionCall();

  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_SetCursorPosition, getId());
    c.send_many(X, Y, Flags);
  }
}

template<bool EnableSync>
BOOL Direct3DDevice9Ex_LSS<EnableSync>::ShowCursor(BOOL bShow) {
  ZoneScoped;
  LogFunctionCall();

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_ShowCursor, getId());
    currentUID = c.get_uid();
    c.send_data(bShow);
  }
  WAIT_FOR_SERVER_RESPONSE("ShowCursor()", false, currentUID);
  BOOL prevShow = (BOOL) DeviceBridge::get_data();
  DeviceBridge::pop_front();

  return prevShow;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** ppSwapChain) {
  ZoneScoped;
  LogFunctionCall();

  if (pPresentationParameters == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  const auto presentationParameters = Direct3DSwapChain9_LSS::sanitizePresentationParameters(*pPresentationParameters, getCreateParams());

  // Insert our own IDirect3DTexture9 interface implementation
  Direct3DSwapChain9_LSS* pLssSwapChain = trackWrapper(new Direct3DSwapChain9_LSS(this, presentationParameters));
  (*ppSwapChain) = pLssSwapChain;

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_CreateAdditionalSwapChain, getId());
    currentUID = c.get_uid();
    c.send_data((uint32_t) pLssSwapChain->getId());
    c.send_data(sizeof(D3DPRESENT_PARAMETERS), &presentationParameters);
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateAdditionalSwapChain()", D3DERR_NOTAVAILABLE, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) {
  ZoneScoped;
  LogFunctionCall();

  if (pSwapChain == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pSwapChain = m_pSwapchain;
    m_pSwapchain->AddRef();
  }

  if (GlobalOptions::getSendReadOnlyCalls()) {
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetSwapChain, getId());
    c.send_data(iSwapChain);
  }
  
  return S_OK;
}

template<bool EnableSync>
unsigned int Direct3DDevice9Ex_LSS<EnableSync>::GetNumberOfSwapChains() {
  ZoneScoped;
  LogFunctionCall();
  // DXVK does not support >1 implicit swapchains (those that are created during CreateDevice).
  static constexpr int kNumImplicitSwapChains = 1;
  if (GlobalOptions::getSendReadOnlyCalls()) {
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetNumberOfSwapChains, getId());
    c.send_data(kNumImplicitSwapChains);
  }
  return kNumImplicitSwapChains;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) {
  ZoneScoped;
  LogFunctionCall();
  HRESULT res = S_OK;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    // Clear all device state and release implicit/internal objects
    releaseInternalObjects();
    // Reset all device state to default values and init implicit/internal objects
    ResetState();
    const auto presParam = Direct3DSwapChain9_LSS::sanitizePresentationParameters(*pPresentationParameters, getCreateParams());
    m_presParams = presParam;
    WndProc::unset();
    WndProc::set(getWinProcHwnd());
    // Tell Server to do the Reset
    size_t currentUID = 0;
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_Reset, getId());
      currentUID = c.get_uid();
      c.send_data(sizeof(D3DPRESENT_PARAMETERS), &presParam);
    }

    // Perform an WAIT_FOR_OPTIONAL_SERVER_RESPONSE but don't return since we still have work to do.
    if (GlobalOptions::getSendAllServerResponses()) {
      const uint32_t timeoutMs = GlobalOptions::getAckTimeout();
      if (Result::Success != DeviceBridge::waitForCommand(Commands::Bridge_Response, timeoutMs, nullptr, true, currentUID)) {
        Logger::err("Direct3DDevice9Ex_LSS::Reset() failed with : no response from server.");
      }
      res = (HRESULT) DeviceBridge::get_data();
      DeviceBridge::pop_front();
      }

    // Reset swapchain and link server backbuffer/depth buffer after the server reset its swapchain, or we will link to the old backbuffer/depth resources
    initImplicitObjects(presParam);
    // Keeping a track of previous present parameters, to detect and handle mode changes
    m_previousPresentParams = *pPresentationParameters;
  }
  return res;
}

HRESULT syncOnPresent() {
#ifdef ENABLE_PRESENT_SEMAPHORE_TRACE
  Logger::trace("Client side Present call received, acquiring semaphore...");
#endif

  // If we're syncing with the server on Present() then wait for the semaphore to be released
  if (GlobalOptions::getPresentSemaphoreEnabled()) {
    const auto maxRetries = GlobalOptions::getCommandRetries();
    size_t numRetries = 0;
    while (gbBridgeRunning && RESULT_FAILURE(gpPresent->wait()) && numRetries++ < maxRetries) {
      Logger::warn("Still waiting on the Present semaphore to be released...");
    }
    if (numRetries >= maxRetries) {
      Logger::err("Max retries reached waiting on the Present semaphore!");
      return ERROR_SEM_TIMEOUT;
    } else if (!gbBridgeRunning) {
      Logger::err("Bridge was disabled while waiting on the Present semaphore, aborting current operation!");
      return ERROR_OPERATION_ABORTED;
#ifdef ENABLE_PRESENT_SEMAPHORE_TRACE
    } else {
      Logger::trace("Present semaphore acquired successfully.");
#endif
    }
  }
  return S_OK;
}

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
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   lights: lamps %u (%u registered, %u released, lit %u times, put out %u times, %u light calls); sun updates %u, frames without shadow rows %u, frames without a sun candidate %u, sun luminance now %.3f",
           h.lamps.nLamps, h.lamps.created, h.lamps.dropped, h.lamps.lit, h.lamps.out, h.lampEvents, h.sunChanges, h.framesNoShadow, h.framesNoSunVote, sims3cam::luminance(h.sun.col));
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   night: sun level %.3f (day reference %.3f), sky brightness %.3f and exposure ceiling %.2f EV sent %u times%s",
           h.skyLevel, h.skyDayRef, h.skyBrightnessSent, h.evMaxSent, h.skySends, GlobalOptions::getExposeRemixApi() ? "" : " (Remix API off: nothing sent)");
  Logger::info(msg);
  uint32_t lampHandles = 0;
  for (uint32_t k = 0; k < h.lamps.nLamps; ++k) { const sims3cam::Lamp& Lh = h.lamps.lamps[k]; lampHandles += (Lh.api ? 1u : 0u) + (Lh.api2 ? 1u : 0u) + (Lh.api3 ? 1u : 0u); }
  uint32_t unmapped = 0, unjudged = 0; char maps[220]; size_t mn = 0; maps[0] = 0;
  for (uint32_t k = 0; k < h.lamps.nLamps; ++k) { const sims3cam::Lamp& L = h.lamps.lamps[k]; if (!L.map) ++unmapped; else if (L.state < 0) ++unjudged; }
  for (int k = 0; k < 4 && mn < sizeof maps - 60; ++k) {
    const Sims3Hook::LightMapEntry& e = h.lightMaps[k];
    if (!e.tex) continue;
    mn += (size_t) snprintf(maps + mn, sizeof maps - mn, "%s%p %ux%u v%u (%u draws%s)", mn ? ", " : "", (void*) e.tex, e.W, e.H, e.version, e.draws, e.valid ? "" : ", unread");
  }
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   light maps: %s; %u versions decoded, %u judgements, %u decode failures; lamps: %u of %u without a map, %u beyond theirs (%u fell beyond), %u changed map",
           mn ? maps : "none noted", h.lightMapDecodes, h.lightMapVerdicts, h.lightMapFails, unmapped, h.lamps.nLamps, unjudged, h.lightMapOutside, h.lightMapSwitches);
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   lamp reporter: %s; %u lamps reported, %u of the hook's %u lamps named by it; %u readings (%u while it was writing), %u searches",
           !sims3cam::lampReporter() ? "switched off (lampReporter = 0)" : (h.lampReportLive ? "live" : (g_sims3LampBlock.load() ? "found, no world loaded" : "not found (the script mod is not installed, or no world has loaded yet)")),
           h.lampReported, h.lampReportMatched, h.lamps.nLamps, h.lampReportReads, h.lampReportStale, g_sims3LampScans.load());
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   api lights: %s; %u calls, sun %s, %u lamp handles; the game's light table: %s",
           GlobalOptions::getExposeRemixApi() ? "on" : "off (no lights: exposeRemixApi is not set)", h.apiLightCalls, h.sunApi ? "live" : "none", lampHandles,
           sims3cam::liteTable().n ? format_string("%u models read from sims3lights.txt, %u meshes matched", sims3cam::liteTable().n, h.liteMatches).c_str() : "no sims3lights.txt next to the DLL (no lamps; run sims3/tools/lite_table.py --hook)");
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   walls: %u draws, openings cut in %u (%u triangles cut, %u removed, %u hidden dropped; %u geometries built, %u evicted, %u build failures, %u skipped, %u masks decoded)",
           h.wallDraws, h.wallCutDraws, h.wallCutTriangles, h.wallRemovedTriangles, h.wallHiddenTriangles, h.wallBuilt, h.wallEvicted, h.wallBuildFailed, h.wallSkipped, h.wallMasksDecoded);
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   reflections: %u mirrored camera uploads; %u draws dropped in %u frames (%u of them by the stencil mirror's render states alone)",
           h.mirroredUploads, h.reflectionDrops, h.reflectionFrames, h.reflectionDropsByStates);
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   terrain: %u base draws and %u layer passes (%u lot chunk copies) for the baker, %u without a variant; %u pixel shader variants (%u unlit, %u with alpha forced to 1), %u sampler states copied, sRGB sampling turned off %u times, both held across %u terrain blocks (%u holds cancelled by the game, %u uncaptured draws let through); markers %s; %u lot chunk copies dropped",
           h.terrainBaseDraws, h.terrainLayerDraws, h.terrainLotCopyDraws, h.terrainNoVariant, h.psVariantsMade, h.psVariantsUnlit, h.psVariantsAlpha, h.samplerCopies, h.srgbOffs, h.tblockFlushes, h.tblockCancelled, h.tblockKept, h.markersConfigSent ? "tagged in rtx.conf" : (h.marker[0] ? "made, NOT tagged in rtx.conf" : "not made yet"), h.lotCopyDrops);
  Logger::info(msg);
  MEMORYSTATUSEX ms = {}; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   lot paint: %u composite second passes, %u lot re-submissions split in two; %u changes seen on the lot paint textures; client copies of surfaces %u MB, address space in use %u of %u MB",
           h.compositePasses, h.splitDraws, h.lotPaintChanges, (unsigned) (Direct3DSurface9_LSS::sims3ShadowBytes() >> 20), (unsigned) ((ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20), (unsigned) (ms.ullTotalVirtual >> 20));
  Logger::info(msg);
  snprintf(msg, sizeof msg, "Sims 3 camera hook:   cameras: transforms sent to the runtime %u times (%u with the projection changed); %u frames with a second main camera (%u uploads not adopted); sky dome draws presented as the sky %u",
           h.transformSends, h.transformSendsProj, h.framesWithAlt, h.altUploadsTotal, h.skyDraws);
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

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::Present(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion) {
  ZoneScoped;
  LogFunctionCall();

  // If the bridge was disabled in the meantime for some reason we want to bail
  // out here so we don't spend time waiting on the Present semaphore or trying
  // to send keyboard state to the server.
  if (!gbBridgeRunning) {
    return D3D_OK;
  }

  if (remixapi::g_bInterfaceInitialized && remixapi::g_presentCallback) {
    remixapi::g_presentCallback();
  }

  // The Sims 3 camera hook: periodic diagnostics. The summary line every 600 frames (short
  // sessions still get one from inside the lot), the per-shader capture table every 3600.
  if (sims3cam::enabled()) {
    ++g_sims3.frames;
    if (g_sims3.frames == 300 || g_sims3.frames % 600 == 0) {
      sims3LogStats(g_sims3.frames == 300 || g_sims3.frames % 3600 == 0);
    }
    // the frame's camera variety (run 69 diagnostics): more than one main camera in a frame
    auto& h = g_sims3;
    if (h.frameAltUploads) {
      ++h.framesWithAlt;
      if (h.altLogged < 16) {
        ++h.altLogged;
        const sims3cam::Camera& f = h.frameCam;
        const float pa = f.proj._33;
        const float farZ = (std::fabs(pa + 1.f) > 1e-6f) ? pa * f.nearZ / (pa + 1.f) : 0.f;
        char msg[1024];
        int n = snprintf(msg, sizeof msg, "Sims 3 camera hook: camera variety at frame %u -> first main camera fov %.2f near %.4f far %.1f (P33 %.7f P43 %.5f) aspect %.3f eye %.2f,%.2f,%.2f fwd.y %.3f (%u uploads; %u unlike it, not adopted):",
                         h.frames, f.fovY * 57.2958f, f.nearZ, farZ, f.proj._33, f.proj._43, f.aspect, f.pos[0], f.pos[1], f.pos[2], f.fwd[1], h.frameMainUploads - h.frameAltUploads, h.frameAltUploads);
        for (uint32_t k = 0; k < h.frameAltCount && n > 0 && n < (int) sizeof msg - 200; ++k) {
          const Sims3Hook::AltCam& a = h.frameAlts[k];
          n += snprintf(msg + n, sizeof msg - n, " [%u: fov %.2f near %.4f far %.1f (P33 %.7f P43 %.5f) aspect %.3f eye %.2f,%.2f,%.2f fwd.y %.3f x%u%s, first VS %016llx]", k + 1, a.fovY * 57.2958f, a.nearZ, a.farZ, a.p33, a.p43, a.aspect, a.pos[0], a.pos[1], a.pos[2], a.fwdY, a.count, a.first ? " (second upload of the frame)" : "", (unsigned long long) a.vs);
        }
        Logger::info(msg);
      }
    }
    h.frameCamSet = false; h.frameMainUploads = 0; h.frameAltUploads = 0; h.frameAltCount = 0;
    h.lotCopies.clear();   // the lot meshes drawn this frame (milestone 16)
    sims3TerrainBlockEnd(h, this);   // the frame is over: the game's sampler states back (milestone 18g)
    {
      char t[80];
      snprintf(t, sizeof t, "PRESENT d%u C%u U%u D%u H%u R%u tw%u tl%u cam%u/%u/%u", h.frameDraws, h.fCaptured, h.fUncaptured, h.fDropped, h.fHeld, h.fReplayed, h.fTerrainWorld, h.fTerrainLot, h.fCamAdopt, h.fCamAlt, h.fCamMirror);
      sims3RingPush(h, t);
      h.fCaptured = h.fUncaptured = h.fDropped = h.fHeld = h.fReplayed = h.fTerrainWorld = h.fTerrainLot = h.fCamAdopt = h.fCamAlt = h.fCamMirror = 0;
      const bool f9 = ((GetAsyncKeyState(VK_F9) | GetAsyncKeyState(sims3cam::markKey()) | GetAsyncKeyState(VK_OEM_3)) & 0x8000) != 0;   // F9, the configured key (sims3hook.txt markKey) or backtick
      if (f9 && !h.f9Down) { sims3RingDump(h, "F9 pressed"); h.markDump = 2; h.markDumpLogged = 0; h.markRigLogged = 0; h.markMeshCount = 0; }   // and the lamp objects and object draws of the next two frames (milestones 24, 29)
      else if (h.markDump) --h.markDump;
      h.f9Down = f9;
    }
    h.frameDraws = 0;
  }

  // The Sims 3 camera hook: the lamps (milestone 23): the game's own lights, drawn as Remix API
  // sphere lights while their witnesses say they are on.
  if (sims3cam::enabled()) {
    auto& h = g_sims3;
    const bool api = GlobalOptions::getExposeRemixApi();
    const uint32_t raysThisFrame = h.lamps.nRays;
    // The lamps' own word (milestone 36): the lamp reporter's block, found once and read every frame.
    // A lamp it names takes its state, its colour and its level from it; the maps below decide the rest.
    if (sims3cam::lampReporter()) {
      const uint8_t* block = g_sims3LampBlock.load();
      if (!block && !g_sims3LampScanBusy.load() && (h.lampReportScanFrame == 0 || h.frames - h.lampReportScanFrame > (g_sims3LampScans.load() < 12u ? 600u : 3600u))) {
        h.lampReportScanFrame = h.frames ? h.frames : 1;
        g_sims3LampScanBusy = true;
        std::thread(sims3LampScan).detach();
      }
      h.lampReportLive = false;
      if (block) {
        if (h.lampRecords.size() < (size_t) (sims3cam::kLampCapacity + 1) * sims3cam::kLampFloats) h.lampRecords.resize((size_t) (sims3cam::kLampCapacity + 1) * sims3cam::kLampFloats);
        static std::vector<float> fresh; if (fresh.size() < h.lampRecords.size()) fresh.resize(h.lampRecords.size());
        uint32_t lamps = 0, seq = 0; bool world = false;
        const int r = sims3LampRead(block, fresh.data(), lamps, seq, world);
        if (r == 0) { h.lampRecords.swap(fresh); h.lampReported = lamps; h.lampReportSeq = seq; h.lampReportWorld = world; ++h.lampReportReads; h.lampReportFails = 0; }
        else if (r == 1) ++h.lampReportStale;   // being written: the last whole reading stands
        else if (++h.lampReportFails > 300u) { g_sims3LampBlock = nullptr; h.lampReported = 0; h.lampReportWorld = false; h.lampReportFails = 0; Logger::info("Sims 3 camera hook: the lamp reporter's block is gone; searching again"); }
        h.lampReportLive = h.lampReportReads > 0 && h.lampReportWorld;
        if (h.lampReportLive && !h.lampReportAnnounced) {
          h.lampReportAnnounced = true;
          const float* f0 = h.lampRecords.data();
          char msg[300];
          snprintf(msg, sizeof msg, "Sims 3 camera hook: the lamp reporter found at %p after %u searches, frame %u: %u lamps within %.0f units of the camera target (%.1f, %.1f, %.1f); the game's intensity levels dim %.2f, normal %.2f, bright %.2f",
                   (const void*) block, g_sims3LampScans.load(), h.frames, h.lampReported, f0[3], f0[0], f0[1], f0[2], f0[4], f0[5], f0[6]);
          Logger::info(msg);
        }
      }
      h.lampReportMatched = 0;
      for (uint32_t k = 0; k < h.lamps.nLamps; ++k) {
        sims3cam::Lamp& L = h.lamps.lamps[k];
        const float* rec = h.lampReportLive ? sims3cam::lampReportFind(h.lampRecords.data() + sims3cam::kLampFloats, h.lampReported, L.anchor) : nullptr;
        if (!rec) { L.reported = false; continue; }
        const sims3cam::LampWord w = sims3cam::lampWordFromRecord(rec, L.def, L.defIntensity, h.lampRecords[5]);
        const bool was = L.reported && L.state == 1;
        L.reported = true; ++h.lampReportMatched;
        L.state = w.on ? (int8_t) 1 : (int8_t) 0;
        for (int q = 0; q < 3; ++q) L.col[q] = w.col[q];
        if (was != w.on && h.lampWordsLogged < 600u) {
          ++h.lampWordsLogged; char msg[300];
          snprintf(msg, sizeof msg, "Sims 3 camera hook: lamp at (%.1f, %.1f, %.1f) %s by the game's own word at frame %u: colour preset %d (%.2f, %.2f, %.2f), intensity %.2f (level x%.2f), the engine's dimmer %.2f; sent colour %.2f, %.2f, %.2f",
                   L.anchor[0], L.anchor[1], L.anchor[2], w.on ? "ON" : "off", h.frames, (int) (rec[9] + 0.5f), rec[3], rec[4], rec[5], rec[6], w.level, rec[7], w.col[0], w.col[1], w.col[2]);
          Logger::info(msg);
        }
      }
    }
    // Every light map carried this frame is decoded anew when its content changed (milestone 29) and
    // then judges the lamps that sample it, drawn this frame or not; a lamp its map has not judged yet
    // is judged as soon as it can be. A map no draw has carried for ten seconds is forgotten (its lamps
    // go back to their witnesses); a map is only ever read through a texture bound this frame.
    bool changed[4] = {};
    for (int k = 0; k < 4; ++k) {
      Sims3Hook::LightMapEntry& e = h.lightMaps[k];
      if (!e.tex) continue;
      if (h.frames - e.lastFrame > 600u) { e.tex = nullptr; e.valid = false; e.data.clear(); e.prev.clear(); e.hasPrev = false; continue; }
      if (h.frames - e.lastFrame > 1u) continue;
      const bool unseen = h.frames - e.lastCheck > 120u;   // not looked at for two seconds: its changes since are not one switch's
      e.lastCheck = h.frames;
      if (!sims3LightMapDecode(h, e, unseen)) continue;
      changed[k] = e.judged != e.version; e.judged = e.version;
    }
    // Each lamp's own part of the light at its base, per map (milestones 33, 34). By night -- the sun
    // level under the night level for ten seconds with the maps live; a level of 0 at a load says
    // nothing yet -- the map is read as it is, by the reading of runs 143 to 145. By day a lamp's own
    // part moves only with an abrupt change of its own base. On when the own part in any map reaches
    // the on level.
    bool live = false;
    for (int k = 0; k < 4; ++k) live = live || (h.lightMaps[k].tex && h.lightMaps[k].valid && h.frames - h.lightMaps[k].lastFrame <= 1u);
    if (live && h.skyLevel >= 0.f && h.skyLevel < sims3cam::lampNightLevel()) { if (h.lampNightFrames < 100000u) ++h.lampNightFrames; }
    else if (live) h.lampNightFrames = 0;
    const bool night = h.lampNightFrames >= 600u;
    const bool turned = night != h.lampNight;   // dusk or dawn: every lamp is read again
    if (turned) {
      h.lampNight = night;
      char msg[200];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: the lamps' maps are read %s from frame %u (sun level %.2f)", night ? "as they are: night" : "by their changes: day", h.frames, h.skyLevel);
      Logger::info(msg);
    }
    const float jump = sims3cam::lampJump(), onLevel = sims3cam::lampOwnOn(), ring = sims3cam::lampRing();
    for (uint32_t k = 0; k < h.lamps.nLamps; ++k) {
      sims3cam::Lamp& L = h.lamps.lamps[k];
      if (L.reported) continue;   // the game's own word stands (milestone 36)
      bool covered = false, wentOut = false, cameOn = false; float best = 0.f;
      for (int j = 0; j < 4; ++j) {
        const Sims3Hook::LightMapEntry& e = h.lightMaps[j];
        if (!e.tex || !e.valid) continue;
        const bool init = !(L.ownInit & (1u << j));
        if (init || changed[j] || turned) {
          Sims3LampReading r;
          if (!sims3LightMapOwn(e, L.anchor, ring, r)) { L.ownInit &= (uint8_t) ~(1u << j); L.own[j] = 0.f; continue; }   // beyond this map
          const float was = init ? 0.f : L.own[j];
          const bool compared = changed[j] && !init && e.hasPrev && !e.fresh;
          const float seen = init ? (float) r.base : L.seen[j];
          if (night) L.own[j] = sims3cam::lampLitByNight(r.base, r.darkest) ? (float) r.base : 0.f;
          else if (compared) L.own[j] = sims3cam::lampOwnDay(was, (float) r.base, (float) r.dBase, (float) r.dRing, jump);
          else L.own[j] = sims3cam::lampOwnAfterGap(was, (float) r.base, seen);   // by day with no version to compare: the lamp's record of its base
          L.seen[j] = (float) r.base;
          L.ownInit |= (uint8_t) (1u << j); ++h.lightMapVerdicts;
          if ((was >= onLevel) != (L.own[j] >= onLevel)) {
            if (!night) { if (L.own[j] >= onLevel) cameOn = true; else wentOut = true; }
            if (h.lampFlipsLogged < (night ? 150u : 600u)) {   // the day's switches have the larger budget: they are the ones in question
              ++h.lampFlipsLogged; char msg[340];
              snprintf(msg, sizeof msg, "Sims 3 camera hook: lamp at (%.1f, %.1f, %.1f) %s by light map %ux%u v%u at frame %u: base %d (moved by %d, %.0f at its last reading), the ring around it %d (moved by %d), the darkest near it %d; its own part %.0f -> %.0f; %s, sun level %.2f",
                       L.anchor[0], L.anchor[1], L.anchor[2], L.own[j] >= onLevel ? "ON" : "off", e.W, e.H, e.version, h.frames, r.base, r.dBase, seen, r.around, r.dRing, r.darkest,
                       was, L.own[j], night ? "read as it is (night)" : (compared ? "by its change (day)" : "no version to compare (day)"), h.skyLevel);
              Logger::info(msg);
            }
          }
        }
        if (L.ownInit & (1u << j)) covered = true;
      }
      // by day a map that saw the lamp go out puts it out in every map (milestone 35): another map may
      // not have seen the change, and what it holds would keep the lamp lit until the night
      if (wentOut && !cameOn) for (int j = 0; j < 4; ++j) L.own[j] = 0.f;
      for (int j = 0; j < 4; ++j) if ((L.ownInit & (1u << j)) && L.own[j] > best) best = L.own[j];
      if (covered) L.state = best >= onLevel ? (int8_t) 1 : (int8_t) 0;
      else if (L.state >= 0) { L.state = -1; ++h.lightMapOutside; }   // beyond every map now: back to its witnesses
    }
    h.lamps.solve();
    for (uint32_t k = 0; k < h.lamps.nEvents && h.lampEventsLogged < 600; ++k) {   // every lamp's lighting and putting out, while the budget lasts (milestone 35: 600, a day and a night of auto-lights took 120 in six minutes)
      const sims3cam::LampSolver::Event& e = h.lamps.events[k];
      ++h.lampEventsLogged; char msg[260];
      if (e.kind == 1)
        snprintf(msg, sizeof msg, "Sims 3 camera hook: lamp lit at frame %u on the object at (%.1f, %.1f, %.1f), light height %.1f: %u witness rays, colour %.2f,%.2f,%.2f",
                 h.frames, e.anchor[0], e.anchor[1], e.anchor[2], e.y, e.votes, e.col[0], e.col[1], e.col[2]);
      else
        snprintf(msg, sizeof msg, "Sims 3 camera hook: lamp out at frame %u, the object at (%.1f, %.1f, %.1f), light height %.1f, after %u lit frames (%s)", h.frames, e.anchor[0], e.anchor[1], e.anchor[2], e.y, e.age, e.votes ? "the map or the witnesses said off" : "its object undrawn for ten seconds");
      Logger::info(msg);
    }
    for (uint32_t k = 0; k < h.lamps.nDropped; ++k) {   // the lamps gone this frame
      if (h.lamps.droppedApi[k]) { remixapi::remixapi_DestroyLight((remixapi_LightHandle) h.lamps.droppedApi[k]); ++h.apiLightCalls; ++h.lampEvents; }
    }
    for (uint32_t k = 0; k < h.lamps.nLamps && api; ++k) {
      sims3cam::Lamp& L = h.lamps.lamps[k];
      if (L.age < sims3cam::LampSolver::kConfirmFrames) continue;
      const bool changed = !L.sent || sims3cam::lampDist(L.sentPos, L.pos) > 0.25f ||
                           std::fabs(L.sentCol[0] - L.col[0]) > 0.1f || std::fabs(L.sentCol[1] - L.col[1]) > 0.1f || std::fabs(L.sentCol[2] - L.col[2]) > 0.1f ||
                           sims3cam::dot3(L.sentDir, L.dir) < 0.999f * sims3cam::dot3(L.dir, L.dir);   // the object turned
      if (changed) {
        h.apiLightCalls += sims3ApiLamp(L);
        for (int q = 0; q < 3; ++q) { L.sentPos[q] = L.pos[q]; L.sentCol[q] = L.col[q]; L.sentDir[q] = L.dir[q]; }
        L.sent = true; ++h.lampEvents;
        if (L.kind < 32 && !(h.loggedShapes & (1u << L.kind))) {   // the first lamp of each type, with its shape (milestone 32)
          h.loggedShapes |= 1u << L.kind;
          char msg[360];
          snprintf(msg, sizeof msg, "Sims 3 camera hook: first lamp of type %u (3 point, 4 spot, 5 lamp shade, 6 tube) forwarded at (%.1f, %.1f, %.1f): its light travels (%.2f, %.2f, %.2f), cone %.0f degrees from the axis, opposite cone %.0f, shade light %.2f,%.2f,%.2f, tube %.2f; shapes %s, cone scale %.2f; API lights: main %s, opposite %s, shade %s",
                   (unsigned) L.kind, L.pos[0], L.pos[1], L.pos[2], L.dir[0], L.dir[1], L.dir[2], L.angle, L.bottom, L.shade[0], L.shade[1], L.shade[2], L.tube,
                   sims3cam::lampShapes() ? "on" : "off", sims3cam::lampConeScale(), L.api ? "yes" : "no", L.api2 ? "yes" : "no", L.api3 ? "yes" : "no");
          Logger::info(msg);
        }
        if (!h.loggedLamp) {
          h.loggedLamp = true;
          char msg[260];
          snprintf(msg, sizeof msg, "Sims 3 camera hook: first lamp forwarded as an API sphere light at (%.1f, %.1f, %.1f), the object at (%.1f, %.1f, %.1f), colour %.2f,%.2f,%.2f, %u witness rays; %u rig rays this frame",
                   L.pos[0], L.pos[1], L.pos[2], L.anchor[0], L.anchor[1], L.anchor[2], L.col[0], L.col[1], L.col[2], L.support, raysThisFrame);
          Logger::info(msg);
        }
      }
      if (L.api) remixapi::remixapi_DrawLightInstance((remixapi_LightHandle) L.api);   // every frame the lamp is on
      if (L.api2) remixapi::remixapi_DrawLightInstance((remixapi_LightHandle) L.api2);
      if (L.api3) remixapi::remixapi_DrawLightInstance((remixapi_LightHandle) L.api3);
    }
  }

  // The Sims 3 camera hook: the sun as light 0. Its direction comes from the shadow-map rows
  // seen this frame (the vote picks a different key light indoors); the rig vote supplies the
  // colour of the candidate that matches that direction. Without fresh shadow rows, the vote
  // decides, with hysteresis.
  if (sims3cam::enabled()) {
    auto& h = g_sims3;
    sims3cam::SunVote v;
    const bool haveVote = h.voter.best(v);
    if (!haveVote) ++h.framesNoSunVote;   // no rig light above the horizon brighter than the floor this frame (milestone 19d)
    if (h.frames != h.shadowDirFrame) ++h.framesNoShadow;
    bool send = false;
    sims3cam::SunVote next = h.sun;
    if (h.shadowDirValid) {
      // Once the shadow map has given a direction it keeps it (the far view may stop drawing
      // the shaders that carry the rows; the sun does not move in the meantime).
      if (!h.sunSet) {
        for (int q = 0; q < 3; ++q) next.dir[q] = h.shadowDir[q];
      } else {
        // A direction far from the current sun (more than ~11 degrees) has to hold for 30 frames
        // before it moves the sun (milestone 17x): the sun never jumps, but rows read from an
        // upload that is not the sun's shadow matrix would otherwise swing the shadows.
        const float agree = sims3cam::dot3(h.shadowDir, h.sun.dir);
        if (agree < 0.98f) ++h.sunJumpFrames; else h.sunJumpFrames = 0;
        if (agree >= 0.98f || h.sunJumpFrames >= 30) {
          for (int q = 0; q < 3; ++q) next.dir[q] = h.sun.dir[q] + 0.1f * (h.shadowDir[q] - h.sun.dir[q]);
          const float n = sims3cam::len3(next.dir);
          if (n > 1e-6f) for (int q = 0; q < 3; ++q) next.dir[q] /= n;
        }
      }
      // Colour: the brightest matching candidate. Brighter is adopted at once; a much darker
      // one (a view of indoor objects, which see the sun attenuated -- or dusk) after a second
      // and then half-way per update (milestone 19e: the former 20-second hold and 20 % steps
      // took four minutes of real time to reach night, longer than a game night at top speed;
      // run 121's sun trace).
      sims3cam::SunVote match;
      if (h.voter.matching(h.shadowDir, match)) {
        const float lm = sims3cam::luminance(match.col), lc = sims3cam::luminance(h.sun.col);
        bool adopt = !h.sunSet || lm >= 0.7f * lc;
        if (!adopt && ++h.sunDarkFrames >= 60) adopt = true;
        if (!adopt && h.sunDarkFrames == 1 && ++h.sunDarkHeld <= 20) { char msg[200]; snprintf(msg, sizeof msg, "Sims 3 camera hook: sun: a darker matching rig candidate (luminance %.3f vs the sun's %.3f) held back at frame %u", lm, lc, h.frames); Logger::info(msg); }
        if (adopt) {
          h.sunDarkFrames = 0;
          const float a = h.sunSet ? 0.5f : 1.f;
          for (int q = 0; q < 3; ++q) next.col[q] = h.sun.col[q] + a * (match.col[q] - h.sun.col[q]);
          next.count = match.count;
        }
      } else if (!h.sunSet && haveVote) {
        for (int q = 0; q < 3; ++q) next.col[q] = v.col[q];
        next.count = v.count;
      }
      h.sunCandFrames = 0;
      send = !h.sunSet || !sims3cam::sameSun(next, h.sun);
      if (send && !h.loggedShadowSun) {
        h.loggedShadowSun = true;
        char msg[240];
        snprintf(msg, sizeof msg, "Sims 3 camera hook: sun direction taken from the shadow map (toward %.3f,%.3f,%.3f); colour %.2f,%.2f,%.2f from the matching rig candidate (%u votes)",
                 next.dir[0], next.dir[1], next.dir[2], next.col[0], next.col[1], next.col[2], next.count);
        Logger::info(msg);
      }
    } else if (haveVote) {
      if (!h.sunSet) {
        next = v; send = true;
      } else if (!sims3cam::sameSun(v, h.sun)) {
        // Hysteresis: a different winner must hold for 15 frames before it replaces the sun.
        if (h.sunCandFrames > 0 && sims3cam::sameSun(v, h.sunCand)) ++h.sunCandFrames;
        else { h.sunCand = v; h.sunCandFrames = 1; }
        if (h.sunCandFrames >= 15) { next = v; send = true; h.sunCandFrames = 0; }
      } else {
        h.sunCandFrames = 0;
      }
    }
    if (send) {
      if (GlobalOptions::getExposeRemixApi()) {   // milestones 20b, 23: the API or nothing
        h.sunApi = sims3ApiSun(h.sunApi, next); ++h.apiLightCalls;
        if (!h.loggedApiLights) {
          h.loggedApiLights = true; char msg[240];
          snprintf(msg, sizeof msg, "Sims 3 camera hook: the sun goes out as a Remix API distant light (angular diameter %.2f degrees, radiance x%.2f per unit of colour), the lamps as sphere lights (radius %.2f, radiance x%.1f)",
                   sims3cam::sunAngle(), sims3cam::sunRadiance(), sims3cam::lampRadius(), sims3cam::lampRadiance());
          Logger::info(msg);
        }
      } else if (!h.apiLightsWarned) {
        h.apiLightsWarned = true;
        Logger::warn("Sims 3 camera hook: no lights: the Remix API is off (exposeRemixApi = True in .trex\\bridge.conf turns it on for the bridge server)");
      }
      h.sun = next; h.sunSet = true;
      ++h.sunChanges;
      const float lum = sims3cam::luminance(next.col);
      const bool moved = h.sunLogLum < 0.f || std::fabs(lum - h.sunLogLum) > 0.1f * (h.sunLogLum > 0.f ? h.sunLogLum : 1.f) || sims3cam::dot3(next.dir, h.sunLogDir) < 0.996f;
      if (moved && h.sunLogged < 200) {   // the sun trace (milestone 19d): every step of brightness or direction
        ++h.sunLogged; h.sunLogLum = lum; for (int q = 0; q < 3; ++q) h.sunLogDir[q] = next.dir[q];
        char msg[240];
        snprintf(msg, sizeof msg, "Sims 3 camera hook: sun forwarded as light 0 at frame %u (toward %.3f,%.3f,%.3f; colour %.2f,%.2f,%.2f, luminance %.3f; %u votes of %u candidates)",
                 h.frames, next.dir[0], next.dir[1], next.dir[2], next.col[0], next.col[1], next.col[2], lum, next.count, h.voter.n);
        Logger::info(msg);
      }
    }
    if (h.sunApi) remixapi::remixapi_DrawLightInstance((remixapi_LightHandle) h.sunApi);   // every frame (milestone 20b)
    // Night from the sun (milestone 20d): the luminance of the sun the hook holds follows the
    // game's clock (0.97 at noon, orange 0.3 at dusk, 0.07 late, then no candidate above the
    // floor at all); the game's rig carries no sky light of its own (an outdoor rig is the sun
    // alone). Relative to full day it sets the runtime's sky brightness -- the sky probe scaled
    // wherever a ray escapes, ambient and backdrop together -- and the ceiling of the
    // auto-exposure, which would otherwise brighten the dark scene back up (its default range
    // reaches +5 EV). Without a candidate for a second the level decays to the floor: night.
    if (sims3cam::skyFromSun()) {
      if (haveVote) h.skyNoCandFrames = 0; else ++h.skyNoCandFrames;
      float target = -1.f;
      if (h.skyNoCandFrames > 60) target = 0.f;
      else if (h.sunSet) target = sims3cam::luminance(h.sun.col);
      if (target >= 0.f) {
        h.skyLevel = h.skyLevel < 0.f ? target : h.skyLevel + 0.05f * (target - h.skyLevel);
        if (h.skyLevel > h.skyDayRef) h.skyDayRef = h.skyLevel;
        const float dayRef = h.skyDayRef > sims3cam::skyDayLevel() ? h.skyDayRef : sims3cam::skyDayLevel();
        float b = h.skyLevel / dayRef;
        if (b > 1.f) b = 1.f;
        if (b < sims3cam::skyMinBrightness()) b = sims3cam::skyMinBrightness();
        const float ev = sims3cam::nightEvMax() + (sims3cam::dayEvMax() - sims3cam::nightEvMax()) * b;
        const bool due = h.skyBrightnessSent < 0.f || ((std::fabs(b - h.skyBrightnessSent) > 0.01f || std::fabs(ev - h.evMaxSent) > 0.05f) && h.frames - h.skySendFrame >= 10);
        if (due) {
          if (GlobalOptions::getExposeRemixApi()) {
            char val[32];
            snprintf(val, sizeof val, "%.3f", b); remixapi::remixapi_SetConfigVariable("rtx.skyBrightness", val);
            snprintf(val, sizeof val, "%.2f", ev); remixapi::remixapi_SetConfigVariable("rtx.autoExposure.evMaxValue", val);
            const bool step = h.skyBrightnessSent < 0.f || std::fabs(b - h.skyBrightnessSent) > 0.1f;
            h.skyBrightnessSent = b; h.evMaxSent = ev; h.skySendFrame = h.frames; ++h.skySends;
            if (step && h.skyBrightLogged < 100) {
              ++h.skyBrightLogged; char msg[220];
              snprintf(msg, sizeof msg, "Sims 3 camera hook: sky brightness %.3f and exposure ceiling %.2f EV sent at frame %u (sun level %.3f, day reference %.3f%s)",
                       b, ev, h.frames, h.skyLevel, dayRef, h.skyNoCandFrames > 60 ? ", no sun candidate: night" : "");
              Logger::info(msg);
            }
          } else if (!h.skyApiWarned) {
            h.skyApiWarned = true;
            Logger::warn("Sims 3 camera hook: night not driven: the Remix API is off (exposeRemixApi = True in .trex\\bridge.conf turns it on for the bridge server)");
          }
        }
      }
    }
    h.voter.clear();
  }

  return m_pSwapchain->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, 0);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) {
  ZoneScoped;
  LogFunctionCall();
  return m_pSwapchain->GetBackBuffer(iBackBuffer, Type, ppBackBuffer);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) {
  ZoneScoped;
  LogFunctionCall();
  return m_pSwapchain->GetRasterStatus(pRasterStatus);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetDialogBoxMode(BOOL bEnableDialogs) {
  LogFunctionCall();

  UID currentUID = 0;
  // Send command to server and wait for response
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_SetDialogBoxMode, getId());
    currentUID = c.get_uid();
    c.send_data(bEnableDialogs);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetDialogBoxMode()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::SetGammaRamp(UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP* pRamp) {
  ZoneScoped;
  LogFunctionCall();

  {
    BRIDGE_DEVICE_LOCKGUARD();
    m_gammaRamp = *pRamp;
  }
  ClientMessage c(Commands::IDirect3DDevice9Ex_SetGammaRamp, getId());
  c.send_many(iSwapChain, Flags);
  c.send_data(sizeof(D3DGAMMARAMP), (void*) &m_gammaRamp);
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) {
  ZoneScoped;
  LogFunctionCall();

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pRamp = m_gammaRamp;
  }
  if (GlobalOptions::getSendReadOnlyCalls()) {
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetGammaRamp, getId());
    c.send_data(iSwapChain);
  }
}

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
  const uint8_t* a = (const uint8_t*) 0x10000; const uint8_t* found = nullptr;
  MEMORY_BASIC_INFORMATION mbi;
  while (!found && VirtualQuery(a, &mbi, sizeof mbi) == sizeof mbi) {
    if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE && mbi.RegionSize <= ((size_t) 256 << 20))
      found = sims3LampScanRegion((const uint8_t*) mbi.BaseAddress, mbi.RegionSize);
    const uint8_t* next = (const uint8_t*) mbi.BaseAddress + mbi.RegionSize;
    if (next <= a) break;
    a = next;
  }
  if (found) g_sims3LampBlock = found;
  ++g_sims3LampScans;
  g_sims3LampScanBusy = false;
}
// The block read whole: its head checked, its records copied, and its sequence the same before and
// after (an odd one means the reporter is writing). 0 read, 1 being written, 2 not the block any more.
static int sims3LampRead(const uint8_t* block, float* out, uint32_t& lamps, uint32_t& sequence, bool& world) {
  __try {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(block, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT || (mbi.Protect & 0xFF) != PAGE_READWRITE) return 2;
    uint8_t head[64]; memcpy(head, block, sizeof head);
    if (!sims3cam::lampReportHead(head, (uint32_t) (uintptr_t) block, lamps, sequence, world)) return 2;
    if (sequence & 1u) return 1;
    memcpy(out, block + sims3cam::kLampHead, (size_t) (lamps + 1) * sims3cam::kLampFloats * sizeof(float));
    uint32_t after; memcpy(&after, block + 20, sizeof after);
    return after == sequence ? 0 : 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 2; }
}

// The Sims 3 camera hook (milestones 27-29): the lot's light maps. A map noted by the draws
// (sims3LightMapNote) is decoded on the client whenever its version changes -- the brightest
// channel per texel, the map being coloured -- and read at a world position through its rows
// (u = P . row0, v = P . row1); beyond it (u or v outside 0..1) nothing is read. Returns the
// value 0..255, or -1.
static bool sims3LightMapDecode(Sims3Hook& h, Sims3Hook::LightMapEntry& e, bool unseen) {
  auto* t = bridge_cast<Direct3DTexture9_LSS*>(e.tex);
  const uint32_t ver = t->sims3Level0Version();
  if (ver == e.version && e.valid) return true;
  const D3DSURFACE_DESC d = t->getLevelDesc(0);
  // the version before is kept to compare with (milestone 33), unless the map is new, resized or was unseen for a while
  const bool follows = e.valid && e.W == d.Width && e.H == d.Height && !unseen;
  if (follows) e.prev.swap(e.data); else e.prev.clear();
  e.hasPrev = follows; e.fresh = !follows;
  e.version = ver; e.W = d.Width; e.H = d.Height;
  e.valid = sims3cam::decodeMaskMax((uint32_t) d.Format, t->sims3Level0Data(), sims3cam::maskBytes((uint32_t) d.Format, d.Width, d.Height), d.Width, d.Height, e.data);
  if (e.valid) ++h.lightMapDecodes; else ++h.lightMapFails;
  if (!e.valid || e.prev.size() != e.data.size()) { e.hasPrev = false; e.fresh = true; }
  return e.valid;
}
static Sims3Hook::LightMapEntry* sims3LightMapOf(Sims3Hook& h, const void* tex) {
  for (int k = 0; k < 4; ++k) if (tex && (const void*) h.lightMaps[k].tex == tex) return &h.lightMaps[k];
  return nullptr;
}
static int sims3LightMapRead(const Sims3Hook::LightMapEntry& e, const std::vector<uint8_t>& img, const float* P) {
  if (!e.valid || !e.rowsValid || e.W == 0 || e.H == 0 || img.size() < (size_t) e.W * e.H) return -1;
  const float* r = e.rows;
  const float u = P[0]*r[0] + P[1]*r[1] + P[2]*r[2] + r[3], v = P[0]*r[4] + P[1]*r[5] + P[2]*r[6] + r[7];
  if (!(u >= 0.f && u < 1.f && v >= 0.f && v < 1.f)) return -1;   // beyond the map (milestone 28: no wrapping -- a street lamp is not judged by the lot's map)
  const uint32_t x = (uint32_t) (u * e.W) % e.W, y = (uint32_t) (v * e.H) % e.H;
  return img[y * e.W + x];
}
static int sims3LightMapAt(const Sims3Hook::LightMapEntry& e, const float* P) { return sims3LightMapRead(e, e.data, P); }
// What a map says at a lamp's base B (milestones 33, 34): the base, its change since the previous
// version (0 without one), the median of eight points on a ring around it and the median of their
// changes (a point dark in both versions -- a wall, the outside -- is left out; the median, so that
// another lamp on one side does not pass for the surroundings), and the darkest of the four points
// 1.5 units around it that the reading by night uses. False beyond the map.
struct Sims3LampReading { int base, dBase, around, dRing, darkest; };
static bool sims3LightMapOwn(const Sims3Hook::LightMapEntry& e, const float* B, float ring, Sims3LampReading& r) {
  r.base = sims3LightMapAt(e, B); r.dBase = r.around = r.dRing = 0; r.darkest = 255;
  if (r.base < 0) return false;
  const bool cmp = e.hasPrev && e.prev.size() == e.data.size();
  r.dBase = cmp ? r.base - sims3LightMapRead(e, e.prev, B) : 0;
  static const float d[8][2] = { { 1.f, 0.f }, { -1.f, 0.f }, { 0.f, 1.f }, { 0.f, -1.f }, { 0.7071f, 0.7071f }, { -0.7071f, 0.7071f }, { 0.7071f, -0.7071f }, { -0.7071f, -0.7071f } };
  int now[8], delta[8], n = 0;
  for (int k = 0; k < 8; ++k) {
    const float Q[3] = { B[0] + d[k][0] * ring, B[1], B[2] + d[k][1] * ring };
    const int v = sims3LightMapAt(e, Q);
    if (v < 0) continue;
    const int was = cmp ? sims3LightMapRead(e, e.prev, Q) : v;
    if (v < 3 && was < 3) continue;
    now[n] = v; delta[n] = v - was; ++n;
  }
  if (n > 0) {
    std::sort(now, now + n); std::sort(delta, delta + n);
    r.around = n & 1 ? now[n / 2] : (now[n / 2 - 1] + now[n / 2]) / 2;
    r.dRing = n & 1 ? delta[n / 2] : (delta[n / 2 - 1] + delta[n / 2]) / 2;
  }
  for (int k = 0; k < 4; ++k) {
    const float Q[3] = { B[0] + d[k][0] * 1.5f, B[1], B[2] + d[k][1] * 1.5f };
    const int v = sims3LightMapAt(e, Q);
    if (v >= 0 && v < r.darkest) r.darkest = v;
  }
  return true;
}

// The Sims 3 camera hook (milestone 20b): the sun and the lamps as Remix API lights. An API
// light is immutable: a change destroys the old one and creates a new one under the same hash
// (the runtime's identity for it, kept for temporal stability); a handle is drawn every frame
// its light should exist. Radiance and size are explicit here, where the fixed-function path
// left them to the runtime's conversion (a 2-degree disc at intensity 1 per unit of colour for
// the sun; a 4-unit sphere for every lamp).
static const uint64_t kSims3SunHash = 0x53494D5333535541ull, kSims3LampHash = 0x53494D53334C4D00ull;
static void* sims3ApiLightReplace(void* old, const remixapi_LightInfo& info) {
  if (old) remixapi::remixapi_DestroyLight((remixapi_LightHandle) old);
  remixapi_LightHandle h = nullptr;
  if (remixapi::remixapi_CreateLight(&info, &h) != REMIXAPI_ERROR_CODE_SUCCESS) return nullptr;
  return (void*) h;
}
static void* sims3ApiSun(void* old, const sims3cam::SunVote& s) {
  remixapi_LightInfoDistantEXT d = {};
  d.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  d.direction.x = -s.dir[0]; d.direction.y = -s.dir[1]; d.direction.z = -s.dir[2];   // the direction the light travels
  d.angularDiameterDegrees = sims3cam::sunAngle();
  d.volumetricRadianceScale = 1.f;
  remixapi_LightInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO; info.pNext = &d; info.hash = kSims3SunHash;
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

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) {
  ZoneScoped;
  LogFunctionCall();

  if (ppTexture == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  // When Levels is 0, D3D9 will calculate the mip requirements
  if (Levels == 0) {
    Levels = CalculateNumMipLevels(Width, Height);
  }

  UID currentUID = 0;
  {
    const TEXTURE_DESC desc { Width, Height, 1, Levels, Usage, Format, Pool };
    auto* const pLssTexture = trackWrapper(new Direct3DTexture9_LSS(this, desc));
    (*ppTexture) = pLssTexture;
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateTexture, getId());
      currentUID = c.get_uid();
      c.send_many(Width, Height, Levels, Usage, Format, Pool, (uint32_t) pLssTexture->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateTexture()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) {
  ZoneScoped;
  LogFunctionCall();

  if (ppVolumeTexture == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  // When Levels is 0, D3D9 will calculate the mip requirements
  if (Levels == 0) {
    Levels = CalculateNumMipLevels(Width, Height, Depth);
  }

  UID currentUID = 0;
  {
    const TEXTURE_DESC desc { Width, Height, Depth, Levels, Usage, Format, Pool };
    auto* const pLssVolumeTexture = trackWrapper(new Direct3DVolumeTexture9_LSS(this, desc));
    (*ppVolumeTexture) = pLssVolumeTexture;
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateVolumeTexture, getId());
      currentUID = c.get_uid();
      c.send_many(Width, Height, Depth, Levels, Usage, Format, Pool, (uint32_t) pLssVolumeTexture->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateVolumeTexture()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) {
  ZoneScoped;
  LogFunctionCall();

  if (ppCubeTexture == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  // When Levels is 0, D3D9 will calculate the mip requirements
  if (Levels == 0) {
    Levels = CalculateNumMipLevels(EdgeLength);
  }

  UID currentUID = 0;
  {
    const TEXTURE_DESC desc { EdgeLength, EdgeLength, 6, Levels, Usage, Format, Pool };
    auto* const pLssCubeTexture = trackWrapper(new Direct3DCubeTexture9_LSS(this, desc));
    (*ppCubeTexture) = pLssCubeTexture;
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateCubeTexture, getId());
      currentUID = c.get_uid();
      c.send_many(EdgeLength, Levels, Usage, Format, Pool, (uint32_t) pLssCubeTexture->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateCubeTexture()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) {
  ZoneScoped;
  LogFunctionCall();
  if (Length == 0) {
    return D3DERR_INVALIDCALL;
  }

  if (ppVertexBuffer == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  const D3DVERTEXBUFFER_DESC desc { D3DFMT_VERTEXDATA, D3DRTYPE_VERTEXBUFFER, Usage, Pool, Length, FVF };
  UID currentUID = 0;
  {
    auto* const pLssVertexBuffer = trackWrapper(new Direct3DVertexBuffer9_LSS(this, desc));
    (*ppVertexBuffer) = pLssVertexBuffer;
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateVertexBuffer, getId());
      currentUID = c.get_uid();
      c.send_many(Length, Usage, FVF, Pool, (uint32_t) pLssVertexBuffer->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateVertexBuffer()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) {
  ZoneScoped;
  LogFunctionCall();

  if (Length == 0) {
    return D3DERR_INVALIDCALL;
  }

  if (ppIndexBuffer == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  const D3DINDEXBUFFER_DESC desc { Format, D3DRTYPE_INDEXBUFFER, Usage, Pool, Length };
  UID currentUID = 0;
  {
    auto* const pLssIndexBuffer = trackWrapper(new Direct3DIndexBuffer9_LSS(this, desc));
    (*ppIndexBuffer) = (IDirect3DIndexBuffer9*) pLssIndexBuffer;
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateIndexBuffer, getId());
      currentUID = c.get_uid();
      c.send_many(Length, Usage, Format, Pool, (uint32_t) pLssIndexBuffer->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateIndexBuffer()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) {
  ZoneScoped;
  LogFunctionCall();

  if (ppSurface == nullptr || Width == 0 || Height == 0) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    D3DSURFACE_DESC desc;
    desc.Width = Width;
    desc.Height = Height;
    desc.Format = Format;
    desc.MultiSampleType = MultiSample;
    desc.MultiSampleQuality = MultisampleQuality;
    desc.Usage = D3DUSAGE_RENDERTARGET;
    desc.Pool = D3DPOOL_DEFAULT;
    desc.Type = D3DRTYPE_SURFACE;

    // Insert our own IDirect3DSurface9 interface implementation
    Direct3DSurface9_LSS* pLssSurface = trackWrapper(new Direct3DSurface9_LSS(this, desc));
    (*ppSurface) = (IDirect3DSurface9*) pLssSurface;

    {
      // Add a handle for the surface
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateRenderTarget, getId());
      currentUID = c.get_uid();
      c.send_many(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, pLssSurface->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateRenderTarget()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) {
  ZoneScoped;
  LogFunctionCall();

  if (ppSurface == nullptr || Width == 0 || Height == 0) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    D3DSURFACE_DESC desc;
    desc.Width = Width;
    desc.Height = Height;
    desc.Format = Format;
    desc.MultiSampleType = MultiSample;
    desc.MultiSampleQuality = MultisampleQuality;
    desc.Usage = D3DUSAGE_DEPTHSTENCIL;
    desc.Pool = D3DPOOL_DEFAULT;
    desc.Type = D3DRTYPE_SURFACE;

    Direct3DSurface9_LSS* pLssSurface = trackWrapper(new Direct3DSurface9_LSS(this, desc));
    (*ppSurface) = (IDirect3DSurface9*) pLssSurface;

    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateDepthStencilSurface, getId());
      currentUID = c.get_uid();
      c.send_many(Width, Height, Format, MultiSample, MultisampleQuality, Discard, pLssSurface->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateDepthStencilSurface()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::UpdateSurface(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint) {
  ZoneScoped;
  LogFunctionCall();

  if (pSourceSurface == nullptr || pDestinationSurface == nullptr || pSourceSurface == pDestinationSurface) {
    return D3DERR_INVALIDCALL;
  }

  const auto pLssSrcSurface = bridge_cast<Direct3DSurface9_LSS*>(pSourceSurface);
  const auto pLssDestSurface = bridge_cast<Direct3DSurface9_LSS*>(pDestinationSurface);
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_UpdateSurface, getId());
    currentUID = c.get_uid();
    c.send_data(pLssSrcSurface->getId());
    c.send_data(sizeof(RECT), (void*) pSourceRect);
    c.send_data(pLssDestSurface->getId());
    c.send_data(sizeof(POINT), (void*) pDestPoint);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("UpdateSurface()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync> template<typename T>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::UpdateTextureImpl(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) {
  ZoneScoped;

  if (pSourceTexture == nullptr || pDestinationTexture == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  auto pLssSourceTexture = bridge_cast<T*>(pSourceTexture);
  auto pLssDestinationTexture = bridge_cast<T*>(pDestinationTexture);
  assert(pLssSourceTexture && "UpdateTexture: unable to cast source texture!");
  assert(pLssDestinationTexture && "UpdateTexture: unable to cast destination texture!");
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_UpdateTexture, getId());
    currentUID = c.get_uid();
    c.send_data((uint32_t) pLssSourceTexture->getId());
    c.send_data((uint32_t) pLssDestinationTexture->getId());
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("UpdateTextureImpl()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) {
  ZoneScoped;
  LogFunctionCall();

  assert(pSourceTexture->GetType() == pDestinationTexture->GetType() && "UpdateTexture: texture type mismatch!");

  switch (pSourceTexture->GetType()) {
  case D3DRTYPE_TEXTURE:
    return UpdateTextureImpl<Direct3DTexture9_LSS>(pSourceTexture, pDestinationTexture);
  case D3DRTYPE_CUBETEXTURE:
    return UpdateTextureImpl<Direct3DCubeTexture9_LSS>(pSourceTexture, pDestinationTexture);
  case D3DRTYPE_VOLUMETEXTURE:
    return UpdateTextureImpl<Direct3DVolumeTexture9_LSS>(pSourceTexture, pDestinationTexture);
  default:
    assert(0 && "UpdateTexture: unexpected texture type!");
  }

  return D3DERR_INVALIDCALL;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) {
  ZoneScoped;
  LogFunctionCall();

  const auto pLssSourceSurface = bridge_cast<Direct3DSurface9_LSS*>(pRenderTarget);
  const auto pLssDestinationSurface = bridge_cast<Direct3DSurface9_LSS*>(pDestSurface);

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetRenderTargetData, getId());
    currentUID = c.get_uid();
    c.send_data(pLssSourceSurface->getId());
    c.send_data(pLssDestinationSurface->getId());
  }

  // Wait for response from server
  return copyServerSurfaceRawData(pLssDestinationSurface, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) {
  ZoneScoped;
  LogFunctionCall();

  if (pDestSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  const auto pLssDestinationSurface = bridge_cast<Direct3DSurface9_LSS*>(pDestSurface);

  UID currentUID = 0;
  {
    // Direct API call to server
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetFrontBufferData, getId());
    currentUID = c.get_uid();
    c.send_many(iSwapChain, pLssDestinationSurface->getId());
  }

  return copyServerSurfaceRawData(pLssDestinationSurface, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::StretchRect(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter) {
  ZoneScoped;
  LogFunctionCall();

  if (pSourceSurface == nullptr || pDestSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  if (Filter != D3DTEXF_LINEAR && Filter != D3DTEXF_POINT
      && Filter != D3DTEXF_NONE) {
    return D3DERR_INVALIDCALL;
  }

  const auto pLssSrcSurface = bridge_cast<Direct3DSurface9_LSS*>(pSourceSurface);
  const auto pLssDstSurface = bridge_cast<Direct3DSurface9_LSS*>(pDestSurface);
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_StretchRect, getId());
    currentUID = c.get_uid();
    c.send_data(pLssSrcSurface->getId());
    c.send_data(sizeof(RECT), (void*) pSourceRect);
    c.send_data(pLssDstSurface->getId());
    c.send_data(sizeof(RECT), (void*) pDestRect);
    c.send_data(Filter);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("StretchRect()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::ColorFill(IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color) {
  ZoneScoped;
  LogFunctionCall();

  if (pSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  const auto pLssSurface = bridge_cast<Direct3DSurface9_LSS*>(pSurface);
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_ColorFill, getId());
    currentUID = c.get_uid();
    c.send_data(pLssSurface->getId());
    c.send_data(sizeof(RECT), (void*) pRect);
    c.send_data(sizeof(D3DCOLOR), (void*) &color);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("ColorFill()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) {
  ZoneScoped;
  LogFunctionCall();

  if (ppSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    D3DSURFACE_DESC desc;
    desc.Width = Width;
    desc.Height = Height;
    desc.Format = Format;
    desc.MultiSampleType = D3DMULTISAMPLE_NONE;
    desc.MultiSampleQuality = 0;
    desc.Usage = D3DUSAGE_RENDERTARGET;
    desc.Pool = Pool;
    desc.Type = D3DRTYPE_SURFACE;

    // Insert our own IDirect3DSurface9 interface implementation
    Direct3DSurface9_LSS* pLssSurface = trackWrapper(new Direct3DSurface9_LSS(this, desc));
    (*ppSurface) = (IDirect3DSurface9*) pLssSurface;

    {
      // Add a handle for the surface
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateOffscreenPlainSurface, getId());
      currentUID = c.get_uid();
      c.send_many(Width, Height, Format, Pool, pLssSurface->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateOffscreenPlainSurface()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) {
  ZoneScoped;
  LogFunctionCall();
  auto* const pLssRenderTarget = bridge_cast<Direct3DSurface9_LSS*>(pRenderTarget);
  // The Sims 3 camera hook: is render target 0 the backbuffer-sized target (the 3D pass)?
  if (sims3cam::enabled() && RenderTargetIndex == 0) {
    bool primary = true;
    g_sims3.rt0W = g_sims3.rt0H = 0; g_sims3.rt0Fmt = 0;
    if (pLssRenderTarget != nullptr) {
      D3DSURFACE_DESC d = {};
      if (SUCCEEDED(pLssRenderTarget->GetDesc(&d))) {
        g_sims3.rt0W = (uint16_t) d.Width; g_sims3.rt0H = (uint16_t) d.Height; g_sims3.rt0Fmt = (uint32_t) d.Format;
        if (m_pSwapchain != nullptr) {
          const D3DPRESENT_PARAMETERS& pp = m_pSwapchain->getPresentationParameters();
          primary = (d.Width == pp.BackBufferWidth && d.Height == pp.BackBufferHeight);
        }
      }
    }
    g_sims3.rtIsPrimary = primary;
    g_sims3.rt0 = pRenderTarget;   // the lot ground replay waits for a 2D draw on the scene's target (milestone 17e)
    { char t[80]; snprintf(t, sizeof t, "RT0 %04x %ux%u fmt %u %s d%u", (unsigned) ((((uintptr_t) pRenderTarget) >> 4) & 0xFFFFu), (unsigned) g_sims3.rt0W, (unsigned) g_sims3.rt0H, (unsigned) g_sims3.rt0Fmt, primary ? "primary" : "offscreen", g_sims3.frameDraws); sims3RingPush(g_sims3, t); }
  }
  UID currentUID = 0;
  {
    UID id = 0;
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (pLssRenderTarget) {
        m_state.renderTargets[RenderTargetIndex] = MakeD3DAutoPtr(pLssRenderTarget);
        id = pLssRenderTarget->getId();
      } else {
        m_state.renderTargets[RenderTargetIndex].reset(nullptr);
      }
    }

    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetRenderTarget, getId());
      currentUID = c.get_uid();
      c.send_many(RenderTargetIndex, id);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetRenderTarget()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) {
  ZoneScoped;
  LogFunctionCall();

  if (ppRenderTarget == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  Direct3DSurface9_LSS* pLssRenderTarget = nullptr;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    pLssRenderTarget = bridge_cast<Direct3DSurface9_LSS*>(*m_state.renderTargets[RenderTargetIndex]);
  }
  *ppRenderTarget = pLssRenderTarget;
  UID currentUID = 0;
  if (pLssRenderTarget) {
    pLssRenderTarget->AddRef();
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_GetRenderTarget, getId());
      currentUID = c.get_uid();
      c.send_many(RenderTargetIndex, pLssRenderTarget->getId());
    }
    WAIT_FOR_OPTIONAL_SERVER_RESPONSE("GetRenderTarget()", D3DERR_INVALIDCALL, currentUID);
  }
  
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) {
  ZoneScoped;
  LogFunctionCall();  
  UID currentUID = 0;
  {
    UID id = 0;
    {
      BRIDGE_DEVICE_LOCKGUARD();
      auto* const pLssDepthStencil = bridge_cast<Direct3DSurface9_LSS*>(pNewZStencil);
      if (pLssDepthStencil) {
        m_state.depthStencil = MakeD3DAutoPtr(pLssDepthStencil);
        id = pLssDepthStencil->getId();
      } else {
        m_state.depthStencil.reset(nullptr);
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetDepthStencilSurface, getId());
      currentUID = c.get_uid();
      c.send_data(id);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetDepthStencilSurface()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) {
  ZoneScoped;
  LogFunctionCall();

  if (ppZStencilSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    Direct3DSurface9_LSS* pLssDepthStencil = bridge_cast<Direct3DSurface9_LSS*>(*m_state.depthStencil);
    *ppZStencilSurface = pLssDepthStencil;
    if (pLssDepthStencil) {
      pLssDepthStencil->AddRef();
      {
        ClientMessage c(Commands::IDirect3DDevice9Ex_GetDepthStencilSurface, getId());
        currentUID = c.get_uid();
        c.send_data(pLssDepthStencil->getId());
      }
    } else {
      // No depth-stencil surface is currently bound. D3D9 semantics (and the DXVK
      // runtime one layer down) return D3DERR_NOTFOUND here with a NULL out-pointer.
      // Falling through to WAIT_FOR_OPTIONAL_SERVER_RESPONSE would return D3D_OK, which
      // tells callers that guard on FAILED(hr) that the NULL pointer is valid; The Sims 3
      // then dereferences it (IDirect3DSurface9::GetDesc on a null this) and crashes.
      return D3DERR_NOTFOUND;
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("GetDepthStencilSurface()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::BeginScene() {
  ZoneScoped;
  LogFunctionCall();

  {
    BRIDGE_DEVICE_LOCKGUARD();
    if (gSceneState == WaitBeginScene) {
      gSceneState = SceneInProgress;
    }
  }
  
  if (remixapi::g_bInterfaceInitialized && remixapi::g_beginSceneCallback) {
    remixapi::g_beginSceneCallback();
  }

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_BeginScene, getId());
    currentUID = c.get_uid();
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("BeginScene()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::EndScene() {
  ZoneScoped;
  LogFunctionCall();

  {
    BRIDGE_DEVICE_LOCKGUARD();
    if (gSceneState == SceneInProgress) {
      gSceneState = SceneEnded;
    }
  }

  if (remixapi::g_bInterfaceInitialized && remixapi::g_endSceneCallback) {
    remixapi::g_endSceneCallback();
  }

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_EndScene, getId());
    currentUID = c.get_uid();
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("EndScene()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::Clear(DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) {
  ZoneScoped;
  LogFunctionCall();

  if (Count == 0 && pRects != NULL) {
    return D3DERR_INVALIDCALL;
  }
  if (Count != 0 && pRects == NULL) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_Clear, getId());
    currentUID = c.get_uid();
    c.send_many(Count, Flags);
    c.send_data(sizeof(float), &Z);
    c.send_data(Stencil);
    c.send_data(sizeof(D3DRECT) * Count, (void*) pRects);
    c.send_data(sizeof(D3DCOLOR), (void*) &Color);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("Clear()", D3DERR_INVALIDCALL, currentUID);
}

namespace {
  static inline size_t mapXformStateTypeToIdx(const D3DTRANSFORMSTATETYPE Type) {
    if (Type == D3DTS_VIEW) {
      return 0;
    }
    if (Type == D3DTS_PROJECTION) {
      return 1;
    }
    if (Type >= D3DTS_TEXTURE0 && Type <= D3DTS_TEXTURE7) {
      return 2 + (Type - D3DTS_TEXTURE0);
    }
    return 10 + (Type - D3DTS_WORLD);
  }
}

bool isValidD3drtansformstatetype(D3DTRANSFORMSTATETYPE Type) {
  if (Type == D3DTS_VIEW) {
    return true;
  }
  if (Type == D3DTS_PROJECTION) {
    return true;
  }
  if (Type >= D3DTS_TEXTURE0 && Type <= D3DTS_TEXTURE7) {
    return true;
  }
  if (Type >= D3DTS_WORLDMATRIX(0) && Type < D3DTS_WORLDMATRIX(256)) {
    return true;
  }
  return false;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) {
  ZoneScoped;
  LogFunctionCall();

  if (pMatrix == nullptr || !isValidD3drtansformstatetype(State)) {
    return D3DERR_INVALIDCALL;
  }

  const auto idx = mapXformStateTypeToIdx(State);
  // The Sims 3 camera hook: the game's own View / Projection, if it ever sets them (ours are marked)
  if (sims3cam::enabled() && !g_sims3.ourState) {
    if (State == D3DTS_VIEW) { g_sims3.gameXform[0] = *pMatrix; g_sims3.gameXformSet[0] = true; g_sims3.held.kind = sims3cam::Kind::None; }
    else if (State == D3DTS_PROJECTION) { g_sims3.gameXform[1] = *pMatrix; g_sims3.gameXformSet[1] = true; g_sims3.held.kind = sims3cam::Kind::None; }
  }
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_stateRecording->m_dirtyFlags.transforms[idx] &&
            memcmp(&m_stateRecording->m_captureState.transforms[idx], pMatrix, sizeof(D3DMATRIX)) == 0) {
          return S_OK;
        }
        m_stateRecording->m_captureState.transforms[idx] = *pMatrix;
        m_stateRecording->m_dirtyFlags.transforms[idx] = true;
      } else {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            memcmp(&m_state.transforms[idx], pMatrix, sizeof(D3DMATRIX)) == 0) {
          return S_OK;
        }
        m_state.transforms[idx] = *pMatrix;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetTransform, getId());
      currentUID = c.get_uid();
      c.send_data(State);
      c.send_data(sizeof(D3DMATRIX), (void*) pMatrix);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetTransform()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) {
  ZoneScoped;
  LogFunctionCall();

  if (pMatrix == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  const auto idx = mapXformStateTypeToIdx(State);
  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pMatrix = m_state.transforms[idx];
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::MultiplyTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) {
  ZoneScoped;
  LogFunctionCall();

  if (pMatrix == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  BRIDGE_DEVICE_LOCKGUARD();
  const auto idx = mapXformStateTypeToIdx(State);
  D3DMATRIX result = { 0 };
  D3DMATRIX current = (m_stateRecording && m_stateRecording->m_dirtyFlags.transforms[idx]) ? m_stateRecording->m_captureState.transforms[idx] : m_state.transforms[idx];

  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      float value = 0.0f;
      for (int k = 0; k < 4; k++) {
        value += current.m[i][k] * pMatrix->m[k][j];
      }
      result.m[i][j] = value;
    }
  }

  if (m_stateRecording) {
    m_stateRecording->m_captureState.transforms[idx] = result;
    m_stateRecording->m_dirtyFlags.transforms[idx] = true;
  } else {
    m_state.transforms[idx] = result;
  }
  
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetViewport(CONST D3DVIEWPORT9* pViewport) {
  ZoneScoped;
  LogFunctionCall();

  if (pViewport == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        m_stateRecording->m_captureState.viewport = *pViewport;
        m_stateRecording->m_dirtyFlags.viewport = true;
      } else {
        m_state.viewport = *pViewport;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetViewport, getId());
      currentUID = c.get_uid();
      c.send_data(sizeof(D3DVIEWPORT9), (void*) pViewport);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetViewport()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetViewport(D3DVIEWPORT9* pViewport) {
  ZoneScoped;
  LogFunctionCall();

  if (pViewport == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pViewport = m_state.viewport;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetMaterial(CONST D3DMATERIAL9* pMaterial) {
  ZoneScoped;
  LogFunctionCall();

  if (pMaterial == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        m_stateRecording->m_captureState.material = *pMaterial;
        m_stateRecording->m_dirtyFlags.material = true;
      } else {
        m_state.material = *pMaterial;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetMaterial, getId());
      currentUID = c.get_uid();
      c.send_data(sizeof(D3DMATERIAL9), (void*) pMaterial);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetMaterial()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetMaterial(D3DMATERIAL9* pMaterial) {
  ZoneScoped;
  LogFunctionCall();

  if (pMaterial == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pMaterial = m_state.material;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetLight(DWORD Index, CONST D3DLIGHT9* pLight) {
  ZoneScoped;
  LogFunctionCall();

  if (pLight == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_stateRecording->m_dirtyFlags.lights[Index] &&
            memcmp(&m_stateRecording->m_captureState.lights[Index], pLight, sizeof(D3DLIGHT9)) == 0) {
          return S_OK;
        }
        m_stateRecording->m_captureState.lights[Index] = *pLight;
        m_stateRecording->m_dirtyFlags.lights[Index] = true;
      } else {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            memcmp(&m_state.lights[Index], pLight, sizeof(D3DLIGHT9)) == 0) {
          return S_OK;
        }
        m_state.lights[Index] = *pLight;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetLight, getId());
      currentUID = c.get_uid();
      c.send_data(Index);
      c.send_data(sizeof(D3DLIGHT9), (void*) pLight);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetLight()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetLight(DWORD Index, D3DLIGHT9* pLight) {
  ZoneScoped;
  LogFunctionCall();

  if (pLight == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pLight = m_state.lights[Index];
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::LightEnable(DWORD LightIndex, BOOL bEnable) {
  ZoneScoped;
  LogFunctionCall();

  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_stateRecording->m_dirtyFlags.bLightEnables[LightIndex] &&
            (m_stateRecording->m_captureState.bLightEnables[LightIndex] == (bool)bEnable)) {
          return S_OK;
        }
        m_stateRecording->m_captureState.bLightEnables[LightIndex] = bEnable;
        m_stateRecording->m_dirtyFlags.bLightEnables[LightIndex] = true;
      } else {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_state.bLightEnables[LightIndex] == (bool)bEnable) {
          return S_OK;
        }
        m_state.bLightEnables[LightIndex] = bEnable;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_LightEnable, getId());
      currentUID = c.get_uid();
      c.send_many(LightIndex, (uint32_t) bEnable);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("LightEnable()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetLightEnable(DWORD Index, BOOL* pEnable) {
  ZoneScoped;
  LogFunctionCall();
  if (pEnable == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    // This is the true value for light-enables found through experimentation
    constexpr BOOL LightEnableTrue = 128;
    *pEnable = m_state.bLightEnables[Index] ? LightEnableTrue : 0;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetClipPlane(DWORD Index, CONST float* pPlane) {
  ZoneScoped;
  LogFunctionCall();

  if (pPlane == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        m_stateRecording->m_dirtyFlags.clipPlanes[Index] = true;
        for (int i = 0; i < 4; i++) {
          m_stateRecording->m_captureState.clipPlanes[Index][i] = pPlane[i];
        }
      } else {
        for (int i = 0; i < 4; i++) {
          m_state.clipPlanes[Index][i] = pPlane[i];
        }
      }
    }
    {
      // pPlane is a four-element array with the clipping plane coefficients
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetClipPlane, getId());
      currentUID = c.get_uid();
      c.send_data(Index);
      c.send_data(sizeof(float) * 4, (void*) pPlane);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetClipPlane()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetClipPlane(DWORD Index, float* pPlane) {
  ZoneScoped;
  LogFunctionCall();

  if (pPlane == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    for (int i = 0; i < 4; i++) {
      pPlane[i] = m_state.clipPlanes[Index][i];
    }
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
  ZoneScoped;
  LogFunctionCall();
  // The Sims 3 camera hook: the game's own render states (not the hook's) are trusted from here on
  if (sims3cam::enabled() && !g_sims3.ourState) {
    if ((DWORD) State < 256) g_sims3.rsSet[State] = true;
    if (State == D3DRS_TEXTUREFACTOR) { g_sims3.gameFactor = Value; g_sims3.factorOurs = false; g_sims3.sentFactor = Value; }
  }

  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_stateRecording->m_dirtyFlags.renderStates[State] && m_stateRecording->m_captureState.renderStates[State] == Value) {
          return S_OK;
        }
        m_stateRecording->m_captureState.renderStates[State] = Value;
        m_stateRecording->m_dirtyFlags.renderStates[State] = true;
      } else {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_state.renderStates[State] == Value) {
          return S_OK;
        }
        m_state.renderStates[State] = Value;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetRenderState, getId());
      currentUID = c.get_uid();
      c.send_many(State, Value);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetRenderState()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) {
  ZoneScoped;
  LogFunctionCall();

  if (pValue == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  
  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pValue = m_state.renderStates[State];
  }
  return S_OK;
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::StateBlockSetPixelCaptureFlags(BaseDirect3DDevice9Ex_LSS::StateCaptureDirtyFlags& flags) {
  flags.renderStates[D3DRS_ZENABLE] = true;
  flags.renderStates[D3DRS_FILLMODE] = true;
  flags.renderStates[D3DRS_SHADEMODE] = true;
  flags.renderStates[D3DRS_ZWRITEENABLE] = true;
  flags.renderStates[D3DRS_ALPHATESTENABLE] = true;
  flags.renderStates[D3DRS_LASTPIXEL] = true;
  flags.renderStates[D3DRS_SRCBLEND] = true;
  flags.renderStates[D3DRS_DESTBLEND] = true;
  flags.renderStates[D3DRS_ZFUNC] = true;
  flags.renderStates[D3DRS_ALPHAREF] = true;
  flags.renderStates[D3DRS_ALPHAFUNC] = true;
  flags.renderStates[D3DRS_DITHERENABLE] = true;
  flags.renderStates[D3DRS_FOGSTART] = true;
  flags.renderStates[D3DRS_FOGEND] = true;
  flags.renderStates[D3DRS_FOGDENSITY] = true;
  flags.renderStates[D3DRS_ALPHABLENDENABLE] = true;
  flags.renderStates[D3DRS_DEPTHBIAS] = true;
  flags.renderStates[D3DRS_STENCILENABLE] = true;
  flags.renderStates[D3DRS_STENCILFAIL] = true;
  flags.renderStates[D3DRS_STENCILZFAIL] = true;
  flags.renderStates[D3DRS_STENCILPASS] = true;
  flags.renderStates[D3DRS_STENCILFUNC] = true;
  flags.renderStates[D3DRS_STENCILREF] = true;
  flags.renderStates[D3DRS_STENCILMASK] = true;
  flags.renderStates[D3DRS_STENCILWRITEMASK] = true;
  flags.renderStates[D3DRS_TEXTUREFACTOR] = true;
  flags.renderStates[D3DRS_WRAP0] = true;
  flags.renderStates[D3DRS_WRAP1] = true;
  flags.renderStates[D3DRS_WRAP2] = true;
  flags.renderStates[D3DRS_WRAP3] = true;
  flags.renderStates[D3DRS_WRAP4] = true;
  flags.renderStates[D3DRS_WRAP5] = true;
  flags.renderStates[D3DRS_WRAP6] = true;
  flags.renderStates[D3DRS_WRAP7] = true;
  flags.renderStates[D3DRS_WRAP8] = true;
  flags.renderStates[D3DRS_WRAP9] = true;
  flags.renderStates[D3DRS_WRAP10] = true;
  flags.renderStates[D3DRS_WRAP11] = true;
  flags.renderStates[D3DRS_WRAP12] = true;
  flags.renderStates[D3DRS_WRAP13] = true;
  flags.renderStates[D3DRS_WRAP14] = true;
  flags.renderStates[D3DRS_WRAP15] = true;
  flags.renderStates[D3DRS_COLORWRITEENABLE] = true;
  flags.renderStates[D3DRS_BLENDOP] = true;
  flags.renderStates[D3DRS_SCISSORTESTENABLE] = true;
  flags.renderStates[D3DRS_SLOPESCALEDEPTHBIAS] = true;
  flags.renderStates[D3DRS_ANTIALIASEDLINEENABLE] = true;
  flags.renderStates[D3DRS_TWOSIDEDSTENCILMODE] = true;
  flags.renderStates[D3DRS_CCW_STENCILFAIL] = true;
  flags.renderStates[D3DRS_CCW_STENCILZFAIL] = true;
  flags.renderStates[D3DRS_CCW_STENCILPASS] = true;
  flags.renderStates[D3DRS_CCW_STENCILFUNC] = true;
  flags.renderStates[D3DRS_COLORWRITEENABLE1] = true;
  flags.renderStates[D3DRS_COLORWRITEENABLE2] = true;
  flags.renderStates[D3DRS_COLORWRITEENABLE3] = true;
  flags.renderStates[D3DRS_BLENDFACTOR] = true;
  flags.renderStates[D3DRS_SRGBWRITEENABLE] = true;
  flags.renderStates[D3DRS_SEPARATEALPHABLENDENABLE] = true;
  flags.renderStates[D3DRS_SRCBLENDALPHA] = true;
  flags.renderStates[D3DRS_DESTBLENDALPHA] = true;
  flags.renderStates[D3DRS_BLENDOPALPHA] = true;

  for (uint32_t i = 0; i < caps::MaxTexturesPS + 1; i++) {
    flags.samplerStates[i][D3DSAMP_ADDRESSU] = true;
    flags.samplerStates[i][D3DSAMP_ADDRESSV] = true;
    flags.samplerStates[i][D3DSAMP_ADDRESSW] = true;
    flags.samplerStates[i][D3DSAMP_BORDERCOLOR] = true;
    flags.samplerStates[i][D3DSAMP_MAGFILTER] = true;
    flags.samplerStates[i][D3DSAMP_MINFILTER] = true;
    flags.samplerStates[i][D3DSAMP_MIPFILTER] = true;
    flags.samplerStates[i][D3DSAMP_MIPMAPLODBIAS] = true;
    flags.samplerStates[i][D3DSAMP_MAXMIPLEVEL] = true;
    flags.samplerStates[i][D3DSAMP_MAXANISOTROPY] = true;
    flags.samplerStates[i][D3DSAMP_SRGBTEXTURE] = true;
    flags.samplerStates[i][D3DSAMP_ELEMENTINDEX] = true;
  }
  for (auto& fConst : flags.pixelConstants.fConsts) {
    fConst = true;
  }
  for (auto& iConst : flags.pixelConstants.iConsts) {
    iConst = true;
  }
  for (auto& bConst : flags.pixelConstants.bConsts) {
    bConst = true;
  }
  for (auto& stage : flags.textureStageStates) {
    std::fill(std::begin(stage), std::end(stage), true);
  }
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::StateBlockSetVertexCaptureFlags(BaseDirect3DDevice9Ex_LSS::StateCaptureDirtyFlags& flags) {
  flags.renderStates[D3DRS_CULLMODE] = true;
  flags.renderStates[D3DRS_FOGENABLE] = true;
  flags.renderStates[D3DRS_FOGCOLOR] = true;
  flags.renderStates[D3DRS_FOGTABLEMODE] = true;
  flags.renderStates[D3DRS_FOGSTART] = true;
  flags.renderStates[D3DRS_FOGEND] = true;
  flags.renderStates[D3DRS_FOGDENSITY] = true;
  flags.renderStates[D3DRS_RANGEFOGENABLE] = true;
  flags.renderStates[D3DRS_AMBIENT] = true;
  flags.renderStates[D3DRS_COLORVERTEX] = true;
  flags.renderStates[D3DRS_FOGVERTEXMODE] = true;
  flags.renderStates[D3DRS_CLIPPING] = true;
  flags.renderStates[D3DRS_LIGHTING] = true;
  flags.renderStates[D3DRS_LOCALVIEWER] = true;
  flags.renderStates[D3DRS_EMISSIVEMATERIALSOURCE] = true;
  flags.renderStates[D3DRS_AMBIENTMATERIALSOURCE] = true;
  flags.renderStates[D3DRS_DIFFUSEMATERIALSOURCE] = true;
  flags.renderStates[D3DRS_SPECULARMATERIALSOURCE] = true;
  flags.renderStates[D3DRS_VERTEXBLEND] = true;
  flags.renderStates[D3DRS_CLIPPLANEENABLE] = true;
  flags.renderStates[D3DRS_POINTSIZE] = true;
  flags.renderStates[D3DRS_POINTSIZE_MIN] = true;
  flags.renderStates[D3DRS_POINTSPRITEENABLE] = true;
  flags.renderStates[D3DRS_POINTSCALEENABLE] = true;
  flags.renderStates[D3DRS_POINTSCALE_A] = true;
  flags.renderStates[D3DRS_POINTSCALE_B] = true;
  flags.renderStates[D3DRS_POINTSCALE_C] = true;
  flags.renderStates[D3DRS_MULTISAMPLEANTIALIAS] = true;
  flags.renderStates[D3DRS_MULTISAMPLEMASK] = true;
  flags.renderStates[D3DRS_PATCHEDGESTYLE] = true;
  flags.renderStates[D3DRS_POINTSIZE_MAX] = true;
  flags.renderStates[D3DRS_INDEXEDVERTEXBLENDENABLE] = true;
  flags.renderStates[D3DRS_TWEENFACTOR] = true;
  flags.renderStates[D3DRS_POSITIONDEGREE] = true;
  flags.renderStates[D3DRS_NORMALDEGREE] = true;
  flags.renderStates[D3DRS_MINTESSELLATIONLEVEL] = true;
  flags.renderStates[D3DRS_MAXTESSELLATIONLEVEL] = true;
  flags.renderStates[D3DRS_ADAPTIVETESS_X] = true;
  flags.renderStates[D3DRS_ADAPTIVETESS_Y] = true;
  flags.renderStates[D3DRS_ADAPTIVETESS_Z] = true;
  flags.renderStates[D3DRS_ADAPTIVETESS_W] = true;
  flags.renderStates[D3DRS_ENABLEADAPTIVETESSELLATION] = true;
  flags.renderStates[D3DRS_NORMALIZENORMALS] = true;
  flags.renderStates[D3DRS_SPECULARENABLE] = true;
  flags.renderStates[D3DRS_SHADEMODE] = true;

  flags.vertexDecl = true;
  std::fill(std::begin(flags.streamFreqs), std::end(flags.streamFreqs), true);
  // Lights in the map are always transferred if they exist 
  // LightEnables in the map are always transferred if they exist 
  for (uint32_t i = caps::MaxTexturesPS + 1; i < BaseDirect3DDevice9Ex_LSS::kMaxStageSamplerStateTypes; i++) {
    flags.samplerStates[i][D3DSAMP_DMAPOFFSET] = true;
  }

  for (auto& fConst : flags.vertexConstants.fConsts) {
    fConst = true;
  }
  for (auto& iConst : flags.vertexConstants.iConsts) {
    iConst = true;
  }
  for (auto& bConst : flags.vertexConstants.bConsts) {
    bConst = true;
  }

  for (uint32_t i = 0; i < flags.streamFreqs.size(); i++) {
    flags.streamFreqs[i] = true;
  }

}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::StateBlockSetCaptureFlags(D3DSTATEBLOCKTYPE Type, BaseDirect3DDevice9Ex_LSS::StateCaptureDirtyFlags& flags) {
  if (Type == D3DSBT_PIXELSTATE || Type == D3DSBT_ALL) {
    StateBlockSetPixelCaptureFlags(flags);
  }
  if (Type == D3DSBT_VERTEXSTATE || Type == D3DSBT_ALL) {
    StateBlockSetVertexCaptureFlags(flags);
  }
  if (Type == D3DSBT_ALL) {
    std::fill(std::begin(flags.textures), std::end(flags.textures), true);
    std::fill(std::begin(flags.streams), std::end(flags.streams), true);
    std::fill(std::begin(flags.streamOffsetsAndStrides), std::end(flags.streamOffsetsAndStrides), true);

    flags.indices = true;
    flags.viewport = true;
    flags.scissorRect = true;

    std::fill(std::begin(flags.clipPlanes), std::end(flags.clipPlanes), true);

    std::fill(std::begin(flags.transforms), std::end(flags.transforms), true);

    flags.material = true;
  }
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) {
  ZoneScoped;
  LogFunctionCall();

  if (ppSB == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    
    Direct3DStateBlock9_LSS* pLssSB = nullptr;
    {
      BRIDGE_DEVICE_LOCKGUARD();
      // Insert our own IDirect3DStateBlock9 interface implementation
      pLssSB = trackWrapper(new Direct3DStateBlock9_LSS(this));
      (*ppSB) = pLssSB;
      StateBlockSetCaptureFlags(Type, pLssSB->m_dirtyFlags);
      pLssSB->LocalCapture();
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateStateBlock, getId());
      currentUID = c.get_uid();
      c.send_many(Type, (uint32_t) pLssSB->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateStateBlock()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::BeginStateBlock() {
  ZoneScoped;
  LogFunctionCall();

  {
    BRIDGE_DEVICE_LOCKGUARD();
    if (m_stateRecording) {
      return D3DERR_INVALIDCALL;
    }
    m_stateRecording = trackWrapper(new Direct3DStateBlock9_LSS(this));
  }
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_BeginStateBlock, getId());
    currentUID = c.get_uid();
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("BeginStateBlock()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::EndStateBlock(IDirect3DStateBlock9** ppSB) {
  ZoneScoped;
  LogFunctionCall();

  if (ppSB == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  BRIDGE_DEVICE_LOCKGUARD();

  if (!m_stateRecording) {
    return D3DERR_INVALIDCALL;
  }
  (*ppSB) = m_stateRecording;

  UID currentUID = 0;
  {
    // Add a handle for the sb
    ClientMessage c(Commands::IDirect3DDevice9Ex_EndStateBlock, getId());
    currentUID = c.get_uid();
    c.send_data((uint32_t) m_stateRecording->getId());
    m_stateRecording = nullptr;
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("EndStateBlock()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetClipStatus(CONST D3DCLIPSTATUS9* pClipStatus) {
  ZoneScoped;
  LogFunctionCall();

  if (pClipStatus == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    m_clipStatus = *pClipStatus;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetClipStatus(D3DCLIPSTATUS9* pClipStatus) {
  ZoneScoped;
  LogFunctionCall();

  if (pClipStatus == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pClipStatus = m_clipStatus;
  }
  return S_OK;
}

namespace {
  static inline bool isInvalidSamplerStage(const DWORD samplerStage) {
    if (samplerStage > 15 && samplerStage < D3DDMAPSAMPLER) {
      return true;
    }
    if (samplerStage > D3DVERTEXTEXTURESAMPLER3) {
      return true;
    }
    return false;
  }
  static inline DWORD mapSamplerStageToIdx(const DWORD samplerStage) {
    if (samplerStage >= D3DDMAPSAMPLER) {
      return caps::MaxTexturesPS + (samplerStage - D3DDMAPSAMPLER);
    }
    return samplerStage;
  }
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) {
  ZoneScoped;
  LogFunctionCall();
  if (isInvalidSamplerStage(Stage) || ppTexture == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  const auto idx = mapSamplerStageToIdx(Stage);
  {
    BRIDGE_DEVICE_LOCKGUARD();

    if (*m_state.textures[idx] != nullptr) {
      switch (m_state.textureTypes[idx]) {
      case D3DRTYPE_TEXTURE:
      {
        (*ppTexture) = bridge_cast<Direct3DTexture9_LSS*>(*m_state.textures[idx]);
        break;
      }
      case D3DRTYPE_CUBETEXTURE:
      {
        (*ppTexture) = bridge_cast<Direct3DCubeTexture9_LSS*>(*m_state.textures[idx]);
        break;
      }
      case D3DRTYPE_VOLUMETEXTURE:
      {
        (*ppTexture) = bridge_cast<Direct3DVolumeTexture9_LSS*>(*m_state.textures[idx]);
        break;
      }
      default:
        assert(0);
        return E_FAIL;
      }
      if ((*ppTexture)) {
        (*ppTexture)->AddRef();
      }
    } else {
      *ppTexture = nullptr;
      return S_OK;
    }
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) {
  ZoneScoped;
  LogFunctionCall();

  if (isInvalidSamplerStage(Stage)) {
    return D3DERR_INVALIDCALL;
  }

  // The Sims 3 camera hook: track what the game binds per stage (not our own remap calls).
  if (sims3cam::enabled() && !g_sims3.inRemap && Stage < 16 && !m_stateRecording) sims3NoteTexture(g_sims3, Stage, pTexture);

  IDirect3DBaseTexture9* pD3DObject = nullptr;
  D3DAutoPtr objectRef;

  const auto idx = mapSamplerStageToIdx(Stage);

  D3DRESOURCETYPE type = D3DRTYPE_FORCE_DWORD;

  {
    BRIDGE_DEVICE_LOCKGUARD();
    if (pTexture != nullptr) {
      switch (pTexture->GetType()) {
      case D3DRTYPE_TEXTURE:
      {
        auto* const pLssTexture = bridge_cast<Direct3DTexture9_LSS*>(pTexture);
        pD3DObject = (pLssTexture->D3D<IDirect3DBaseTexture9>());
        objectRef = MakeD3DAutoPtr(pLssTexture);
        break;
      }
      case D3DRTYPE_CUBETEXTURE:
      {
        auto* const pLssCubeTexture = bridge_cast<Direct3DCubeTexture9_LSS*>(pTexture);
        pD3DObject = (pLssCubeTexture->D3D<IDirect3DBaseTexture9>());
        objectRef = MakeD3DAutoPtr(pLssCubeTexture);
        break;
      }
      case D3DRTYPE_VOLUMETEXTURE:
      {
        auto* const pLssVolumeTexture = bridge_cast<Direct3DVolumeTexture9_LSS*>(pTexture);
        pD3DObject = (pLssVolumeTexture->D3D<IDirect3DBaseTexture9>());
        objectRef = MakeD3DAutoPtr(pLssVolumeTexture);
        break;
      }
      default:
        assert(0);
        return E_FAIL;
      }
      type = pTexture->GetType();
    }
    if (m_stateRecording) {
      m_stateRecording->m_captureState.textures[idx] = std::move(objectRef);
      m_stateRecording->m_captureState.textureTypes[idx] = type;
      m_stateRecording->m_dirtyFlags.textures[idx] = true;
    } else {
      m_state.textures[idx] = std::move(objectRef);
      m_state.textureTypes[idx] = type;
    }
  }
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_SetTexture, getId());
    currentUID = c.get_uid();
    c.send_many(Stage, (uint32_t) pD3DObject);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetTexture()", D3DERR_INVALIDCALL, currentUID);
}

namespace {
  enum TextureStageStateType {
    ColorOp = 0,
    ColorArg1 = 1,
    ColorArg2 = 2,
    AlphaOp = 3,
    AlphaArg1 = 4,
    AlphaArg2 = 5,
    BumpEnvMat00 = 6,
    BumpEnvMat01 = 7,
    BumpEnvMat10 = 8,
    BumpEnvMat11 = 9,
    TexCoordIdx = 10,
    BumpEnvLScale = 11,
    BumpEnvLOffset = 12,
    TexXformFlags = 13,
    ColorArg0 = 14,
    AlphaArg0 = 15,
    ResultArg = 16,
    Constant = 17,
    kCount
  };
  static_assert(BaseDirect3DDevice9Ex_LSS::kMaxTexStageStateTypes == (size_t) TextureStageStateType::kCount);
  static size_t TexStageStateTypeToIdx(const D3DTEXTURESTAGESTATETYPE type) {
    switch (type) {
    case D3DTSS_COLOROP: return (size_t) TextureStageStateType::ColorOp;
    case D3DTSS_COLORARG1: return (size_t) TextureStageStateType::ColorArg1;
    case D3DTSS_COLORARG2: return (size_t) TextureStageStateType::ColorArg2;
    case D3DTSS_ALPHAOP: return (size_t) TextureStageStateType::AlphaOp;
    case D3DTSS_ALPHAARG1: return (size_t) TextureStageStateType::AlphaArg1;
    case D3DTSS_ALPHAARG2: return (size_t) TextureStageStateType::AlphaArg2;
    case D3DTSS_BUMPENVMAT00: return (size_t) TextureStageStateType::BumpEnvMat00;
    case D3DTSS_BUMPENVMAT01: return (size_t) TextureStageStateType::BumpEnvMat01;
    case D3DTSS_BUMPENVMAT10: return (size_t) TextureStageStateType::BumpEnvMat10;
    case D3DTSS_BUMPENVMAT11: return (size_t) TextureStageStateType::BumpEnvMat11;
    case D3DTSS_TEXCOORDINDEX: return (size_t) TextureStageStateType::TexCoordIdx;
    case D3DTSS_BUMPENVLSCALE: return (size_t) TextureStageStateType::BumpEnvLScale;
    case D3DTSS_BUMPENVLOFFSET: return (size_t) TextureStageStateType::BumpEnvLOffset;
    case D3DTSS_TEXTURETRANSFORMFLAGS: return (size_t) TextureStageStateType::TexXformFlags;
    case D3DTSS_COLORARG0: return (size_t) TextureStageStateType::ColorArg0;
    case D3DTSS_ALPHAARG0: return (size_t) TextureStageStateType::AlphaArg0;
    case D3DTSS_RESULTARG: return (size_t) TextureStageStateType::ResultArg;
    case D3DTSS_CONSTANT: return (size_t) TextureStageStateType::Constant;
    }
    return (size_t) -1;
  }
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) {
  ZoneScoped;
  LogFunctionCall();
  if (isInvalidSamplerStage(Stage) || pValue == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  const auto typeIdx = TexStageStateTypeToIdx(Type);
  if (typeIdx >= kMaxTexStageStateTypes) {
    return D3DERR_INVALIDCALL;
  }
  const auto stageIdx = mapSamplerStageToIdx(Stage);
  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pValue = m_state.textureStageStates[stageIdx][typeIdx];
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
  ZoneScoped;
  LogFunctionCall();
  if (Stage >= caps::MaxSimultaneousTextures) {
    return D3DERR_INVALIDCALL;
  }
  const auto typeIdx = TexStageStateTypeToIdx(Type);
  if (typeIdx >= kMaxTexStageStateTypes) {
    return D3DERR_INVALIDCALL;
  }
  const auto stageIdx = mapSamplerStageToIdx(Stage);
  // The Sims 3 camera hook: the game's own stage-0 values (ours are marked), put back before uncaptured draws
  if (sims3cam::enabled() && !g_sims3.ourState && Stage == 0) {
    const int k = Type == D3DTSS_COLOROP ? 0 : Type == D3DTSS_COLORARG1 ? 1 : Type == D3DTSS_COLORARG2 ? 2 : Type == D3DTSS_TEXCOORDINDEX ? 3 : -1;
    if (k >= 0) {
      g_sims3.gameTss0[k] = Value;
      if (k == 3) g_sims3.uvIndexHidden = false; else g_sims3.tssOurs &= (uint8_t) ~(1u << k);   // the runtime now holds the game's value; ours is re-applied at the next captured draw
    }
  }
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_stateRecording->m_dirtyFlags.textureStageStates[stageIdx][typeIdx] && m_stateRecording->m_captureState.textureStageStates[stageIdx][typeIdx] == Value) {
          return S_OK;
        }
        m_stateRecording->m_captureState.textureStageStates[stageIdx][typeIdx] = Value;
        m_stateRecording->m_dirtyFlags.textureStageStates[stageIdx][typeIdx] = true;
      } else {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_state.textureStageStates[stageIdx][typeIdx] == Value) {
          return S_OK;
        }
        m_state.textureStageStates[stageIdx][typeIdx] = Value;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetTextureStageState, getId());
      currentUID = c.get_uid();
      c.send_many(Stage, Type, Value);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetTextureStageState()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) {
  ZoneScoped;
  LogFunctionCall();
  if (isInvalidSamplerStage(Sampler) || pValue == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  const auto typeIdx = Type - 1;
  if (typeIdx >= kMaxStageSamplerStateTypes) {
    return D3DERR_INVALIDCALL;
  }
  const auto samplerIdx = mapSamplerStageToIdx(Sampler);
  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pValue = m_state.samplerStates[samplerIdx][typeIdx];
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
  ZoneScoped;
  LogFunctionCall();
  if (isInvalidSamplerStage(Sampler)) {
    return D3DERR_INVALIDCALL;
  }
  const auto typeIdx = Type - 1;
  if (typeIdx >= kMaxStageSamplerStateTypes) {
    return D3DERR_INVALIDCALL;
  }
  const auto samplerIdx = mapSamplerStageToIdx(Sampler);
  // The Sims 3 camera hook (milestone 18g): the game setting a sampler state the terrain block
  // holds cancels that state's restore -- the game's value is the current one
  if (sims3cam::enabled() && !g_sims3.ourSampler && !m_stateRecording && g_sims3.tblockActive) {
    auto& h = g_sims3;
    if ((int) Sampler == h.tblockStage) for (int i = 0; i < Sims3Hook::kSamplerCopies; ++i) if (kSims3SamplerCopy[i] == Type && (h.tblockSet & (1u << i))) { h.tblockSet &= (uint8_t) ~(1u << i); ++h.tblockCancelled; }
    if (Type == D3DSAMP_SRGBTEXTURE && Sampler < 16 && (h.tblockSrgb & (1u << Sampler))) { h.tblockSrgb &= (uint16_t) ~(1u << Sampler); ++h.tblockCancelled; }
  }
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        if (GlobalOptions::getEliminateRedundantSetterCalls() &&
            m_stateRecording->m_dirtyFlags.samplerStates[samplerIdx][typeIdx] &&
            m_stateRecording->m_captureState.samplerStates[samplerIdx][typeIdx] == Value) {
          return S_OK;
        }
        m_stateRecording->m_captureState.samplerStates[samplerIdx][typeIdx] = Value;
        m_stateRecording->m_dirtyFlags.samplerStates[samplerIdx][typeIdx] = true;
      } else {
        if (GlobalOptions::getEliminateRedundantSetterCalls() && 
            m_state.samplerStates[samplerIdx][typeIdx] == Value) {
          return S_OK;
        }
        m_state.samplerStates[samplerIdx][typeIdx] = Value;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetSamplerState, getId());
      currentUID = c.get_uid();
      c.send_many(Sampler, Type, Value);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetSamplerState()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::ValidateDevice(DWORD* pNumPasses) {
  ZoneScoped;
  LogFunctionCall();

  if (pNumPasses == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  // Since we're running graphics on a strictly better graphics API and HW,
  // always return 1 rendering pass which is the best case for d3d8/d3d9.
  *pNumPasses = 1;

  return D3D_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetPaletteEntries(UINT PaletteNumber, CONST PALETTEENTRY* pEntries) {
  ZoneScoped;
  LogFunctionCall();

  if (pEntries == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    m_paletteEntries[PaletteNumber] = *pEntries;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) {
  ZoneScoped;
  LogFunctionCall();

  if (pEntries == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pEntries = m_paletteEntries[PaletteNumber];
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetCurrentTexturePalette(UINT PaletteNumber) {
  ZoneScoped;
  LogFunctionCall();
  {
    BRIDGE_DEVICE_LOCKGUARD();
    m_curTexPalette = PaletteNumber;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetCurrentTexturePalette(UINT* pPaletteNumber) {
  ZoneScoped;
  LogFunctionCall();

  if (pPaletteNumber == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pPaletteNumber = m_curTexPalette;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetScissorRect(CONST RECT* pRect) {
  ZoneScoped;
  LogFunctionCall();

  if (pRect == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        m_stateRecording->m_captureState.scissorRect = *pRect;
        m_stateRecording->m_dirtyFlags.scissorRect = true;
      } else {
        m_state.scissorRect = *pRect;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetScissorRect, getId());
      currentUID = c.get_uid();
      c.send_data(sizeof(RECT), (void*) pRect);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetScissorRect()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetScissorRect(RECT* pRect) {
  ZoneScoped;
  LogFunctionCall();

  if (pRect == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pRect = m_state.scissorRect;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetSoftwareVertexProcessing(BOOL bSoftware) {
  ZoneScoped;
  LogFunctionCall();
  {
    BRIDGE_DEVICE_LOCKGUARD();
    if (m_bSoftwareVtxProcessing == bSoftware)
      return D3D_OK;
    m_bSoftwareVtxProcessing = bSoftware;
  }
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_SetSoftwareVertexProcessing, getId());
    currentUID = c.get_uid();
    c.send_data(bSoftware);
  }
  WAIT_FOR_SERVER_RESPONSE("SetSoftwareVertexProcessing()", D3DERR_INVALIDCALL, currentUID);

  HRESULT hresult = DeviceBridge::get_data();
  DeviceBridge::pop_front();

  return hresult;
}

template<bool EnableSync>
int Direct3DDevice9Ex_LSS<EnableSync>::GetSoftwareVertexProcessing() {
  ZoneScoped;
  LogFunctionCall();
  BOOL result;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    result = m_bSoftwareVtxProcessing;
  }
  return result;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetNPatchMode(float nSegments) {
  LogFunctionCall();
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      m_NPatchMode = nSegments;
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetNPatchMode, getId());
      currentUID = c.get_uid();
      c.send_data(sizeof(float), &nSegments);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetNPatchMode()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
float Direct3DDevice9Ex_LSS<EnableSync>::GetNPatchMode() {
  ZoneScoped;
  LogFunctionCall();
  float result;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    result = m_NPatchMode;
  }
  return result;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) {
  ZoneScoped;
  LogFunctionCall();
  SIMS3_BEGIN_DRAW();
  UID currentUID = 0;
  if (sims3cam::enabled() && g_sims3.splitDraw && PrimitiveType == D3DPT_TRIANGLELIST && PrimitiveCount >= 2) {   // a lot's re-submission as two half draws (milestone 17r)
    const UINT half_ = PrimitiveCount / 2;
    { ClientMessage c(Commands::IDirect3DDevice9Ex_DrawPrimitive, getId()); currentUID = c.get_uid(); c.send_many(PrimitiveType, StartVertex, half_); }
    { ClientMessage c(Commands::IDirect3DDevice9Ex_DrawPrimitive, getId()); currentUID = c.get_uid(); c.send_many(PrimitiveType, StartVertex + half_ * 3, PrimitiveCount - half_); }
    ++g_sims3.splitDraws;
  } else {
    ClientMessage c(Commands::IDirect3DDevice9Ex_DrawPrimitive, getId());
    currentUID = c.get_uid();
    c.send_many(PrimitiveType, StartVertex, PrimitiveCount);
  }
  if (sims3cam::enabled() && g_sims3.compositeSecond) { SIMS3_END_DRAW(); sims3CompositeSecondPass(g_sims3, this, false, PrimitiveType, 0, 0, 0, StartVertex, PrimitiveCount); } else
  SIMS3_END_DRAW();
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("DrawPrimitive()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::DrawIndexedPrimitive(D3DPRIMITIVETYPE Type, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) {
  ZoneScoped;
  LogFunctionCall();
  SIMS3_BEGIN_DRAW();
  // The Sims 3 camera hook: a captured draw whose render states make it invisible in-game
  // (colour writes off, a depth test that never passes, an alpha test that never passes, a
  // blend of zero source and one destination) contributes nothing to the game's image, so
  // it is not sent to the ray tracer either. Only states the game has set are trusted.
  if (sims3cam::enabled() && g_sims3.held.kind == sims3cam::Kind::Main && primCount > 0) {
    auto& h = g_sims3;
    const DWORD* rs = m_state.renderStates.data();
    const bool cwOff = h.rsSet[D3DRS_COLORWRITEENABLE] && (rs[D3DRS_COLORWRITEENABLE] & 0xF) == 0;
    const bool zNever = h.rsSet[D3DRS_ZFUNC] && rs[D3DRS_ZFUNC] == D3DCMP_NEVER && (!h.rsSet[D3DRS_ZENABLE] || rs[D3DRS_ZENABLE] != D3DZB_FALSE);
    const bool aNever = h.rsSet[D3DRS_ALPHATESTENABLE] && rs[D3DRS_ALPHATESTENABLE] && h.rsSet[D3DRS_ALPHAFUNC] && rs[D3DRS_ALPHAFUNC] == D3DCMP_NEVER;
    const bool bZero = h.rsSet[D3DRS_ALPHABLENDENABLE] && rs[D3DRS_ALPHABLENDENABLE] && h.rsSet[D3DRS_SRCBLEND] && h.rsSet[D3DRS_DESTBLEND] && rs[D3DRS_SRCBLEND] == D3DBLEND_ZERO && rs[D3DRS_DESTBLEND] == D3DBLEND_ONE;
    if (cwOff || zNever || aNever || bZero) {
      ++h.invisibleDrawsSkipped;
      if (h.invisibleLogged < 6) {
        ++h.invisibleLogged;
        char msg[300];
        snprintf(msg, sizeof msg, "Sims 3 camera hook: invisible draw skipped at frame %u -> VS %016llx PS %016llx: colour writes %lu, zfunc %lu, alpha test %lu/%lu, blend %lu %lu/%lu, %u primitives",
                 h.frames + 1, (unsigned long long) h.vsHash, (unsigned long long) h.psHash,
                 (unsigned long) rs[D3DRS_COLORWRITEENABLE], (unsigned long) rs[D3DRS_ZFUNC], (unsigned long) rs[D3DRS_ALPHATESTENABLE], (unsigned long) rs[D3DRS_ALPHAFUNC],
                 (unsigned long) rs[D3DRS_ALPHABLENDENABLE], (unsigned long) rs[D3DRS_SRCBLEND], (unsigned long) rs[D3DRS_DESTBLEND], (unsigned) primCount);
        Logger::info(msg);
      }
      SIMS3_END_DRAW();
      return D3D_OK;
    }
  }
  UID currentUID = 0;
  {
    // The Sims 3 camera hook: a captured hardware-instanced draw goes to the runtime once per
    // instance (see Sims3Hook::drawCaptured), the instance streams' offsets stepping through
    // their data and stream 0 at one instance for the duration; the game's settings come back
    // afterwards.
    uint32_t instances_ = 1; uint32_t instStreams_ = 0;
    if (sims3cam::enabled() && g_sims3.drawCaptured && (m_state.streamFreqs[0] & D3DSTREAMSOURCE_INDEXEDDATA)) {
      instances_ = m_state.streamFreqs[0] & 0x3FFFFFFFu;
      for (uint32_t s_ = 1; s_ < caps::MaxStreams; ++s_)
        if ((m_state.streamFreqs[s_] & D3DSTREAMSOURCE_INSTANCEDATA) && *m_state.streams[s_] != nullptr && m_state.streamStrides[s_] > 0) instStreams_ |= 1u << s_;
      if (instances_ <= 1 || instances_ > 1024 || instStreams_ == 0) instances_ = 1;
    }
    if (instances_ > 1) {
      const UINT freq0_ = m_state.streamFreqs[0];
      IDirect3DVertexBuffer9* vb_[caps::MaxStreams] = {}; UINT off_[caps::MaxStreams] = {}, stride_[caps::MaxStreams] = {};
      for (uint32_t s_ = 1; s_ < caps::MaxStreams; ++s_) if (instStreams_ & (1u << s_)) {
        vb_[s_] = (IDirect3DVertexBuffer9*) bridge_cast<Direct3DVertexBuffer9_LSS*>(*m_state.streams[s_]);
        vb_[s_]->AddRef();   // held across the re-bindings below (each one drops the state's reference first)
        off_[s_] = m_state.streamOffsets[s_]; stride_[s_] = m_state.streamStrides[s_];
      }
      // The runtime folds draws of one frame with the same material, geometry and vertex-shader
      // hash into one object, and the vertex-shader hash of a captured draw covers the bytecode AND
      // the float constants the shader can reach (d3d9_rtx_geometry.cpp). A per-instance value in
      // c255 (never uploaded by the game; the variant bound for the draw reads it, see
      // appendConstantRead) keeps each instance its own object.
      float savedTag_[4]; memcpy(savedTag_, &m_state.vertexConstants.fConsts[255], sizeof savedTag_);
      SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 1u);
      for (uint32_t i_ = 0; i_ < instances_; ++i_) {
        for (uint32_t s_ = 1; s_ < caps::MaxStreams; ++s_) if (instStreams_ & (1u << s_)) SetStreamSource(s_, vb_[s_], off_[s_] + i_ * stride_[s_], stride_[s_]);
        const float tag_[4] = { (float) (i_ + 1), 0.f, 0.f, 0.f };
        SetVertexShaderConstantF(255, tag_, 1);
        ClientMessage c(Commands::IDirect3DDevice9Ex_DrawIndexedPrimitive, getId());
        currentUID = c.get_uid();
        c.send_many(Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
      }
      SetVertexShaderConstantF(255, savedTag_, 1);
      for (uint32_t s_ = 1; s_ < caps::MaxStreams; ++s_) if (instStreams_ & (1u << s_)) { SetStreamSource(s_, vb_[s_], off_[s_], stride_[s_]); vb_[s_]->Release(); }
      SetStreamSourceFreq(0, freq0_);
      ++g_sims3.deinstancedDraws; g_sims3.deinstancedInstances += instances_;
      if (g_sims3.deinstanceLogged < 8) {
        ++g_sims3.deinstanceLogged; char msg[224];
        snprintf(msg, sizeof msg, "Sims 3 camera hook: instanced draw split at frame %u -> %u instances, %u primitives each, instance streams %x, VS %016llx PS %016llx", g_sims3.frames + 1, instances_, (unsigned) primCount, instStreams_, (unsigned long long) g_sims3.vsHash, (unsigned long long) g_sims3.psHash);
        Logger::info(msg);
      }
    } else {
      // The Sims 3 camera hook (milestone 22): the game's own lamp lights. An object shader's draw
      // names its model by the hash of its index buffer (the client's copy, hashed once per
      // buffer); a model in the light table registers each of its lights at its exact world
      // position, through the object's World rows. The rig's rays then only say whether it is lit.
      if (sims3cam::enabled() && g_sims3.psRig != nullptr && g_sims3.objWorldValid && *m_state.indices != nullptr && sims3cam::liteTable().n > 0) {
        auto* ib_ = bridge_cast<Direct3DIndexBuffer9_LSS*>(*m_state.indices);
        const uint32_t ibId_ = (uint32_t) ib_->getId();
        int model_ = -2;
        for (uint32_t k = 0; k < g_sims3.ibModelCount && model_ == -2; ++k) if (g_sims3.ibModelId[k] == ibId_) model_ = g_sims3.ibModel[k];
        if (model_ == -2) {
          const uint8_t* d_ = ib_->sims3Data(); const uint32_t sz_ = ib_->sims3Size();
          const uint64_t hash_ = d_ ? sims3cam::fnv1a64(d_, sz_) : 0;
          const sims3cam::LiteModel* m_ = d_ ? sims3cam::findLiteModel(hash_) : nullptr;
          model_ = m_ ? (int) (m_ - sims3cam::liteTable().models) : -1;
          if (d_ && g_sims3.ibModelCount < 512) { g_sims3.ibModelId[g_sims3.ibModelCount] = ibId_; g_sims3.ibModel[g_sims3.ibModelCount] = model_; g_sims3.ibHash[g_sims3.ibModelCount] = hash_; ++g_sims3.ibModelCount; }
          if (m_) {
            ++g_sims3.liteMatches;
            if (g_sims3.liteLogged < 100) {
              ++g_sims3.liteLogged; char msg_[300];
              snprintf(msg_, sizeof msg_, "Sims 3 camera hook: lamp model at frame %u: index buffer [%u] (%u bytes, hash %016llx) is model %016llx with %u light(s), the first of type %u at (%.2f, %.2f, %.2f) in the model, intensity %.0f; object at (%.1f, %.1f, %.1f)",
                       g_sims3.frames + 1, ibId_, sz_, (unsigned long long) hash_, (unsigned long long) m_->inst, m_->n, m_->lights[0].type, m_->lights[0].pos[0], m_->lights[0].pos[1], m_->lights[0].pos[2], m_->lights[0].intensity,
                       g_sims3.objWorld[0], g_sims3.objWorld[1], g_sims3.objWorld[2]);
              Logger::info(msg_);
            }
          }
        }
        // At the mark key (milestone 29): every object-shader draw of the marked frames -- its mesh, its
        // light-table match, the texture at s2 and its rows -- to see which objects carry which map, and
        // to name a lamp the table does not know (run 142's ceiling pendant).
        if (g_sims3.markDump && g_sims3.markRigLogged < 80 && model_ < 0) {
          uint64_t ibHash_ = 0;
          for (uint32_t k = 0; k < g_sims3.ibModelCount; ++k) if (g_sims3.ibModelId[k] == ibId_) { ibHash_ = g_sims3.ibHash[k]; break; }
          bool named_ = false;
          for (uint32_t k = 0; k < g_sims3.markMeshCount && !named_; ++k) named_ = g_sims3.markMeshes[k] == ibHash_;
          if (!named_ && g_sims3.markMeshCount < 80) g_sims3.markMeshes[g_sims3.markMeshCount++] = ibHash_;
          if (!named_) ++g_sims3.markRigLogged;
          const float* r_ = g_sims3.objRowsAfter; char msg_[400];
          if (!named_) {
          snprintf(msg_, sizeof msg_, "Sims 3 camera hook: object draw at the mark, frame %u, object at (%.1f, %.1f, %.1f), mesh [%u] %u bytes hash %016llx (%s), VS %016llx PS %016llx, s2 %p %ux%u format %u, rows after World %s(%.4g %.4g %.4g %.4g) (%.4g %.4g %.4g %.4g)",
                   g_sims3.frames + 1, g_sims3.objWorld[0], g_sims3.objWorld[1], g_sims3.objWorld[2], ibId_, ib_->sims3Size(), (unsigned long long) ibHash_, model_ >= 0 ? "in the light table" : "not in the light table",
                   (unsigned long long) g_sims3.vsHash, (unsigned long long) g_sims3.psHash, (void*) g_sims3.boundTex[2], (unsigned) g_sims3.boundW[2], (unsigned) g_sims3.boundH[2], (unsigned) g_sims3.boundFmt[2],
                   g_sims3.objRowsAfterValid ? "" : "(stale) ", r_[0], r_[1], r_[2], r_[3], r_[4], r_[5], r_[6], r_[7]);
          Logger::info(msg_);
          }
        }
        if (model_ >= 0) {
          const sims3cam::LiteModel& m_ = sims3cam::liteTable().models[model_];
          // The draw registers the lamp (or finds it again) and names the light map its draw carries
          // (milestone 29: a shared A8R8G8B8 texture at s2, adopted at once or after three consecutive
          // draws with it); its state comes from that map at the frame's end, never from this draw's texels.
          Sims3Hook::LightMapEntry* e_ = sims3LightMapOf(g_sims3, g_sims3.boundTex[2]);
          void* mapTex_ = (e_ && e_->shared) ? (void*) e_->tex : nullptr;
          for (uint8_t li = 0; li < m_.n; ++li) {
            const sims3cam::LiteLight& L_ = m_.lights[li];
            float wp_[3]; sims3cam::worldPoint(g_sims3.objWorldRows, L_.pos, wp_);
            const uint32_t id_ = sims3cam::LampSolver::originId(g_sims3.objWorld) ^ (0x9E3779B9u * (uint32_t) (li + 1));
            g_sims3.lamps.addModelLamp(id_, li, g_sims3.objWorld, wp_, L_.col, L_.intensity, L_.type, (int8_t) -1);
            {
              // the shape (milestone 32): the definition's direction points from the lit side back to the
              // light, so the light travels the other way; carried to the world by the object's rotation
              float wa_[3]; sims3cam::worldDir(g_sims3.objWorldRows, L_.at, wa_);
              const float travel_[3] = { -wa_[0], -wa_[1], -wa_[2] }, none_[3] = {};
              g_sims3.lamps.shapeLamp(li, g_sims3.objWorld, travel_, (L_.type == 4 || L_.type == 5) ? L_.d[0] : 0.f, L_.type == 5 ? L_.d[2] : 0.f, L_.type == 5 ? L_.d + 3 : none_, L_.type == 6 ? L_.d[0] : 0.f);
            }
            void* from_ = nullptr;
            if (mapTex_ && g_sims3.lamps.mapLamp(li, g_sims3.objWorld, mapTex_, &from_) && from_ != nullptr) {
              ++g_sims3.lightMapSwitches;
              if (g_sims3.lightMapSwitchesLogged < 40) {
                ++g_sims3.lightMapSwitchesLogged; char msg_[240];
                snprintf(msg_, sizeof msg_, "Sims 3 camera hook: the lamp at (%.1f, %.1f, %.1f) is now sampled through light map %p (%ux%u) instead of %p, frame %u",
                         g_sims3.objWorld[0], g_sims3.objWorld[1], g_sims3.objWorld[2], mapTex_, e_->W, e_->H, from_, g_sims3.frames + 1);
                Logger::info(msg_);
              }
            }
          }
          // At the mark key (milestones 26, 29): this lamp object's draw -- the texture at s2, the map it is
          // judged by and its standing word -- and every live light map's brightness at the lamp's light,
          // at its base and 1.5 units away, to see which map says what.
          if (g_sims3.markDump && g_sims3.markDumpLogged < 60) {
            ++g_sims3.markDumpLogged;
            const sims3cam::Lamp* Lm_ = g_sims3.lamps.find(0, g_sims3.objWorld);
            char msg_[700];
            size_t n_ = (size_t) snprintf(msg_, sizeof msg_, "Sims 3 camera hook: lamp object at the mark, frame %u, object at (%.1f, %.1f, %.1f) model %016llx: s2 %p%s; its draws' map %p, standing word %d;",
                                          g_sims3.frames + 1, g_sims3.objWorld[0], g_sims3.objWorld[1], g_sims3.objWorld[2], (unsigned long long) m_.inst, (void*) g_sims3.boundTex[2],
                                          e_ ? (e_->shared ? " (a shared map)" : " (seen on this object only)") : " (not a noted map)", Lm_ ? Lm_->map : nullptr, Lm_ ? (int) Lm_->state : -2);
            {
              const float* rec_ = g_sims3.lampReportLive ? sims3cam::lampReportFind(g_sims3.lampRecords.data() + sims3cam::kLampFloats, g_sims3.lampReported, g_sims3.objWorld) : nullptr;
              if (rec_ && n_ < sizeof msg_ - 160) n_ += (size_t) snprintf(msg_ + n_, sizeof msg_ - n_, " the game's own word: %s, colour preset %d (%.2f, %.2f, %.2f), intensity %.2f, dimmer %.2f, at (%.1f, %.1f, %.1f);",
                                                                          rec_[8] > 0.5f ? "ON" : "off", (int) (rec_[9] + 0.5f), rec_[3], rec_[4], rec_[5], rec_[6], rec_[7], rec_[0], rec_[1], rec_[2]);
              else if (n_ < sizeof msg_ - 60) n_ += (size_t) snprintf(msg_ + n_, sizeof msg_ - n_, " the game's own word: none (%s);", g_sims3.lampReportLive ? "the reporter does not name it" : "no reporter");
            }
            if (n_ < sizeof msg_ - 60) n_ += (size_t) snprintf(msg_ + n_, sizeof msg_ - n_, " sun level %.2f, maps read %s;", g_sims3.skyLevel, g_sims3.lampNight ? "as they are (night)" : "by their changes (day)");
            for (int k_ = 0; k_ < 4 && n_ < sizeof msg_ - 140; ++k_) {
              const Sims3Hook::LightMapEntry& em_ = g_sims3.lightMaps[k_];
              if (!em_.tex || !em_.valid) continue;
              Sims3LampReading rd_;
              const bool in_ = sims3LightMapOwn(em_, g_sims3.objWorld, sims3cam::lampRing(), rd_);
              n_ += (size_t) snprintf(msg_ + n_, sizeof msg_ - n_, " map %p %ux%u v%u: at the base %d (%.0f at its last reading), the ring around it %d, the darkest near it %d, its own part %.0f%s;",
                                      (void*) em_.tex, em_.W, em_.H, em_.version, rd_.base, (Lm_ && (Lm_->ownInit & (1u << k_))) ? Lm_->seen[k_] : -1.f, rd_.around, rd_.darkest, (Lm_ && (Lm_->ownInit & (1u << k_))) ? Lm_->own[k_] : -1.f, in_ ? "" : " (beyond it)");
            }
            Logger::info(msg_);
          }
        }
      } else if (sims3cam::enabled() && g_sims3.markDump && g_sims3.markRigLogged < 80 && g_sims3.objWorldValid && *m_state.indices != nullptr) {
        // At the mark key (milestone 31): a draw WITHOUT a rig shader at a known object position -- a lamp
        // switched on may be drawn through another shader (run 144's pendant left the rig draws once on):
        // its mesh, its shaders and its textures, once per mesh.
        auto* ibn_ = bridge_cast<Direct3DIndexBuffer9_LSS*>(*m_state.indices);
        const uint8_t* dn_ = ibn_->sims3Data(); const uint32_t szn_ = ibn_->sims3Size();
        const uint64_t hn_ = dn_ ? sims3cam::fnv1a64(dn_, szn_) : 0;
        bool named_ = false;
        for (uint32_t k = 0; k < g_sims3.markMeshCount && !named_; ++k) named_ = g_sims3.markMeshes[k] == hn_;
        if (!named_) {
          if (g_sims3.markMeshCount < 80) g_sims3.markMeshes[g_sims3.markMeshCount++] = hn_;
          ++g_sims3.markRigLogged;
          char msg_[420];
          snprintf(msg_, sizeof msg_, "Sims 3 camera hook: draw without a rig at the mark, frame %u, object at (%.1f, %.1f, %.1f), mesh [%u] %u bytes hash %016llx (%s), VS %016llx PS %016llx, textures s0 %ux%u s1 %ux%u s2 %ux%u s3 %ux%u",
                   g_sims3.frames + 1, g_sims3.objWorld[0], g_sims3.objWorld[1], g_sims3.objWorld[2], (uint32_t) ibn_->getId(), szn_, (unsigned long long) hn_, sims3cam::findLiteModel(hn_) ? "in the light table" : "not in the light table",
                   (unsigned long long) g_sims3.vsHash, (unsigned long long) g_sims3.psHash, (unsigned) g_sims3.boundW[0], (unsigned) g_sims3.boundH[0], (unsigned) g_sims3.boundW[1], (unsigned) g_sims3.boundH[1],
                   (unsigned) g_sims3.boundW[2], (unsigned) g_sims3.boundH[2], (unsigned) g_sims3.boundW[3], (unsigned) g_sims3.boundH[3]);
          Logger::info(msg_);
        }
      }
      // The Sims 3 camera hook: a captured wall draw gets its window and door openings cut into
      // the geometry (milestone 13, sims3_walls.h). The pixel shader's mask test is evaluated on
      // the client from the buffers' shadow copies and the mask atlas, and the cut triangles are
      // drawn from the hook's own buffers; the game's bindings come back afterwards.
      bool wallDone_ = false;
      if (sims3cam::enabled() && sims3cam::wallCutEnabled() && g_sims3.drawCaptured && Type == D3DPT_TRIANGLELIST && g_sims3.wallLayout.valid && g_sims3.vsWall && g_sims3.vsWall->valid
          && g_sims3.psAuto && g_sims3.psAuto->valid && g_sims3.psAuto->maskSampler >= 0 && g_sims3.psAuto->maskSampler < 16
          && *m_state.streams[0] != nullptr && *m_state.streams[1] != nullptr && *m_state.indices != nullptr) {
        auto wallDraw_ = [&]() -> bool {
          auto& h = g_sims3;
          const sims3cam::PsAnalysis& ps = *h.psAuto;
          const DWORD* rs = m_state.renderStates.data();
          // the discard threshold: texkill at 0.5; walls C alpha-tests mask + z - 0.5 against the reference
          float thr = -1.f;
          if (ps.maskKill) thr = 0.5f;
          else if (ps.maskAlpha && rs[D3DRS_ALPHATESTENABLE] && (rs[D3DRS_ALPHAFUNC] == D3DCMP_GREATEREQUAL || rs[D3DRS_ALPHAFUNC] == D3DCMP_GREATER)) thr = 0.5f + (float) (rs[D3DRS_ALPHAREF] & 0xFFu) / 255.f;
          if (thr < 0.f) return false;
          auto* vb0 = bridge_cast<Direct3DVertexBuffer9_LSS*>(*m_state.streams[0]);
          auto* vb1 = bridge_cast<Direct3DVertexBuffer9_LSS*>(*m_state.streams[1]);
          auto* ib = bridge_cast<Direct3DIndexBuffer9_LSS*>(*m_state.indices);
          const uint8_t* d0 = vb0->sims3Data(); const uint8_t* d1 = vb1->sims3Data(); const uint8_t* di = ib->sims3Data();
          if (!d0 || !d1 || !di) { ++h.wallSkipped; return false; }
          // the mask: the texture at the pixel shader's mask sampler (none bound: black everywhere)
          const int s = ps.maskSampler;
          uint32_t maskId = 0, maskVer = 0, maskW = 0, maskH = 0, maskFmt = 0; uint64_t maskHash = 0; const uint8_t* mask = nullptr; bool maskOk = true;
          if (h.boundTex[s] != nullptr) {
            if ((h.boundKind[s] & 0x7F) != 1) {
              maskOk = false;
            } else {
              auto* tex = bridge_cast<Direct3DTexture9_LSS*>(h.boundTex[s]);
              const D3DSURFACE_DESC d = tex->getLevelDesc(0);
              maskId = (uint32_t) tex->getId(); maskVer = tex->sims3Level0Version(); maskW = d.Width; maskH = d.Height; maskFmt = (uint32_t) d.Format;
              mask = sims3WallMask(h, maskId, maskVer, maskFmt, d.Width, d.Height, tex->sims3Level0Data(), maskHash);
              maskOk = mask != nullptr;
            }
          }
          if (!maskOk) {
            ++h.wallSkipped;
            if (h.wallSkipLogged < 4) {
              ++h.wallSkipLogged; char fb[16]; char m[256];
              snprintf(m, sizeof m, "Sims 3 camera hook: wall draw not cut at frame %u -> the mask at stage %d (%s %ux%u) is not readable on the client, VS %016llx PS %016llx", h.frames + 1, s, sims3FormatName(maskFmt, fb, sizeof fb), maskW, maskH, (unsigned long long) h.vsHash, (unsigned long long) h.psHash);
              Logger::info(m);
            }
            return false;
          }
          sims3cam::WallCutInput in;
          in.layout = h.wallLayout;
          in.vb0 = d0; in.vb0Size = vb0->sims3Size(); in.offset0 = m_state.streamOffsets[0]; in.stride0 = m_state.streamStrides[0];
          in.vb1 = d1; in.vb1Size = vb1->sims3Size(); in.offset1 = m_state.streamOffsets[1]; in.stride1 = m_state.streamStrides[1];
          in.ib = di; in.ibSize = ib->sims3Size(); in.ib32 = ib->getDesc().Format == D3DFMT_INDEX32;
          in.baseVertex = BaseVertexIndex; in.startIndex = startIndex; in.primCount = primCount;
          in.mask = mask; in.maskW = maskW; in.maskH = maskH;
          float ck[4]; memcpy(ck, &m_state.vertexConstants.fConsts[h.vsWall->clampReg], sizeof ck);
          in.params.clampLo = ck[0]; in.params.clampHi = ck[1]; in.params.clampVis = ck[2]; in.params.kScale = h.vsWall->kScale; in.params.threshold = thr;
          // the key follows the buffers' and the mask's CONTENT (hashed once per upload), the draw range, the
          // declaration and the constants the cut depends on
          struct { uint64_t vb0Hash, vb1Hash, ibHash, maskHash; uint32_t off0, st0, off1, st1, ib32, declId; int32_t base; uint32_t start, prims; float lo, hi, vis, kScale, thr; } k;
          memset(&k, 0, sizeof k);
          k.vb0Hash = sims3ContentHash(h, (uint32_t) vb0->getId(), vb0->sims3Version, d0, in.vb0Size);
          k.vb1Hash = sims3ContentHash(h, (uint32_t) vb1->getId(), vb1->sims3Version, d1, in.vb1Size);
          k.ibHash = sims3ContentHash(h, (uint32_t) ib->getId(), ib->sims3Version, di, in.ibSize);
          k.maskHash = maskHash; k.off0 = in.offset0; k.st0 = in.stride0; k.off1 = in.offset1; k.st1 = in.stride1; k.ib32 = in.ib32 ? 1u : 0u; k.declId = h.wallDeclId;
          k.base = BaseVertexIndex; k.start = startIndex; k.prims = primCount; k.lo = ck[0]; k.hi = ck[1]; k.vis = ck[2]; k.kScale = in.params.kScale; k.thr = thr;
          const uint64_t key = sims3cam::fnv1a64(&k, sizeof k);
          sims3cam::WallCutStats st; bool built = false;
          Sims3Hook::WallEntry* e = sims3WallEntry(h, this, key, in, st, built);
          ++h.wallDraws;
          if (built && st.hidden && h.wallHiddenLogged < 8) {
            ++h.wallHiddenLogged; char m[320];
            snprintf(m, sizeof m, "Sims 3 camera hook: wall draw with hidden vertices at frame %u -> VS %016llx PS %016llx: %u of %u triangles dropped (%u with mixed corners, %u hidden corners), %u cut, %u removed; cK = (%.2f, %.2f, %.2f)",
                     h.frames + 1, (unsigned long long) h.vsHash, (unsigned long long) h.psHash, st.hidden, (unsigned) primCount, st.hiddenMixed, st.hiddenCorners, st.cut, st.removed, ck[0], ck[1], ck[2]);
            Logger::info(m);
          }
          if (!e || !e->changed) return false;
          ++h.wallCutDraws;
          // the game's buffers are held by a reference of the hook's own while its own are bound
          IDirect3DVertexBuffer9* gvb0 = (IDirect3DVertexBuffer9*) vb0; IDirect3DVertexBuffer9* gvb1 = (IDirect3DVertexBuffer9*) vb1; IDirect3DIndexBuffer9* gib = (IDirect3DIndexBuffer9*) ib;
          gvb0->AddRef(); gvb1->AddRef(); gib->AddRef();
          const UINT off0 = m_state.streamOffsets[0], off1 = m_state.streamOffsets[1], st0 = m_state.streamStrides[0], st1 = m_state.streamStrides[1];
          SetStreamSource(0, e->vb0, 0, st0); SetStreamSource(1, e->vb1, 0, st1); SetIndices(e->ib);
          {
            ClientMessage c(Commands::IDirect3DDevice9Ex_DrawIndexedPrimitive, getId());
            currentUID = c.get_uid();
            const INT base0 = 0; const UINT min0 = 0, start0 = 0, nv = e->vertexCount, np = e->triangleCount;
            c.send_many(Type, base0, min0, nv, start0, np);
          }
          SetStreamSource(0, gvb0, off0, st0); SetStreamSource(1, gvb1, off1, st1); SetIndices(gib);
          gvb0->Release(); gvb1->Release(); gib->Release();
          if (built && h.wallLogged < 8) {
            ++h.wallLogged; char m[400];
            snprintf(m, sizeof m, "Sims 3 camera hook: wall openings cut at frame %u -> VS %016llx PS %016llx: %u triangles -> %u (%u cut, %u removed, %u hidden; %u cells, %u with openings, %u rectangles), %u vertices, mask s%d %ux%u%s, up-ness %.2f..%.2f x%.2f, visible from %.2f, threshold %.2f",
                     h.frames + 1, (unsigned long long) h.vsHash, (unsigned long long) h.psHash, (unsigned) primCount, e->triangleCount, st.cut, st.removed, st.hidden, st.cells, st.cellsWithOpenings, st.holeRects, e->vertexCount, s, maskW, maskH, mask ? "" : " (none bound: black)", ck[0], ck[1], in.params.kScale, ck[2], thr);
            Logger::info(m);
          }
          return true;
        };
        wallDone_ = wallDraw_();
      }
      if (!wallDone_ && sims3cam::enabled() && g_sims3.splitDraw && Type == D3DPT_TRIANGLELIST && primCount >= 2) {
        // The Sims 3 camera hook: a lot's re-submission as two half draws (milestone 17r), each its
        // own geometry to the runtime's draw tracker; together they bake the same triangles.
        const UINT half_ = primCount / 2;
        { ClientMessage c(Commands::IDirect3DDevice9Ex_DrawIndexedPrimitive, getId()); currentUID = c.get_uid(); c.send_many(Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, half_); }
        { ClientMessage c(Commands::IDirect3DDevice9Ex_DrawIndexedPrimitive, getId()); currentUID = c.get_uid(); c.send_many(Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex + half_ * 3, primCount - half_); }
        ++g_sims3.splitDraws;
      } else if (!wallDone_) {
        ClientMessage c(Commands::IDirect3DDevice9Ex_DrawIndexedPrimitive, getId());
        currentUID = c.get_uid();
        c.send_many(Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
      }
    }
  }
  if (sims3cam::enabled() && g_sims3.compositeSecond) { SIMS3_END_DRAW(); sims3CompositeSecondPass(g_sims3, this, true, Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount); } else
  SIMS3_END_DRAW();
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("DrawIndexedPrimitive()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
  ZoneScoped;
  LogFunctionCall();
  SIMS3_BEGIN_DRAW();
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_DrawPrimitiveUP, getId());
    currentUID = c.get_uid();
    c.send_many(PrimitiveType, PrimitiveCount);

    uint32_t numIndices = GetIndexCount(PrimitiveType, PrimitiveCount);
    uint32_t vertexDataSize = numIndices * VertexStreamZeroStride;

    c.send_data(vertexDataSize, (void*) pVertexStreamZeroData);
    c.send_data(VertexStreamZeroStride);
  }
  SIMS3_END_DRAW();
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("DrawPrimitiveUP()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinIndex, UINT NumVertices, UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
  ZoneScoped;
  LogFunctionCall();
  SIMS3_BEGIN_DRAW();
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_DrawIndexedPrimitiveUP, getId());
    currentUID = c.get_uid();
    c.send_many(PrimitiveType, MinIndex, NumVertices, PrimitiveCount, IndexDataFormat, VertexStreamZeroStride);

    uint32_t numIndices = GetIndexCount(PrimitiveType, PrimitiveCount);
    uint32_t indexStride = IndexDataFormat == D3DFMT_INDEX16 ? 2 : 4;
    uint32_t indexDataSize = numIndices * indexStride;
    uint32_t vertexDataSize = NumVertices * VertexStreamZeroStride;

    c.send_data(indexDataSize, (void*) pIndexData);
    c.send_data(vertexDataSize, (void*) pVertexStreamZeroData);
  }
  SIMS3_END_DRAW();
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("DrawIndexedPrimitiveUP()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) {
  ZoneScoped;
  LogMissingFunctionCall();

  if (pDestBuffer == nullptr || pVertexDecl == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  auto* const pLssVtxDecl = bridge_cast<Direct3DVertexDeclaration9_LSS*>(pVertexDecl);
  const UID vtxDeclId = (pLssVtxDecl) ? (UID) pLssVtxDecl->getId() : 0;

  auto* const pLssDestBuffer = bridge_cast<Direct3DVertexBuffer9_LSS*>(pDestBuffer);
  const UID destBufferId = (pLssDestBuffer) ? (UID) pLssDestBuffer->getId() : 0;

  // Send command to server and wait for response
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_ProcessVertices, getId());
    currentUID = c.get_uid();
    c.send_many(SrcStartIndex, DestIndex, VertexCount);
    c.send_data(destBufferId);
    c.send_data(vtxDeclId);
    c.send_data(Flags);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("ProcessVertices()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) {
  ZoneScoped;
  LogFunctionCall();
  if (pVertexElements == nullptr || ppDecl == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  UID currentUID = 0;
  {
    auto* const pLssVtxDecl = trackWrapper(new Direct3DVertexDeclaration9_LSS(this, pVertexElements));
    (*ppDecl) = pLssVtxDecl;

    size_t numElem = 1; // We add one so we send the end marker as well
    const auto pStart = pVertexElements;
    const auto* pVtxElemItr = pVertexElements;
    while (pVtxElemItr->Stream != 0xFF) {
      numElem++;
      pVtxElemItr++;
    }

    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_CreateVertexDeclaration, getId());
      currentUID = c.get_uid();
      c.send_data(numElem);
      c.send_data(sizeof(D3DVERTEXELEMENT9) * numElem, (void*) pStart);
      c.send_data((uint32_t) pLssVtxDecl->getId());
    }
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateVertexDeclaration()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) {
  ZoneScoped;
  LogFunctionCall();

  auto* const pLssVtxDecl = bridge_cast<Direct3DVertexDeclaration9_LSS*>(pDecl);
  const UID id = (pLssVtxDecl) ? (UID) pLssVtxDecl->getId() : 0;
  if (sims3cam::enabled()) sims3NoteDecl(g_sims3, pDecl);
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      m_state.vertexDecl = MakeD3DAutoPtr(pLssVtxDecl);
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetVertexDeclaration, getId());
      currentUID = c.get_uid();
      c.send_data(id);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetVertexDeclaration()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) {
  ZoneScoped;
  LogFunctionCall();

  if (ppDecl == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    auto* const pLssVertexDecl = bridge_cast<Direct3DVertexDeclaration9_LSS*>(*m_state.vertexDecl);
    *ppDecl = (IDirect3DVertexDeclaration9*) pLssVertexDecl;
    if ((*ppDecl)) {
      (*ppDecl)->AddRef();
    }
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetFVF(DWORD FVF) {
  ZoneScoped;
  LogFunctionCall();
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      m_FVF = FVF;
      if (sims3cam::enabled()) { g_sims3.declIs3D = sims3cam::fvfIs3D(FVF); g_sims3.wallLayout = sims3cam::WallLayout(); g_sims3.wallDeclId = 0; }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetFVF, getId());
      currentUID = c.get_uid();
      c.send_data(FVF);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetFVF()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetFVF(DWORD* pFVF) {
  ZoneScoped;
  LogFunctionCall();

  if (pFVF == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pFVF = m_FVF;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateVertexShader(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader) {
  ZoneScoped;
  LogFunctionCall();

  if (ppShader == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  if (m_caps.VertexShaderVersion == D3DVS_VERSION(0, 0))
    return D3DERR_INVALIDCALL;

  // The Sims 3 camera hook: shaders are recognised by the hash of the ORIGINAL bytecode.
  // Some get their diffuse texcoord promoted to TEXCOORD0 (see kTexcoordPromotes); the
  // patched copy is what both the wrapper and the runtime receive.
  const DWORD* function = pFunction;
  std::vector<DWORD> sims3Patched;
  const sims3cam::ShaderPatch* sims3ConstPatch = nullptr;
  const bool sims3Ours = g_sims3.creatingVariant;   // a promoted variant of the game's shader, made by the hook: no tables, no dump
  const bool sims3Hooked = sims3cam::enabled() && !sims3Ours;
  const size_t sims3Count = sims3Hooked ? sims3cam::shaderTokenCount(pFunction) : 0;   // 0: no END token, the analyses refuse the stream
  const uint64_t sims3Hash = sims3Count ? sims3cam::fnv1a64(pFunction, sims3Count * sizeof(DWORD)) : 0;
  if (sims3Hooked) {
    sims3ConstPatch = sims3cam::findShaderPatch(sims3Hash);
    if (const sims3cam::TexcoordPromote* promote = sims3cam::findTexcoordPromote(sims3Hash)) {
      const size_t n = sims3Count;
      sims3Patched.assign(pFunction, pFunction + n);
      const uint32_t changed = sims3cam::promoteTexcoord(sims3Patched.data(), n, promote->texcoordIndex);
      char msg[224];
      if (changed) {
        function = sims3Patched.data();
        snprintf(msg, sizeof msg, "Sims 3 camera hook: texcoord %u promoted to TEXCOORD0 (%u tokens) -> %s", (unsigned) promote->texcoordIndex, changed, promote->name);
      } else {
        snprintf(msg, sizeof msg, "Sims 3 camera hook: texcoord promotion matched nothing in the bytecode -> %s (left unpatched)", promote->name);
      }
      Logger::info(msg);
    }
  }

  CommonShader shader(function);
  if (D3DSHADER_VERSION_MAJOR(m_caps.VertexShaderVersion) < shader.getMajorVersion())
    return D3DERR_INVALIDCALL;

  auto* const pLssVertexShader = trackWrapper(new Direct3DVertexShader9_LSS(this, shader));
  (*ppShader) = pLssVertexShader;
  if (sims3Hooked) {
    pLssVertexShader->sims3Patch = sims3ConstPatch;
    pLssVertexShader->sims3Hash = sims3Hash;
    sims3DumpShader("vs", sims3Hash, pFunction, sims3Count);
    if (pLssVertexShader->sims3Patch) {
      char msg[192];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: shader recognised -> %s", pLssVertexShader->sims3Patch->name);
      Logger::info(msg);
    }
    if (const sims3cam::NeverCapture* n = sims3cam::findNeverCapture(pLssVertexShader->sims3Hash)) {
      pLssVertexShader->sims3NeverCapture = sims3cam::neverCaptureMode(n);
      char msg[224];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: %s of this shader are never captured -> %s", n->blendedOnly ? "alpha-blended draws" : "draws", n->name);
      Logger::info(msg);
    } else if (sims3cam::isSkyDomeShader(pFunction, sims3Count)) {
      pLssVertexShader->sims3SkyDome = true;
      char msg[224];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: vertex shader %016llx pins its position's z to w (a sky dome) -> its draws are presented to the runtime as the sky", (unsigned long long) sims3Hash);
      Logger::info(msg);
    }
    // where the world-space normal leaves the shader (milestone 11), from the original bytecode
    sims3cam::analyzeVertexNormal(pFunction, sims3Count, pLssVertexShader->sims3Normal);
    if (pLssVertexShader->sims3Normal.hasNormalInput && g_sims3.normalShaderLogged < 80) {
      ++g_sims3.normalShaderLogged;
      const sims3cam::VsNormalInfo& ni = pLssVertexShader->sims3Normal;
      char list[128] = {}; int ln = 0;
      for (int i = 0; i < 16 && ln < (int) sizeof list - 20; ++i) if (ni.candidates & (1u << i)) ln += snprintf(list + ln, sizeof list - ln, "%sTEXCOORD%d (o%u)", ln ? ", " : "", i, (unsigned) ni.outReg[i]);
      char msg[288];
      if (ni.candidates) snprintf(msg, sizeof msg, "Sims 3 camera hook: vertex shader %016llx (vs_%u_0): world normal output %s", (unsigned long long) pLssVertexShader->sims3Hash, (unsigned) ni.version, list);
      else snprintf(msg, sizeof msg, "Sims 3 camera hook: vertex shader %016llx (vs_%u_0): normal input but no output derived from it alone (input hidden from the capture)", (unsigned long long) pLssVertexShader->sims3Hash, (unsigned) ni.version);
      Logger::info(msg);
    }
    // a wall shader (milestone 13): the clamp of the up-ness flag, for the opening cut
    if (sims3cam::analyzeWallVertexShader(pFunction, sims3Count, pLssVertexShader->sims3Wall)) {
      char msg[224];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: vertex shader %016llx is a wall shader (opening test; up-ness clamp at c%u, scale %.3f)", (unsigned long long) pLssVertexShader->sims3Hash, (unsigned) pLssVertexShader->sims3Wall.clampReg, pLssVertexShader->sims3Wall.kScale);
      Logger::info(msg);
    }
  }

  uint32_t dataSize = 0;
  pLssVertexShader->GetFunction(nullptr, &dataSize);

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_CreateVertexShader, getId());
    currentUID = c.get_uid();
    c.send_data((uint32_t) pLssVertexShader->getId());
    c.send_data(dataSize);
    c.send_data(dataSize, (void*) function);
    
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateVertexShader()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetVertexShader(IDirect3DVertexShader9* pShader) {
  ZoneScoped;
  LogFunctionCall();

  // NULL is an allowed value for pShader
  auto* const pLssVertexShader = bridge_cast<Direct3DVertexShader9_LSS*>(pShader);
  const auto id = (pLssVertexShader) ? (uint32_t) pLssVertexShader->getId() : 0;
  if (sims3cam::enabled() && !g_sims3.swappingVs && !m_stateRecording) sims3NoteVertexShader(g_sims3, pShader);   // our own variant swap around a draw leaves the game's facts in place; a shader set while a state block records does not reach the device
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        m_stateRecording->m_captureState.vertexShader = MakeD3DAutoPtr(pLssVertexShader);
        m_stateRecording->m_dirtyFlags.vertexShader = true;
      } else {
        m_state.vertexShader = MakeD3DAutoPtr(pLssVertexShader);
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetVertexShader, getId());
      currentUID = c.get_uid();
      c.send_data(id);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetVertexShader()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetVertexShader(IDirect3DVertexShader9** ppShader) {
  ZoneScoped;
  LogFunctionCall();

  if (ppShader == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    auto pLssVertexShader = bridge_cast<Direct3DVertexShader9_LSS*>(*m_state.vertexShader);
    (*ppShader) = (IDirect3DVertexShader9*) pLssVertexShader;
    if ((*ppShader)) {
      (*ppShader)->AddRef();
    }
  }
  return S_OK;
}

using ShaderType = BaseDirect3DDevice9Ex_LSS::ShaderConstants::ShaderType;
using ConstantType = BaseDirect3DDevice9Ex_LSS::ShaderConstants::ConstantType;

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetVertexShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  // The Sims 3 camera hook: the object's world position (World translation) for the lamp solver.
  // (The hook's own uploads -- a lifted lot re-submission's c0..c6 and their restore, milestone
  // 17p -- are neither tracked nor classified: ourConsts.)
  if (sims3cam::enabled() && !g_sims3.ourConsts && !m_stateRecording && g_sims3.vsWorldReg >= 0 && StartRegister <= (UINT) g_sims3.vsWorldReg &&
      StartRegister + Vector4fCount >= (UINT) g_sims3.vsWorldReg + 3) {
    const float* w = pConstantData + ((UINT) g_sims3.vsWorldReg - StartRegister) * 4;
    g_sims3.objWorld[0] = w[3]; g_sims3.objWorld[1] = w[7]; g_sims3.objWorld[2] = w[11];
    memcpy(g_sims3.objWorldRows, w, sizeof g_sims3.objWorldRows);
    g_sims3.objWorldValid = true;
    g_sims3.objRowsAfterValid = StartRegister + Vector4fCount >= (UINT) g_sims3.vsWorldReg + 5;
    if (g_sims3.objRowsAfterValid) memcpy(g_sims3.objRowsAfter, w + 12, sizeof g_sims3.objRowsAfter);
  }
  // The Sims 3 camera hook: the sun's direction from the shadow-map view-projection rows.
  if (sims3cam::enabled() && !g_sims3.ourConsts && !m_stateRecording && g_sims3.vsShadowReg >= 0 && StartRegister <= (UINT) g_sims3.vsShadowReg &&
      StartRegister + Vector4fCount >= (UINT) g_sims3.vsShadowReg + 4) {
    const float* rows = pConstantData + ((UINT) g_sims3.vsShadowReg - StartRegister) * 4;
    float d[3];
    if (sims3cam::shadowLightDir(rows, d)) {
      g_sims3.shadowDir[0] = d[0]; g_sims3.shadowDir[1] = d[1]; g_sims3.shadowDir[2] = d[2];
      g_sims3.shadowDirValid = true; g_sims3.shadowDirFrame = g_sims3.frames;
    }
  }
  // The Sims 3 camera hook: the World rows c4..c6 as the device holds them, for the lot terrain's
  // per-frame copy key (milestone 16); an upload may cover them partly.
  if (sims3cam::enabled() && !g_sims3.ourConsts && !m_stateRecording) {
    const UINT first = StartRegister > 4 ? StartRegister : 4, last = (StartRegister + Vector4fCount < 7) ? StartRegister + Vector4fCount : 7;
    for (UINT r = first; r < last; ++r) memcpy(g_sims3.rows4to6 + (r - 4) * 4, pConstantData + (r - StartRegister) * 4, 4 * sizeof(float));
  }
  // The Sims 3 camera hook: rewrite the constants that make the bound shader hide geometry
  // (see kShaderPatches in sims3_camera_hook.h), so every vertex reaches Remix as a point.
  float patchedData[256 * 4];
  const float* data = pConstantData;
  if (sims3cam::enabled() && g_sims3.patch && Vector4fCount <= 256 &&
      sims3cam::patchIntersects(g_sims3.patch, StartRegister, Vector4fCount)) {
    memcpy(patchedData, pConstantData, Vector4fCount * 4 * sizeof(float));
    sims3cam::applyPatches(g_sims3.patch, StartRegister, patchedData, Vector4fCount);
    data = patchedData;
    const uint32_t bit = 1u << (uint32_t) (g_sims3.patch - sims3cam::kShaderPatches);
    if (!(g_sims3.loggedPatches & bit)) {
      g_sims3.loggedPatches |= bit;
      char msg[192];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: constants patched in flight -> %s", g_sims3.patch->name);
      Logger::info(msg);
    }
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult =
      setShaderConstants<
      ShaderType::Vertex,
      ConstantType::Float>(
        StartRegister,
        data,
        Vector4fCount);
  }
  if (SUCCEEDED(hresult)) {
    UID currentUID = 0;
    SetShaderConst(SetVertexShaderConstantF,
                   StartRegister,
                   data,
                   Vector4fCount,
                   Vector4fCount * 4 * sizeof(float), currentUID);

    // The Sims 3 camera hook: the game never calls SetTransform, so recover View and
    // Projection from the fused matrix it uploads at c0 and forward them; without this
    // the Remix runtime finds no camera and captures no geometry. See sims3_camera_hook.h.
    // (Must precede WAIT_FOR_OPTIONAL_SERVER_RESPONSE, which returns unconditionally.)
    if (StartRegister == 0 && Vector4fCount >= 4 && sims3cam::enabled() && !g_sims3.ourConsts && !m_stateRecording) {
      // Maintain the verified main camera; the transforms themselves are applied per draw
      // (sims3ApplyForDraw), where the depth-test state and vertex layout are known.
      auto& h = g_sims3;
      sims3cam::Camera cam;
      const sims3cam::Kind kind = sims3cam::classify(pConstantData, Vector4fCount, cam);
      char msg[256];
      if (kind == sims3cam::Kind::Main) {
        // the play camera does not move within a frame: a main camera unlike the frame's first one
        // (lens or position) is not adopted (run 70: the once-per-frame origin camera at a horizon tilt)
        ++h.frameMainUploads;
        bool adopt = true;
        if (!h.frameCamSet) { h.frameCam = cam; h.frameCamSet = true; }
        else {
          const sims3cam::Camera& f = h.frameCam;
          const float dp[3] = { cam.pos[0] - f.pos[0], cam.pos[1] - f.pos[1], cam.pos[2] - f.pos[2] };
          // the same camera re-derived from another object's fused matrix agrees to ~1e-6 relative;
          // anything beyond is another lens (near / far plane), position or direction
          const bool differs = std::fabs(cam.fovY - f.fovY) > 1e-4f || std::fabs(cam.nearZ - f.nearZ) > 1e-3f * f.nearZ || std::fabs(cam.aspect - f.aspect) > 1e-3f
                            || std::fabs(cam.proj._33 - f.proj._33) > 2e-6f || std::fabs(cam.proj._43 - f.proj._43) > 1e-4f * std::fabs(f.proj._43) + 1e-5f
                            || sims3cam::len3(dp) > 0.05f || sims3cam::dot3(cam.fwd, f.fwd) < 0.99999f;
          if (differs) {
            ++h.frameAltUploads; adopt = false;
            uint32_t k = 0;
            for (; k < h.frameAltCount; ++k) {
              const Sims3Hook::AltCam& a = h.frameAlts[k];
              const float d[3] = { cam.pos[0] - a.pos[0], cam.pos[1] - a.pos[1], cam.pos[2] - a.pos[2] };
              if (std::fabs(cam.fovY - a.fovY) < 1e-4f && std::fabs(cam.nearZ - a.nearZ) < 1e-3f * a.nearZ && std::fabs(cam.proj._33 - a.p33) < 2e-6f && sims3cam::len3(d) < 0.05f && std::fabs(cam.fwd[1] - a.fwdY) < 1e-3f) break;
            }
            if (k < h.frameAltCount) ++h.frameAlts[k].count;
            else if (h.frameAltCount < 4) {
              Sims3Hook::AltCam& a = h.frameAlts[h.frameAltCount++];
              const float pa = cam.proj._33, pb = cam.proj._43;
              a.fovY = cam.fovY; a.nearZ = cam.nearZ; a.farZ = (std::fabs(pa + 1.f) > 1e-6f) ? pa * cam.nearZ / (pa + 1.f) : 0.f; a.aspect = cam.aspect; a.p33 = pa; a.p43 = pb;
              a.pos[0] = cam.pos[0]; a.pos[1] = cam.pos[1]; a.pos[2] = cam.pos[2]; a.fwdY = cam.fwd[1]; a.vs = h.vsHash; a.count = 1;
              a.first = (h.frameMainUploads == 2);   // the odd one came right after the frame's first
            }
          }
        }
        if (adopt) { h.cam = cam; h.cameraValid = true; h.camMirrored = false; ++h.fCamAdopt; }
        else ++h.altUploadsTotal;
        { char t[80]; snprintf(t, sizeof t, "CAM %s d%u eye %.0f,%.0f,%.0f fwd.y %.2f %08x", adopt ? "main" : "alt", h.frameDraws, cam.pos[0], cam.pos[1], cam.pos[2], cam.fwd[1], (unsigned) (h.vsHash >> 32)); sims3RingPush(h, t); if (!adopt) ++h.fCamAlt; }
        if (!h.loggedMain) {
          h.loggedMain = true;
          snprintf(msg, sizeof msg, "Sims 3 camera hook: main camera verified (fovY=%.1f deg, aspect=%.3f, near=%.4f, P33=%.7f P43=%.5f, eye=%.1f,%.1f,%.1f)",
                   cam.fovY * 57.2958f, cam.aspect, cam.nearZ, cam.proj._33, cam.proj._43, cam.pos[0], cam.pos[1], cam.pos[2]);
          Logger::info(msg);
        }
      } else if (kind == sims3cam::Kind::OtherCamera) {
        h.cameraValid = false; h.camMirrored = true;   // a reflection pass: its 3D draws are dropped
        ++h.fCamMirror;
        { char t[80]; snprintf(t, sizeof t, "CAM mirror d%u eye %.0f,%.0f,%.0f fwd.y %.2f %08x", h.frameDraws, cam.pos[0], cam.pos[1], cam.pos[2], cam.fwd[1], (unsigned) (h.vsHash >> 32)); sims3RingPush(h, t); }
        ++h.mirroredUploads;
        if (!h.loggedOther) {
          h.loggedOther = true;
          snprintf(msg, sizeof msg, "Sims 3 camera hook: reflection camera (mirrored basis; eye=%.1f,%.1f,%.1f, fwd.y=%.2f) at frame %u -> its draws are dropped", cam.pos[0], cam.pos[1], cam.pos[2], cam.fwd[1], h.frames + 1);
          Logger::info(msg);
        }
        if (cam.fwd[1] < -0.05f && !h.loggedMirrorCam) {
          h.loggedMirrorCam = true;
          snprintf(msg, sizeof msg, "Sims 3 camera hook: reflection camera looking down at frame %u (a wall mirror's pass; eye=%.1f,%.1f,%.1f, fwd=%.2f,%.2f,%.2f) -> its draws are dropped", h.frames + 1, cam.pos[0], cam.pos[1], cam.pos[2], cam.fwd[0], cam.fwd[1], cam.fwd[2]);
          Logger::info(msg);
        }
      }
    }

    WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetVertexShaderConstantF()", D3DERR_INVALIDCALL, currentUID);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = getShaderConstants<
      ShaderType::Vertex,
      ConstantType::Float>(
        StartRegister,
        pConstantData,
        Vector4fCount);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetVertexShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = setShaderConstants<
      ShaderType::Vertex,
      ConstantType::Int>(
        StartRegister,
        pConstantData,
        Vector4iCount);
  }
  if (SUCCEEDED(hresult)) {
    UID currentUID = 0;
    SetShaderConst(SetVertexShaderConstantI,
                   StartRegister,
                   pConstantData,
                   Vector4iCount,
                   Vector4iCount * 4 * sizeof(UINT), currentUID);
    WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetVertexShaderConstantI()", D3DERR_INVALIDCALL, currentUID);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = getShaderConstants<
      ShaderType::Vertex,
      ConstantType::Int>(
        StartRegister,
        pConstantData,
        Vector4iCount);
  }

  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetVertexShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT  BoolCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = setShaderConstants<
      ShaderType::Vertex,
      ConstantType::Bool>(
        StartRegister,
        pConstantData,
        BoolCount);
  }

  if (SUCCEEDED(hresult)) {
    UID currentUID = 0;
    SetShaderConst(SetVertexShaderConstantB,
                   StartRegister,
                   pConstantData,
                   BoolCount,
                   BoolCount * sizeof(BOOL), currentUID);
    WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetVertexShaderConstantB()", D3DERR_INVALIDCALL, currentUID);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = getShaderConstants<
      ShaderType::Vertex,
      ConstantType::Bool>(
        StartRegister,
        pConstantData,
        BoolCount);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) {
  ZoneScoped;
  LogFunctionCall();
  auto* const pLssStreamData = bridge_cast<Direct3DVertexBuffer9_LSS*>(pStreamData);
  const UID id = (pStreamData) ? (UID) pLssStreamData->getId() : 0;
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        m_stateRecording->m_captureState.streams[StreamNumber] = MakeD3DAutoPtr(pLssStreamData);
        if (pStreamData != nullptr) {
          m_stateRecording->m_captureState.streamOffsets[StreamNumber] = OffsetInBytes;
          m_stateRecording->m_captureState.streamStrides[StreamNumber] = Stride;
          m_stateRecording->m_dirtyFlags.streamOffsetsAndStrides[StreamNumber] = true;
        }
        m_stateRecording->m_dirtyFlags.streams[StreamNumber] = true;
      } else {
        m_state.streams[StreamNumber] = MakeD3DAutoPtr(pLssStreamData);
        if (pStreamData != nullptr) {
          m_state.streamOffsets[StreamNumber] = OffsetInBytes;
          m_state.streamStrides[StreamNumber] = Stride;
        }
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetStreamSource, getId());
      currentUID = c.get_uid();
      c.send_many(StreamNumber, id, OffsetInBytes, Stride);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetStreamSource()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* pOffsetInBytes, UINT* pStride) {
  ZoneScoped;
  LogFunctionCall();

  if (ppStreamData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    auto* pLssVertexBuffer = bridge_cast<Direct3DVertexBuffer9_LSS*>(*m_state.streams[StreamNumber]);
    (*ppStreamData) = (IDirect3DVertexBuffer9*) pLssVertexBuffer;
    (*pOffsetInBytes) = m_state.streamOffsets[StreamNumber];
    (*pStride) = m_state.streamStrides[StreamNumber];
    if ((*ppStreamData)) {
      (*ppStreamData)->AddRef();
    }
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetStreamSourceFreq(UINT StreamNumber, UINT Divider) {
  ZoneScoped;
  LogFunctionCall();
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        m_stateRecording->m_captureState.streamFreqs[StreamNumber] = Divider;
        m_stateRecording->m_dirtyFlags.streamFreqs[StreamNumber] = true;
      } else {
        m_state.streamFreqs[StreamNumber] = Divider;
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetStreamSourceFreq, getId());
      currentUID = c.get_uid();
      c.send_many(StreamNumber, Divider);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetStreamSourceFreq()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetStreamSourceFreq(UINT StreamNumber, UINT* Divider) {
  ZoneScoped;
  LogFunctionCall();
  if (Divider == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  {
    BRIDGE_DEVICE_LOCKGUARD();
    *Divider = m_state.streamFreqs[StreamNumber];
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetIndices(IDirect3DIndexBuffer9* pIndexData) {
  ZoneScoped;
  LogFunctionCall();
  auto* const pLssIndexData = bridge_cast<Direct3DIndexBuffer9_LSS*>(pIndexData);
  const UID id = (pLssIndexData) ? (UID) pLssIndexData->getId() : 0;
  UID currentUID = 0;
  {
    {
      BRIDGE_DEVICE_LOCKGUARD();
      if (m_stateRecording) {
        m_stateRecording->m_captureState.indices = MakeD3DAutoPtr(pLssIndexData);
        m_stateRecording->m_dirtyFlags.indices = true;
      } else {
        m_state.indices = MakeD3DAutoPtr(pLssIndexData);
      }
    }
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_SetIndices, getId());
      currentUID = c.get_uid();
      c.send_data(id);
    }
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetIndices()", D3DERR_INVALIDCALL, currentUID);
} 

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetIndices(IDirect3DIndexBuffer9** ppIndexData) {
  ZoneScoped;
  LogFunctionCall();

  if (ppIndexData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    auto* pLssIndexBuffer = bridge_cast<Direct3DIndexBuffer9_LSS*>(*m_state.indices);
    (*ppIndexData) = (IDirect3DIndexBuffer9*) pLssIndexBuffer;
    if ((*ppIndexData)) {
      (*ppIndexData)->AddRef();
    }
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader) {
  ZoneScoped;
  LogFunctionCall();
 
  if(ppShader == NULL)
    return D3DERR_INVALIDCALL;

  (*ppShader) = NULL;

  if (ppShader == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  if (m_caps.PixelShaderVersion == D3DPS_VERSION(0, 0))
    return D3DERR_INVALIDCALL;

  CommonShader shader(pFunction);
  if (D3DSHADER_VERSION_MAJOR(m_caps.PixelShaderVersion) < shader.getMajorVersion())
    return D3DERR_INVALIDCALL;

  auto* const pLssPixelShader = trackWrapper(new Direct3DPixelShader9_LSS(this, shader));
  (*ppShader) = pLssPixelShader;
  if (sims3cam::enabled() && !g_sims3.creatingVariant) {   // a terrain variant made by the hook: no tables, no dump
    const size_t count = sims3cam::shaderTokenCount(pFunction);   // 0: no END token, the analysis refuses the stream
    const uint64_t hash = count ? sims3cam::fnv1a64(pFunction, count * sizeof(DWORD)) : 0;
    pLssPixelShader->sims3Hash = hash;
    sims3DumpShader("ps", hash, pFunction, count);
    if (const sims3cam::AlbedoStage* a = sims3cam::findAlbedoStage(hash)) {
      pLssPixelShader->sims3AlbedoStage = a->stage;
      pLssPixelShader->sims3LightRig = a->rig ? a : nullptr;
      pLssPixelShader->sims3TintReg = a->tint ? sims3cam::kTintRegister : -1;
      char msg[224];
      snprintf(msg, sizeof msg, "Sims 3 camera hook: albedo is texture stage %u for %s%s%s", (unsigned) a->stage, a->name, a->rig ? "; carries the light rig" : "", a->tint ? "; tint constant c8 forwarded as texture factor" : "");
      Logger::info(msg);
    }
    // what the bytecode says about its samplers: the albedo choice for untabled shaders, the
    // coordinate for tabled ones drawn with an untabled vertex shader, the wall opening mask
    sims3cam::analyzePixelShader(pFunction, count, pLssPixelShader->sims3Auto);
  }

  uint32_t dataSize = 0;
  pLssPixelShader->GetFunction(nullptr, &dataSize);

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_CreatePixelShader, getId());
    currentUID = c.get_uid();
    c.send_data((uint32_t) pLssPixelShader->getId());
    c.send_data(dataSize);
    c.send_data(dataSize, (void*) pFunction);
  }
  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreatePixelShader()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetPixelShader(IDirect3DPixelShader9* pShader) {
  ZoneScoped;
  LogFunctionCall();
  Direct3DPixelShader9_LSS* pLssPixelShader = bridge_cast<Direct3DPixelShader9_LSS*>(pShader);
  const auto id = (pLssPixelShader) ? (uint32_t) pLssPixelShader->getId() : 0;
  if (sims3cam::enabled() && !g_sims3.swappingPs && !m_stateRecording) sims3NotePixelShader(g_sims3, pShader);   // our own variant swap around a terrain draw leaves the game's facts in place; not while a state block records
  {
    BRIDGE_DEVICE_LOCKGUARD();
    if (m_stateRecording) {
      m_stateRecording->m_captureState.pixelShader = MakeD3DAutoPtr(pLssPixelShader);
      m_stateRecording->m_dirtyFlags.pixelShader = true;
    } else {
      m_state.pixelShader = MakeD3DAutoPtr(pLssPixelShader);
    }
  }
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_SetPixelShader, getId());
    currentUID = c.get_uid();
    c.send_data(id);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetPixelShader()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetPixelShader(IDirect3DPixelShader9** ppShader) {
  ZoneScoped;
  LogFunctionCall();

  if (ppShader == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    auto pLssPixelShader = bridge_cast<Direct3DPixelShader9_LSS*>(*m_state.pixelShader);
    (*ppShader) = (IDirect3DPixelShader9*) pLssPixelShader;
    if ((*ppShader)) {
      (*ppShader)->AddRef();
    }
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetPixelShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  // The Sims 3 camera hook: a light-rig upload (c0..c3 directions, c4..c7 colours) casts a
  // vote for this frame's sun; Present forwards the winner as a fixed-function light.
  if (sims3cam::enabled() && g_sims3.psRig != nullptr && StartRegister == 0 && Vector4fCount >= 8) {
    g_sims3.voter.add(pConstantData, pConstantData + 16);
    memcpy(g_sims3.rig, pConstantData, sizeof g_sims3.rig);   // kept for the lamp solver at draw time
    g_sims3.rigValid = true;
  }
  if (sims3cam::enabled() && g_sims3.psRig != nullptr && StartRegister < 16) {   // c0..c15 as uploaded (milestone 24 diagnostic)
    const UINT last = StartRegister + Vector4fCount < 16 ? StartRegister + Vector4fCount : 16;
    for (UINT r = StartRegister; r < last; ++r) { memcpy(g_sims3.psConst + r * 4, pConstantData + (r - StartRegister) * 4, 16); g_sims3.psConstMask |= (uint16_t) (1u << r); }
  }
  // ...and the Create-A-Style tint constant of the bound pixel shader.
  if (sims3cam::enabled() && g_sims3.psTintReg >= 0 && (UINT) g_sims3.psTintReg >= StartRegister && (UINT) g_sims3.psTintReg < StartRegister + Vector4fCount) {
    const float* t = pConstantData + ((UINT) g_sims3.psTintReg - StartRegister) * 4;
    g_sims3.tint[0] = t[0]; g_sims3.tint[1] = t[1]; g_sims3.tint[2] = t[2];
  }
  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = setShaderConstants<ShaderType::Pixel, ConstantType::Float>(StartRegister, pConstantData, Vector4fCount);
  }

  if (SUCCEEDED(hresult)) {
    UID currentUID = 0;
    SetShaderConst(SetPixelShaderConstantF,
                   StartRegister,
                   pConstantData,
                   Vector4fCount,
                   Vector4fCount * 4 * sizeof(float), currentUID);
    WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetPixelShaderConstantF()", D3DERR_INVALIDCALL, currentUID);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = getShaderConstants<ShaderType::Pixel, ConstantType::Float>(StartRegister, pConstantData, Vector4fCount);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetPixelShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = setShaderConstants<ShaderType::Pixel, ConstantType::Int>(StartRegister, pConstantData, Vector4iCount);
  }
  if (SUCCEEDED(hresult)) {
    UID currentUID = 0;
    SetShaderConst(SetPixelShaderConstantI,
                   StartRegister,
                   pConstantData,
                   Vector4iCount,
                   Vector4iCount * 4 * sizeof(UINT), currentUID);
    WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetPixelShaderConstantI()", D3DERR_INVALIDCALL, currentUID);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) {
  LogFunctionCall();

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = getShaderConstants<ShaderType::Pixel, ConstantType::Int>(StartRegister, pConstantData, Vector4iCount);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetPixelShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT  BoolCount) {
  ZoneScoped;
  LogFunctionCall();

  if (pConstantData == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = setShaderConstants<ShaderType::Pixel, ConstantType::Bool>(StartRegister, pConstantData, BoolCount);
  }
  if (SUCCEEDED(hresult)) {
    UID currentUID = 0;
    SetShaderConst(SetPixelShaderConstantB,
                   StartRegister,
                   pConstantData,
                   BoolCount,
                   BoolCount * sizeof(BOOL), currentUID);
    WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetPixelShaderConstantB()", D3DERR_INVALIDCALL, currentUID);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) {
  ZoneScoped;
  LogFunctionCall();

  HRESULT hresult = D3DERR_INVALIDCALL;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    hresult = getShaderConstants<ShaderType::Pixel, ConstantType::Bool>(StartRegister, pConstantData, BoolCount);
  }
  return hresult;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::DrawRectPatch(UINT Handle, CONST float* pNumSegs, CONST D3DRECTPATCH_INFO* pRectPatchInfo) {
  ZoneScoped;
  LogMissingFunctionCall();
  return D3D_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::DrawTriPatch(UINT Handle, CONST float* pNumSegs, CONST D3DTRIPATCH_INFO* pTriPatchInfo) {
  ZoneScoped;
  LogMissingFunctionCall();
  return D3D_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::DeletePatch(UINT Handle) {
  ZoneScoped;
  LogMissingFunctionCall();
  return D3D_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) {
  ZoneScoped;
  LogFunctionCall();
  // MSDN: This parameter can be set to NULL to see if a query is supported. 
  if (nullptr == ppQuery) {
    return S_OK;
  }

  auto* const pLssQuery = trackWrapper(new Direct3DQuery9_LSS(this, Type));
  (*ppQuery) = pLssQuery;

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_CreateQuery, getId());
    currentUID = c.get_uid();
    c.send_many(Type, (uint32_t) pLssQuery->getId());
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetConvolutionMonoKernel(UINT width, UINT height, float* rows, float* columns) {
  ZoneScoped;
  LogFunctionCall();

  UID currentUID = 0;
  // Send command to server and wait for response
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_SetConvolutionMonoKernel, getId());
    currentUID = c.get_uid();
    c.send_data(width);
    c.send_data(height);
    c.send_data(sizeof(float) * width, (void*) rows);
    c.send_data(sizeof(float) * height, (void*) columns);
  }
  WAIT_FOR_OPTIONAL_SERVER_RESPONSE("SetConvolutionMonoKernel()", E_FAIL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::ComposeRects(IDirect3DSurface9* pSrc, IDirect3DSurface9* pDst, IDirect3DVertexBuffer9* pSrcRectDescs, UINT NumRects, IDirect3DVertexBuffer9* pDstRectDescs, D3DCOMPOSERECTSOP Operation, int Xoffset, int Yoffset) {
  ZoneScoped;
  LogFunctionCall();

  // Send command to server and wait for response
  {
    const auto pLssSourceSurface = bridge_cast<Direct3DSurface9_LSS*>(pSrc);
    const auto pLssDestinationSurface = bridge_cast<Direct3DSurface9_LSS*>(pDst);

    auto* const pLssSrcRect = bridge_cast<Direct3DVertexBuffer9_LSS*>(pSrcRectDescs);
    const UID idSrcRect = (pSrcRectDescs) ? (UID) pLssSrcRect->getId() : 0;

    auto* const pLssDestRect = bridge_cast<Direct3DVertexBuffer9_LSS*>(pDstRectDescs);
    const UID idDestRect = (pDstRectDescs) ? (UID) pLssDestRect->getId() : 0;

    if (pLssSourceSurface && pLssDestinationSurface) {
      UID currentUID = 0;
      {
        ClientMessage c(Commands::IDirect3DDevice9Ex_ComposeRects, getId());
        currentUID = c.get_uid();
        c.send_many(pLssSourceSurface->getId(), pLssDestinationSurface->getId(), idSrcRect, idDestRect, NumRects, Operation, Xoffset, Yoffset);
      }
      WAIT_FOR_OPTIONAL_SERVER_RESPONSE("ComposeRects()", D3DERR_INVALIDCALL, currentUID);
    }
  }
  return D3DERR_INVALIDCALL;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::PresentEx(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion, DWORD dwFlags) {
  ZoneScoped;
  LogMissingFunctionCall();
  assert(m_ex);

  // If the bridge was disabled in the meantime for some reason we want to bail
  // out here so we don't spend time waiting on the Present semaphore or trying
  // to send keyboard state to the server.
  if (!gbBridgeRunning) {
    return D3D_OK;
  }

  return m_pSwapchain->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetGPUThreadPriority(INT* pPriority) {
  ZoneScoped;
  LogFunctionCall();

  // Function does not claim to support returning D3DERR_INVALIDCALL
  if (pPriority == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pPriority = m_gpuThreadPriority;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetGPUThreadPriority(INT Priority) {
  ZoneScoped;
  LogFunctionCall();
  {
    BRIDGE_DEVICE_LOCKGUARD();
    m_gpuThreadPriority = Priority;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::WaitForVBlank(UINT iSwapChain) {
  ZoneScoped;
  LogMissingFunctionCall();

  // This API always returns D3D_OK
  return D3D_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CheckResourceResidency(IDirect3DResource9** pResourceArray, UINT32 NumResources) {
  ZoneScoped;
  LogMissingFunctionCall();

  return D3D_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::SetMaximumFrameLatency(UINT MaxLatency) {
  ZoneScoped;
  LogFunctionCall();
  {
    BRIDGE_DEVICE_LOCKGUARD();
    m_maxFrameLatency = MaxLatency;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetMaximumFrameLatency(UINT* pMaxLatency) {
  ZoneScoped;
  LogFunctionCall();

  if (pMaxLatency == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  {
    BRIDGE_DEVICE_LOCKGUARD();
    *pMaxLatency = m_maxFrameLatency;
  }
  return S_OK;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CheckDeviceState(HWND hDestinationWindow) {
  ZoneScoped;
  LogFunctionCall();

  UID currentUID = 0;
  // Send command to server and wait for response
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_CheckDeviceState, getId());
    currentUID = c.get_uid();
    c.send_data((uint32_t) hDestinationWindow);
  }
  WAIT_FOR_SERVER_RESPONSE("CheckDeviceState()", E_FAIL, currentUID);
  HRESULT res = (HRESULT) DeviceBridge::get_data();
  DeviceBridge::pop_front();

  return res;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateRenderTargetEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage) {
  ZoneScoped;
  assert(m_ex);
  LogFunctionCall();

  if (ppSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  D3DSURFACE_DESC desc;
  desc.Width = Width;
  desc.Height = Height;
  desc.Format = Format;
  desc.MultiSampleType = MultiSample;
  desc.MultiSampleQuality = MultisampleQuality;
  desc.Usage = D3DUSAGE_RENDERTARGET;
  desc.Pool = D3DPOOL_DEFAULT;
  desc.Type = D3DRTYPE_SURFACE;

  // Insert our own IDirect3DSurface9 interface implementation
  Direct3DSurface9_LSS* pLssSurface = trackWrapper(new Direct3DSurface9_LSS(this, desc));
  (*ppSurface) = (IDirect3DSurface9*) pLssSurface;

  UID currentUID = 0;
  {
    // Add a handle for the surface
    ClientMessage c(Commands::IDirect3DDevice9Ex_CreateRenderTargetEx, getId());
    currentUID = c.get_uid();
    c.send_many(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, Usage, pLssSurface->getId());
  }

  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateRenderTargetEx()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateOffscreenPlainSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage) {
  ZoneScoped;
  assert(m_ex);
  LogFunctionCall();

  if (ppSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  D3DSURFACE_DESC desc;
  desc.Width = Width;
  desc.Height = Height;
  desc.Format = Format;
  desc.MultiSampleType = D3DMULTISAMPLE_NONE;
  desc.MultiSampleQuality = 0;
  desc.Usage = D3DUSAGE_RENDERTARGET;
  desc.Pool = Pool;
  desc.Type = D3DRTYPE_SURFACE;

  // Insert our own IDirect3DSurface9 interface implementation
  Direct3DSurface9_LSS* pLssSurface = trackWrapper(new Direct3DSurface9_LSS(this, desc));
  (*ppSurface) = (IDirect3DSurface9*) pLssSurface;

  UID currentUID = 0;
  {
    // Add a handle for the surface
    ClientMessage c(Commands::IDirect3DDevice9Ex_CreateOffscreenPlainSurfaceEx, getId());
    currentUID = c.get_uid();
    c.send_many(Width, Height, Format, Pool, Usage, pLssSurface->getId());
  }

  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateOffscreenPlainSurfaceEx()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::CreateDepthStencilSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage) {
  ZoneScoped;
  assert(m_ex);
  LogFunctionCall();

  if (ppSurface == nullptr) {
    return D3DERR_INVALIDCALL;
  }

  D3DSURFACE_DESC desc;
  desc.Width = Width;
  desc.Height = Height;
  desc.Format = Format;
  desc.MultiSampleType = MultiSample;
  desc.MultiSampleQuality = MultisampleQuality;
  desc.Usage = D3DUSAGE_DEPTHSTENCIL;
  desc.Pool = D3DPOOL_DEFAULT;
  desc.Type = D3DRTYPE_SURFACE;

  Direct3DSurface9_LSS* pLssSurface = trackWrapper(new Direct3DSurface9_LSS(this, desc));
  (*ppSurface) = (IDirect3DSurface9*) pLssSurface;

  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_CreateDepthStencilSurfaceEx, getId());
    currentUID = c.get_uid();
    c.send_many(Width, Height, Format, MultiSample, MultisampleQuality, Discard, Usage, pLssSurface->getId());
  }

  WAIT_FOR_OPTIONAL_CREATE_FUNCTION_SERVER_RESPONSE("CreateDepthStencilSurfaceEx()", D3DERR_INVALIDCALL, currentUID);
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::ResetEx(D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode) {
  ZoneScoped;
  assert(m_ex);
  LogFunctionCall();
  HRESULT res = S_OK;
  {
    BRIDGE_DEVICE_LOCKGUARD();
    // Clear all device state and release implicit/internal objects
    releaseInternalObjects(false);
    
    const auto presParam = Direct3DSwapChain9_LSS::sanitizePresentationParameters(*pPresentationParameters, getCreateParams());
    m_presParams = presParam;
    WndProc::unset();
    WndProc::set(getWinProcHwnd());
    // Tell Server to do the Reset
    size_t currentUID = 0;
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_ResetEx, getId());
      currentUID = c.get_uid();
      c.send_data(sizeof(D3DPRESENT_PARAMETERS), &presParam);
      c.send_data(sizeof(D3DDISPLAYMODEEX), pFullscreenDisplayMode);
    }

    // Perform an WAIT_FOR_OPTIONAL_SERVER_RESPONSE but don't return since we still have work to do.
    if (GlobalOptions::getSendAllServerResponses()) {
      const uint32_t timeoutMs = GlobalOptions::getAckTimeout();
      if (Result::Success != DeviceBridge::waitForCommand(Commands::Bridge_Response, timeoutMs, nullptr, true, currentUID)) {
        Logger::err("Direct3DDevice9Ex_LSS::ResetEx() failed with : no response from server.");
      }
      res = (HRESULT) DeviceBridge::get_data();
      DeviceBridge::pop_front();
    }

    // Reset swapchain and link server backbuffer/depth buffer after the server reset its swapchain, or we will link to the old backbuffer/depth resources
    initImplicitObjects(presParam);
    // Keeping a track of previous present parameters, to detect and handle mode changes
    m_previousPresentParams = *pPresentationParameters;
  }
  return res;
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation) {
  ZoneScoped;
  assert(m_ex);
  LogFunctionCall();

  if (pMode == NULL || pRotation == NULL)
    return D3DERR_INVALIDCALL;

  
  UID currentUID = 0;
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_GetDisplayModeEx, getId());
    currentUID = c.get_uid();
    c.send_data(iSwapChain);
  }
  WAIT_FOR_SERVER_RESPONSE("GetDisplayModeEx()", D3DERR_INVALIDCALL, currentUID);

  HRESULT hresult = DeviceBridge::get_data();

  if (SUCCEEDED(hresult)) {
    uint32_t len = DeviceBridge::copy_data(*pMode);
    if (len != sizeof(D3DDISPLAYMODEEX) && len != 0) {
      Logger::err("GetDisplayModeEx() failed getting display mode due to issue with data returned from server.");
      hresult = D3DERR_INVALIDCALL;
    }

    len = DeviceBridge::copy_data(*pRotation);
    if (len != sizeof(D3DDISPLAYROTATION) && len != 0) {
      Logger::err("GetDisplayModeEx() failed getting display rotation due to issue with data returned from server.");
      hresult = D3DERR_INVALIDCALL;
    }
  }
  DeviceBridge::pop_front();
  return hresult;
}

using ShaderType = BaseDirect3DDevice9Ex_LSS::ShaderConstants::ShaderType;
using ConstantType = BaseDirect3DDevice9Ex_LSS::ShaderConstants::ConstantType;
using Vec4f = BaseDirect3DDevice9Ex_LSS::ShaderConstants::Vec4<float>;
using Vec4i = BaseDirect3DDevice9Ex_LSS::ShaderConstants::Vec4<int>;

template <ShaderType   ShaderT,
  ConstantType ConstantT,
  typename     T>
HRESULT BaseDirect3DDevice9Ex_LSS::setShaderConstants(const uint32_t startRegister,
                                                      const T* const pConstantData,
                                                      const uint32_t count) {
  const auto [commonHresult, adjCount] =
    commonGetSetConstants<ShaderT, ConstantT, T>(startRegister, pConstantData, count);
  if (!SUCCEEDED(commonHresult) || adjCount == 0) {
    return commonHresult;
  }

  auto setHelper = [&](auto& set) {
    if constexpr (ConstantT == ConstantType::Float) {
      const size_t size = adjCount * sizeof(Vec4f);
      std::memcpy(set.fConsts[startRegister].data, pConstantData, size);
      if (m_stateRecording) {
        for (int i = 0; i < adjCount; i++) {
          if (ShaderT == ShaderType::Vertex) {
            m_stateRecording->m_dirtyFlags.vertexConstants.fConsts[startRegister + i] = true;
          } else {
            m_stateRecording->m_dirtyFlags.pixelConstants.fConsts[startRegister + i] = true;
          }
        }
      }
    } else if constexpr (ConstantT == ConstantType::Int) {
      const size_t size = adjCount * sizeof(Vec4i);
      std::memcpy(set.iConsts[startRegister].data, pConstantData, size);
      if (m_stateRecording) {
        for (int i = 0; i < adjCount; i++) {
          if (ShaderT == ShaderType::Vertex) {
            m_stateRecording->m_dirtyFlags.vertexConstants.iConsts[startRegister + i] = true;
          } else {
            m_stateRecording->m_dirtyFlags.pixelConstants.iConsts[startRegister + i] = true;
          }
        }
      }
    } else {
      for (uint32_t i = 0; i < adjCount; i++) {
        const uint32_t constantIdx = startRegister + i;
        const uint32_t arrayIdx = constantIdx / 32;
        const uint32_t bitIdx = constantIdx % 32;
        const uint32_t bit = 1u << bitIdx;

        set.bConsts[arrayIdx] &= ~bit;
        if (pConstantData[i]) {
          set.bConsts[arrayIdx] |= bit;
        }
        if (m_stateRecording) {
          if (ShaderT == ShaderType::Vertex) {
            m_stateRecording->m_dirtyFlags.vertexConstants.bConsts[startRegister + i] = true;
          } else {
            m_stateRecording->m_dirtyFlags.pixelConstants.bConsts[startRegister + i] = true;
          }
        }
      }
    }
    return D3D_OK;
  };
  if (m_stateRecording) {
    return ShaderT == ShaderType::Vertex
      ? setHelper(m_stateRecording->m_captureState.vertexConstants)
      : setHelper(m_stateRecording->m_captureState.pixelConstants);
  }
  return ShaderT == ShaderType::Vertex
    ? setHelper(m_state.vertexConstants)
    : setHelper(m_state.pixelConstants);
}

template <ShaderType   ShaderT,
  ConstantType ConstantT,
  typename     T>
HRESULT BaseDirect3DDevice9Ex_LSS::getShaderConstants(const uint32_t startRegister,
                                                            T* const pConstantData,
                                                      const uint32_t count) {
  const auto [commonHresult, adjCount] =
    commonGetSetConstants<ShaderT, ConstantT, T>(startRegister, pConstantData, count);
  if (!SUCCEEDED(commonHresult) || adjCount == 0) {
    return commonHresult;
  }

  auto getHelper = [&](const auto& set) {
    if constexpr (ConstantT == ConstantType::Float) {
      const float* source = set.fConsts[startRegister].data;
      const size_t size = adjCount * sizeof(Vec4f);
      std::memcpy(pConstantData, source, size);
    } else if constexpr (ConstantT == ConstantType::Int) {
      const int* source = set.iConsts[startRegister].data;
      const size_t size = adjCount * sizeof(Vec4i);
      std::memcpy(pConstantData, source, size);
    } else {
      for (uint32_t i = 0; i < adjCount; i++) {
        const uint32_t constantIdx = startRegister + i;
        const uint32_t arrayIdx = constantIdx / 32;
        const uint32_t bitIdx = constantIdx % 32;
        const uint32_t bit = (1u << bitIdx);

        const bool constValue = set.bConsts[arrayIdx] & bit;
        pConstantData[i] = constValue ? TRUE : FALSE;
      }
    }
    return D3D_OK;
  };
  return ShaderT == ShaderType::Vertex
    ? getHelper(m_state.vertexConstants)
    : getHelper(m_state.pixelConstants);
}

template <ShaderType   ShaderT,
  ConstantType ConstantT,
  typename     T>
std::tuple<HRESULT, size_t> BaseDirect3DDevice9Ex_LSS::commonGetSetConstants(const uint32_t startRegister,
                                                                            const T* const pConstantData,
                                                                            const uint32_t count) {
  const     uint32_t regCountHardware = ShaderConstants::getHardwareRegCount<ShaderT, ConstantT>();
  constexpr uint32_t regCountSoftware = ShaderConstants::getSoftwareRegCount<ShaderT, ConstantT>();
  if (startRegister + count > regCountSoftware) {
    return { D3DERR_INVALIDCALL, count };
  }
  const auto adjCount = UINT(
    std::max<INT>(
      std::clamp<INT>(count + startRegister, 0, regCountHardware) - INT(startRegister),
      0));
  if (adjCount == 0) {
    return { D3D_OK, adjCount };
  }
  if (pConstantData == nullptr) {
    return { D3DERR_INVALIDCALL, adjCount };
  }
  return { D3D_OK, adjCount };
}

template<bool EnableSync>
HRESULT Direct3DDevice9Ex_LSS<EnableSync>::ResetState() {
  for (uint32_t stageIdx = 0; stageIdx < kNumStageSamplers; ++stageIdx) {
    // Reset Texture States
    m_state.textureStageStates[stageIdx][TextureStageStateType::ColorOp] = stageIdx == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
    m_state.textureStageStates[stageIdx][TextureStageStateType::ColorArg1] = D3DTA_TEXTURE;
    m_state.textureStageStates[stageIdx][TextureStageStateType::ColorArg2] = D3DTA_CURRENT;
    m_state.textureStageStates[stageIdx][TextureStageStateType::AlphaOp] = stageIdx == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
    // We can't predict the textures setup when do reset (many cases the textures will just be released), so keep D3DTA_TEXTURE as default state
    m_state.textureStageStates[stageIdx][TextureStageStateType::AlphaArg1] = D3DTA_TEXTURE;
    m_state.textureStageStates[stageIdx][TextureStageStateType::AlphaArg2] = D3DTA_CURRENT;
    m_state.textureStageStates[stageIdx][TextureStageStateType::BumpEnvMat00] = bit_cast<DWORD>(0.f);
    m_state.textureStageStates[stageIdx][TextureStageStateType::BumpEnvMat01] = bit_cast<DWORD>(0.f);
    m_state.textureStageStates[stageIdx][TextureStageStateType::BumpEnvMat10] = bit_cast<DWORD>(0.f);
    m_state.textureStageStates[stageIdx][TextureStageStateType::BumpEnvMat11] = bit_cast<DWORD>(0.f);
    m_state.textureStageStates[stageIdx][TextureStageStateType::TexCoordIdx] = stageIdx;
    m_state.textureStageStates[stageIdx][TextureStageStateType::BumpEnvLScale] = bit_cast<DWORD>(0.f);
    m_state.textureStageStates[stageIdx][TextureStageStateType::BumpEnvLOffset] = bit_cast<DWORD>(0.f);
    m_state.textureStageStates[stageIdx][TextureStageStateType::TexXformFlags] = D3DTTFF_DISABLE;
    m_state.textureStageStates[stageIdx][TextureStageStateType::ColorArg0] = D3DTA_CURRENT;
    m_state.textureStageStates[stageIdx][TextureStageStateType::AlphaArg0] = D3DTA_CURRENT;
    m_state.textureStageStates[stageIdx][TextureStageStateType::ResultArg] = D3DTA_CURRENT;
    m_state.textureStageStates[stageIdx][TextureStageStateType::Constant] = 0x00000000;

    // Reset Sampler States
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_ADDRESSU - 1] = D3DTADDRESS_WRAP;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_ADDRESSV - 1] = D3DTADDRESS_WRAP;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_ADDRESSW - 1] = D3DTADDRESS_WRAP;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_BORDERCOLOR - 1] = 0x00000000;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_MAGFILTER - 1] = D3DTEXF_POINT;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_MINFILTER - 1] = D3DTEXF_POINT;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_MIPFILTER - 1] = D3DTEXF_NONE;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_MIPMAPLODBIAS - 1] = 0;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_MAXMIPLEVEL - 1] = 0;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_MAXANISOTROPY - 1] = 1;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_SRGBTEXTURE - 1] = 0;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_ELEMENTINDEX - 1] = 0;
    m_state.samplerStates[stageIdx][_D3DSAMPLERSTATETYPE::D3DSAMP_DMAPOFFSET - 1] = 0;
  }

  // Referencing defaults from: https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3drenderstatetype
  m_state.renderStates[D3DRS_ZENABLE] = m_pSwapchain && m_pSwapchain->getPresentationParameters().EnableAutoDepthStencil;
  m_state.renderStates[D3DRS_FILLMODE] = D3DFILL_SOLID;
  m_state.renderStates[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
  m_state.renderStates[D3DRS_ZWRITEENABLE] = TRUE;
  m_state.renderStates[D3DRS_ALPHATESTENABLE] = FALSE;
  m_state.renderStates[D3DRS_LASTPIXEL] = TRUE;
  m_state.renderStates[D3DRS_SRCBLEND] = D3DBLEND_ONE;
  m_state.renderStates[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
  m_state.renderStates[D3DRS_CULLMODE] = D3DCULL_CCW;
  m_state.renderStates[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
  m_state.renderStates[D3DRS_ALPHAREF] = 0;
  m_state.renderStates[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
  m_state.renderStates[D3DRS_DITHERENABLE] = FALSE;
  m_state.renderStates[D3DRS_ALPHABLENDENABLE] = FALSE;
  m_state.renderStates[D3DRS_FOGENABLE] = FALSE;
  m_state.renderStates[D3DRS_SPECULARENABLE] = FALSE;
  m_state.renderStates[D3DRS_FOGCOLOR] = 0;
  m_state.renderStates[D3DRS_FOGTABLEMODE] = D3DFOG_NONE;
  m_state.renderStates[D3DRS_FOGSTART] = bit_cast<DWORD>(0.f);
  m_state.renderStates[D3DRS_FOGEND] = bit_cast<DWORD>(1.f);
  m_state.renderStates[D3DRS_FOGDENSITY] = bit_cast<DWORD>(1.f);
  m_state.renderStates[D3DRS_RANGEFOGENABLE] = FALSE;
  m_state.renderStates[D3DRS_STENCILENABLE] = FALSE;
  m_state.renderStates[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
  m_state.renderStates[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
  m_state.renderStates[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
  m_state.renderStates[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  m_state.renderStates[D3DRS_STENCILREF] = 0;
  m_state.renderStates[D3DRS_STENCILMASK] = 0xFFFFffff;
  m_state.renderStates[D3DRS_STENCILWRITEMASK] = 0xFFFFffff;
  m_state.renderStates[D3DRS_TEXTUREFACTOR] = 0xFFFFffff;
  for(uint32_t i=0 ; i<8 ; i++)
    m_state.renderStates[D3DRS_WRAP0+i] = 0;
  m_state.renderStates[D3DRS_CLIPPING] = TRUE;
  m_state.renderStates[D3DRS_LIGHTING] = TRUE;
  m_state.renderStates[D3DRS_AMBIENT] = 0;
  m_state.renderStates[D3DRS_FOGVERTEXMODE] = D3DFOG_NONE;
  m_state.renderStates[D3DRS_COLORVERTEX] = TRUE;
  m_state.renderStates[D3DRS_LOCALVIEWER] = TRUE;
  m_state.renderStates[D3DRS_NORMALIZENORMALS] = FALSE;
  m_state.renderStates[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
  m_state.renderStates[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
  m_state.renderStates[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
  m_state.renderStates[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
  m_state.renderStates[D3DRS_VERTEXBLEND] = D3DVBF_DISABLE;
  m_state.renderStates[D3DRS_CLIPPLANEENABLE] = 0;
  m_state.renderStates[D3DRS_POINTSIZE] = bit_cast<DWORD>(1.f);
  m_state.renderStates[D3DRS_POINTSIZE_MIN] = bit_cast<DWORD>(1.f);
  m_state.renderStates[D3DRS_POINTSPRITEENABLE] = FALSE;
  m_state.renderStates[D3DRS_POINTSCALEENABLE] = FALSE;
  m_state.renderStates[D3DRS_POINTSCALE_A] = bit_cast<DWORD>(1.f);
  m_state.renderStates[D3DRS_POINTSCALE_B] = bit_cast<DWORD>(0.f);
  m_state.renderStates[D3DRS_POINTSCALE_C] = bit_cast<DWORD>(0.f);
  m_state.renderStates[D3DRS_MULTISAMPLEANTIALIAS] = TRUE;
  m_state.renderStates[D3DRS_MULTISAMPLEMASK] = 0xFFFFffff;
  m_state.renderStates[D3DRS_PATCHEDGESTYLE] = D3DPATCHEDGE_DISCRETE;
  m_state.renderStates[D3DRS_DEBUGMONITORTOKEN] = D3DDMT_ENABLE;
  m_state.renderStates[D3DRS_POINTSIZE_MAX] = bit_cast<DWORD>(8192.f);
  m_state.renderStates[D3DRS_INDEXEDVERTEXBLENDENABLE] = FALSE;
  m_state.renderStates[D3DRS_COLORWRITEENABLE] = 0x0000000F;
  m_state.renderStates[D3DRS_TWEENFACTOR] = bit_cast<DWORD>(0.f);
  m_state.renderStates[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
  m_state.renderStates[D3DRS_POSITIONDEGREE] = D3DDEGREE_CUBIC;
  m_state.renderStates[D3DRS_NORMALDEGREE] = D3DDEGREE_LINEAR;
  m_state.renderStates[D3DRS_SCISSORTESTENABLE] = FALSE;
  m_state.renderStates[D3DRS_SLOPESCALEDEPTHBIAS] = 0;
  m_state.renderStates[D3DRS_ANTIALIASEDLINEENABLE] = FALSE;
  m_state.renderStates[D3DRS_MINTESSELLATIONLEVEL] = bit_cast<DWORD>(1.f);
  m_state.renderStates[D3DRS_MAXTESSELLATIONLEVEL] = bit_cast<DWORD>(1.f);
  m_state.renderStates[D3DRS_ADAPTIVETESS_X] = bit_cast<DWORD>(0.f);
  m_state.renderStates[D3DRS_ADAPTIVETESS_Y] = bit_cast<DWORD>(0.f);
  m_state.renderStates[D3DRS_ADAPTIVETESS_Z] = bit_cast<DWORD>(1.f);
  m_state.renderStates[D3DRS_ADAPTIVETESS_W] = bit_cast<DWORD>(0.f);
  m_state.renderStates[D3DRS_ENABLEADAPTIVETESSELLATION] = FALSE;
  m_state.renderStates[D3DRS_TWOSIDEDSTENCILMODE] = FALSE;
  m_state.renderStates[D3DRS_CCW_STENCILFAIL] = D3DSTENCILOP_KEEP;
  m_state.renderStates[D3DRS_CCW_STENCILZFAIL] = D3DSTENCILOP_KEEP;
  m_state.renderStates[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
  m_state.renderStates[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
  m_state.renderStates[D3DRS_COLORWRITEENABLE1] = 0x0000000f;
  m_state.renderStates[D3DRS_COLORWRITEENABLE2] = 0x0000000f;
  m_state.renderStates[D3DRS_COLORWRITEENABLE3] = 0x0000000f;
  m_state.renderStates[D3DRS_BLENDFACTOR] = 0xFFFFffff;
  m_state.renderStates[D3DRS_SRGBWRITEENABLE] = 0;
  m_state.renderStates[D3DRS_DEPTHBIAS] = bit_cast<DWORD>(0.f);
  for (uint32_t i = 0; i < 8; i++)
    m_state.renderStates[D3DRS_WRAP8 + i] = 0;
  m_state.renderStates[D3DRS_SEPARATEALPHABLENDENABLE] = FALSE;
  m_state.renderStates[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
  m_state.renderStates[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
  m_state.renderStates[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;


  // Reset Light States
  for (uint32_t i = 0; i < caps::MaxEnabledLights; ++i) {
    m_state.bLightEnables[i] = 0;
  }

  // Reset Stream Frequency
  for (uint32_t i = 0; i < caps::MaxStreams; ++i) {
    m_state.streamFreqs[i] = 1;
  }

  // Set The Current Texture Palette entry to it's default
  // found through experimentation.
  m_curTexPalette = 65535;

  return S_OK;
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::initImplicitObjects(const D3DPRESENT_PARAMETERS& presParam) {
  initImplicitSwapchain(presParam);
  initImplicitRenderTarget();
  if (presParam.EnableAutoDepthStencil) {
    initImplicitDepthStencil();
  }
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::initImplicitSwapchain(const D3DPRESENT_PARAMETERS& presParam) {
  auto* const pLssSwapChain = trackWrapper(new Direct3DSwapChain9_LSS(this, presParam));
  // To have a more consistent display when toggling windowed mode
  if (presParam.Windowed != m_previousPresentParams.Windowed && !(m_createParams.BehaviorFlags & D3DCREATE_NOWINDOWCHANGES)) {
    SetGammaRamp(0, 0, &m_gammaRamp);
  }
  m_pSwapchain = pLssSwapChain;
  m_pSwapchain->reset(presParam);
  {
    gSwapChainMapMutex.lock();
    gSwapChainMap[m_pSwapchain->getPresentationParameters().hDeviceWindow] = { m_pSwapchain->getPresentationParameters(), this->getCreateParams(),  m_pSwapchain->getId()};
    gSwapChainMapMutex.unlock();
  }

  m_implicitRefCnt++;
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::initImplicitRenderTarget() {
  IDirect3DSurface9* pRenderTarget = nullptr;

  // Creating a place-holder surface
  D3DSURFACE_DESC desc;
  desc.Width = GET_PRES_PARAM().BackBufferWidth;
  desc.Height = GET_PRES_PARAM().BackBufferHeight;
  desc.Format = GET_PRES_PARAM().BackBufferFormat;
  desc.MultiSampleType = GET_PRES_PARAM().MultiSampleType;
  desc.MultiSampleQuality = GET_PRES_PARAM().MultiSampleQuality;
  desc.Usage = D3DUSAGE_RENDERTARGET;
  desc.Pool = D3DPOOL_DEFAULT;
  desc.Type = D3DRTYPE_SURFACE;

  // Insert our own IDirect3DSurface9 interface implementation
  Direct3DSurface9_LSS* pLssSurface = trackWrapper(new Direct3DSurface9_LSS(this, desc));
  pRenderTarget = (IDirect3DSurface9*) pLssSurface;

  m_pImplicitRenderTarget = bridge_cast<Direct3DSurface9_LSS*>(pRenderTarget);
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_LinkBackBuffer, getId());
    c.send_many(0, m_pImplicitRenderTarget->getId());
  }
  m_state.renderTargets[0] = MakeD3DAutoPtr(m_pImplicitRenderTarget);
  m_implicitRefCnt++;
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::initImplicitDepthStencil() {
  assert(GET_PRES_PARAM().EnableAutoDepthStencil);
  IDirect3DSurface9* pShadowDepthBuffer = nullptr;

  // Creating a place-holder surface
  D3DSURFACE_DESC desc;
  desc.Width = GET_PRES_PARAM().BackBufferWidth;
  desc.Height = GET_PRES_PARAM().BackBufferHeight;
  desc.Format = GET_PRES_PARAM().AutoDepthStencilFormat;
  desc.MultiSampleType = GET_PRES_PARAM().MultiSampleType;
  desc.MultiSampleQuality = GET_PRES_PARAM().MultiSampleQuality;
  desc.Usage = D3DUSAGE_DEPTHSTENCIL;
  desc.Pool = D3DPOOL_DEFAULT;
  desc.Type = D3DRTYPE_SURFACE;

  Direct3DSurface9_LSS* pLssSurface = trackWrapper(new Direct3DSurface9_LSS(this, desc));
  pShadowDepthBuffer = (IDirect3DSurface9*) pLssSurface;

  m_pImplicitDepthStencil = bridge_cast<Direct3DSurface9_LSS*>(pShadowDepthBuffer);
  {
    ClientMessage c(Commands::IDirect3DDevice9Ex_LinkAutoDepthStencil, getId());
    c.send_data(m_pImplicitDepthStencil->getId());
  }
  m_state.depthStencil = MakeD3DAutoPtr(m_pImplicitDepthStencil);
  m_implicitRefCnt++;
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::destroyImplicitObjects() {
  // Release implicit RenderTarget
  const auto rtRefCnt = m_pImplicitRenderTarget->Release();
  assert(rtRefCnt == 0 && "Implicit RenderTarget has not been released!");
  m_pImplicitRenderTarget = nullptr;
  --m_implicitRefCnt;
  m_state.renderTargets[0].reset(nullptr);

  // Release implicit DepthStencil
  if (GET_PRES_PARAM().EnableAutoDepthStencil) {
    const auto dsRefCnt = m_pImplicitDepthStencil->Release();
    assert(dsRefCnt == 0 && "Implicit DepthStencil has not been released!");
    m_pImplicitDepthStencil = nullptr;
    --m_implicitRefCnt;
    m_state.depthStencil.reset(nullptr);
  }

  const size_t nBackBuf = GET_PRES_PARAM().BackBufferCount;
  for (size_t iBackBuf = 0; iBackBuf < nBackBuf; ++iBackBuf) {
    m_pSwapchain->Release();
  }
  // Release implicit SwapChain, must happen last so PresParam still exist prior
  const auto scRefCnt = m_pSwapchain->Release();
  assert(scRefCnt == 0 && "Implicit Swapchain has not been released!");
  m_pSwapchain = nullptr;
  --m_implicitRefCnt;
}

template<bool EnableSync>
void Direct3DDevice9Ex_LSS<EnableSync>::setupFPU() {
  // Should match d3d9 float behaviour.

  // For MSVC we can use these cross arch and platform funcs to set the FPU.
  // This will work on any platform, x86, x64, ARM, etc.

  // Clear exceptions.
  _clearfp();

  // Disable exceptions
  _controlfp(_MCW_EM, _MCW_EM);

  // Round to nearest
  _controlfp(_RC_NEAR, _MCW_RC);
}

// Always instantiate non-syncable variant
template class Direct3DDevice9Ex_LSS<false>;

#ifdef WITH_MULTITHREADED_DEVICE
// Do not waste code and instantiate syncable variant only when necessary
template class Direct3DDevice9Ex_LSS<true>;
#endif

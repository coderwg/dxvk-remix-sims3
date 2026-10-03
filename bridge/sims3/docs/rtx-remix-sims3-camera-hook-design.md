> **Historical document (kept as written, 2026-09-03 to milestone 18f).** It records how the camera hook
> came about. Parts of the design it describes were replaced later -- the sun from a vote over the objects'
> light rigs, the fixed-function sun and lamps, the lot replay among them. The code's comments and the
> commit history on branch sims3-hook describe the hook as it is.

# Sims 3 + RTX Remix: why ray tracing captures nothing, and what a camera hook would need

**Status (2026-09-03):** the crash is fixed (see `rtx-remix-sims3-update-2.md`); the game
runs at 300+ fps in Remix passthrough. With ray tracing enabled it runs at ~76 fps but
Remix captures **zero geometry** (HUD: `# BLAS 0, Instances 0, Lights 0, TraceRays 0`);
world geometry is intercepted from the raster path and never path-traced, so it is
invisible. Grass, shadow-map blobs, plumbob and UI (all passthrough classes) still show.

## Root cause — proven at every layer

| Layer | Evidence |
|---|---|
| Runtime log | `[RTX-Compatibility-Info] Trying to raytrace but not detecting a valid camera.` |
| Runtime source | `d3d9_rtx.cpp:310-311` — the camera is taken **only** from `d3d9State().transforms[D3DTS_VIEW/PROJECTION]`, even on the programmable-shader path. `rtx_camera_manager.cpp:50` bails if `viewToProjection` is exactly identity. There is no vertex-shader-constant camera analysis. |
| Native apitrace | **0 `SetTransform` calls** in 40,000 in-world calls. Sims 3 never sets `D3DTS_*`. |
| Other inputs | `customWorldToProjection` / `CustomVertexTransformEnabled` is internal-only (a comment and an enum; nothing sets it); no `rtx.conf` option; the Remix C API has no camera setter. |

Vertex capture (on by default for programmable VS) cannot help: it un-projects captured
positions with `inverse(viewToProjection)`, which is meaningless for identity.

**Conclusion:** stock Remix cannot ray-trace The Sims 3. The only path is a
compatibility hook that reconstructs View and Projection from what the game already
uploads and feeds them to `SetTransform` — the same thing gta4-rtx does for GTA IV at
the ASI layer.

## What Sims 3 actually uploads (from the native trace, in-world frame)

- Matrices go via `SetVertexShaderConstantF`. Many shaders, many layouts (block sizes
  3…187 registers; 129/180/187 are skinning bone palettes for Sims).
- **c0–c3 = fused World×View×Projection, per object**, column-vector convention
  (`P·V·W`, translation in the last column). Rows 3 and 4 nearly identical = RH
  perspective (`P[2][2] = f/(n−f) ≈ −1.0002`, `P[3][2] = −1`).
- **World is uploaded separately** near c4 (as 3×4 or 4×4 depending on shader).
- Several cameras per frame: the main isometric view (26° vertical FOV, 16:9), 90° FOV
  x-mirrored shadow/cube cameras, a Y-flipped reflection camera, and an orthographic
  portrait/thumbnail transform (c180).
- Verified exactly: `WVP(call 1277907) · World⁻¹` equals `c0(call 1278332)` (an
  identity-World draw), so conventions and the per-object structure are confirmed.

## Recovering the camera (validated numerically in `vp_decompose.py`)

Given a main-view `VP` (from any identity-World draw, or `WVP · W⁻¹` using the c4 World):

1. Row 4 of `VP` = `−V[2]` (view-space Z row incl. `tz`).
2. `a = VP[2][0] / V[2][0]` → `P[2][2]`; `b = VP[2][3] − a·V[2][3]` → `P[2][3]`;
   `near = b/a`, `far = a·near/(a+1)`.
3. `fx = ‖VP[0][:3]‖`, `fy = ‖VP[1][:3]‖`; `V[0] = VP[0]/fx`, `V[1] = VP[1]/fy`.
4. `P = diag-perspective(fx, fy, a, b)`, `V = [R|t]`. Check `P·V == VP`.

The script reproduces the captured VP to 1e-4 (`P·V == VP`), the rotation is
orthonormal, and the numbers are clean: fx = 2.4142 = 1/tan(22.5°) → exactly 45°
horizontal / 26.2° vertical FOV at 16:9, near = 0.34. The recovered camera position
(1115.1, −10.7, 1020.5) matches a vector the game *separately* uploads as a constant
(its eye position for specular lighting) — two independent derivations agree.

**Detail the hook must respect:** forward the game's depth terms `P[2][2]`, `P[2][3]`
**verbatim**. The main camera's `P[2][2]` is about −1.0003 (a finite far, roughly
4000× near; an earlier "infinite far" reading was a rounding artifact of the 4-decimal
trace data). Substituting any other far plane makes `inverse(View·Projection)` differ
from what the vertex shader applied, and Remix's vertex capture un-projects the
captured clip-space output — the standalone test showed a substituted far shifts
reconstructed depth by ~2e-3 in z/w at 49 units, i.e. a scale error that grows with
distance.

## What the in-game runs taught (2026-09-03)

- **Run 1 (M1):** Remix accepted a camera for the first time (124 instances) but the
  image was garbage. Cause: a projection composed with any *rigid* World is still a
  valid-looking camera, just in object space, so the hook forwarded object-space
  cameras and Remix locked its viewpoint to one. Fix: require the camera position the
  candidate implies to equal a float4 in the same constant block — the game uploads its
  eye position there for specular lighting. Also: the UI uploads a pixel-to-clip scale at
  c0 and the sea-reflection pass renders into the backbuffer; both now reset the
  transforms to identity (= plain rasterization, exactly the pre-hook behaviour).
- **Run 2 (M1b):** UI back, but zero instances. Cause: I had the two cameras
  backwards. The world is **Y-up**; the play camera sits ~40 units above the sea plane
  (y ≈ 28) looking **down** (forward.y < 0); the reflection pass is the same camera
  mirrored across that plane, looking up. The trace's draw counts agree (78 uploads per
  frame carry the down-looking eye, 23 the up-looking one). Fixed in M1c by flipping one
  comparison. The captured calls 1277907/1278332 used throughout this document are
  reflection-pass draws; the math is identical.

- **Run 3 (M1c):** correct camera (verified eye at height 57, looking down; 110
  instances), still unusable. Cause: a correct camera is not enough — every draw that
  never uploads a matrix inherits it. The frame's tail is a bloom chain of two-float
  full-screen quads drawn with depth testing off, composited onto the backbuffer (the
  white sheet; the "UI slab" with glowing blobs is the bloom texture itself), then the
  UI. The runtime cannot tell (programmable-shader UI is only detected via tagged
  textures or pre-transformed vertices). Fix (M1d): decide per *draw* in the client. A
  draw gets the main camera only if depth testing is on and the bound vertex layout
  has a 3-component position (Sims 3 packs 176 of its 209 layouts as SHORT4, a few as
  four bytes; UI and post quads are FLOAT2); otherwise identity. The constant hook now
  only maintains the verified camera.

- **Run 4 (M1d):** identical to run 3, and the log proved the hook did exactly what it
  was built to do (all five events, correct camera, correct per-draw resets). So the
  fault was in an assumption, not the mechanism. Reading the runtime's capture path
  end to end (raw draw transforms, exact row-vector inverse, early perspective divide)
  shows it is exact for every *point* vertex regardless of far plane — and has one
  hole: a vertex with clip `w = 0` reconstructs to `1/0`. The trace has exactly one
  such draw per frame (a `FLOAT4`-position layout, 36 triangles, dynamic buffer), and
  dumping its buffer from the trace confirmed **all its vertices have `w = 0`**:
  infinities and NaNs in the BLAS, triangles through the camera — the fans converging
  on the focal point and the giant sheets of every run. Fix (M1e): homogeneous
  (`FLOAT4`) positions are never captured; the draw rasterizes instead.
- **Missing UI is a separate, expected Remix behaviour:** the runtime injects the
  ray-traced image at the first draw it recognises as UI (only via tagged UI textures
  or pre-transformed vertices for programmable-shader games), otherwise at Present —
  and everything rasterized before that point, the UI included, is overwritten. The
  standard fix is the standard Remix workflow: tag the UI textures (Alt+X → Game Setup
  → UI Texture). The hook cannot do this from the bridge.

- **Run 5 (M1e):** with the UI textures tagged in Remix's menu the UI came back, and
  the "slab" turned out to be the lot itself at night (lit windows on unlit ground) —
  so the scene *is* captured and placed. The fans and sheets remained, and the new log
  line showed a finite far plane, so neither of my two "1/0" theories held: the buffer
  whose vertices have `w = 0` feeds a shader that rebuilds `w = 1` (exclusion reverted).
  **The real cause is the engine's own way of hiding geometry, read from the shaders
  in the trace** (apitrace prints every vertex shader's disassembly): the four wall
  shaders scale their projected x, y, z by a per-vertex visibility factor floored by a
  constant the game uploads as 0 (the *cut-away* view hides wall segments between
  camera and room), and the lot terrain shader moves vertices outside a ±128-unit
  rectangle to a far clip position. The game's rasterizer draws nothing for those
  vertices; Remix's capture reconstructs them onto the focal axis at the near plane —
  the fans converging on the screen centre — and partially scaled ones become the
  sheets. All 30 other main-pass shaders form `w` from a constant (proper points).
  **Fix (M1f):** the client hashes each vertex shader's bytecode at creation and, for
  the five known shaders, rewrites the relevant constants in flight (`c8.z`/`c12.z` = 1
  for walls; `c7` = (0, 0, 0.5, 0.5) for the terrain rectangle). Effect: walls always
  visible (the walls-down lowering is a separate lerp and still works), complete
  terrain chunks. Other situations (roofs, foliage, Sims) may reveal more such shaders;
  the table is meant to grow.

- **Run 6 (M1f): the fans are gone.** The log shows all five shaders recognised at
  start-up and their constants patched when drawn. The lot renders from the correct
  viewpoint — door frames, walls, furniture, plumbob — with the UI on top (the user
  tagged and saved the UI textures). Remaining defect: every upward-facing surface is
  black while sides glow, i.e. a shading/orientation problem, not geometry. A
  handedness flip of the forwarded pair was tried and rejected by a new determinant
  test (the right-handed pair *is* the proper rotation for the play camera; the
  reflection camera is its mirror). Next suspects, testable without a rebuild: Remix's
  captured vertex normals (Sims 3 packs normals as colours and decodes them in-shader,
  so the raw captured ones are garbage — now disabled in `rtx.conf`), then triangle
  winding/culling (live toggle in the Remix menu, Geometry → Enable Triangle Culling).

- **Run 7:** with captured normals off, the user toggled triangle culling off live and
  a large slab appeared over the lot: the **roof**, which the game hides in this view
  by the same cut-away trick the wall patches disabled. With culling on, Remix had
  culled its top as a back face while it still blocked shadow rays — so run 6's black
  interior was the shadow of an invisible roof, not bad normals. The facing inversion
  itself is in the runtime: it infers a "mirrored view" from the sign of
  det(projection·view), which is negative for a right-handed projection, and flips
  every front face; since the product is the game's own matrix, no decomposition can
  change it. Fix: two-sided rendering (`rtx.enableCulling = False`), with a
  bridge-side cull-mode flip for captured draws as the milestone-2 alternative. The
  roof is hidden through Remix's instance-hiding texture category, which the user
  applies in the menu like the UI tags.

- **Run 8:** roof hidden via the texture tag; fridge, door frames, table and plumbob
  recognisable — but the floor still black, the interior dark, and only 56 instances
  from ~300 draws. The trace's texture bindings explain it: a single 32-pixel
  **environment cube map** is bound at sampler 0 by a dozen different shaders (walls,
  the floor shader, objects, Sims), with the diffuse at sampler 1. Remix takes stage 0
  as the albedo and drops any draw whose stage-0 texture has no content hash, which a
  cube map never gets — so the floor and most of the interior were never captured, and
  the black was Remix's default sky seen through the foundation cut-out. Fix (M1g): for
  captured draws on the primary render target whose stage 0 is a cube map or render
  target, the client binds the first colour 2D texture to stage 0 for the duration of
  the draw and restores it afterwards. The game's rasterization of that draw samples
  the wrong texture, but the ray-traced image replaces it; the shadow and reflection
  passes are excluded by the primary-target test, which also stops the main camera
  leaking into the shadow pass.

- **Run 9 (M1g): milestone reached.** Side by side with the raster view, the ray-traced
  frame has the whole house in place — every wall, door and window, the furniture, the
  fridge and shower, the mailbox, bin and chimney, the terrain, and the Sim — lit by the
  fallback light with ray-traced shadows, at 232 captured instances (56 the run before),
  60 fps. The log shows the remap presenting stage 1 as the albedo, all five shader
  patches, and the primary-target rule active.

## Milestone 1 status: ACHIEVED (2026-09-03)

What ships: the patched bridge client (`camera-hook-m1g.patch` on top of the crash fix)
plus `rtx.conf.sims3-m1` and two in-menu texture tags (UI, roof). The game runs, ray
tracing engages, and the lot is geometrically correct.

## Milestone 2 (started 2026-09-03): materials, lights, Sims

**Item 1 implemented (M2a):** the trace's pixel shaders were paired with the main-pass
vertex shaders and read for the texcoord that feeds the albedo sampler; 17 shaders need
a different texcoord promoted to TEXCOORD0 (walls A/B/D, the floor, the Sims' skin
shader, and a dozen object shaders), 6 already have it there, 3 keep the UV in the .zw
half of TEXCOORD0 (not handled yet) and 2 never sample. The client patches a copy of the
bytecode at `CreateVertexShader` — a usage-index swap in two `dcl_texcoord` output
declarations for vs_3_0, an `oT0`/`oTk` register renumber for vs_2_0 — verified on the
game's own shader blobs from the trace, and hands the patched copy to both the wrapper
and the runtime. Shaders are still recognised by the hash of the original bytes.

**Item 2 implemented (M2b, the sun):** the object vertex shaders do no lighting; they
pass the world-space normal to the pixel shader, which lights with a four-light rig
uploaded per draw as pixel constants: c0–c3 are unit directions toward the lights,
c4–c7 their colours — in the traced frame a bluish-white key from 38° above the horizon
plus three dim fills. Indoor objects carry lamp rigs, so the client treats each rig
upload as a vote for the brightest above-horizon light and, at Present, forwards the
majority as a fixed-function directional light (`SetLight(0)`/`LightEnable(0)`) whenever
it changes. Remix converts fixed-function lights into ray-traced ones and retires its
fallback light once a real one exists. Five object pixel shaders are recognised by
hash. Lamps (point lights with positions) are not translated yet; the rig only carries
directions.

**Run 10 (M2a+b) and the correction (M2c):** all 17 promotions applied and the sun vote
settled unanimously on the sunset (warm, 45° up, drifting as it set) — the "coloured
lights" were its bounce off the now-textured walls. Texture and material counts nearly
doubled. But furniture rendered white, the floor grey and the Sim red/blue, and the
pixel shaders' arithmetic explained it: "the first colour 2D texture" is not the diffuse
for multi-texture materials. The object shaders keep an *emissive* map at s2 (hence
glowing lamps and white furniture) and the diffuse at s3 on TEXCOORD2; the lot's
terrain-paint shaders (which I had mislabelled "floor" — their draws carry thousands of
triangles each) keep a *normal map* at s1 and blend the paint layers from s2/s3/s4 on
TEXCOORD0; two door/window families and walls B likewise. Vertex-buffer heights from
the trace later proved that all thirteen wall-family draws index one 1,620-vertex wall
ring three units tall, so the roof is drawn by a shader outside that family. Each of the 46 pixel shaders was read for
the sample the light multiplies, giving a per-pixel-shader **albedo stage** table (36
entries, keyed by pixel-shader hash) that now takes precedence over the heuristic, and
five promotion rules that had pointed at normal or lighting maps were removed while
the object family gained one. The Sim's skin is a ramp lookup driven by masks and has
no single albedo texture; it stays on the heuristic for now.

**Run 11 (M2c) and the tint (M2d):** exterior walls now show their wallpaper and
material counts rose again, but furniture stayed grey and the Sim fully red. Two
additions: per-stage remap statistics logged periodically (to see which stage each
captured draw actually presented), and the Create-A-Style tint. The object pixel
shaders multiply their diffuse sample by a per-object constant (c8) before lighting —
for recoloured furniture the texture is a greyscale pattern and c8 is the colour — and
Remix's legacy material multiplies the albedo by the fixed-function texture factor when
stage 0's second colour argument is TFACTOR, state the game's own shaders ignore. So
the client captures c8 from the constant uploads and forwards it as
`D3DRS_TEXTUREFACTOR` per captured draw (white for shaders without a tint). A guess
that the roof was the walls-B shader family was checked against that family's vertex
buffer in the trace and rejected (it is the three-unit exterior wall ring).

**The Sim (M2e):** her draws bind two cube maps, two small overlays, a 64-pixel
skin-tone ramp, and a 1,024-pixel composited body texture at stage 6 on TEXCOORD2; the
shader samples a second 1,024-pixel map and uses that sample to index the ramp, which
is the skin tone. No single texture gives the true skin, but the body texture is the
right albedo (clothing and skin layout) instead of the overlay mask that rendered her
red, so her pixel shader gets albedo stage 6 and her vertex shader promotes texcoord 2.

**The roof (M2f):** a scan of all 155 main-pass draws (each draw's vertex buffer
decoded and, where the shader's World registers are known, placed in the world) found
it. Two vertex shaders — handles `0x10474b80` and `0x1046e8c0`, hashes
`0x5048a794a3f737f9` and `0xb077115faa5f77b4`, five draws of 36–132 triangles, vertex
buffers in the same procedural-geometry region as the wall ring — have no World matrix
at all: each vertex carries a template position, a per-vertex offset (POSITION1) and a
per-vertex rotation/pitch (POSITION2); the shader adds x·pitch to y (the slope),
rotates about the vertical axis, translates, and projects with a camera-relative fused
matrix (c12..c15, eye at c16). That is roof pieces assembled at one pitch. Their pixel
shader hides the roof with alpha = saturate(lighting luminance − c3.x): a large c3.x
makes it transparent in the game's own rendering, but Remix's material only sees the
texture's alpha, so under capture the roof was an opaque slab. The two hashes are now
in the never-capture table (and dropped from the promotion table): their draws go to
the rasterizer, where the ray-traced image covers them, so the roof texture tags are no
longer needed. Consequence: the roof will not appear in ray tracing even in walls-up
mode; the refinement is to capture it only while c3.x is small. The scan's other
"tall" candidates were not roof: the object family's 0..128 short range is a scaled
encoding, the four-draw shader with scrolling texture coordinates and a fade inside a
projected rectangle is the water/outer terrain, and the thin slab at height 111 is a
skinned mesh.

**Run 12 (M2f) and the M2g build — walls, windows, the slab, the tint:**

- *Wall textures flickering with the camera.* The four wall pixel shaders read the same
  way: the per-draw texture sampled with the first coordinate set and multiplied by the
  light colour is the wallpaper (256×512 DXT5, a different one per draw); the texture
  every wall draw shares — 1024×512 A8R8G8B8, one level, sampled with the second
  coordinate set — is the lot's baked lightmap. My table had walls A and D presenting
  the lightmap (stages 1 and 0) instead of the wallpaper (stages 2 and 1), while walls
  B and C were right. The game moves wall segments between the four families as the
  camera moves (cut-away state, distance), so the same wall alternated between
  wallpaper and lightmap: the flicker. Fixed in the table; walls A and D no longer need
  a coordinate promotion (their wallpaper is on TEXCOORD0). Walls D's own texture is a
  16×64 trim strip: it draws the wall tops and edges.
- *No holes in the walls where windows and doors sit.* All four wall pixel shaders
  discard pixels (`texkill`) where a mask texture, sampled through a third coordinate
  set, is dark — the cut-outs exist only per pixel, in a mask Remix's material never
  receives. There is no state to forward. A fix means composing, per wall segment, the
  wallpaper with the mask as its alpha and presenting that with alpha test on, which
  needs per-segment draws; that is a milestone-3 job. Until then walls are solid and
  light will not enter through windows.
- *The white slab over the front rooms.* Not the pitched-roof family (those draws are
  no longer captured and the log confirms both shaders were recognised). The scan's
  remaining candidates: three untextured 16-triangle draws that open the frame (a
  skinned mesh with scrolling texture coordinates and rim lighting, drawn into an
  offscreen target — most likely the plumbob the HUD renders live; captured only if
  that target is backbuffer-sized), and the late flat draws, which turned out to be the
  object drop-shadow decals (25 instanced quads per draw, multiplicative blend, at
  ground level). M2g adds a per-(vertex shader, pixel shader) capture table to the
  periodic log line so the next run names the slab's shader directly.
- *Culling.* Triangle culling stays off: the runtime flips the front face for this
  game's right-handed projection, so with culling on it discards the faces the camera
  sees (run 7's black tops). The slab is not a culling matter; a captured roof blocks
  light whether or not culling is on.
- *The tint.* No non-white tint reached the runtime in run 12, and the trace agrees:
  every object upload in the in-world frame carries c8 = (1, 1, 1, 1). The object
  shader's diffuse (stage 3) is a static 1024×1024 DXT1 texture with mips — the
  Create-A-Style composite is built by the game and uploaded as an ordinary texture, so
  its colour is already in the texture; the texture-factor path is harmless but
  contributes nothing for these objects. Whether furniture is "grey" under Remix now
  needs an RT-off comparison of the same view.

**Culling, corrected (user's observation after run 12):** with triangle culling on, the
white slabs — over the front rooms and the one previously hidden by texture tags over
the main rooms — disappear, exactly as in the game. A white surface at wall-top height
that the game never shows from above is a *ceiling*: the game draws ceilings facing
down, for the low camera angles it allows, and culls them from above. So the runtime's
facing flip for this game's right-handed projection is the consistency measure it is
meant to be, not an error: with culling on, Remix culls the same faces the game culls.
Run 7's dark interior under culling was the shadow of the culled ceilings (invisible to
the camera, opaque to light), which is also what the user wants: the ceiling as a light
blocker so rooms are lit through windows and lamps. Consequences: `rtx.enableCulling`
goes back to its default (on); the instance-hiding tags for the slabs are no longer
needed; and until the window cut-outs exist (milestone 3), rooms under a ceiling are
lit by lamps only, since the walls are forced up by the cut-away patch. The pitched
roof stays never-captured: it faces up, so culling would not hide it in the cut-away
view where the game fades it out.

## Milestone 3a (2026-09-04): lamps located by the bridge

The user wants lamps that Sims switch on and off, without texture tags. The game never
uploads lamp positions — lamp light on walls and floors is baked into the lot lightmap —
but every object draw carries the four-light rig computed *at that object*: c0..c3 are
unit directions toward the lights, c4..c7 the attenuated colours (world space; the
object shaders take their normals to world space with the same World rows before the
dot products). A rig direction toward a lamp is therefore a ray from the object's
position — the World translation, c12..c14 for two object shaders and c16..c18 for the
third — and the rays of the objects around one lamp converge on it. The sun and the sky
fills are the same direction for every object; parallel rays never meet.

`LampSolver` (in `sims3_camera_hook.h`): captured draws of a rig shader with a known
World register add up to four rays (the sun's direction is skipped, duplicates of the
same object are folded). At Present, rays are first assigned to the lamps already known
(perpendicular distance under 0.75 units, lamp ahead of the ray); the unassigned rays
are paired, each non-parallel pair's closest approach is kept when the two rays pass
within 0.6 units of each other at a plausible distance (0.3–40 units), the meeting
points are clustered within 1 unit, and a cluster with three or more pairs becomes a
lamp. Each supported lamp is refined by least squares over its rays (the 3×3 system
Σ(I − d dᵀ) p = Σ(I − d dᵀ) o), its colour is the least-attenuated supporting colour,
and a lamp with no support for 60 frames is dropped — that is a Sim switching it off.
Up to seven lamps take fixed-function light slots 1..7 (the sun is slot 0) and are
forwarded with `SetLight`/`LightEnable` on change, as `D3DLIGHT_POINT` with range 15
and attenuation (1, 0, 0.2): the runtime solves the attenuation for an end distance,
clamps it to the range and squares it for the intensity, so the rig colour's magnitude
scales each lamp and `rtx.lightConversionIntensityFactor` /
`rtx.lightConversionMaxIntensity` tune the whole. The periodic log line now carries
the lamp count, creations, drops and light calls; the first lamp is logged with its
position and colour. Limits: a lamp needs three or more lit objects with a known World
register around it (Sims carry only a two-light rig and are not used yet); a lamp in an
otherwise empty room is not found.

**Run 14 (M3a):** lamps lit the rooms when switched on, but wobbled; terrain and wall
textures flickered or vanished with camera movement while furniture held. The log
explained the lamps: in one minute the solver created 22 lamps and dropped 19, with
2,447 light updates — lamps lost and refound, and positions and colours re-fitted from
whichever objects happened to be drawn (the colour was the nearest object's rig
colour). **M3b** stabilises the solver: assignment and meeting tolerances widened (the
rig is taken at the object's centre, the ray starts at its origin), no new lamp within
three units of a known one, the colour read from *all* supporting rays as the median of
luminance × distance² (an inverse-square reading) with the summed hue, position and
colour smoothed, a lamp forwarded only after three supported frames, dropped after 90
frames without support, and updates sent only for moves over 0.15 units or colour
changes over 0.05.

For the textures the same log showed the cause: the largest captured groups are shader
pairs the trace never contained (the game has hundreds of permutations; the trace frame
had 35 + 46), and the biggest of them — 26,774 draws a minute — binds no colour
texture the hook recognises, which is what vanishing looks like. M3b therefore dumps
the bytecode of every shader the game creates, once per hash, to
`Game\Bin\rtx-remix\logs\sims3-shaders\{vs,ps}_<hash>.bin`, and the capture table now
prints each pair's bound textures (stage, format, size, cube/render-target). The new
`disasm_dxso` tool (D3DDisassemble via the system `d3dcompiler_47.dll`; 81/81 of the
trace's shaders round-trip) turns the dump into `.asm`, from which the albedo, promotion,
World-register and rig tables can be completed for the real in-game set. That is the
next analysis step after run 15.

**Run 15 (M3b) and the M3c build — the sun flicker and the in-game shader set.** Lamps
held: seven found, none dropped in two minutes. The user saw the scene flip between a
lit and a dark interior as the camera moved. The log names it: the sun vote flipped
from the daylight key (colour 1.00, 1.00, 0.89) to the dim bluish sky fill (0.08, 0.09,
0.15, three votes of eight) — a view of mostly indoor objects elects the fill by
majority, the whole scene's key light dims ten-fold, and the runtime's auto-exposure
lifts the faint interior into view; move again and it snaps back. M3c makes the sun
the brightest candidate with real support (two rigs and a fifth of the votes) and
adds hysteresis: a different winner must hold for fifteen frames.

The dump held 3,250 vertex and 3,210 pixel shaders (the trace frame had 35 and 46);
78 (VS, PS) pairs reached the ray tracer during the run. `analyze_shaders.py` turns
the dump and the capture table into one report (archived as
`shader-analysis/shader-report-run15.txt`). What it settled:

- *Terrain flicker* — two coincident terrain passes every frame: the lit terrain
  family and an unlit four-layer paint blend (weights from a 128×128 map, four 1024²
  layers) drawn over the same chunks. The blend pass is now never captured, as are
  the instanced drop-shadow decals (a 1024² shadow atlas multiplied over the ground,
  which came through as dark opaque quads).
- *Floors* were presenting their 256×128 room lightmap; the pattern is stage 2 on the
  first coordinate set (both floor pixel shaders), and the floor-tile family likewise
  had its lightmap and colour texture swapped. A fourth wall pixel-shader variant
  (walls-D vertex shader) presents its wallpaper at stage 2.
- *Skinned object variants* (doors and other animated objects) get the same
  promotion as their static siblings and World registers (c192, c196) so they feed
  the lamp solver; two object pixel-shader variants join the rig list.
- Explicit albedo stages for nine more families (outer ground, a Sim part, the
  offset-and-rotation family, and the in-game Sim body shader, which now presents its
  1024×512 body composite instead of leaving the townies red).
- The ceilings are five untextured draws a frame (two vertex-shader variants), captured
  every frame — they were never the cause of the flicker.

Also removed from the live config: a stale "Add Light to Texture" tag the user had not
set, which was lighting the lawn green from the plumbob.

**Run 16 (M3c) and the M3d build — the sun from the shadow map.** Steadier, but the
user still saw two suns trading places as they zoomed, with different shadow
directions. The trace explains why no vote can fix this: within one main-pass frame the
object rigs' brightest above-horizon light is (0.321, 0.247, −0.914) for five objects,
(0.750, 0.600, −0.278) for three and (0.576, 0.774, 0.265) for three — indoor objects
carry a *different key light* than outdoor ones, so the majority follows the view. The
game does expose the true sun every frame: shaders that read the 2048×2048 shadow map
receive the light's view-projection as four constant rows applied to the world position
(the terrain-paint shader at c0..c3, the Sim body shader at c180..c183, three object
families at c0..c3), and the cross product of the x and y clip gradients is the light
axis. In the trace it is (0.576, 0.774, 0.265) — identical to the warm, bright candidate
(colour 1.00, 1.00, 0.79) to three decimals, and not the majority. M3d takes the sun's
direction from those rows (smoothed), and the colour from the vote candidate whose
direction matches within five degrees; with no match (indoor views) the colour is
kept. The vote-with-hysteresis path remains only as the fallback when no shadow rows
have been seen for two seconds. `shadowLightDir` and `findShadowSource` are tested on
the trace's rows.

**Run 17 (M3d) and the M3e build.** Much steadier; the direction still switched
occasionally at full zoom-out, and the log showed the remaining hole: the first sun
sent had the right direction but the colour 0.08, 0.09, 0.15 "from the matching rig
candidate (6 votes)". Indoor objects report the sun's *own direction* with a heavily
attenuated colour, so choosing the most-voted match still dims the sun to a tenth
whenever indoor objects dominate the view; the far view, meanwhile, can stop drawing
the shaders that carry the shadow rows, and the two-second fallback handed the
direction back to the vote. M3e: the matching candidate is the *brightest* one; a
darker colour is adopted only after it persists for 1,200 frames (twenty seconds,
still far shorter than dusk) while a brighter one is adopted at once; and once the
shadow map has given a direction it is kept, the vote deciding only before the first
sighting. The periodic log line now carries the sun update count and the frames
without shadow rows.

*The red townies:* the in-game Sim body pixel shader reads its stage-2 texture channel
by channel as a mask (red when shown as an albedo). Its skin is the two 1024² textures
at TEXCOORD2 blended and tinted through the 64×64 ramp by c17.x, and the 1024×512
clothing composite at TEXCOORD1 (stage 3) is layered over that skin by a
difference-based factor. The clothing composite is the one texture that reaches the
colour as-is, so both Sim pixel shaders now present stage 3; the skin stays
approximate until a bridge-side compose (skin × ramp under clothing) exists.

*Walls:* the interior faces render as white boards with vertical seams; whether that is
the wallpaper or the lot lightmap needs the user's ray-tracing-off reference of the
same view. Walls C, for the record, has no albedo at all: its colour is lighting × a
constant, its only 2D texture a cut-out mask — it draws the wall tops and cut edges.

**Run 18 (M3e) and the M3f build — the walls were right all along.** The user's
ray-tracing-off reference of the same view settled it: the bedroom's tan planks, the
living room's black-and-white stripes, the bathroom tile and the hallway's wood are all
there in the ray-traced frames, blown out to white — the planks read as white boards,
the stripes survive as faint lines, the black leather armchairs read light grey, the
carpets read pale. Auto-exposure settles on the sunlit exterior, so an interior brighter
than that means the lamps were outshining the midday sun. The runtime derives a
converted point light's intensity from the square of the end distance it solves from
the attenuation, and with (1, 0, 0.2) that distance ran to the 15-unit range. M3f sets
(1, 0, 4) with range 10, which brings the lamps down by roughly an order of magnitude;
`rtx.lightConversionIntensityFactor` remains the user's knob on top. Both wall vertex
shaders were read in full on the way: their wallpaper coordinates are x and y of the
input set scaled by 1/512 (the y lerped by the cut-away factor at c8.x/c8.y, which is
1 with walls up), so the capture had them right. A diagnostic stays in: the first
sixteen wall draws log the texture object they present.

*The Sims turning red at distance:* the game switches Sim shaders with distance. Two
distance variants keep the clothing composite at stage 3 on TEXCOORD1 and were
presenting the stage-2 mask by fallback; the far variant keeps it at stage 2 on
TEXCOORD5. All three are tabled, with the promotions to match.

**Run 19 (M3f) and the M3g build — the walls, for real this time.** With the lamps
dimmed the living room still showed a soft grey wall with faint seams where the
reference has bold black-and-white stripes, so the over-exposure reading was wrong.
The wall-draw diagnostic cleared the remap: every wall draw presented a distinct
256×512 DXT5 wallpaper object (ids 19501, 19422, 19158, …). The runtime's source then
explained the rest. For a draw whose vertex declaration carries a texture-coordinate
element with the index equal to stage 0's `D3DTSS_TEXCOORDINDEX`, the 1.5.2 runtime
samples with those *raw input* coordinates and ignores the vertex shader's output; the
captured TEXCOORD0 output is used only when no such element exists (the
`useVertexCapturedTexcoords` option that forces it is not in 1.5.2 — the runtime binary
has no such string). Objects and Sims pass their input set through nearly unchanged, so
raw looked right; the terrain has no input set at all, so it was captured and right; but
the wall shaders keep their coordinates in texel units and divide by 512 in the shader
(`mul oT0.x, v2.x, c14.w` with c14.w = 1/512), so the runtime tiled each wallpaper five
hundred times across a wall — grey mush with seams. The trace has no
`SetTextureStageState` or `SetTransform` call at all, so the game never sets the index.

M3g therefore points stage 0's texture-coordinate index at the unused set 7 for the
draws of verified families — the promoted shaders and the wall and floor families in
`kCapturedUv` — so the runtime finds no input element and samples with the shader's own
TEXCOORD0 output, which is what the pixel shader samples with by construction. The state
toggles only when the family changes (a handful of calls per frame). Families not yet
verified keep the raw input that happens to work for them. With this the texcoord
promotions finally do the job they were written for: object patterns get their tiling
constants, Sims their clothing set.

**Run 20 (M3g) and the M3h build — the walls that pop.** The wallpapers now render at
their true scale (the teal siding's boards are visible in the user's shot). The wall
piece that "appears randomly as the camera moves" was hovered in the Remix texture
list: a 256×512 BC3 wallpaper, and in the frame it has the stair-stepped edge of the
game's own cut-away, which lowers wall vertices one by one (the per-vertex factor v6.x,
clamped by c8.x and c8.y: height = base + factor × (top − base)) as the camera nears a
wall. The clip-scale patch (c8.z) made hidden walls visible but left the height
animation to the game, so walls kept dipping and rising with the camera, and their
dark interior faces showed through the gaps. Since the ceilings are the light blockers
on the walls-up premise anyway, M3h extends the four wall patches to c8.xyz (c12.xyz
for walls B): every wall shown, at full height, always. The trade is that the game's
walls-down and cut-away buttons no longer do anything under ray tracing; respecting
them would mean skipping the draws the game hides (never captured, so no fans) rather
than forcing the constants, which is a later option if the user wants the modes back.

**Run 21 (M3h) and the M3i build — reverted, and what the register really is.** The
user wants the game's walls-down and cut-away to keep working under ray tracing, as
they did before M3h, and reported the popping walls unchanged by the full-height
forcing. Both facts fit the trace: the game's own value of the wall register is
(0, 1, 0, 0). With c8.x at 0 the per-vertex factor v6.x sets each wall's height (that
is how walls-down and cut-away work, and why they survived the clip-scale patch), and
with c8.z at 0 the per-vertex flag v6.z decides visibility, which my c8.z = 1 overrides
for every wall. M3i restores the single-component patch. Since forcing full height
changed nothing, the stair-stepped piece is not the cut-away either, and the trace has
no blended wall draw in a normal view; so the popping is still without a confirmed
cause. M3i logs, for the first wall draws and for any wall draw the game blends, cuts or
hides, the blend, alpha-test, cull and depth-write states and the game's own register
values, so the next run names it. The principled end state, if the per-vertex hiding
turns out to matter, is a bytecode patch that sends hidden vertices to w = 0 (NaN after
the capture's divide, which Vulkan treats as an inactive triangle) instead of forcing
the constant.

**Run 22 (M3i) and the M3j build — the "walls" were the roof.** The diagnostic showed
the game never touches the wall register at all: sixty-four wall draws, all opaque,
one-sided, register (0, 1, 0, 0). So the wall families are not what pops. What pops
is a shader tabled from the dump without being named — "offset+rotation family",
about one draw a frame in the runs where the user zoomed. Read in full it is the roof
family's arithmetic exactly: a per-piece offset (POSITION1) and rotation (POSITION2),
the y coordinate sheared by x × pitch, camera-relative projection with the eye at
c16, and a pixel shader that hides the piece by alpha through the same constant as the
trace's roof shader. It is an in-game permutation of the roof (normal-mapped, two
tilings) whose hash my two roof entries did not cover, so it was captured and the
ray tracer showed each piece whole and dark whenever the game began drawing it — the
texture the user hovered was roof shingles, 256×512, not wallpaper. M3j names that
hash, and more usefully recognises the family by shape: only roof pieces declare both
a second and a third POSITION input (Sims declare POSITION1 for morphs, never
POSITION2), so `hasPositionOneAndTwo` on the bytecode keeps every permutation the
game compiles out of the scene. Tested on the trace's roof, the in-game roof, walls
and the Sim body.

**Run 23 (M3j) and the M3k build — the cut-away, done properly.** The roof was wrong
too: the piece the user hovered is exterior siding, and the second shot shows a near
wall the game has cut away standing at full height. That is my own first wall patch.
The wall shaders scale the clip position by max(v.z, c8.z); the game keeps c8.z at 0,
so the per-vertex flag decides, and a hidden vertex collapses to the clip centre. The
cut-away hides the *top row* of a wall while its bottom row stays, so under capture the
mixed triangles fanned to the camera (milestone 1's fans); forcing c8.z to 1 cured the
fans by keeping every hidden vertex in place — which is exactly why cut walls stand and
"pop" as the camera moves and the game re-flags them. M3k removes the constant patch
and edits two instructions in each wall shader's bytecode instead (`kHideFolds`,
`applyHideFold`, exact token patterns from the dump): after the height factor's clamp
`min rH.w, rV.x, c8.y` it inserts `mul rH.w, rH.w, rV.y` (rV.y being the game's
max(v.z, c8.z)), so a hidden vertex's height becomes 0 and a cut wall lies flat at
floor level like a lowered one — a hidden top row over a visible bottom row gives
zero-height slivers, and the game's own walls-up override (c8.z = 1) still raises
everything; and `mul oPos.xyz, rV.y, rP` becomes `mul oPos.xyz, <literal 1>, rP` so
nothing collapses to the centre any more. The fold is idempotent and a family whose
stream does not match both patterns is left unpatched and logged. Verified three ways:
token tests on the trace bytecode, the same edits applied offline to all four in-game
streams and disassembled (`min r1.w, r2.x, c8.y` / `mul r1.w, r1.w, r2.y` /
`mul oPos.xyz, c14.z, r1`, and the runtime accepting
the patched shaders.

**Run 24 (M3k) and the M3l build — the green walls are grass, and a bug of mine.**
The wall modes work again with the fold; the popping continued, so it was never the
walls. The run's capture table has a rarely drawn pair binding *two* 256×512 sheets —
37 draws in this run, thousands in the runs where the user zoomed most. Its vertex
shader is camera-relative, fades by distance and collapses hidden instances to the
origin, picks one of four axis orientations per instance from a constant array, and
blends two frames with a wind phase (a 2π constant); its pixel shader lerps the two
frames and discards by alpha. That is the game's close-range grass and flower
sprites. Under capture they are solid quads textured with a green tuft sheet: the
"greener wall that should not be there", appearing exactly as the camera comes close
and vanishing with distance. M3l never captures that shader (`0x5a2deada1e077b44`);
the game's close-up grass is rasterized under the ray-traced image, a loss the
ray-traced ground carries easily.

The same log exposed a bug in M3j: the roof-by-shape test fired 823 times in one
session. Sampled, those shaders declare POSITION1..5 and NORMAL1..4 with blend indices
and weights — Sims with morph targets, which the test had been keeping out of the
scene. The test now also requires no BLENDINDICES/BLENDWEIGHT, no NORMAL1 and no
POSITION3, which no roof has and every morphing Sim does; tested on a five-target Sim
shader from the dump, the grass shader, walls and both roofs.

## Milestone 4 (2026-09-04): the wall cut-away mask, evaluated on the client

The grass fix did not end the popping. The user's scene capture at a popping moment
(`captures/capture_2026-09-04_22-01-27.usda`, walls-down mode) settled what it is:
the lot's walls appear twice — flattened meshes at floor level (the M3k fold at
work) and six full-height wall groups with their wallpapers, instanced twice each,
plus a full-height top strip (the "floating top face"). Their vertex layout is the
walls' (the capture records a layout hash per mesh), so they are the wall families
themselves, with the height factor and the visibility flag both at one. The game hides
them *per pixel*: every wall pixel shader samples a cut-away mask through
TEXCOORD4 / 4096 and discards where

    mask + 0.5·(1 − L/4096) − 0.5 < alphaRef,   L = lerp(TEXCOORD2.z, TEXCOORD2.y, h),

h being the height factor min(max(TEXCOORD5.x, c8.x), c8.y); the faces also require
the fade term (TEXCOORD5.y as a saturated colour) above 0.5, and the tops (walls C)
alpha-test the same expression at 64/255. The mask is a DXT1 atlas the game uploads
white (the trace's upload decodes to all-white) and darkens per wall segment as the
wall modes change; the walls' real declaration (0x13df3fc0, not the plumbob's
0x121a4b40 I had mis-assigned) keeps the four coordinate sets as shorts in a 44-byte
stream and the height/fade/visibility flags as a separate 4-byte UBYTE4 stream, which is
why the game can rewrite them every frame.

The runtime cannot run that discard, but the bridge client already keeps a shadow copy
of every vertex and index buffer (`LockableBuffer::m_shadow`) and of every locked
surface (`Direct3DSurface9_LSS::m_shadow`), so M4 evaluates the discard at each vertex
on the client: `readVertexElement` decodes the declaration's elements, `maskSample`
reads the mask (DXT1 decoded per texel; L8/A8/A8R8G8B8 too) at the wrapped UV,
`wallVertexKilled` applies the family's rule (`HideFold` now carries the height
register, the mask stage, the fade source and the alpha threshold), and
`DrawIndexedPrimitive` drops every triangle whose three corners are all discarded. When
some survive, their indices go through a 64K-entry index buffer of our own (created
once, discard-locked at the first use of each frame, no-overwrite appends after) and
the draw is re-pointed at it; when none survive the draw is not sent at all. Partially
discarded triangles (the cut-away diagonal, the window holes) are kept whole, so this
handles hidden walls and tops, not the holes. The periodic log line reports triangles
hidden and draws skipped / filtered / untouched; tests cover the DXT1 decode, the
wrapped sampling, the element readers, and the rule's four cases.

**Runs 26 and 27 (2026-09-04) settled nothing.** Both sessions lasted about a hundred
seconds: the lot loaded near frame 3600 and the game exited twenty seconds later, so the
only statistics lines came from the loading screen and the "never engaged" reading of run
26 was premature. Run 27 carried early-exit logging and printed no early exit, so in the
lot the evaluation either ran cleanly on every wall draw or failed its silent gate (a wall
family bound, the main camera held, an indexed triangle list). The trace confirms the
walls are indexed triangle lists on the real declaration, and shows how the mask is
written: the 256×256 DXT1 texture is created white, then rewritten once the lot is up
through a full lock of level 0 (546 of 4096 blocks darkened, 7942 texels, with walls up
and the camera far); the client's surface shadow is the locked memory itself and persists,
so the evaluation reads the current mask. Revision M4c makes a short session conclusive:
it logs why a wall draw was not evaluated (camera state, primitive type, the capture
inputs), describes the first evaluation per family and the first three that saw a dark
mask (triangles hidden, corners killed, the height clamps, the mask's format and dark
block count, the sample range, how many corners had the fade flag), prints the summary
line every 600 frames, and prints it once more at shutdown.

**Run 28 (2026-09-04) found the real rule.** The evaluation ran on every wall draw and
discarded nothing: the corner samples stayed above 0.9 while the mask held 586 darkened
blocks. The coordinate was wrong. All four wall vertex shaders build it the same way: with
h the clamped height factor and t = lerp(TEXCOORD2.z, TEXCOORD2.y, h) / 4096, the mask is
read at u = TEXCOORD4.x/4096 + (TEXCOORD2.x/4096)(TEXCOORD4.z/4096) and
v = TEXCOORD4.y/4096 + t·(TEXCOORD4.w/4096), and the pixel is discarded where the mask
is below 0.5 t plus the alpha reference, with the face's TEXCOORD5.y flag set (walls B
read the raw byte; my table had wrongly pointed B at TEXCOORD3.y). So every face owns a
rectangle of the mask that the pixel walks along the face and up its height, and the
foot of a face can never be discarded. Decoding the trace's lot mask shows what the game
paints there: a door and a window as black shapes with white margins, and, when a face is
hidden, the whole rectangle. The mask's stage depends on the pixel shader, not the vertex
shader: the D vertex shader pairs with two pixel shaders whose masks sit at stages 3 and
2, and the earlier table put stage 2 on the wallpaper. Revision M4d evaluates the corners
with the real coordinate, and, because the foot corners always survive, decides a triangle
by sampling a 15-point grid over its interior when any corner is discarded: hidden only
when every point above the foot is discarded, so openings and partial cuts keep the face
whole. The mask stage comes from a pixel-shader table with a fallback to any bound 256-or
-larger DXT1 texture. Each change of the mask's content is saved to disk (up to 16
versions) so the game's painting during a close zoom can be read offline. Open question
for run 29: if the game hides near walls by painting only the upper part of a face (a
cut-away stub), whole-triangle dropping cannot express it and the geometry itself must be
cut, which is milestone 5's job: copy the draw's vertices into the bridge's own buffers
and lower each top vertex to the cut height read from its mask column.

**Run 29 (2026-09-07) refuted the mask.** During a full zoom-in the mask never changed:
only its two load-time versions were saved, and they hold the lot's doors and windows.
Not one of 32,125 wall draws had a single partly discarded triangle. So the game does
not hide the near walls through the mask at all. The walls A pixel shader multiplies its
colour by the face flag and takes alpha from the result's luminance, so a flag-clear face
is black with alpha zero, but at the trace's far wall draw alpha test and blending were
both off, under which such a face would draw black rather than vanish; whatever hides
the near walls comes with a state the trace window never showed. Revision M4e is a safe
blind fix plus a recorder: any captured draw whose game-set states make it invisible
(colour writes off, a depth or alpha test that never passes, a zero-by-one blend) is not
sent to the ray tracer; a wall triangle whose three corners have a clear flag is hidden
when the draw is alpha-tested or blended; and each wall family's render-state, height
clamp and flag signature is logged whenever it changes. The popping walls are parked at
the user's request. When resumed, read those state lines first: they name the mechanism.

**Limitation found on the way:** the main object shader's pixel shader declares two
cube maps and three 2D samplers — the signature of Sims 3's *Create-A-Style* materials:
a greyscale pattern texture, colour masks, and the actual colours as per-object
constants. Under Remix those objects (and CAST walls and floors) will show their pattern
but stay grey/white until the tinted albedo is composed somewhere. That is a larger,
separate job (milestone 3): either bake the masked tint into a replacement texture per
material in the bridge, or feed Remix a vertex/constant colour it can multiply in.
Terrain, foliage and most world objects are conventional textures and are unaffected.

1. **Textures sample grey.** The right texture is now bound, but Remix captures the
   texture coordinate from the vertex shader's TEXCOORD0 output, and Sims 3 shaders put
   other things there: the main object shader emits a 3-vector (cube-map direction) on
   TEXCOORD0 with the diffuse UV on TEXCOORD2; wall shader B emits a lighting coordinate
   on TEXCOORD0 with the pattern UV on TEXCOORD1; the floor shader's planar UV *is* on
   TEXCOORD0. Fix: per shader, read the pixel shader to find which coordinate feeds the
   diffuse sampler, then patch the vertex shader bytecode in the client at creation —
   for vs_3_0 swap the usage indices of the two `dcl_texcoord` output declarations, for
   vs_2_0 swap the `oT` register numbers — so the diffuse UV leaves as TEXCOORD0. The
   game's own rasterization of those draws breaks (irrelevant: the ray-traced image
   replaces it); the reflection texture may look wrong. Same table mechanism as the
   constant patches. Estimate: 1–2 days for the shaders in a typical lot.
2. **Lights.** Remix sees one fallback light. Sims 3 lights are shader constants; the
   bridge could translate the game's light constants into D3D9 fixed-function lights
   (`SetLight`/`LightEnable`), which Remix converts. Estimate: days.
3. **Sims** render pink (skin material via the wrong stage/UV) — covered by 1.
4. **Foliage/flowers** show as white planes (alpha-tested textures via wrong stage/UV)
   — covered by 1, plus Remix's alpha-test categories.

Implemented exactly as described below: `src/client/sims3_camera_hook.h` (header-only
decomposer + validation + change detection) and a call in `SetVertexShaderConstantF`,
placed *before* `WAIT_FOR_OPTIONAL_SERVER_RESPONSE` (that macro returns on every path;
code after it is silently dead — one build was lost to this). No per-shader table: the
hook tries "c4–c6 is the World" and "World is identity" and lets validation choose.
Only draws to the primary (backbuffer-sized) render target reach Remix's camera
manager, so the texture-pass cameras (shadow, reflection) cannot claim a frame.
Standalone test on captured constants: 12/12 (main camera via both paths, exact x/y/w
clip round trip, camera position equals the eye constant, shadow camera rejected).
Patch, DLL and test: `rtx-remix-investigation\built-bridge-notfound-fix\camera-hook-m1\`.

## Where the hook lives

In the bridge **client** we already build (`bridge-remix/src/client/d3d9_device.cpp`,
`Direct3DDevice9Ex_LSS::SetVertexShaderConstantF`): on uploads to c0 with ≥4 registers,
decode the WVP; identify the World from the shader's layout (per-`SetVertexShader`
table, built from the trace); compute `VP`, split to `V`/`P`; forward
`SetTransform(D3DTS_VIEW, V)`, `SetTransform(D3DTS_PROJECTION, P)` and per-draw
`SetTransform(D3DTS_WORLD, W)` to the server so `d3d9State().transforms` are populated
and Remix's camera manager accepts the frame. Only the main-view camera should be fed
(filter by FOV/aspect; shadow and reflection cameras must not become "the camera").

## Honest scope

| Milestone | Effort | Notes |
|---|---|---|
| Terrain + walls + objects appear, Remix fallback light | 1–3 days | main-view camera only, 2–3 common shader layouts |
| All static shaders correct; untextured surfaces | 1–2 weeks | second known blocker: `Texture 0 without valid hash, skipping drawcall` drops untextured draws |
| Sims (skinned via vertex capture), lights translated, proper look | weeks → months | this is the gta4-rtx-scale compat mod; lights are shader constants too (Remix sees `# Lights 0`) |

Also known: `D3DRS_MULTISAMPLEANTIALIAS` is unhandled by DXVK-Remix (game MSAA is a
no-op); point-list primitives (particles) are ignored.

Evidence: `Documents\rtx-remix-investigation\` — `native2-inworld-window.txt` (calls
1243000–1283000), `vp_decompose.py`, cloned `xoxor4d-dxvk-remix` (source lines cited),
`logs-1.5.2-RT-on\` (runtime compatibility messages).

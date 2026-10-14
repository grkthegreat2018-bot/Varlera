# Varlera

Blocksworld-like 3D building sandbox in pure C — Vulkan + GLFW, no engine.

## Build

```
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="D:/windsurf/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release   # or Debug (enables VK_LAYER_KHRONOS_validation)
```

Deps via vcpkg manifest: `glfw3`, `vulkan-headers`, `vulkan-loader` (NOT the Vulkan SDK
for headers/lib — glslc from the SDK is used to compile shaders).
Exe lands in `build/<config>/varlera.exe` with `shaders/*.spv` copied beside it.

Linux: `sudo apt install libglfw3-dev libvulkan-dev glslc` (or vulkan-sdk), then
`cmake -B build -S . && cmake --build build`. Prebuilt binary: the `linux`
GitHub Actions workflow builds every push and uploads a `varlera-linux`
artifact (exe + shaders; run needs Vulkan driver + `libglfw3` + a display).
CMake falls back to
`find_package(Vulkan)` + `find_package(glfw3)` when vcpkg vars are absent, and
links m/dl/pthread. Win32-only code (file dialog, DPI awareness) is `_WIN32`
-guarded; OBJ import on Linux uses zenity or `import.obj` beside the exe.

## Layout

- `src/rk.c` / `rk.h` — renderer, frame + draw path. Vulkan 1.3 dynamic
  rendering, reversed-Z main depth (clear 0, GREATER), 4x MSAA + resolve,
  2048px directional shadow map (forward-Z ortho `m4_ortho`, hardware-compare
  sampler + 4-tap PCF), instanced drawing (`RkInst` = pos/scale/RGBA8/flags;
  bits: 0 unlit, 1-2 rot90, 3 nofog, 4 iconVP, 5-10 texid, 11-12 texmode,
  13 texoverride, 14 vcol, 15 spin, 17-18 material MATTE/GLOSS/METAL/GLOW —
  Blinn spec or self-bright in world.frag, GLOW feeds the bloom bright pass),
  deferred command queue
  replayed for shadow pass. HDR post chain: scene renders into an R16F
  target (UNORM fallback), 4-mip bloom downsample (soft-knee bright pass) +
  tent upsample/add, then `post.frag` ACES tonemap to the swapchain; icons
  + UI draw in a second LDR pass (fresh 1x depth `uiDepth`) so they are
  never tonemapped. Post shaders: `post.vert` fullscreen tri,
  `down.frag`/`up.frag` mip chain, `post.frag` composite — all hot-reloadable
  via F8. UI quads are NDC rects (`UIVert` pos/col/uv/tex
  — `rk_ui_rect` solid, `rk_ui_texrect` samples the atlas via set1 binding 1
  for real texture previews). `rk_init` returns 1 on success.
- `src/rk_vk.c` — renderer init/resources: instance/device pick, swapchain,
  pipelines, memory (`buf_create`/`make_image`, manual VkDeviceMemory — VMA
  3.x is C++-only, do not re-add to a C target), texture-atlas + model-mesh
  uploads.
- `src/rk_int.h` — renderer-internal shared state: `RkG G`, queue/UBO types,
  `VKC`, `rk_swapchain_recreate`, `rk_light_dir`.
- `src/meshgen.c` / `.h` — CPU generation of the built-in unit-box shape
  meshes (`meshgen_shape` emits one shape for CPU bakes;
  `meshgen_build_all` emits all + wireframe box + grid plane). CAGEBALL is
  a birdcage built from `tube_arc` swept-tube rings (4 meridian ribs +
  latitude rings, ~3.7k verts) — keep generated shapes under 4k verts:
  sculpt.c's per-shape buffer is `soup[4096]`, model.c's is `tmp[8192]`.
- `src/app.h` — shared `App A` global (window, camera, mode, selection, drag state).
- `src/main.c` — entry, glfw callbacks -> dispatch, frame loop.
- `src/camera.c` — free-fly cam (yaw/pitch basis, ray gen, WASD/QE move).
- `src/edit.c` — world editing: ray pick (scaled unit-AABB slabs), place ghost,
  move/resize/marquee state machine, selection outlines + 6 per-face resize
  handles (opposite face anchored, block recenters), fuse/unfuse (ctrl+B/U).
- `src/model.c` — fused-block baking: per-child vertex colors + texture layers
  (`Vert.tex` float, -1 = inherit), internal cube face culling, baked vertex
  AO (`bake_ao`: 8-ray hemisphere occlusion vs child AABBs darkens the vert
  color — fused/premade path only, sculpted topo needs accel first), `models.bw`
  persistence + `rk_model_mesh_add` upload. v7 stores per-vert material id
  (`mvtex`, int8 -1 = inherit) + layer lock (`mvlock`) + `adv` flag; v6 and
  older migrate. `Model.smooth` (flags bit1, no version bump) rebakes a
  sculpted model with averaged per-vertex normals — script-menu SHADE row
  (vox models only) toggles it via `models_rebake_all`.
  `models_bake_builtins()` seeds pack 1
  (CRATE/TABLE/CHAIR/PERSON/TREE/FLOWER/ARCH/LAMP/FENCE/PILLAR) at v7 and
  `models_bake_pack2()` seeds 12 game-dev props (DOOR/BUTTON/LEVER/SIGN/
  CAR/BARREL/LADDER/COIN/SPIKES/PLATFORM/CANNON/GEAR) at v8 — each pack
  bakes once per save-version bump; `dedupe_adv()` drops same-named
  premade dupes on init so upgrades never double them.
- `src/panel.c` — right panel with 6 tabs: SHAPES (categories BASIC/RAMPS/
  SPECIAL/SCULPTED, 15 assets + sculpted models, scrollable, tooltips), PAINT
  (16 presets + HSV picker + custom swatches + ALPHA slider -> `A.paintAlpha`
  -> `Block.alpha`/`BF_GLASS`), TEX (ART TOOL buttons EDIT/NEW/SHEET on top ->
  `art_open`, MODE/TYPE cycling `A.paintTexM` + MAT row = material
  MATTE/GLOSS/METAL/GLOW in texm bits3-4, scrollable grid of real atlas
  previews via `rk_ui_texrect`, drag-to-reorder via `tex_at`/`tex_move`
  (display-order permutation in tex.c, textures.bw trailer, NONE pinned),
  footer strip shows selected tex name/dims/ANIM), WORLD (SAVE/LOAD/UNDO/REDO/CLEAR + block count
  + ENVIRONMENT + WORLD SETTINGS — see panel_world.c), MODELS (fused block
  groups), PROPS (`adv` premade assets).
  Asset geometry, paint color and texture are fully decoupled. Script menu
  docks left of panel on select: single-select shows `BLOCK #id` + mesh name
  + pos/size + TEX/alpha info header; GRAVITY/COLLIDE/LOCKED toggles +
  NAME (textMode 3 rename, tracked by `Block.id`) + TRIG/ACT/TGT/PARAM
  cycle rows (full BW script row) + UNFUSE/RESCULPT. The SHAPES search box
  also sits on MODELS/PROPS and filters those grids (`model_matches`).
- `src/world.c` — block store (`Block` 68B: pos/size/col/alpha/id/name/shape/
  rot/tex/texm/flags/trig/act/actParam/tgt/traction/bounce/weight —
  stats are u8 0-15, defaults BS_TRACT 12 / BS_BOUNCE 0 / BS_WEIGHT 8,
  editable via script-menu TRAC/BOUNCE/WEIGHT rows), undo/redo stacks,
  `world.bw`
  binary save/load. Format v7: 56B header (magic BWLB, ver, nBlocks,
  nextid, created, modified, name[32]) then the packed block-record
  stream as one LZSS blob `{rawBytes, compBytes}` (src/lzss.c — records:
  flag byte, id, 9 metadata bytes, u8 color/alpha, traction/bounce/weight,
  6xi16 quantized pos/size (1/16 grid, raw f32 when off-grid), optional
  name[12]; ~15.6B/blk compressed vs 68B v3). v4-v6 readers decode the
  same records inline; v1/v2/v3 migrate. v<6 remaps block shapes >=16
  up 16 (RK_MESH_BASE_MODEL moved 16->32).
  Readers migrate v1/v2/v3. `world_save_async` snapshots the block array
  (~280KB memcpy) then serializes+LZSS+writes on a worker thread
  (_beginthreadex/pthread, detached); `world_save_poll` reaps it —
  autosave + quicksave use it (quicksave falls back to sync when busy).
  `worlds_file(name,ext)` -> `<dir>/<san>.bw`
  (dir = `worlds_dir`, default "worlds", user-picked via WORLD tab DIR;
  persisted in settings.bw v2). Saves also write `worlds/<name>.bmp`
  thumbnail; autosave every 90s to `worlds/<name>.auto.bw` when
  `world_rev()` changed. v4 files end with an optional 32B `WSet`
  settings trailer (phys mode / chunkShape+Size / lod / advRender /
  gravMul / skyAssets + skyObj[4] carved from pad — 0=none, 1+=model
  index; fogCurve + pcfRad ride UBO `fogParams.zw` — fog exponent and
  shadow PCF texel radius, EXPERIMENTAL rows 17-18) — `world_wset()`
  is the live record, WORLD tab
  edits it and dangerous rows (`!`) get an APPLY/CANCEL wipe-warning
  dialog. `tests/world_v4.c` (ctest) covers round-trip + v3 migration;
  `tests/unit.c` (ctest `unit`) covers lzss round-trip/corrupt input +
  all meshgen shapes (vert bounds, normals, 4096 sculpt cap).
  `world_pick_block`/`ray_unit_box` are the shared slab-pick used by
  edit.c and physics.c.
- `src/physics.c` — play mode: `world_physics(dt, trigHeld)` gravity settle
  (XZ uniform-grid broadphase over a packed support grid), trigger-edge
  script actions (COLOR/TEX/DEL with `tgt` scoping, SAY/TITLE toasts,
  BOOST thruster), snapshot/restore (`world_snapshot_play`/`_restore_play`,
  `world_reset_physics`). Extracted from world.c; physics.h is the API.
  Honors WSet: `phys` RIGID freezes gravity settle (script acts still run),
  `gravMul` scales gravity, `dragOff` gates LMB drag; `weight` resists
  BOOST thrust, `bounce` gives landing restitution, `traction` is the
  walk-mode grip — ground accel lerps 6-40/s by the stand block's stat
  (0 = ice slide, bare floor = full, air = 3.5/s) and hazard knockback
  decays through it. LMB drag:
  `world_drag_grab/move/release` — kinematic cursor-ray move clamped to
  support height (floor clamp only in RIGID mode). Walk player
  (`WSet.playerMode` FLY/WALK): `world_player_enter/exit/tick` —
  kinematic feet-anchored box vs block AABBs, axis-separated push-out,
  jump/step-walk, y=0 floor + void respawn; tick replaces `cam_frame`
  and writes A.camPos to eye height. texm game bits (`TEXM_HAZARD` 0x20,
  `TEXM_PICKUP` 0x40 — free render bits): hazard touch = -1 heart +
  knockback + 1s invuln, 0 hearts = respawn at 3; pickups are
  non-solid, collect on touch (`s_taken` hides them till restore).
  HUD: HP/score bottom-left in play mode. Game flags: `BF_SPAWN` (spawn at
  block top) + `BF_GOAL` (touch -> burst + `GOAL n/m` toast, deduped per
  play session) — script-menu SPAWN/GOAL toggle rows.
- `src/filepick.c` — OS pickers shared by panel_world/sculpt: `pick_save_path`/
  `pick_open_path`/`pick_dir` (Win32 common dialogs + SHBrowseForFolder;
  zenity fallback on Linux).
- `src/export.c` — `world_export_obj(path)`: world -> Wavefront OBJ with
  `v x y z r g b` vertex colors (Blender-style), `vn` normals, `o blk<id>`
  groups; built-ins use the meshgen soup through the instance transform,
  fused models emit child soups in model space normalized like bake_mesh,
  sculpted models emit indexed `mv`/`mf`.
- `src/panel_world.c` — WORLD tab extracted from panel.c: name field,
  10-button grid (SAVE/LOAD/UNDO/REDO/CLEAR/SAVEAS/DIR/EXPORT/IMPORT/MESH —
  MESH = .obj export via export.c), env
  presets (env.bw: SKY/GROUND colors + FLOOR pattern GRID/CHECK/DOTS/PLAIN),
  WSet settings rows + wipe-warning confirm dialog.
  panel.c delegates via `panel_world_tab_press/_draw/_confirm_click/
  _confirm_draw`; shared chrome constants (`PW`, `in_rect`, `panel_x`)
  live in panel.h.
- `src/prof.c` / `prof.h` — frame cost logging. `prof_zone_begin/end(PZ_*)`
  around frame phases; `prof_count(PC_*)` for draws/insts/verts/blocks/
  particles (`PC_PARTS`). GPU times come from `rk_gpu_times` (per-in-flight
  timestamp query pools: shadow / main / post / total). `prof_frame()`
  accumulates a 120-frame ring; `prof_draw()` renders the overlay — F3
  cycles off->page1->page2->off; page 2 = per-kind queue insts
  (`rk_queue_insts`), pool usage (`rk_pool_usage`), WSet + cam summary.
  Add zones, not printf spam.
- `src/particles.c` / `.h` — fixed-pool (1024) CPU particle system:
  `particles_burst(kind, pos, vel, n)` with FIRE/SMOKE/DEBRIS/SPARK presets,
  gravity+drag sim, life-fade size/alpha. Draws via `rk_ghost` instanced
  cubes — GLOW-flagged kinds feed the bloom bright pass for free.
  Emitters: script trigger edges in `physics.c` (BOOST→smoke/sparks,
  DELETE→debris, etc.) + placement puff in `edit.c`.
- `src/settings.c` — settings.bw v3: window rect/fullscreen, cam speed/fov/
  invertY, prof overlay, guide toggle + seen bitmask (pad-carved, same size),
  v2 saveDir, v3 autoSec (0=file-default 90s, <0 = off) + sens (mouse-look
  multiplier, `cam_look` * A.sens).
  `settings_load` pre-window, `apply` post-cam_init,
  `save` on toggle + dirty flag. Keys: Y invert, -/= fov, G guide,
  ,/. sens, [/] autosave cycle (30/60/90/180/300/600/OFF).
- `src/log.c` — `log_init()` at main() start tees fd-2 through a pipe:
  every `fprintf(stderr)` mirrors to real stderr AND `varlera.log`
  (truncated per run); glslc children inherit the pipe so compile errors
  land in the log too. Win32 crash filter writes `crash_<pid>.dmp`.
- `src/toast.c` — guide toasts: 10s idle tip cycler when `A.guideOn`,
  `toast_tip(id)` = one-shot contextual tips (place per category, arm, fuse,
  sculpt, play) persisted via `A.guideSeen`; `toast_say` = transient message.
- `src/font.h` — glyphs baked into reserved atlas tile (7,7) at tex_init
  (`font_bake`); `ui_text` = one tinted `rk_ui_texrect_col` quad per char.
- `src/meshgen.c` — `meshgen_shape()` emits flat Vert soups per RK_MESH_*
  id (cube..ring + MESHGEN_PLANE); adding a shape = enum + gen fn +
  build_all off[] + panel ASSETS/CATS row.
- Screenshot: F12 or `VARLERA_SHOT=out.bmp` (auto at frame 45) —
  swapchain TRANSFER_SRC copy -> staging -> 24bpp BMP next frame.
- Other test hooks (all in `main.c` init/frame block): `VARLERA_GEN=N`
  block scatter, `VARLERA_GENGLOW=N` every-Nth block GLOW, `VARLERA_TAB=N`
  open panel tab, `VARLERA_PARTS=N` particle burst at frame 20,
  `VARLERA_EXIT=N` quit at frame N (clean-shutdown validation checks),
  `VARLERA_SAVE`/`VARLERA_BWX`/`VARLERA_BWXIN`/`VARLERA_OBJ` world IO at
  frame 50,
  `VARLERA_MENU=1` enter the main menu at frame 10.
- `src/menu.c` — main menu (`APP_MENU`, enum value 2 in `AppMode`): boots
  by default unless any VARLERA_* test env is set; scans `worlds_dir` for
  `*.bw`/`*.bwx` newest-first (Win32 `_findfirst`, POSIX `readdir`),
  click-select + click-open, Up/Down/Enter keys, wheel scroll,
  RESUME/NEW/IMPORT/QUIT buttons; world renders behind the dim overlay.
  MENU button (246,10 next to PLAY/SCULPT) returns to it; Esc/Tab/RESUME
  go back to editing.
- Shader hot-reload: F8 or `VARLERA_RELOAD=1` (fires once at frame 60).
  `rk_reload_shaders` re-runs glslc per shader (GLSLC + SHADER_SRC baked as
  compile defines; CreateProcess CREATE_NO_WINDOW on Win32, system() on
  Linux), failed compiles keep the old .spv, then all 14 pipelines are
  destroyed/recreated under vkDeviceWaitIdle.
- Frustum culling: `aabb_in_frustum` (math3d) in edit_draw_world;
  off-camera opaque blocks go to `rk_caster` (K_CASTER = shadow pass
  only) via `rk_can_shadow_cast` (light-space ortho rect extruded).
- Sky: K_SKY/pSky fullscreen-triangle gradient + sun disc
  (sky.vert unprojects through inverse VP; env colors drive it).
  `sky_objects()` in main.c emits up to 4 `rk_world` model instances on a
  400-unit dome (under the 500-unit far plane) when `WSet.skyAssets` is
  on — slot 0 tracks `light_dir` (sun replacement), slots 1-3 sit at
  fixed az/el; UNLIT+NOFOG flags. WORLD tab `SKY OBJS` row gates 4 model
  pickers via packed row visibility. Day/night: `WSet.dayCycle` speed +
  persisted `dayPhase`; `world_day_tick` advances phase, `world_sun_dir`
  orbits the sun (below-horizon reflects to a moon dir), `rk_set_day(k)`
  lerps env colors to a dim blue night tint (rk keeps env bases in G);
  `world_day_set`/`world_day_reload` for SET-TIME/load re-seed.
- `src/font.h` — 5x7 bitmap font rendered as UI rects (`ui_text`/`ui_text_clip`).
- `src/math3d.h` — `v3` union (`{x,y,z}` / `.e[3]`), column-major `m4` float[16],
  `m4_perspective_rz`/`m4_ortho_rz` reversed-Z variants.

## Controls

Cam: RMB look | WASD+QE fly (smoothed velocity) | Space/Shift up/down |
wheel=zoom to cursor | ctrl+wheel=speed | MMB pan | F focus. Build: drag asset
to place (release on panel = arm; wheel rotates ghost). While armed each LMB
click places and stays armed; RMB click or Esc disarms. Placement attaches to
the hit block's face snapped to that block's own lattice (clamped inside the
face), and any ghost that would overlap an existing block lifts on top of it -
placements can never embed. Paste lifts the whole formation the same way |
LMB select, drag past
6px = move | alt+drag = clone-drag | RMB click = eyedropper color pick |
double-click = select same color | shift multi | marquee | axis handles resize |
R/T rotate +-90 | arrows nudge 0.5 view-relative | PgUp/PgDn nudge Y |
ctrl+C/X/V clipboard | ctrl+D dup | ctrl+A all | ctrl+Z/Y | ctrl+S or F5 save |
ctrl+B fuse sel -> custom model | ctrl+U unfuse | F9 load | Del delete |
Drop files on the window: .bw loads a world (undo-pushed), .bwx imports a
bundle, .obj opens the sculpt modal pre-loaded, .png imports a texture |
TAB or PLAY toggles play mode (gravity + script triggers + restore) |
Esc cancel/edit | F3 prof overlay (stderr `prof |` report every 4s:
fps/frame avg,p95,max + wait/sim/queue/submit CPU zones + GPU
shadow/main ms + draws/inst/ui-verts/blocks/models).

Scripting (BW-style): select a block -> docked menu -> cycle TRIG
(ALWAYS/SPACE/W/A/S/D/E/Q/CLICK), ACT (NONE/SPIN/GLOW/BOOST/COLOR/TEX/
DELETE/SAY/TITLE), TGT (SELF/ASSET/NAME/ALL) and PARAM (0-15). SPIN/GLOW/
BOOST are held acts; COLOR/TEX/DELETE are discrete — they fire on the
trigger edge (`s_prevHeld` diff in world_physics) and apply to the TGT
set (`tgt_match`). SAY/TITLE are edge-triggered global text acts:
`g_msgs[16][32]` world message table (MSG row edits the actParam slot,
textMode 5): SAY shows via `toast_say`, TITLE via `toast_title` — a
full-width gold-ruled scrim band + big centered text (fades in/out,
panel-clipped, own channel so a SAY can run under it); stored as a
`BWMS` file trailer.
NAME edits `Block.name` (textMode 3, `renBuf`, commit via
`panel_rename_commit` keyed on stable `Block.id`); TGT NAME targets blocks
sharing that name. ACT_COLOR paints `g_palette[actParam&15]` — the shared
16-color table in world.c that the PAINT tab also uses. Legacy BF_SPIN
behaves like ALWAYS+SPIN.
RMB on a SCULPTED/MODELS/PROPS cell opens a DELETE/KEEP confirm dialog —
on confirm placed instances auto-unfuse and mesh ids re-bake dense; ESC or
any non-DELETE click cancels. Eyedropper (RMB world click) picks
color+texture.

Modals (main.c routes all input by A.modal): 1 = pixel-art tool (art.c,
TEX tab EDIT/NEW/SHEET at the top; tools PEN/ERASE/FILL/LINE/RECT/PICK/SEL +
MIRROR/UNDO/CLEAR/ONION/PLAY, wheel zoom, RMB pan, per-stroke snapshot
(SEL = marquee select: drag inside lifts+moves w/ ghost preview, Del clears,
Ctrl+C/X/V clipboard + follow-mouse paste ghost, Esc deselects),
undo, DONE commits + saves, CANCEL/ESC discards; multi-tile sheets edit
one tile per canvas with a frame strip + onion skin for flipbook anim —
TEXM_ANIM mode plays tiles row-major at tex_fps per texture), 2 = sculpt mode (sculpt.c, SCULPT
button top-left next to PLAY or SHAPES footer). Editor is paused while a
modal is open; camera still flies. Sculpt: direct vertex editing on a welded tri mesh —
LMB grabs a vertex dot and drags on a camera-parallel plane (snapped to the
marker grid), short RMB click splits the nearest edge (RMB drag still looks),
Del removes the selected vertex, release welds overlapping verts and splits
crossing edges. The 1X/2X/4X cube is a size marker only — the mesh may
exceed it, and a second outline then shows the true AABB. DONE bakes to a
CUSTOM model; RESCULPT row reloads + edits it in place. models.bw v6 stores
verts/faces (nch=-2 record); v4/v5 voxel saves migrate through
sculpt_bake_verts + weld_topo. Unfuse explodes a sculpted model into one
micro-cube per vertex. Model meshes are normalized to the unit box at bake
time (world instancing multiplies by block size — never store world-space
extents in a baked mesh). Sculpt sidebar: MOVE/ADDV/PART tools, DEL V/
SMOOTH/WELD, DOT DENSITY + SUBDIV, snap 0.125/0.25/0.5, MIRROR off/X/4-WAY,
START shape picker (6 presets), PASTE/IMPORT/GRID, palette, MATERIAL row
(TEX: cycles texture layer on the PART island or whole mesh, LOCK freezes
that layer's color+texture - locked verts refuse later edits), per-island
dot colors (union-find components tint dots so disconnected objects read
apart), OPT -50% = QEM decimate (`src/decimate.c` — face+boundary
quadrics, cheapest-edge collapse, verts compact with their color/tex/
lock attrs). verts carry `s_vtex`/`s_vlock` through every split/weld/delete.
- `src/marching.c` — legacy density-field bake (marching tets over SC_N³
  grid) + voxel payload unpackers; only used for models.bw <= v5 migration.

Window: GUI subsystem (/ENTRY:mainCRTStartup, no console; stderr still works
when redirected). Starts borderless fullscreen on the primary monitor;
F11 toggles back to the saved windowed rect.

## Verify

Debug config runs with the validation layer enabled — run it and check stderr for
VK_VALIDATION messages after changing renderer code.

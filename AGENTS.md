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
`cmake -B build -S . && cmake --build build`. CMake falls back to
`find_package(Vulkan)` + `find_package(glfw3)` when vcpkg vars are absent, and
links m/dl/pthread. Win32-only code (file dialog, DPI awareness) is `_WIN32`
-guarded; OBJ import on Linux uses zenity or `import.obj` beside the exe.

## Layout

- `src/rk.c` / `rk.h` — renderer. Vulkan 1.3 dynamic rendering, reversed-Z main depth
  (clear 0, GREATER), 4x MSAA + resolve, 2048px directional shadow map (forward-Z
  ortho `m4_ortho`, hardware-compare sampler + 4-tap PCF), instanced drawing
  (`RkInst` = pos/scale/RGBA8/flags; bits: 0 unlit, 1-2 rot90, 3 nofog, 4 iconVP),
  deferred command queue replayed for shadow pass. UI quads are NDC rects
  (`UIVert` pos/col/uv/tex — `rk_ui_rect` solid, `rk_ui_texrect` samples the
  atlas via set1 binding 1 for real texture previews). Manual VkDeviceMemory
  (VMA 3.x is C++-only — do not re-add to a C target). `rk_init` returns 1 on
  success.
- `src/app.h` — shared `App A` global (window, camera, mode, selection, drag state).
- `src/main.c` — entry, glfw callbacks -> dispatch, frame loop.
- `src/camera.c` — free-fly cam (yaw/pitch basis, ray gen, WASD/QE move).
- `src/edit.c` — world editing: ray pick (scaled unit-AABB slabs), place ghost,
  move/resize/marquee state machine, selection outlines + 6 per-face resize
  handles (opposite face anchored, block recenters), fuse/unfuse (ctrl+B/U).
- `src/model.c` — fused-block baking: per-child vertex colors + texture layers
  (`Vert.tex` float, -1 = inherit), internal cube face culling, `models.bw`
  persistence + `rk_model_mesh_add` upload. v7 stores per-vert material id
  (`mvtex`, int8 -1 = inherit) + layer lock (`mvlock`) + `adv` flag; v6 and
  older migrate. `models_bake_builtins()` seeds the 10 premade PROPS assets
  (CRATE/TABLE/CHAIR/PERSON/TREE/FLOWER/ARCH/LAMP/FENCE/PILLAR) on first run
  or after loading a pre-v7 file.
- `src/panel.c` — right panel with 6 tabs: SHAPES (categories BASIC/RAMPS/
  SPECIAL/SCULPTED, 15 assets + sculpted models, scrollable, tooltips), PAINT
  (16 presets + HSV picker + custom swatches + ALPHA slider -> `A.paintAlpha`
  -> `Block.alpha`/`BF_GLASS`), TEX (ART TOOL buttons EDIT/NEW/SHEET on top ->
  `art_open`, MODE/TYPE cycling `A.paintTexM`, scrollable grid of real atlas
  previews via `rk_ui_texrect`), WORLD (SAVE/LOAD/UNDO/REDO/CLEAR + block count
  + ENVIRONMENT: SKY x6 / GROUND x5 preset cycler -> `rk_set_env`, saved to
  `env.bw`), MODELS (fused block groups), PROPS (`adv` premade assets).
  Asset geometry, paint color and texture are fully decoupled. Script menu
  docks left of panel on select: GRAVITY/COLLIDE/LOCKED toggles + TRIG/ACT
  cycle rows (BW scripting) + UNFUSE.
- `src/world.c` — block store (`Block` 44B: pos/size/col/shape/rot/tex/flags/
  trig/act), undo/redo stacks, `world.bw` binary save/load (v2; reads v1),
  play-mode gravity settle + snapshot/restore.
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
TAB or PLAY toggles play mode (gravity + script triggers + restore) |
Esc cancel/edit.

Scripting (BW-style): select a block -> docked menu -> cycle TRIG
(ALWAYS/SPACE/W/A/S/D/E/Q/CLICK) and ACT (NONE/SPIN/GLOW/BOOST). In play mode
the action runs while the trigger key/button is held; BOOST is a thruster
(velY += 30/s up to 10 u/s). Legacy BF_SPIN behaves like ALWAYS+SPIN.
RMB on a SCULPTED/MODELS/PROPS cell opens a DELETE/KEEP confirm dialog —
on confirm placed instances auto-unfuse and mesh ids re-bake dense; ESC or
any non-DELETE click cancels. Eyedropper (RMB world click) picks
color+texture.

Modals (main.c routes all input by A.modal): 1 = pixel-art tool (art.c,
TEX tab EDIT/NEW/SHEET at the top; tools PEN/ERASE/FILL/LINE/RECT/PICK +
MIRROR/UNDO/CLEAR, wheel zoom, RMB pan, per-stroke snapshot undo, DONE
commits + saves, CANCEL/ESC discards), 2 = sculpt mode (sculpt.c, SCULPT
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
apart). verts carry `s_vtex`/`s_vlock` through every split/weld/delete.

Window: GUI subsystem (/ENTRY:mainCRTStartup, no console; stderr still works
when redirected). Starts borderless fullscreen on the primary monitor;
F11 toggles back to the saved windowed rect.

## Verify

Debug config runs with the validation layer enabled — run it and check stderr for
VK_VALIDATION messages after changing renderer code.

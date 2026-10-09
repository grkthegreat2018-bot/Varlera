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

## Layout

- `src/rk.c` / `rk.h` — renderer. Vulkan 1.3 dynamic rendering, reversed-Z main depth
  (clear 0, GREATER), 4x MSAA + resolve, 2048px directional shadow map (forward-Z
  ortho `m4_ortho`, hardware-compare sampler + 4-tap PCF), instanced drawing
  (`RkInst` = pos/scale/RGBA8/flags; bits: 0 unlit, 1-2 rot90, 3 nofog, 4 iconVP),
  deferred command queue replayed for shadow pass. Manual VkDeviceMemory (VMA 3.x
  is C++-only — do not re-add to a C target). `rk_init` returns 1 on success.
- `src/app.h` — shared `App A` global (window, camera, mode, selection, drag state).
- `src/main.c` — entry, glfw callbacks -> dispatch, frame loop.
- `src/camera.c` — free-fly cam (yaw/pitch basis, ray gen, WASD/QE move).
- `src/edit.c` — world editing: ray pick (scaled unit-AABB slabs), place ghost,
  move/resize/marquee state machine, selection outlines + 6 per-face resize
  handles (opposite face anchored, block recenters), fuse/unfuse (ctrl+B/U).
- `src/model.c` — fused-block baking: per-child vertex colors, internal cube
  face culling, `models.bw` persistence + `rk_model_mesh_add` upload.
- `src/panel.c` — right panel with 3 tabs: SHAPES (collapsible categories SIMPLE/
  SLOPES/ROUND/CUSTOM, 20 assets + baked models, scrollable, tooltips), PAINT
  (16 presets + HSV picker + texture row: NONE/STUDS/CHECK/STRIP/DOTS/GRID),
  WORLD (SAVE/LOAD/UNDO/REDO/CLEAR + block count). Asset geometry, paint color
  and texture are fully decoupled. Script menu docks left of panel on select:
  GRAVITY/COLLIDE/LOCKED toggles + TRIG/ACT cycle rows (BW scripting) + UNFUSE.
- `src/world.c` — block store (`Block` 44B: pos/size/col/shape/rot/tex/flags/
  trig/act), undo/redo stacks, `world.bw` binary save/load (v2; reads v1),
  play-mode gravity settle + snapshot/restore.
- `src/font.h` — 5x7 bitmap font rendered as UI rects (`ui_text`/`ui_text_clip`).
- `src/math3d.h` — `v3` union (`{x,y,z}` / `.e[3]`), column-major `m4` float[16],
  `m4_perspective_rz`/`m4_ortho_rz` reversed-Z variants.

## Controls

Cam: RMB look | WASD+QE fly (smoothed velocity) | Space/Shift up/down |
wheel=zoom to cursor | ctrl+wheel=speed | MMB pan | F focus. Build: drag asset
to place (release on panel = arm; wheel rotates ghost) | LMB select, drag past
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
RMB on a CUSTOM cell deletes the model — placed instances auto-unfuse and
mesh ids are re-baked dense. Eyedropper (RMB world click) picks color+texture.

## Verify

Debug config runs with the validation layer enabled — run it and check stderr for
VK_VALIDATION messages after changing renderer code.

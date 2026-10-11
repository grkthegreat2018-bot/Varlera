# Varlera Roadmap

Sorted easy → hard. Effort is rough engineering scale: **S** = days,
**M** = 1-2 weeks, **L** = multi-week. "Touches" names the real
subsystems involved. Do items in a tier in any order; earlier tiers
unblock and de-risk later ones.

Measured baseline (profiler, `F3` / stderr `prof |`): GPU ~0.04ms,
CPU queue ~0.08ms, `uiv` ~8k verts/frame — UI text-as-rects is the
largest CPU cost in an empty scene. Re-measure after each perf item.

## Tier 1 — easy wins

| Feature | Effort | Touches | Notes |
|---|---|---|---|
| Font atlas (text as textured quads) | S | `font.h`, `tex.c`, `rk_ui_flush` | Kills ~8k verts/frame of rect-built glyphs. Highest measured ROI. |
| Frustum culling in `edit_draw_world` | S | `edit.c`, `math3d.h` | Skip blocks/models outside camera planes before `rk_world`/`rk_ghost`. Needed before world scale grows. |
| Settings file (`settings.bw`) | S | `main.c`, `camera.c` | FOV, sens, invert-Y, prof autostart, vsync. Same `bw` binary pattern as env/undo files. |
| Better default stylized skybox/ground | S | `rk.c`, `edit.c` env presets | Gradient dome + sun disc draw before world; ground plane styles per env preset. `rk_set_env` already exists. |
| Side panel polish | S | `panel.c` | Use `rk_draw_icon` (renders real meshes with material) for block icons; spacing/hover states; collapse animation. |
| Object indexing + metadata | S | `world.c`, `model.c`, `panel.c` | Name/tag/search fields on models + palette entries; filter box in MODELS tab. |
| More default block shapes | S | `meshgen.c`, `panel.c` | Wedges, corners, pipes, cylinders — `meshgen_shape` is table-driven; each new shape = verts + SHAPES row. |
| QOL pass | S | `panel.c`, `edit.c`, `main.c` | Hotkey hints bar, action toasts, drag multi-select in MODELS, block search, ghost preview in panel tooltips. |
| Debugging v1 | S | `prof.c`, `main.c` | Overlay page 2 (per-zone sparklines + counters), `varlera.log` file mirror of stderr, Win32 crash minidump (`_WIN32` guard), `VK_VALIDATION` hotkey toggle. |
| Material flags | S→M | `world.c`, `tex.c`, shader | Rough/metal/emissive bits into `texm`/instance flags + shader branches (matte vs glossy, glow feeds bloom). Foundation for shader effects. |

## Tier 2 — medium (one subsystem each)

| Feature | Effort | Touches | Notes |
|---|---|---|---|
| Overhauled texture menu | M | `panel.c` TEX tab | UX rework of existing palette editor: drag slots, import/export PNG strip, per-texture preview. Stepping stone to the art tool. |
| Aseprite-like art tool | M | `panel.c`/`tex.c` (or new `art.c`) | Layers + opacity, selection/move/fill/line tools, color pick+swatches, undo stack, zoom pan. Self-contained; worth extracting from `panel.c` once it grows past ~1k lines. |
| Better world data handling | M | `world.c` save/load | Format v4: header with name/created/modified, `worlds/` dir, thumbnails (render one frame offscreen or save atlas cell), autosave timer, load-version migration (v3 reader already exists). |
| Better world file size | M | `world.c` save/load | Folds into v4: quantize pos/size to grid, RLE over sorted blocks (long runs share shape/tex/col), varint lengths, optional embedded deflate (small public-domain impl fits the no-dep style). Expect ~3-5x on typical worlds. |
| Better I/O | M | `main.c`, `world.c`, `tex.c` | One save worker thread (async world+models+atlas), export world→OBJ and atlas→PNG, import OBJ drag-drop, file dialogs on every path (Win32 common dialog exists; zenity guarded on Linux). |
| Proper main menu / world menu | M | `main.c` + new `menu.c` | Needs an app-state enum (menu / edit / play) — `main.c` is currently one monolithic loop. World list browser reuses `modal_load` file picking. |
| Better mesh handling | M | `meshgen.c`, `model.c`, `rk.c` | Per-model vertex cache keyed by hash (skip re-bake when unchanged), merge `rk_world` draws by material/shape, single upload for all model VBs. |
| Play-mode greedy mesh (frozen blocks) | M | `world.c`, `meshgen.c` | At play entry: union-find over touching unsupported blocks with no script → one combined mesh/instance; re-splits when a member gets a trigger/action. Cuts draw+physics cost for static scenery. |
| Better sculpted object rendering | M | `sculpt.c`, `model.c` bake, shader | Smooth/flat shading toggle per model, better facet normals, cheap LOD (decimate topo at distance). |
| Memory handling | M | all | Arena allocators for per-frame UI verts + bake buffers (kills malloc churn), pool for `Block`s, `prof_count` hook for peak/alloc counts. |
| Better sculpt mode | M | `sculpt.c` | Brush sizes/falloff, mirror symmetry, smooth/relax vertex op, rotate/scale gizmo, undo already exists. Incremental on the 71KB module. |
| Particle system | M | new `particles.c`, `rk.c`, `tex.c`, `world.c` | Fixed pool (~4k): billboard sprites sampling atlas art (2D) + instanced sculpted-mesh particles via the `RkInst` path (3D). Emitters keyed by `trig`/`act` rows (SPIN→sparks, BOOST→smoke); gravity/fade params; `prof_count` a `PC_PARTICLES` slot. No new draw infrastructure needed. |
| Post chain: bloom + tonemap | M | `rk.c`, `rk_vk.c`, new shaders | Downsample+blur+composite after MSAA resolve (resolve pass already exists); emissive material flags feed the bright pass. Adds filmic tonemap — current output is clamped sRGB. |
| Per-vertex AO bake | M | `model.c` bake, `meshgen.c` | Raycast occlusion per baked vert (blocks + sculpted), stored in vert color multiplier — soft contact shading for free at draw time. |
| Live collision overhaul | M→L | `world.c`, `edit.c`, `camera.c` | Continuous collision for fast/moving blocks (current settle assumes dt steps), capsule player collider, joint-constraint solver groundwork for NPCs. |

## Tier 3 — hard (cross-cutting)

| Feature | Effort | Touches | Notes |
|---|---|---|---|
| Full occlusion culling | L | `rk.c`, `rk_vk.c`, new shader | Vulkan path: depth-pyramid (Hi-Z) build + `vkCmdDrawIndexedIndirectCount` with per-instance visibility. CPU path (cheaper): block PVS per cell for indoor scenes. Do frustum culling first. |
| Code blocks + scripting v2 | L | `world.c` (trig/act rows exist), `panel.c`, `main.c` | Conditions (IF trig THEN act), per-world variables, wire-mode link UI, event bus, timers/sensors/counters/loops. "Massive expansion" = this done fully + a script debugger overlay in F3. |
| Shader effects via code blocks | L | `world.c`, `rk.c`, shaders | Cheap version lands with material flags (M): effect id + params in instance bits → glow pulse, dissolve, wave, blink. Full version rides scripting v2: actions bind effects to triggers, script vars drive params (speed/intensity), per-world effect table. |
| Overhauled building | L | `edit.c`, `panel.c`, `world.c` | Rework of the place/move state machine: mirror-symmetry build mode, line/plane fills, arbitrary-axis rotate (current is 90° steps + rot90 flags), structural stress preview, smart attach (auto-snap to stud/edge lattices), clipboard templates library. |
| Custom NPCs (sculpt + joint planes) | L | `sculpt.c`, `model.c`, `world.c`, new anim | Sculpted limb pieces + joint-plane attachment points → articulated hierarchies; anim state machine; NPC AI hooks into scripting v2. Depends on joints/physics groundwork from live-collision item. |
| Threading + GPU compute | L | `world.c`, `meshgen.c`, `rk_vk.c` | Job pool for mesh bakes + broadphase (Win32 threads, `_WIN32` guarded). Vulkan compute for physics queries — prefer compute over CUDA (no new dep, works on all GPUs); CUDA only if a profiling-measured kernel justifies it. |

## Tier 4 — long-term / engine-level

| Feature | Effort | Notes |
|---|---|---|
| Audio | M | `miniaudio.h` single-header fits the no-dep style. Sounds tied to script actions + particle emitters. |
| Day/night + env animation | M | Sun/light dir lerped on a world-time var; scripting v2 can drive it (ALWAYS + SET_TIME). |
| Player/avatar controller | M | Needs collision overhaul first. Walk/jump/interact in play mode. |
| Game objects (health, respawn, goals) | M | On top of scripting v2. |
| Modding / file-format docs | M | Public spec for `.bw` + texture PNG strip. |
| Multiplayer | XL | Everything above first; deterministic lockstep vs server-auth is its own design. |

## Suggested order

1. Font atlas → frustum culling → settings file (measured wins, tiny).
2. Skybox/ground defaults + panel polish + indexing + new shapes +
   QOL/debugging v1 (visual + feel payoff, all S).
3. Texture menu → art tool (extraction point: new `art.c`).
4. World data v4 (incl. file-size compression) + better I/O + menus
   (app-state refactor is the gateway to everything UI).
5. Greedy-mesh play mode + mesh cache + material flags + particle
   system + bloom/AO (perf and look, needed at scale).
6. Collision overhaul → building overhaul → player controller
   (gameplay unlock).
7. Scripting v2 (the "engine" pivot) → shader effects → NPCs
   (joints ride on it).
8. Occlusion culling + threading only when `prof` shows sim/submit
   zones actually limiting fps at real world sizes.

Defer until profiler says so: Hi-Z occlusion, GPU physics, chunk
streaming — all premature before `sim`/`queue` zones exceed budget
(16.6ms at 60fps; currently 0.55ms total in an empty scene).

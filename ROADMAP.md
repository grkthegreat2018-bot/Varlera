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
| More default block shapes | S | `meshgen.c`, `panel.c` | Wedges, corners, pipes, cylinders — `meshgen_shape` is table-driven; each new shape = verts + SHAPES row. Port BW's full basic-shape set for parity. |
| User settings + controls | S→M | `main.c`, `camera.c`, new `settings.c` | Extends settings file: rebindable keys, theme tint, UI scale, camera presets, autosave interval. |
| QOL pass | S | `panel.c`, `edit.c`, `main.c` | Hotkey hints bar, action toasts, drag multi-select in MODELS, block search, ghost preview in panel tooltips. |
| Debugging v1 | S | `prof.c`, `main.c` | Overlay page 2 (per-zone sparklines + counters), `varlera.log` file mirror of stderr, Win32 crash minidump (`_WIN32` guard), `VK_VALIDATION` hotkey toggle. |
| Material flags | S→M | `world.c`, `tex.c`, shader | Rough/metal/emissive bits into `texm`/instance flags + shader branches (matte vs glossy, glow feeds bloom). Foundation for shader effects. |
| Shader hot-reload | S | `rk_vk.c`, `CMakeLists.txt` | F8: re-run glslc + recreate pipelines in place. Dev-velocity win for every shader item below; pipelines are already isolated creation code. |

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
| Auto mesh optimize | M | `model.c`, `sculpt.c` | Quadric-error edge collapse decimation with a target-tri-count or error-budget knob, aggressive weld dedupe. Run at bake + as a manual OPTIMIZE button; sculpted LOD variant reuses it. |
| Blender auto-conversion | M | `model.c`, `sculpt.c`, new `import.c` | OBJ/GLB import → weld → normalize-to-unit-box → bake (`import.obj` path already exists; extend to drag-drop + GLB). Optional Blender-side exporter script for direct `.bw` write. |
| 2D animation (flipbook) | M | `tex.c`, `art.c`, `world.c`, shader | Atlas strips = frames; blocks/materials get frame-count+fps bits; shader or CPU-side UV offset. Art tool gets onion-skin + frame timeline. Feeds particles + shader effects. |
| Asset manifest | M | new `assets.c` | One index file (paths, hashes, version) for worlds/models/atlas/scripts — basis for modding + per-world texture packs later. |
| Stress test scripts | M | new `tests/` + `world.c` | Scripted scene generators (N random blocks / fused models / worst-case glass sort / full shadow volume) run headless-ish with `prof` logging — regression baselines + feeds "when do we need occlusion" data. Ties into deterministic replay. |
| Codebase docs + tests | M→ongoing | `AGENTS.md`, new `CONTRIBUTING.md`, `tests/` | For OSS + agent velocity: architecture map, per-module ownership, style guide, smoke tests that build+run each commit. Small focused files stay a hard rule (the rk.c/sculpt.c splits proved it). |
| Object physics stats (data) | M | `world.c`, `panel.c` | `Block` gains traction/bounce/weight fields + info-panel editing — data model + persistence only; runtime effect lands with the physics items below. |
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
| Model rig support | L | `model.c`, `sculpt.c`, `rk.c`, shader | Named bones/joints stored per model (joint planes from sculpt mark attach points), per-vert bone weights baked in (`Vert` gets bone idx+weight), GPU skinning in the world VS or CPU skin for low-vert models. Foundation for NPCs + 3D anim. |
| 3D animation creator | L | `sculpt.c`/new `anim.c`, `model.c` | Keyframe pose editor on rigged models (rot/pos per joint, timeline, ease curves, play preview); clip library saved per model; runtime blends poses on instances. Depends on rig support. |
| BW rigid-body play physics | L | `world.c`, new `physics.c` | Connected-component rigid bodies (fused/touching blocks move as one mass — reuses greedy-mesh grouping), drag bodies with the mouse in play, torque/momentum. World-settings toggle: arcade settle (current) vs rigid body. Needs collision overhaul first. |
| Joint settings | L | `world.c`, `physics.c`, `panel.c` | Per-connection joint mode: LOOSE (BW parity — floppy hinges/motors) vs RIGID (welded, no play). Joint planes from sculpt mark articulation points; solver picks constraint type from mode. |
| Physics stats runtime | L | `world.c`, `physics.c`, scripting | traction/bounce/weight fields (see data row) drive contact friction, restitution, gravity scale; settable via code blocks + script actions. |
| Sound editor + music maker | L | new `audio.c` + `soundlab.c` modal | After audio engine: waveform/noise synth editor for SFX (reuses art.c modal pattern), step-sequencer music maker; sounds bound to script actions + world events. |
| BW feature parity | L | all | Umbrella for "everything Blocksworld minus community": motors/hinges/springs/magnets, action sensors, block categories parity, play-mode object dragging, world sharing (export/import file — no servers). Track as a checklist in `docs/BW-PARITY.md` once physics lands. |
| Scene/entity model | L | `world.c`, `main.c`, new `scene.c` | Generalize Block→Entity (transform + components: mesh/collider/script/emitter) so triggers, NPCs, particles, props share one update path. The pivot from "blocks world" to "engine" — do before scripting v2 explodes `Block` further. |
| Custom NPCs (sculpt + joint planes) | L | `sculpt.c`, `model.c`, `world.c`, new anim | Sculpted limb pieces + joint-plane attachment points → articulated hierarchies; anim state machine; NPC AI hooks into scripting v2. Depends on joints/physics groundwork from live-collision item. |
| Threading + GPU compute | L | `world.c`, `meshgen.c`, `rk_vk.c` | Job pool for mesh bakes + broadphase (Win32 threads, `_WIN32` guarded). Vulkan compute for physics queries — prefer compute over CUDA (no new dep, works on all GPUs); CUDA only if a profiling-measured kernel justifies it. |

## Tier 4 — long-term / engine-level

| Feature | Effort | Notes |
|---|---|---|
| Audio | M | `miniaudio.h` single-header fits the no-dep style. Sounds tied to script actions + particle emitters. |
| Day/night + env animation | M | Sun/light dir lerped on a world-time var; scripting v2 can drive it (ALWAYS + SET_TIME). |
| Player/avatar controller | M | Needs collision overhaul first. Walk/jump/interact in play mode. |
| Game objects (health, respawn, goals) | M | On top of scripting v2. |
| Deterministic replay | M | Fixed-dt sim + recorded input stream — free regression tests once scripting exists; also the basis of any future netcode. |
| Editor/runtime split | L | Separate `editor.c` from the play loop; packaged game loads scenes without edit UI. Do after scene/entity model. |
| Full game-dev toolset | XL | Umbrella: prefabs, cutscene/camera tracks, game-mode templates, publish→standalone exe bundling world+atlas+models. Lands piecewise after scene/entity + scripting. |
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
   system + bloom/AO + flipbook anim + mesh auto-optimize
   (perf and look, needed at scale).
6. Collision overhaul → building overhaul → rigid-body physics +
   joints + physics stats runtime → player controller
   (gameplay unlock).
7. Scene/entity refactor → scripting v2 (the "engine" pivot) →
   rig support → 3D anim creator → NPCs → sound/music tools
   (all ride on it).
8. Occlusion culling + threading only when `prof` shows sim/submit
   zones actually limiting fps at real world sizes.

Defer until profiler says so: Hi-Z occlusion, GPU physics, chunk
streaming — all premature before `sim`/`queue` zones exceed budget
(16.6ms at 60fps; currently 0.55ms total in an empty scene).

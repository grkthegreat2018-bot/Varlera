# Varlera Roadmap

Sorted easy → hard. Effort is rough engineering scale: **S** = days,
**M** = 1-2 weeks, **L** = multi-week. "Touches" names the real
subsystems involved. Do items in a tier in any order; earlier tiers
unblock and de-risk later ones.

Status: **✅** shipped (commit in parens) · **⏳** partially landed.

Measured baseline (profiler, `F3` / stderr `prof |`): GPU ~0.08ms
(sh 0.01 / main 0.04 / post 0.04), frame ~0.65ms total, `uiv` ~810
verts/frame (post-font-atlas; was ~8k). Biggest remaining CPU cost is
submit ~0.55ms. Re-measure after each perf item.

## Tier 1 — easy wins

| Feature | Effort | Touches | Notes |
|---|---|---|---|
| ✅ Font atlas (text as textured quads) `984f565` | S | `font.h`, `tex.c`, `rk_ui_flush` | Kills ~8k verts/frame of rect-built glyphs — landed: `uiv` 8196→756. |
| ✅ Frustum culling in `edit_draw_world` `3087517` | S | `edit.c`, `math3d.h` | Per-block AABB test before `rk_world`/`rk_ghost`; `K_CASTER` keeps off-screen shadow casters. |
| ✅ Settings file (`settings.bw`) `5be4a7d` | S | `main.c`, `camera.c`, `settings.c` | Window rect/fullscreen, speed, FOV, invert-Y, prof autostart persist. |
| ✅ Stylized skybox/ground `8d2a896`+`e9157a3` | S | `rk.c`, `edit.c` env presets | Sky pass (gradient dome + sun disc) + ground floor styles: GRID/CHECK/DOTS/PLAIN patterns via `groundCol.a`, FLOOR row in WORLD tab, env.bw v3. |
| ⏳ Side panel polish | S | `panel.c` | Search + richer tooltips landed; icon materials, hover states, collapse animation pending. |
| ✅ Object indexing + metadata `756a1fc` | S | `world.c`, `model.c`, `panel.c` | Search on SHAPES/MODELS/PROPS, info readout w/ stable id, full script rows (TRIG/ACT/TGT/PARAM/NAME), dead ACT_COLOR/TEX/DEL wired live. |
| ⏳ More default block shapes `353686d` | S | `meshgen.c`, `panel.c` | Pass 1 landed: Slab/Plate/Beam/Pipe/Arch/Ring. `6fd575f`: cageball reworked to a real birdcage. `e9157a3`: fixed RING/model-0 mesh-id collision (BASE_MODEL 16→32 + v6 save remap). BW parity pass remains. |
| ⏳ User settings + controls `5be4a7d` | S→M | `main.c`, `camera.c`, `settings.c` | Core file + FOV/invert-Y landed; rebindable keys, UI scale, autosave interval pending. |
| ⏳ QOL pass | S | `panel.c`, `edit.c`, `main.c` | Block search + toasts landed; hotkey hints bar, drag multi-select pending. |
| ⏳ Debugging v1 `d0d6685` | S | `prof.c`, `main.c`, `log.c` | `varlera.log` stderr tee + crash minidump landed; overlay page 2 + validation hotkey pending. |
| ✅ Guide toasts `a3ce468` | S | `panel.c`, `settings.c`, `main.c`, `toast.c` | 12-tip idle cycler + first-time contextual tips; `G` toggle; seen-set in settings.bw. |
| ✅ Material flags `0c3eff5` | S→M | `world.c`, `tex.c`, shader | texm bits 3-4 = MATTE/GLOSS/METAL/GLOW → world.frag Blinn spec/metal/glow; MAT cycler in TEX tab. |
| ✅ Shader hot-reload `6aef718` | S | `rk_vk.c`, `CMakeLists.txt` | F8: glslc re-run + all pipelines rebuilt in place (device-idle); `VARLERA_RELOAD` test hook. |

## Tier 2 — medium (one subsystem each)

| Feature | Effort | Touches | Notes |
|---|---|---|---|
| ~~Overhauled texture menu~~ ✅ | M | `panel.c` TEX tab | Drag-to-reorder slots via `tex_at`/`tex_move` display-order permutation (persisted as a textures.bw trailer, NONE pinned at 0), PNG import/export strip (stb), per-texture footer preview (thumb + dims + ANIM flag), drag ghost follows cursor. |
| Aseprite-like art tool | M | `panel.c`/`tex.c` (or new `art.c`) | Layers + opacity, selection/move/fill/line tools, color pick+swatches, undo stack, zoom pan. Self-contained; worth extracting from `panel.c` once it grows past ~1k lines. |
| ✅ Better world data handling `7db72fe` | M | `world.c` save/load | Format v4: 56B header (name/created/modified/nextid), `worlds/` dir + name field, `.bmp` thumbnails on save, 90s autosave (`world_rev` dirty check), v1-v3 readers kept. |
| ✅ Better world file size `7db72fe`+lzss | M | `world.c`, `lzss.c` | Quantized records + v7 LZSS blob: ~15.6B/blk on mixed scenes (~4x vs v3). |
| ⏳ Better I/O `7db72fe` | M | `main.c`, `world.c`, `tex.c` | `worlds/` layout + thumbs + autosave landed. Pending: async save thread, world→OBJ / atlas→PNG export, OBJ drag-drop, **Save-As to a user-chosen folder** (`GetSaveFileName`). |
| ✅ Portable world bundle | M | `bundle.c` | `.bwx` = magic/ver + 3 part records (world.bw / models.bw / atlas) via temp-file concat — no new formats. WORLD tab EXPORT/IMPORT buttons; `VARLERA_BWX`/`BWXIN` hooks verified end-to-end. Provenance block pending. |
| World settings system | M | `world.c`, `panel.c` WORLD tab | Per-world settings record in `env.bw` v2: physics mode, gravity scale, chunking, LOD, sky assets, render flags. Fields that alter stored data get a destructive-change confirm dialog ("may wipe/reshape this world") before applying. Foundation row for the Tier-3 settings below. |
| Skybox objects | M | `rk.c` sky pass, `model.c`, `panel.c` | ✅ Landed: 4 slots carved from `WSet.pad` (`skyObj[4]`, 0=none / 1+=model index), `SKY OBJS` gate row + 4 model pickers in WORLD tab (packed visibility keeps row indices stable). Slot 0 rides `light_dir` (sun replacement), others at fixed azimuth/elevation; `sky_objects()` in main.c emits `rk_world` instances on a 400-unit dome (under the 500 far plane), UNLIT+NOFOG so they read as distant sky elements. `VARLERA_SKYOBJ=1` test hook verified (crate-as-sun screenshot). |
| Advanced rendering flags | M | `rk.c`, `rk_vk.c`, WORLD settings | Per-world EXPERIMENTAL section: shadow map size, PCF radius, MSAA toggle, fog curve, custom shader path — behind the wipe-warning pattern; gives experienced users control without destabilizing defaults. |
| Proper main menu / world menu | M | `main.c` + new `menu.c` | Needs an app-state enum (menu / edit / play) — `main.c` is currently one monolithic loop. World list browser reuses `modal_load` file picking. |
| Better mesh handling | M | `meshgen.c`, `model.c`, `rk.c` | Per-model vertex cache keyed by hash (skip re-bake when unchanged), merge `rk_world` draws by material/shape, single upload for all model VBs. |
| ~~Play-mode greedy mesh (frozen blocks)~~ ✅ | M | `physics.c`, `model.c` | At play entry `merge_build` union-finds touching clusters of script-free eligible blocks → `model_bake_transient` (no Model record) → one instanced draw each via `world_merged_draw`. Members keep g_blocks slots (support/pick intact) but skip draw+settle via BF_MERGED; script touch / drag grab / delete dissolves the cluster; restore rebakes persistent slots only. Scripted/reachable, translucent, material-finished, spinning, locked, and non-STRETCH textured blocks stay unmerged. |
| Better sculpted object rendering | M | `sculpt.c`, `model.c` bake, shader | Smooth/flat shading toggle per model, better facet normals, cheap LOD (decimate topo at distance). |
| Memory handling | M | all | Arena allocators for per-frame UI verts + bake buffers (kills malloc churn), pool for `Block`s, `prof_count` hook for peak/alloc counts. |
| Better sculpt mode | M | `sculpt.c` | Brush sizes/falloff, mirror symmetry, smooth/relax vertex op, rotate/scale gizmo, undo already exists. Incremental on the 71KB module. |
| Assembly sculpt mode | M | new `build.c` modal | Second sculpt flavor for non-mesh-editors: place basic shapes as mini-blocks inside a model volume — per-block size + rotation (reuses placement/ghost logic + `model.c` bake). Sculpt stays the "pro" path; this is the accessible one. |
| Material layers in sculpt | M | `sculpt.c`, `build.c`, `model.c` | Extends existing `s_vtex`/`s_vlock` layer machinery: mark parts as material layers, each layer its own color/texture; works in both sculpt modes; assembly mode gets per-mini-block layer assign. |
| Assembly auto-merge on save | M | `build.c`, `model.c` bake | Greedy merge same-material/same-shape touching mini-blocks into bigger meshes at save (sibling of the play-mode greedy mesh). Catches "user built a big cube out of small cubes" — collapses to one box. |
| ⏳ Premade object packs | M | `model.c` (`models_bake_builtins`), `panel.c` | Pack 2 landed: +12 (DOOR/BUTTON/LEVER/SIGN/CAR/BARREL/LADDER/COIN/SPIKES/PLATFORM/CANNON/GEAR) → 22 total, v8 seeding + `dedupe_adv`. More packs welcome; mechanical ones activate when joints/scripting ship. |
| ⏳ Dialog/text blocks (basic) | M | `world.c` ACT_* rows, `panel.c`, `font.h` | ACT_SAY/ACT_TITLE landed: `g_msgs[16][32]` table (BWMS trailer) + MSG edit row, fires via toast on trigger edge. Pending: title-card styling, per-block strings, dialog trees (scripting v2). |
| Auto mesh optimize | M | `model.c`, `sculpt.c` | Quadric-error edge collapse decimation with a target-tri-count or error-budget knob, aggressive weld dedupe. Run at bake + as a manual OPTIMIZE button; sculpted LOD variant reuses it. |
| Blender auto-conversion | M | `model.c`, `sculpt.c`, new `import.c` | OBJ/GLB import → weld → normalize-to-unit-box → bake (`import.obj` path already exists; extend to drag-drop + GLB). Optional Blender-side exporter script for direct `.bw` write. |
| ⏳ 2D animation (flipbook) | M | `tex.c`, `art.c`, `world.c`, shader | Landed: TEXM_ANIM mode 3 — a multi-tile sheet's tiles play as row-major frames stepped by `camPos.w * texFps[id]` in world.frag; per-texture fps lives in a `s_fps` table (textures.bw tail trailer, back-compat), edited via the TEX tab FPS button shown in ANIM mode. `VARLERA_ANIM` + comma-separated `VARLERA_SHOT=a,b` hooks verified frames advance. Pending: art-tool frame timeline + onion-skin. |
| Asset manifest | M | new `assets.c` | One index file (paths, hashes, version) for worlds/models/atlas/scripts — basis for modding + per-world texture packs later. |
| ⏳ Stress test scripts | M | `tests/` + `main.c` | `tests/` ctest + `VARLERA_GEN=N` block-scatter generator landed — real data: 4096 blocks → 1.64ms frame (queue 0.68 is now the hot zone). Pending: fused-model / glass-sort / shadow-volume scene variants. |
| ⏳ Codebase docs + tests | M→ongoing | `AGENTS.md`, `CONTRIBUTING.md`, `tests/` | AGENTS.md kept current through every commit; `/W4` clean + file splits done (`rk_vk`/`meshgen`/`marching`). Pending: `CONTRIBUTING.md`, more tests. |
| ⏳ Aseprite-like art tool | M | `art.c`, `tex.c` | `art.c` modal: PEN/ERASE/FILL/LINE/RECT/PICK + MIRROR/UNDO, wheel zoom, RMB pan. Flipbook frame editing landed: multi-tile sheets edit one tile per canvas, atlas-thumbnail frame strip, ONION ghost of prev frame, PLAY preview ticks at the texture's `tex_fps`, `,`/`.` frame keys; `VARLERA_ART=id` hook. Selection/move landed: SEL tool marquee, drag-inside lifts+moves pixels with ghost preview, Del clears, Ctrl+C/X/V clipboard (paste ghost follows mouse), Esc deselects. Pending: layers + opacity. |
| Object physics stats (data) | M | `world.c`, `panel.c` | `Block` gains traction/bounce/weight fields + info-panel editing — data model + persistence only; runtime effect lands with the physics items below. |
| ⏳ Particle system `a4dd47c` | M | `particles.c`, `rk.c`, `world.c` | Landed: fixed 1024-pool, 4 emitters (FIRE/SMOKE/DEBRIS/SPARK), gravity+drag sim, draws through instanced `rk_ghost` path (GLOW particles feed bloom), emitters on script acts + placement puff, `PC_PARTS` profiling, `VARLERA_PARTS` test hook. Pending: billboard sprites sampling atlas art (2D flavor), more emitter kinds, per-emitter params in script rows. |
| ✅ Post chain: bloom + tonemap `43f8eb5` | M | `rk.c`, `rk_vk.c`, new shaders | HDR R16F scene target (MSAA resolves into it) → 4-mip bright-pass downsample + tent-filter upsample bloom chain → ACES tonemap composite; UI/icons ride a separate LDR pass so they stay crisp. `post.vert`/`down.frag`/`up.frag`/`post.frag`; GPU cost ~0.04ms at 800 blocks. |
| ⏳ Per-vertex AO bake `8c778df` | M | `model.c` bake, `meshgen.c` | Landed for fused-block models: 8-ray hemisphere occlusion vs child AABBs, darkens baked vert color (soft contact shading). Sculpted-topo models pending — needs a BVH or grid accel to stay affordable at bake time. |
| Live collision overhaul | M→L | `world.c`, `edit.c`, `camera.c` | Continuous collision for fast/moving blocks (current settle assumes dt steps), capsule player collider, joint-constraint solver groundwork for NPCs. |

## Tier 3 — hard (cross-cutting)

| Feature | Effort | Touches | Notes |
|---|---|---|---|
| Full occlusion culling | L | `rk.c`, `rk_vk.c`, new shader | Vulkan path: depth-pyramid (Hi-Z) build + `vkCmdDrawIndexedIndirectCount` with per-instance visibility. CPU path (cheaper): block PVS per cell for indoor scenes. Do frustum culling first. |
| Code blocks + scripting v2 | L | `world.c` (trig/act rows exist), `panel.c`, `main.c` | Conditions (IF trig THEN act), per-world variables, wire-mode link UI, event bus, timers/sensors/counters/loops. "Massive expansion" = this done fully + a script debugger overlay in F3. |
| Global scripts | M (after scripting v2) | `world.c`, `panel.c` | World-scope scripts not bound to a block: list UI in WORLD tab, each entry minimizable + independently enabled (multiple active at once); accepts code-block scripts now, text-code scripts when the text language lands. |
| Shader effects via code blocks | L | `world.c`, `rk.c`, shaders | Cheap version lands with material flags (M): effect id + params in instance bits → glow pulse, dissolve, wave, blink. Full version rides scripting v2: actions bind effects to triggers, script vars drive params (speed/intensity), per-world effect table. |
| Overhauled building | L | `edit.c`, `panel.c`, `world.c` | Rework of the place/move state machine: mirror-symmetry build mode, line/plane fills, arbitrary-axis rotate (current is 90° steps + rot90 flags), structural stress preview, smart attach (auto-snap to stud/edge lattices), clipboard templates library. |
| Math/algorithm blocks | L | scripting v2 surface | Expression nodes: arithmetic, lerp/clamp/remap, random/chance, min/max, sort/list ops, distance/raycast queries. The "complex systems" tier of the block language. |
| Compound code groups | L | `world.c`, scripting v2, `panel.c` | User-named macro blocks containing visible child blocks — shareable functions like "rigid body physics", "sound physics", "2D platformer", "3D platformer". Ships as premade packs + user-creatable. Needs scripting v2 + nested serialization. |
| Model rig support | L | `model.c`, `sculpt.c`, `rk.c`, shader | Named bones/joints stored per model (joint planes from sculpt mark attach points), per-vert bone weights baked in (`Vert` gets bone idx+weight), GPU skinning in the world VS or CPU skin for low-vert models. Foundation for NPCs + 3D anim. |
| 3D animation creator | L | `sculpt.c`/new `anim.c`, `model.c` | Keyframe pose editor on rigged models (rot/pos per joint, timeline, ease curves, play preview); clip library saved per model; runtime blends poses on instances. Depends on rig support. |
| ⏳ BW rigid-body play physics | L | `physics.c` | Landed `8463838`+`7713f7d`+drag commit: LMB block drag (`world_drag_grab/move/release`, WSet DRAG toggle), PHYS settle/rigid modes, gravMul. Pending: connected-component bodies (fused blocks as one mass), torque/momentum, true joint solver. |
| ⏳ Joint settings | L | `world.c`, `physics.c` | World-level loose/rigid landed (WSet PHYS row). Pending: per-connection joint mode (LOOSE floppy hinges/motors vs RIGID weld), sculpt joint planes as articulation points. |
| ⏳ Physics stats runtime | L | `physics.c` | `weight`->BOOST thrust, `bounce`->landing restitution, `gravMul`->world gravity wired. Pending: traction->contact friction (needs horizontal motion), code-block setters. |
| Sound editor + music maker | L | new `audio.c` + `soundlab.c` modal | After audio engine: waveform/noise synth editor for SFX (reuses art.c modal pattern), step-sequencer music maker; sounds bound to script actions + world events. |
| BW feature parity | L | all | Umbrella for "everything Blocksworld minus community": motors/hinges/springs/magnets, action sensors, block categories parity, play-mode object dragging, world sharing (export/import file — no servers). Track as a checklist in `docs/BW-PARITY.md` once physics lands. |
| Scene/entity model | L | `world.c`, `main.c`, new `scene.c` | Generalize Block→Entity (transform + components: mesh/collider/script/emitter) so triggers, NPCs, particles, props share one update path. The pivot from "blocks world" to "engine" — do before scripting v2 explodes `Block` further. |
| Custom NPCs (sculpt + joint planes) | L | `sculpt.c`, `model.c`, `world.c`, new anim | Sculpted limb pieces + joint-plane attachment points → articulated hierarchies; anim state machine; NPC AI hooks into scripting v2. Depends on joints/physics groundwork from live-collision item. |
| Threading + GPU compute | L | `world.c`, `meshgen.c`, `rk_vk.c` | Job pool for mesh bakes + broadphase (Win32 threads, `_WIN32` guarded). Vulkan compute for physics queries — prefer compute over CUDA (no new dep, works on all GPUs); CUDA only if a profiling-measured kernel justifies it. |
| Chunked world layout | L | `world.c`, `meshgen.c`, `edit.c` | World setting: chunk shape NONE (current flat array) / COLUMN (MC-style vertical) / CUBIC (16³ cells). Chunks own their instanced draw buffers + dirty-region rebuild; prerequisite for streaming, real LOD, and scale beyond MAX_BLOCKS. |
| Provenance + watermarks | L | `world.c` v4, `model.c`, new `author.c` | Every world/model/block-set carries an embedded author record: owner + contributor/roles (modeler/artist), content-hash + signature per asset, contributor list sorted by owned-content share. Ownership handoff = owner-signed transfer record. Honest scope: deters + detects stripping (hash mismatch flags it) — can't be cryptographically unalterable in open-source, but tampering is loud, not silent. "Varlera" baked as default author on premade free-use assets. |

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
| Local identity/profile | M | `author.c`, settings | Offline account: display name, avatar color, signing keypair (the identity provenance signs with), per-profile settings + customization. No server — this is the account layer everything else plugs into. |
| GitHub project integration | XL | HTTPS dep + git | Manage a project's repo from Varlera: clone/push/pull .bwx sources, release tags, collaborator list with owner-set permission levels (GitHub's own repo ACLs do the enforcing — Varlera mirrors roles). Needs the HTTPS client gate + bundling libgit2 or shelling to git. |
| Cloud backup (Drive/Dropbox/OneDrive) | L | Cheap version ships free via Save-As: point saves at the provider's synced folder. Real API upload (GDrive REST + OAuth) needs an HTTPS client dep — defer until `libcurl`-style dep is accepted. Shares the ACL model with GitHub: folder-level share permissions map to owner-set roles. |
| Online accounts | XL | server + HTTPS dep | Login accounts for real permission enforcement, community features, ownership registry across machines. Everything local-first: offline profile works standalone, online layers on top when a service exists. |
| Localization | M→L | Font is a 5x7 ASCII bitmap baked into one atlas tile — i18n needs a real font path (SDF/MSDF or system font bake) plus a string table. Do when the UI text set stabilizes. |

## Suggested order

1. ~~Font atlas → frustum culling → settings file~~ ✅ all landed.
2. ~~Skybox/ground defaults~~ ✅, ~~indexing~~ ✅, shapes ⏳ (pass 1),
   QOL/debugging ⏳, panel polish ⏳ (visual + feel payoff, all S).
3. Texture menu → art tool (`art.c` extracted; tools live, layers pending).
4. ~~World data v4~~ ✅ core landed; better I/O + menus + world-settings
   schema next (app-state refactor is the gateway to everything UI).
5. Greedy-mesh play mode + mesh cache + ~~material flags~~ ✅ +
   ~~particle system~~ ✅ (`a4dd47c`) + ~~bloom~~ ✅/`AO` ✅ +
   flipbook anim + assembly sculpt mode +
   auto-merge (perf and content, needed at scale).
6. Collision overhaul → building overhaul → rigid-body physics +
   joints + physics stats runtime → player controller
   (gameplay unlock).
7. Scene/entity refactor → chunked world layout + LOD → local
   profile + provenance metadata → scripting v2 (the "engine"
   pivot) → global scripts → math/dialog/compound blocks → rig
   support → 3D anim creator → NPCs → sound/music tools (all
   ride on it).
8. Occlusion culling + threading only when `prof` shows sim/submit
   zones actually limiting fps at real world sizes.

Defer until profiler says so: Hi-Z occlusion, GPU physics, chunk
streaming — all premature before `sim`/`queue` zones exceed budget
(16.6ms at 60fps; currently 0.55ms total in an empty scene).

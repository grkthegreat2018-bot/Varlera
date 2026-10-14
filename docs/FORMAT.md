# Varlera file formats

All multi-byte fields are little-endian. `u8/u16/u32/u64/i32/f32` denote
unsigned/signed integers and IEEE-754 floats. All formats are designed for
forward/backward tolerance: readers stop at EOF and apply defaults; writers
append trailers old readers ignore.

## world.bw — `BWLB`, current ver = 7

```
u32 magic        'BWLB' = 0x42574C42
u32 ver          file version (1..7; this doc covers v4..7)
u32 nblocks
u32 nextid       next stable block id
u32 created      unix time (v4+)
u32 modified     unix time (v4+)
u8  name[32]     world name, NUL-padded (v4+)
```

### v7 block-record blob

```
u32 rawBytes     decompressed record-stream length
u32 compBytes    LZSS stream length (0 = records stored raw)
u8  blob[...]    compBytes bytes LZSS, else rawBytes raw records
```

### v4–v6 per-block record (the blob is a stream of these)

```
u8  flag         bit0 RAWPOS = pos/size stored as 6x f32
                 bit1 NAME   = 12-byte name follows
u32 id           stable block id
u8  shape        mesh id (built-in <32; >=32 = model index + 32)
u8  rot          0..3 (90-degree steps about Y)
u8  tex          texture id (0 = none)
u8  texm         texture mode bits: 0-1 STRETCH/LOOP/PROC/ANIM,
                 2 OVR (replace color), 3-4 MATTE/GLOSS/METAL/GLOW
u8  flags        bit0 LOCKED, 1 NOCOLLIDE, 2 NOPHYS, 3 SPIN(legacy),
                 4 GLASS, 5 MERGED(runtime), 6 SPAWN, 7 GOAL
u8  trig         0 ALWAYS, 1 SPACE, 2 W, 3 A, 4 S, 5 D, 6 E, 7 Q, 8 CLICK
u8  act          0 NONE, 1 SPIN, 2 GLOW, 3 BOOST, 4 COLOR, 5 TEX,
                 6 DEL, 7 SAY, 8 TITLE
u8  actParam     0..15 (act arg / message slot for SAY/TITLE)
u8  tgt          0 SELF, 1 ASSET, 2 NAME, 3 ALL
u8  alpha        0..255
u8  traction     0..15  (v5+)
u8  bounce       0..15  (v5+)
u8  weight       0..15  (v5+; default 8)
u8  col[3]       RGB
pos/size         6x i16 quantized (value = field / 16) when all six fit
                 the 1/16 grid in +-2047 range; else 6x f32 (RAWPOS)
u8  name[12]     only when flag bit1 set (v4+)
```

### v4+ trailers (after records / the blob)

```
u8  WSet[32]     world settings, magic 'BWS4' at offset 0
u32 BWMS, u32 MSG_MAX(=16), u8 msgs[16][32]   script SAY/TITLE table
u32 BWST, u32 count                          per-block strings (optional)
  count x { u32 blockId, u8 len, u8 pad, u8 text[len] }   text < 48 bytes
```

`WSet` (32 bytes, fields grow into `pad`; unknown bytes must be zeroed
when writing new versions):

```
u32 magic        'BWS4' = 0x34535742
u8  phys         0 settle, 1 rigid
u8  chunkShape   0 none, 1 column, 2 cubic
u8  chunkSize    0/8/16/32
u8  lod          0/1
u8  advRender    EXPERIMENTAL gate
u8  gravMul      gravity in eighths (8 = 1.0x)
u8  skyAssets    0 preset sky, 1 user skybox objects
u8  dragOff      1 disables play-mode LMB drag
u8  fogNear      0 auto,1 30,2 90,3 150   (advRender)
u8  fogFar       0 auto,1 120,2 260,3 400 (advRender)
u8  bloom        0 auto,1 off,2 low,3 high
u8  skyObj[4]    model slots: 0 none, else model index + 1
                 (0=sun dir, 1-3 fixed az/el)
u8  dayCycle     0 off, 1 slow(10min), 2 med(2min), 3 fast(30s)
u8  dayPhase     persisted time 0..255 -> 0..2pi
u8  playerMode   0 free fly, 1 first-person walk
u8  pad[10]
```

### Older versions

- **v3**: raw `Block` dump (68B/record), no header name/meta.
- **v1/v2**: 40B / 44B records — pos/size/col f32 + shape/rot/tex/flags
  (+trig/act in v2). Read with migration.
- **v<6 remap**: `shape >= 16` becomes `shape + 16` (built-in mesh id
  space moved RK_MESH_BASE_MODEL 16->32).

## LZSS blob (src/lzss.c)

Greedy LZSS, 4095-byte window. Stream = groups of {u8 flag, 8 tokens};
flag bit k set = token k is a match (2 bytes), else a literal (1 byte).
Match encoding: `u16le = (offset << 4) | (len - 3)`, len 3..18, offset is
distance back from the current output position.

## textures.bw — `BWTX`, ver = 1

```
u32 magic 'BWTX', u32 ver(1), u32 ntex
u8  used[TEX_MAX]              slot bitmap (TEX_MAX = RK_TEX_MAX)
per used slot i in 1..ntex-1:
    u8 name[16], u8 tx, u8 ty, u8 tw, u8 th      (Tex record, 20 B)
    pixels: th*128 rows x tw*128 RGBA bytes (atlas stride removed)
-- trailers (optional tail; readers tolerate EOF) --
f32 fps[TEX_MAX]               flipbook rate per id
i32 order[TEX_MAX]             grid display order (permutation of 0..ntex-1)
```

Atlas is 1024x1024 RGBA = 8x8 tiles of 128 px; `tx,ty` tile coords,
`tw,th` tile span (multi-tile sheets = PROC variants or ANIM frames,
row-major). Slot 7,7 is the reserved font tile.

## models.bw — `BWMD`, ver = 8

```
u32 magic 'BWMD', u32 ver(8), u32 nmodels
per model:
    u8  name[16]
    i32 nrec      >=0: nch child blocks;  -2: sculpted topo mesh
    f32 size[3]   normalized unit-box extents
    i32 flags     bit0 = premade (adv) asset
    nrec >= 0:    Block children[nrec] (raw 68B structs)
    nrec == -2:   i32 nvert, i32 nface
                  f32 mv[nvert][3]     unit-normalized positions
                  u8  mvcol[nvert][3]  vertex colors
                  u16 mf[nface][3]     triangle indices
                  i8  mvtex[nvert]     material layer (-1 inherit)
                  u8  mvlock[nvert]    layer lock flag
```

`adv` premade packs are re-seeded per save-version bump (pack 1 at v7,
pack 2 at v8); `dedupe_adv` drops same-named dupes on load.

## env.bw — environment presets (v3)

```
i32 skyIdx    0..5  (DAY/SUNSET/DUSK/NIGHT/MINT/VOID)
i32 groundIdx 0..4  (CONCRETE/GRASS/SAND/SNOW/DARK)
i32 floorStyle 0..3 (GRID/CHECK/DOTS/PLAIN)
```

## settings.bw — `BWTS`, ver = 2

```
u32 magic 'BWTS', u32 ver
f32 speed, f32 fov
i32 invertY, profOn, fullscreen, winX, winY, winW, winH
i32 guideOff
u64 guideSeen
u8  pad[52]
u8  saveDir[384]          (v2+)
```

v1 readers stop at `guideSeen` + pad (prefix-compatible).

## bundle .bwx — `BWXB`, ver = 1

```
u32 magic 'BWXB', u32 ver(1), u32 nparts(3)
per part p in 1..3:   u8 id(p), u8 pad[3], u32 len, u8 bytes[len]
```

Parts: 1 = world.bw, 2 = models.bw, 3 = textures.bw — each an exact
embedded copy of the format above (all migration rules apply).

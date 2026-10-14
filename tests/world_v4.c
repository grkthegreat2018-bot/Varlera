/* tests/world_v4.c - world.bw v4 save/load round-trip check.
 * Build (repo root):  cl /I src tests\world_v4.c src\world.c
 *                     gcc -I src tests/world_v4.c src/world.c -lm
 * Exercises: quantized + raw pos paths, name/alpha/texm/trig/act/tgt
 * round-trip, id allocator, then a tampered/older-version sanity pass. */
#define _CRT_SECURE_NO_WARNINGS
#include "world.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* mirror rk.h texture-mode bits (test stays a black-box on the file format) */
#define TEXM_OVR   4
#define TEXM_METAL 2

static int fails;

/* stub: the app's toast.c is not linked into this test */
void toast_say(const char *msg) { (void)msg; }

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); fails++; } \
} while (0)

int main(void) {
    Block b; memset(&b, 0, sizeof b);
    float col[3] = {0.8f, 0.4f, 0.2f};

    /* on-grid block with full metadata */
    b.pos = v3_make(2.5f, 0.5f, -4.0f); b.size = v3_make(1.0f, 0.5f, 2.0f);
    memcpy(b.col, col, sizeof col); b.alpha = 0.75f;
    b.shape = 3; b.rot = 1; b.tex = 7; b.texm = (uint8_t)((TEXM_METAL << 3) | TEXM_OVR);
    b.flags = BF_GLASS; b.trig = TRIG_SPACE; b.act = ACT_COLOR;
    b.actParam = 5; b.tgt = TGT_NAME; snprintf(b.name, sizeof b.name, "engine");
    b.traction = 3; b.bounce = 15; b.weight = 9;
    int i0 = world_addb(&b);
    CHECK(i0 == 0 && g_blocks[0].id == 1);

    /* off-grid block -> raw float path */
    memset(&b, 0, sizeof b);
    b.pos = v3_make(1.234f, 9.9f, 0.001f); b.size = v3_make(0.7f, 1.3f, 0.9f);
    b.alpha = 1.0f; b.shape = 200;
    CHECK(world_addb(&b) == 1);

    /* world-settings + message trailers: non-defaults, verify round-trip */
    snprintf(g_msgs[3], MSG_LEN, "hello world");
    snprintf(world_block_msg_edit(g_blocks[0].id), BMSG_LEN,
             "per-block dialog");                  /* BWST trailer */
    world_wset()->chunkShape = 2;
    world_wset()->chunkSize  = 32;
    world_wset()->lod        = 1;
    world_wset()->gravMul    = 4;                    /* 0.5x gravity */
    CHECK(world_save("test_v4_tmp.bw", "myworld"));

    /* packed records must stay smaller than the v3 flat dump even with
       the header + WSet + message-table overhead */
    FILE *f = fopen("test_v4_tmp.bw", "rb"); fseek(f, 0, SEEK_END);
    long sz = ftell(f); fclose(f);
    long fixed = 56 + (long)sizeof(WSet) + 8 + MSG_MAX * MSG_LEN;
    CHECK(sz > fixed && sz - fixed < (long)(2 * sizeof(Block)));

    char name[32] = {0};
    memset(g_blocks, 0, sizeof g_blocks); g_nb = 0;
    CHECK(world_load("test_v4_tmp.bw", name));
    CHECK(g_nb == 2);
    CHECK(strcmp(name, "myworld") == 0);
    CHECK(world_wset()->chunkShape == 2 && world_wset()->chunkSize == 32);
    CHECK(world_wset()->lod == 1 && world_wset()->gravMul == 4);
    CHECK(strcmp(g_msgs[3], "hello world") == 0 && !g_msgs[0][0]);
    CHECK(world_block_msg(g_blocks[0].id) &&
          strcmp(world_block_msg(g_blocks[0].id), "per-block dialog") == 0);
    CHECK(world_block_msg(g_blocks[1].id) == NULL);

    Block *p = &g_blocks[0];
    CHECK(p->pos.x == 2.5f && p->pos.z == -4.0f && p->size.z == 2.0f);
    CHECK(p->shape == 3 && p->rot == 1 && p->tex == 7);
    CHECK((p->texm >> 3) == TEXM_METAL && (p->texm & TEXM_OVR));
    CHECK(p->trig == TRIG_SPACE && p->act == ACT_COLOR &&
          p->actParam == 5 && p->tgt == TGT_NAME);
    CHECK(p->traction == 3 && p->bounce == 15 && p->weight == 9);
    CHECK(strcmp(p->name, "engine") == 0);
    CHECK(fabsf(p->alpha - 0.75f) < 0.005f);
    CHECK(fabsf(p->col[0] - 0.8f) < 0.005f);

    p = &g_blocks[1];
    CHECK(fabsf(p->pos.x - 1.234f) < 1e-6f && fabsf(p->pos.y - 9.9f) < 1e-6f);
    CHECK(p->shape == 200);

    /* legacy v3: flat header + raw Block records still loads */
    memset(g_blocks, 0, sizeof g_blocks); g_nb = 0;
    {
        Block lb; memset(&lb, 0, sizeof lb);
        lb.pos = v3_make(9.0f, 1.0f, 9.0f); lb.size = v3_make(1, 1, 1);
        lb.alpha = 0.0f;                       /* v3 zero-alpha -> 1.0 fix */
        lb.shape = 5; lb.id = 77;
        uint32_t hdr[4] = {0x42574C42u, 3u, 1u, 100u};
        f = fopen("test_v3_tmp.bw", "wb");
        fwrite(hdr, 4, 4, f); fwrite(&lb, sizeof lb, 1, f); fclose(f);
    }
    CHECK(world_load("test_v3_tmp.bw", NULL));
    CHECK(g_nb == 1);
    CHECK(g_blocks[0].id == 77 && g_blocks[0].shape == 5);
    CHECK(g_blocks[0].alpha == 1.0f);
    CHECK(g_nextid >= 100);
    CHECK(world_wset()->chunkShape == 0 && world_wset()->gravMul == 8);
    CHECK(!g_msgs[3][0]);                        /* msgs reset on v3 load */
    remove("test_v3_tmp.bw");

    /* v7 LZSS blob: a bulk scene must compress far below the raw record size */
    memset(g_blocks, 0, sizeof g_blocks); g_nb = 0;
    memset(&b, 0, sizeof b);
    b.size = v3_make(1, 1, 1); b.alpha = 1.0f; b.shape = 0;
    for (int i = 0; i < 300; i++) {
        b.pos = v3_make((float)(i % 20), 0.5f, (float)(i / 20));
        b.id = 0;
        CHECK(world_addb(&b) == i);
    }
    CHECK(world_save("test_v7_tmp.bw", "bulk"));
    f = fopen("test_v7_tmp.bw", "rb"); fseek(f, 0, SEEK_END);
    sz = ftell(f); fclose(f);
    CHECK(sz < 300 * 33 / 3);                    /* >3x under ~33B/block raw */
    memset(g_blocks, 0, sizeof g_blocks); g_nb = 0;
    CHECK(world_load("test_v7_tmp.bw", NULL));
    CHECK(g_nb == 300 && g_blocks[299].pos.x == 19.0f &&
          g_blocks[299].pos.z == 14.0f);
    remove("test_v7_tmp.bw");

    /* async save: same format on the worker path, poll reaps the job */
    memset(g_blocks, 0, sizeof g_blocks); g_nb = 0;
    memset(&b, 0, sizeof b);
    b.pos = v3_make(3.0f, 1.0f, 3.0f); b.size = v3_make(2, 1, 2);
    b.alpha = 1.0f; b.shape = 7;
    CHECK(world_addb(&b) == 0);
    CHECK(world_save_async("test_async_tmp.bw", "async") == 1);
    {
        int r, guard = 0;
        do {
            r = world_save_poll();
            if (r == 1) {
#ifdef _WIN32
                Sleep(1);
#else
                usleep(1000);
#endif
            }
        } while (r == 1 && ++guard < 5000);     /* ~5s worst case */
        CHECK(r == 0);                          /* finished ok */
        CHECK(world_save_poll() == 2);          /* nothing in flight now */
    }
    memset(g_blocks, 0, sizeof g_blocks); g_nb = 0;
    CHECK(world_load("test_async_tmp.bw", NULL));
    CHECK(g_nb == 1 && g_blocks[0].shape == 7 && g_blocks[0].pos.x == 3.0f);
    remove("test_async_tmp.bw");

    /* corrupt-version rejection */
    f = fopen("test_v4_tmp.bw", "rb");
    uint8_t bad[16] = {0}; fread(bad, 1, 16, f); fclose(f);
    bad[4] = 99;                                     /* bogus version */
    f = fopen("test_bad.bw", "wb"); fwrite(bad, 1, 16, f); fclose(f);
    CHECK(!world_load("test_bad.bw", NULL));

    remove("test_v4_tmp.bw"); remove("test_bad.bw");
    if (!fails) puts("world_v4: all checks passed");
    return fails ? 1 : 0;
}

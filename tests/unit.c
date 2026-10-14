/* tests/unit.c - lzss round-trip + meshgen shape sanity.
 * Links only the leaf modules: no Vulkan, no glfw runtime. */
#include "lzss.h"
#include "meshgen.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    fails++; } } while (0)

static void test_lzss(void) {
    static uint8_t in[8192], enc[8192 * 2], out[8192];
    /* highly compressible: repeated pattern */
    for (int i = 0; i < 8192; i++) in[i] = (uint8_t)(i % 7);
    size_t en = lzss_encode(in, 8192, enc);
    CHECK(en > 0 && en < 8192 / 2);
    size_t dn = lzss_decode(enc, en, out, sizeof out);
    CHECK(dn == 8192 && memcmp(in, out, 8192) == 0);
    /* incompressible-ish: xorshift noise must round-trip too */
    uint32_t r = 12345;
    for (int i = 0; i < 8192; i++) {
        r ^= r << 13; r ^= r >> 17; r ^= r << 5; in[i] = (uint8_t)r;
    }
    en = lzss_encode(in, 8192, enc);
    CHECK(en > 0);
    dn = lzss_decode(enc, en, out, sizeof out);
    CHECK(dn == 8192 && memcmp(in, out, 8192) == 0);
    /* small edge cases */
    for (int n = 0; n < 40; n++) {
        memset(in, 0xAB, (size_t)n);
        en = lzss_encode(in, (size_t)n, enc);
        dn = lzss_decode(enc, en, out, sizeof out);
        CHECK(dn == (size_t)n && memcmp(in, out, (size_t)n) == 0);
    }
    /* corrupt: truncated stream must fail cleanly, not overrun */
    CHECK(lzss_decode(enc, 3, out, sizeof out) == (size_t)-1);
    fprintf(stderr, "lzss ok (8192 -> %zu patterned)\n", en);
}

static void test_meshgen(void) {
    static Vert buf[8192];
    for (int s = 0; s < RK_MESH_COUNT; s++) {
        int n = meshgen_shape(s, buf, 8192);
        CHECK(n > 0 && n % 3 == 0);
        int bad = 0;
        for (int i = 0; i < n; i++) {
            for (int a = 0; a < 3; a++) {
                float p = buf[i].pos[a];
                if (!(p == p) || fabsf(p) > 0.53f) bad++;   /* NaN or oob */
            }
            float nl = buf[i].nrm[0] * buf[i].nrm[0] +
                       buf[i].nrm[1] * buf[i].nrm[1] +
                       buf[i].nrm[2] * buf[i].nrm[2];
            if (nl < 0.25f || nl > 4.0f) bad++;             /* broken normal */
        }
        if (bad) fprintf(stderr, "shape %d: %d bad verts\n", s, bad);
        CHECK(bad == 0);
    }
    /* sculpt buffer cap: every shape must fit 4096 */
    for (int s = 0; s < RK_MESH_COUNT; s++) {
        int n = meshgen_shape(s, buf, 4096);
        CHECK(n > 0 && n <= 4096);
    }
    fprintf(stderr, "meshgen ok (%d shapes)\n", RK_MESH_COUNT);
}

int main(void) {
    test_lzss();
    test_meshgen();
    if (fails) { fprintf(stderr, "%d FAILURES\n", fails); return 1; }
    fprintf(stderr, "unit: all ok\n");
    return 0;
}

/* lzss.c - greedy LZSS: hash-chain match search over a 4095-byte window.
 * Records are small and highly repetitive (zeros, quantized sizes, flag
 * bytes) so even a short window/short matches compress well. */
#include "lzss.h"
#include <stdlib.h>
#include <string.h>

#define LZ_WIN   4095
#define LZ_HASH  8192
#define LZ_TRIES 16

size_t lzss_bound(size_t n) { return n + n / 8 + 16; }

static unsigned lz_hash3(const uint8_t *p) {
    return ((unsigned)p[0] * 131u ^ (unsigned)p[1] * 977u ^ p[2]) & (LZ_HASH - 1);
}

size_t lzss_encode(const uint8_t *in, size_t n, uint8_t *out) {
    int *head = malloc(LZ_HASH * sizeof(int));
    int *prev = malloc((n ? n : 1) * sizeof(int));
    if (!head || !prev) { free(head); free(prev); return 0; }
    memset(head, -1, LZ_HASH * sizeof(int));

    size_t i = 0, o = 0;
    while (i < n) {
        size_t fi = o++;               /* flag byte, patched after the group */
        uint8_t flag = 0;
        for (int k = 0; k < 8 && i < n; k++) {
            int best = 0; size_t boff = 0;
            if (i + 3 <= n) {
                unsigned h = lz_hash3(in + i);
                int p = head[h], tries = 0;
                while (p >= 0 && i - (size_t)p <= LZ_WIN && tries++ < LZ_TRIES) {
                    int l = 0;
                    while (l < 18 && i + l < n && in[p + l] == in[i + l]) l++;
                    if (l > best) { best = l; boff = i - (size_t)p; if (l == 18) break; }
                    p = prev[p];
                }
                prev[i] = head[h]; head[h] = (int)i;
            }
            if (best >= 3) {
                flag |= (uint8_t)(1u << k);
                out[o++] = (uint8_t)((boff << 4) | (uint8_t)(best - 3));
                out[o++] = (uint8_t)(boff >> 4);
                i += (size_t)best;
            } else {
                out[o++] = in[i++];
            }
        }
        out[fi] = flag;
    }
    free(head); free(prev);
    return o;
}

size_t lzss_decode(const uint8_t *in, size_t inN, uint8_t *out, size_t outN) {
    size_t i = 0, o = 0;
    while (i < inN) {
        uint8_t flag = in[i++];
        for (int k = 0; k < 8 && i < inN; k++) {
            if (flag & (1u << k)) {
                if (i + 2 > inN) return (size_t)-1;
                unsigned m = (unsigned)in[i] | ((unsigned)in[i + 1] << 8);
                i += 2;
                size_t off = m >> 4, len = (m & 15) + 3;
                if (!off || off > o || o + len > outN) return (size_t)-1;
                for (size_t c = 0; c < len; c++) { out[o] = out[o - off]; o++; }
            } else {
                if (o >= outN) return (size_t)-1;
                out[o++] = in[i++];
            }
        }
    }
    return o;
}

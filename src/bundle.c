/* bundle.c - .bwx portable project bundle: world + models + textures packed
 * into one shareable file. Components are serialized with the existing
 * path-based savers into temp files, then concatenated under a small part
 * table; import splits them back out and reloads each through the normal
 * readers (all version migration still applies). */
#define _CRT_SECURE_NO_WARNINGS
#include "bundle.h"
#include "app.h"
#include "world.h"
#include "model.h"
#include "tex.h"
#include "toast.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define BX_MAGIC 0x42585742u   /* 'BWXB' */
#define BX_VER   2

enum { BX_WORLD = 1, BX_MODELS, BX_TEX, BX_PROV };

static BxProv s_prov;                    /* last imported bundle's info */
static int  s_haveProv;
const BxProv *bundle_prov(void) { return s_haveProv ? &s_prov : NULL; }

static const char *os_user(void) {
    if (A.author[0]) return A.author;      /* local profile name wins */
    const char *u = getenv("USERNAME");
    if (!u || !*u) u = getenv("USER");
    return (u && *u) ? u : "unknown";
}

static void tmp_path(char *out, int n, int tag) {
    snprintf(out, n, "%s/.bwx_tmp_%d", worlds_dir(), tag);
}

int bundle_save(const char *path, const char *name) {
    worlds_ensure();
    char tmp[3][512];
    for (int t = 0; t < 3; t++) tmp_path(tmp[t], 512, t);
    int ok = world_save(tmp[0], name) && models_save(tmp[1]) &&
             tex_save(tmp[2]);
    if (ok) {
        FILE *f = fopen(path, "wb");
        if (!f) ok = 0;
        else {
            uint32_t hdr[3] = {BX_MAGIC, BX_VER, 4};
            ok = fwrite(hdr, 12, 1, f) == 1;
            /* provenance first: v1 readers skip unknown part ids */
            if (ok) {
                BxProv pv; memset(&pv, 0, sizeof pv);
                snprintf(pv.world, sizeof pv.world, "%s",
                         name && name[0] ? name : "world");
                snprintf(pv.creator, sizeof pv.creator, "%s", os_user());
                pv.created = pv.modified = (uint32_t)time(NULL);
                snprintf(pv.note, sizeof pv.note, "varlera bundle");
                uint8_t ph[8] = {0};
                uint32_t len = sizeof pv;
                ph[0] = BX_PROV; memcpy(ph + 4, &len, 4);
                ok = fwrite(ph, 8, 1, f) == 1 &&
                     fwrite(&pv, len, 1, f) == 1;
            }
            for (int p = 0; p < 3 && ok; p++) {
                FILE *s = fopen(tmp[p], "rb");
                if (!s) { ok = 0; break; }
                fseek(s, 0, SEEK_END); long len = ftell(s); fseek(s, 0, SEEK_SET);
                if (len < 0) len = 0;
                uint8_t ph[8] = {0};
                ph[0] = (uint8_t)(p + 1);
                memcpy(ph + 4, &len, 4);
                ok = fwrite(ph, 8, 1, f) == 1;
                char buf[8192]; size_t r;
                while (ok && (r = fread(buf, 1, sizeof buf, s)) > 0)
                    ok = fwrite(buf, 1, r, f) == r;
                fclose(s);
            }
            fclose(f);
        }
    }
    for (int t = 0; t < 3; t++) remove(tmp[t]);
    return ok;
}

int bundle_load(const char *path, char *nameOut) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t hdr[3];
    if (fread(hdr, 12, 1, f) != 1 || hdr[0] != BX_MAGIC ||
        hdr[1] < 1 || hdr[1] > BX_VER) {
        fclose(f); return 0;
    }
    s_haveProv = 0;
    char tmp[3][512]; int have[3] = {0};
    worlds_ensure();
    for (uint32_t p = 0; p < hdr[2]; p++) {
        uint8_t ph[8];
        if (fread(ph, 8, 1, f) != 1) break;
        uint32_t len; memcpy(&len, ph + 4, 4);
        int t = ph[0];
        FILE *o = NULL;
        if (t == BX_PROV) {
            char pbuf[512];
            uint32_t keep = len < sizeof pbuf ? len : sizeof pbuf;
            uint32_t skip = len - keep, off = 0;
            while (len) {
                size_t want = len < 8192 ? len : 8192;
                uint8_t buf[8192];
                size_t r = fread(buf, 1, want, f);
                if (!r) break;
                len -= (uint32_t)r;
                uint32_t take = keep - off < (uint32_t)r ? keep - off
                                                       : (uint32_t)r;
                memcpy(pbuf + off, buf, take); off += take;
            }
            (void)skip;
            if (off >= sizeof(BxProv)) { memcpy(&s_prov, pbuf, sizeof s_prov);
                                         s_haveProv = 1; }
            continue;
        }
        if (t >= 1 && t <= 3) {
            tmp_path(tmp[t - 1], 512, t);
            o = fopen(tmp[t - 1], "wb");
            have[t - 1] = 1;
        }
        char buf[8192];
        while (len) {
            size_t want = len < sizeof buf ? len : sizeof buf;
            size_t r = fread(buf, 1, want, f);
            if (!r) break;
            len -= (uint32_t)r;
            if (o) fwrite(buf, 1, r, o);
        }
        if (o) fclose(o);
    }
    fclose(f);
    /* world is mandatory; models/atlas optional (replace-all import) */
    int ok = have[0] && world_load(tmp[0], nameOut);
    if (ok && have[1]) { g_nmodels = 0; models_load(tmp[1]); models_rebake_all(); }
    if (ok && have[2]) tex_load(tmp[2]);
    for (int t = 0; t < 3; t++) if (have[t]) remove(tmp[t]);
    if (ok && s_haveProv) {
        char msg[128];
        snprintf(msg, sizeof msg, "imported '%s' by %s",
                 s_prov.world, s_prov.creator);
        toast_say(msg);
        fprintf(stderr, "bwx provenance: '%s' by %s\n",
                s_prov.world, s_prov.creator);
    }
    return ok;
}

/* prof.c - frame cost logging: CPU zones, counters, GPU pass times,
 * rolling 120-frame stats, periodic stderr report, F3 overlay. */
#include "prof.h"
#include "app.h"
#include "rk.h"
#include "font.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define WIN       120           /* rolling stat window (frames) */
#define LOG_EVERY 4.0           /* seconds between stderr reports */
#define FRAME_60  16.6667

static const char *ZN[PZ_N] = {"wait", "sim", "queue", "submit"};

static double t0[PZ_N], acc[PZ_N];          /* current frame */
static double fr[WIN], zw[PZ_N][WIN];       /* ring: frame + zone ms */
static int    fn, fc;                       /* ring head, fill count */
static int    cnt[PC_N];                    /* counters, last frame */
static float  gpu[4];                       /* shadow, main, post, total ms */
static double tFrame, tLastLog;
static int    s_show;

void prof_reset(void) {
    memset(acc, 0, sizeof acc);
    memset(&fr, 0, sizeof fr);
    memset(&zw, 0, sizeof zw);
    fn = fc = 0; tLastLog = 0;
    tFrame = glfwGetTime();
}
void prof_zone_begin(int z) { t0[z] = glfwGetTime(); }
void prof_zone_end(int z)   { acc[z] += glfwGetTime() - t0[z]; }
void prof_count(int c, int v) { cnt[c] = v; }
void prof_toggle(void) { s_show = !s_show; }
int  prof_shown(void)  { return s_show; }

/* avg / p95 / max over the filled ring */
static void stat(const double *w, int n, double *avg, double *p95, double *mx) {
    double s = 0, m = 0, sorted[WIN];
    for (int i = 0; i < n; i++) { sorted[i] = w[i]; s += w[i]; if (w[i] > m) m = w[i]; }
    *avg = n ? s / n : 0;
    *mx  = m;
    for (int i = 1; i < n; i++) {
        double k = sorted[i]; int j = i - 1;
        while (j >= 0 && sorted[j] > k) { sorted[j+1] = sorted[j]; j--; }
        sorted[j+1] = k;
    }
    int i95 = (int)(n * 0.95); if (i95 >= n) i95 = n - 1;
    *p95 = n ? sorted[i95] : 0;
}

static void report(void) {
    double fa, fp, fm; stat(fr, fc, &fa, &fp, &fm);
    fprintf(stderr,
        "prof | %5.1f fps frame %5.2fms p95 %5.2f max %5.2f |",
        fa > 0 ? 1000.0 / fa : 0.0, fa, fp, fm);
    for (int z = 0; z < PZ_N; z++) {
        double a, p, m; stat(zw[z], fc, &a, &p, &m);
        fprintf(stderr, " %s %.2f", ZN[z], a);
    }
    fprintf(stderr,
        " | gpu %.2f (sh %.2f main %.2f post %.2f)"
        " | draws %d inst %d uiv %d mv %d blocks %d models %d\n",
        gpu[3], gpu[0], gpu[1], gpu[2],
        cnt[PC_DRAWS], cnt[PC_INST], cnt[PC_UIVERTS], cnt[PC_MESHVERTS],
        cnt[PC_BLOCKS], cnt[PC_MODELS]);
}

void prof_frame(void) {
    double now = glfwGetTime();
    fr[fn] = (now - tFrame) * 1000.0;
    for (int z = 0; z < PZ_N; z++) zw[z][fn] = acc[z] * 1000.0;
    fn = (fn + 1) % WIN;
    if (fc < WIN) fc++;
    float g[4];
    if (rk_gpu_times(g)) memcpy(gpu, g, sizeof gpu);
    memset(acc, 0, sizeof acc);
    tFrame = now;
    if (now - tLastLog > LOG_EVERY) { tLastLog = now; report(); }
}

/* ---------------- overlay ---------------- */

void prof_draw(void) {
    if (!s_show || fc < 2) return;
    double fa, fp, fm; stat(fr, fc, &fa, &fp, &fm);
    char b[160];
    float x = 8.0f, y = 8.0f, lh = 11.0f, w = 252.0f;
    int lines = 6;
    rk_ui_rect(x - 4, y - 4, w, lh * lines + 54.0f, 0, 0, 0, 0.55f);

    snprintf(b, sizeof b, "PROF (F3)  %5.1f fps", fa > 0 ? 1000.0 / fa : 0.0);
    ui_text(x + 8, y, b, 1, 0.55f, 0.9f, 1.0f, 1); y += lh;
    snprintf(b, sizeof b, "frame %5.2fms p95 %5.2f max %5.2f", fa, fp, fm);
    ui_text(x + 8, y, b, 1, 1, 1, 1, 1); y += lh;
    b[0] = 0;
    for (int z = 0; z < PZ_N; z++) {
        double a, p, m; stat(zw[z], fc, &a, &p, &m);
        snprintf(b + strlen(b), sizeof b - strlen(b), "%s %.2f  ", ZN[z], a);
    }
    ui_text(x + 8, y, b, 1, 0.8f, 0.85f, 1.0f, 1); y += lh;
    snprintf(b, sizeof b, "gpu %4.2f = sh %.2f + main %.2f + post %.2f",
             gpu[3], gpu[0], gpu[1], gpu[2]);
    ui_text(x + 8, y, b, 1, 0.8f, 0.85f, 1.0f, 1); y += lh;
    snprintf(b, sizeof b, "draws %d inst %d uiv %d mv %d",
             cnt[PC_DRAWS], cnt[PC_INST], cnt[PC_UIVERTS], cnt[PC_MESHVERTS]);
    ui_text(x + 8, y, b, 1, 0.8f, 0.85f, 1.0f, 1); y += lh;
    snprintf(b, sizeof b, "blocks %d models %d", cnt[PC_BLOCKS], cnt[PC_MODELS]);
    ui_text(x + 8, y, b, 1, 0.8f, 0.85f, 1.0f, 1); y += lh + 4;

    /* frame-time strip: 1 bar/frame, green <16.7ms, amber <33ms, red above */
    float gx = x + 8, gw = 2.0f, gh = 40.0f;
    for (int i = 0; i < fc; i++) {
        int ri = (fn - fc + i + WIN) % WIN;
        double ms = fr[ri];
        float h = (float)fmin(gh, ms * 1.2);
        float r = ms < FRAME_60 ? 0.3f : ms < 2*FRAME_60 ? 0.9f : 1.0f;
        float g = ms < FRAME_60 ? 0.9f : ms < 2*FRAME_60 ? 0.7f : 0.25f;
        rk_ui_rect(gx + i * gw, y + gh - h, gw - 1, h, r, g, 0.3f, 1);
    }
    /* 60fps + 30fps guide lines */
    rk_ui_rect(gx, y + gh - (float)(FRAME_60*1.2), fc * gw, 1,
               0.4f, 0.9f, 0.4f, 0.35f);
}

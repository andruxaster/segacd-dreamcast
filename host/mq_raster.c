/* Host model of what the PVR makes of an mq frame: atlas of palette
   indices (from the staged tiles), backdrop / software strips, then every
   rectangle with depth compare ">=" and texel 0 discarded. */
#include <string.h>
#include "../src/mq/mq.h"
#include "mq_raster.h"

static uint8_t  atl[MQ_CELLS][64];
static uint16_t depth[240 * 320];

void mq_raster_tiles(void) {
    for (int i = 0; i < mq.nd; i++) {
        const uint8_t *s = mq.dpx[i];       /* 8 rows of 2 u16 words */
        uint8_t *d = atl[mq.dt[i]];
        for (int r = 0; r < 8; r++) {
            const uint16_t *w = (const uint16_t *)(s + r * 4);
            for (int x = 0; x < 8; x++) d[r * 8 + x] = (uint8_t)((w[x >> 2] >> (12 - 4 * (x & 3))) & 15);
        }
    }
}

void mq_raster(uint16_t *out, const uint16_t *fb, int fb_pitch, int fb_y0) {
    int W = mq.w ? mq.w : 320;
    for (int s = 0; s < mq.ns; s++) {
        const mq_strip_t *st = &mq.s[s];
        for (int y = st->y0; y <= st->y1 && y < 240; y++) {
            uint16_t *o = out + y * 320;
            if (st->sw) memcpy(o, fb + (y + fb_y0) * fb_pitch, W * 2);
            else for (int x = 0; x < W; x++) o[x] = st->back;
            for (int x = 0; x < 320; x++) depth[y * 320 + x] = st->sw ? 0xffff : 0;
        }
    }
    for (int i = 0; i < mq.nq; i++) {
        const mq_quad_t *q = &mq.q[i];
        int du = q->u1 > q->u0 ? 1 : -1, dv = q->v1 > q->v0 ? 1 : -1;
        for (int y = q->y0; y < q->y1; y++) {
            if (y < 0 || y >= 240) continue;
            int v = dv > 0 ? q->v0 + (y - q->y0) : q->v0 - 1 - (y - q->y0);
            for (int x = q->x0; x < q->x1; x++) {
                if (x < 0 || x >= W) continue;
                int u = du > 0 ? q->u0 + (x - q->x0) : q->u0 - 1 - (x - q->x0);
                int t = atl[(v >> 3) * 64 + (u >> 3)][(v & 7) * 8 + (u & 7)];
                if (!t) continue;
                uint16_t *dp = &depth[y * 320 + x];
                if (q->z >= *dp) { *dp = q->z; out[y * 320 + x] = mq.col[q->bank * 16 + t]; }
            }
        }
    }
}

int mq_compare(const uint16_t *hw, const uint16_t *fb, int fb_pitch, int fb_y0, int *fx, int *fy) {
    int n = 0, W = mq.w ? mq.w : 320;
    *fx = *fy = -1;
    for (int s = 0; s < mq.ns; s++) {
        const mq_strip_t *st = &mq.s[s];
        if (st->sw) continue;
        for (int y = st->y0; y <= st->y1 && y < 240; y++)
            for (int x = 0; x < W; x++)
                if (hw[y * 320 + x] != fb[(y + fb_y0) * fb_pitch + x]) {
                    if (!n) { *fx = x; *fy = y; }
                    n++;
                }
    }
    return n;
}

/* debugging: is the model's tile t the same as these VRAM words */
int mq_raster_tile_equal(int t, const uint16_t *w) {
    const uint8_t *d = atl[t];
    for (int r = 0; r < 8; r++)
        for (int x = 0; x < 8; x++)
            if (d[r * 8 + x] != ((w[r * 2 + (x >> 2)] >> (12 - 4 * (x & 3))) & 15)) return 0;
    return 1;
}

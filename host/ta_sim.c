/* Host check of src/dc/mq_pvr.c: decodes the TA packets it wrote
   (MQ_PVR_SIM), the twiddled atlas and the ARGB8888 palette the way the PVR
   reads them, and draws the 640x480 picture (pixel centres, nearest texel,
   opaque list then punch-through with depth >= and alpha test). */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../src/dc/mq_pvr.h"
#include "ta_sim.h"

static float depth[640 * 480];

static uint32_t tw_addr(uint32_t u, uint32_t v, uint32_t w, uint32_t h) {
    uint32_t s = w < h ? w : h, blk = w < h ? v / s : u / s;
    uint32_t uu = u % s, vv = v % s, o = 0;
    for (int b = 0; (1u << b) < s; b++)
        o |= ((vv >> b) & 1) << (2 * b) | ((uu >> b) & 1) << (2 * b + 1);
    return blk * s * s + o;
}

static inline uint16_t c8888_565(uint32_t c) {
    return (uint16_t)(((c >> 19) & 31) << 11 | ((c >> 10) & 63) << 5 | ((c >> 3) & 31));
}

typedef struct { uint32_t flags; float x, y, z, u, v; uint32_t argb, oargb; } vtx_t;

void ta_sim_render(uint16_t *out) {
    for (int i = 0; i < 640 * 480; i++) { out[i] = 0; depth[i] = 0; }
    uint32_t kind = 5, mode3 = 0;
    vtx_t v[4];
    int nv = 0;
    for (uint32_t w = 0; w + 8 <= mq_sim_words; w += 8) {
        const uint32_t *p = &mq_sim_ta[w];
        if ((p[0] & 0xe0000000u) == 0x80000000u) { kind = p[0] & 7; mode3 = p[3]; nv = 0; continue; }
        memcpy(&v[nv++], p, sizeof(vtx_t));
        if (nv < 4) continue;
        nv = 0;
        float x0 = v[0].x, x1 = v[1].x, y0 = v[0].y, y1 = v[2].y;
        int px0 = (int)ceilf(x0 - 0.5f), px1 = (int)ceilf(x1 - 0.5f);
        int py0 = (int)ceilf(y0 - 0.5f), py1 = (int)ceilf(y1 - 0.5f);
        for (int py = py0; py < py1; py++) {
            if (py < 0 || py >= 480) continue;
            float tv = v[0].v + (py + 0.5f - y0) / (y1 - y0) * (v[2].v - v[0].v);
            for (int px = px0; px < px1; px++) {
                if (px < 0 || px >= 640) continue;
                float tu = v[0].u + (px + 0.5f - x0) / (x1 - x0) * (v[1].u - v[0].u);
                float z = v[0].z;
                uint16_t col;
                int pt = 0;
                if (kind == 5) col = c8888_565(v[0].argb);
                else if (kind >= 3) {
                    const uint16_t *t = (const uint16_t *)(mq_sim_vram + ((mode3 & 0x1fffff) << 3));
                    col = t[(int)floorf(tv * 256) * 512 + (int)floorf(tu * 512)];
                } else {
                    pt = 1;
                    int u = (int)floorf(tu * 512), vv = (int)floorf(tv * 512);
                    if (u < 0) u = 0; if (u > 511) u = 511;
                    if (vv < 0) vv = 0; if (vv > 511) vv = 511;
                    const uint8_t *t = mq_sim_vram + ((mode3 & 0x1fffff) << 3);
                    uint32_t a = tw_addr(u, vv, 512, 512);
                    uint32_t c = mq_sim_pal[((mode3 >> 21) & 63) * 16 + ((t[a >> 1] >> ((a & 1) * 4)) & 15)];
                    if (!(c >> 24)) continue;
                    col = c8888_565(c);
                }
                float *dp = &depth[py * 640 + px];
                if (pt ? z >= *dp : z > *dp) { *dp = z; out[py * 640 + px] = col; }
            }
        }
    }
}

int ta_sim_compare(const uint16_t *out640, const uint16_t *pic320, int w, int h, int *fx, int *fy) {
    int ox = 320 - w, oy = 240 - h, n = 0;
    *fx = *fy = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint16_t c = pic320[y * 320 + x];
            for (int k = 0; k < 4; k++)
                if (out640[(oy + 2 * y + (k >> 1)) * 640 + ox + 2 * x + (k & 1)] != c) {
                    if (!n) { *fx = x; *fy = y; } n++; break;
                }
        }
    return n;
}

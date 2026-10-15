/* mq_pvr: shows an mq frame (src/mq/mq.h) with the PVR.

   VRAM: the tile atlas (512x512, 4-bit palette indices, twiddled) and two
   512x256 RGB565 screen textures for the lines PicoDrive drew. Palette:
   ARGB8888, alpha 0 for entry 0 of every bank (low bits zero, so the RGB565
   of PicoDrive survives the 565 framebuffer exactly).

   Scene (640x480, the MD picture 2x, centred):
     opaque list:   backdrop of every hardware strip (z 0.5), software
                    strips from the screen texture (z 200)
     punch-through: one rectangle per mq quad, depth compare >=,
                    z = 1 + z / 256, grouped by palette bank.

   With MQ_PVR_SIM (bench/, host/) the same code writes into RAM, so
   qemu-sh4 counts its instructions and the host can decode the packets. */
#ifndef MQ_PVR_SIM
#include <kos.h>
#include <dc/pvr.h>
#else
#include <stdint.h>
#include <string.h>
#endif
#include "../mq/mq.h"
#include "mq_pvr.h"

#ifdef MQ_PVR_SIM
typedef struct { uint32_t flags; float x, y, z, u, v; uint32_t argb, oargb; } pvr_vertex_t;
typedef struct { uint32_t cmd, mode1, mode2, mode3, d1, d2, d3, d4; } pvr_poly_hdr_t;
typedef uint8_t *pvr_ptr_t;
#define PVR_CMD_VERTEX     0xe0000000
#define PVR_CMD_VERTEX_EOL 0xf0000000
uint8_t  mq_sim_vram[1024 * 1024];
uint32_t mq_sim_pal[1024];
uint32_t mq_sim_ta[MQ_SIM_TA_WORDS];
uint32_t mq_sim_words, mq_sim_keep;
static uint32_t sim_slot[8];
static inline void *sim_target(void) {
    if (mq_sim_keep && mq_sim_words + 8 <= MQ_SIM_TA_WORDS) return &mq_sim_ta[mq_sim_words];
    return sim_slot;
}
#define pvr_dr_target() sim_target()
#define pvr_dr_commit(p) (mq_sim_words += 8)
static inline void pvr_set_pal_entry(uint32_t i, uint32_t v) { mq_sim_pal[i] = v; }
static inline void pvr_txr_load(const void *s, pvr_ptr_t d, uint32_t n) { memcpy(d, s, n); }
#endif

static pvr_ptr_t atlas, scr[2];
static pvr_poly_hdr_t h_atlas, h_scr[2], h_col;
static int scr_cur, ready;
static uint16_t tw8[64];
static uint8_t twb[32];
static uint32_t pal_cur[1024];
uint32_t mq_pvr_headers, mq_pvr_quads, mq_pvr_tiles;
#define TGT() pvr_dr_target()
#define COMMIT(p) pvr_dr_commit(p)

static uint32_t twid(uint32_t x, uint32_t y, int bits) {
    uint32_t o = 0;
    for (int b = 0; b < bits; b++) o |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1);
    return o;
}

int mq_pvr_init(void) {
    for (int i = 0; i < 64; i++) tw8[i] = (uint16_t)twid(i & 7, i >> 3, 3);
    for (int j = 0; j < 4; j++) for (int x = 0; x < 8; x++) twb[j * 8 + x] = (uint8_t)(tw8[2 * j * 8 + x] >> 1);
#ifndef MQ_PVR_SIM
    if (!(atlas = pvr_mem_malloc(512 * 512 / 2))) return -1;
    memset((void *)atlas, 0, 512 * 512 / 2);
    pvr_poly_cxt_t c;
    pvr_poly_cxt_txr(&c, PVR_LIST_PT_POLY, PVR_TXRFMT_PAL4BPP | PVR_TXRFMT_TWIDDLED, 512, 512, atlas, PVR_FILTER_NONE);
    c.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    c.gen.culling = PVR_CULLING_NONE;
    c.txr.env = PVR_TXRENV_REPLACE;
    pvr_poly_compile(&h_atlas, &c);
    for (int i = 0; i < 2; i++) {
        if (!(scr[i] = pvr_mem_malloc(512 * 256 * 2))) return -1;
        pvr_poly_cxt_txr(&c, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, 512, 256, scr[i], PVR_FILTER_NONE);
        c.gen.culling = PVR_CULLING_NONE;
        pvr_poly_compile(&h_scr[i], &c);
    }
    pvr_poly_cxt_col(&c, PVR_LIST_OP_POLY);
    c.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&h_col, &c);
    pvr_set_pal_format(PVR_PAL_ARGB8888);
    PVR_SET(PVR_PT_ALPHA_REF, 0x80);
#else
    atlas = mq_sim_vram;
    h_atlas.cmd = 0x80000000u | 1; h_atlas.mode3 = 0;
    for (int i = 0; i < 2; i++) {
        scr[i] = mq_sim_vram + 131072 + i * 262144;
        h_scr[i].cmd = 0x80000000u | (uint32_t)(3 + i);
        h_scr[i].mode3 = (uint32_t)(scr[i] - mq_sim_vram) >> 3;
    }
    h_col.cmd = 0x80000000u | 5;
#endif
    for (int i = 0; i < 1024; i++) { pal_cur[i] = 0; pvr_set_pal_entry(i, 0); }
    ready = 1;
    return 0;
}

/* ------------------------------------------------------------ uploads */
/* twiddled 8x8 4-bit cell: byte tw8[2j*8+x]>>1 holds texel (x, 2j) in its
   low and (x, 2j+1) in its high nibble */


static void upload_tiles(void) {
    for (int i = 0; i < mq.nd; i++) {
        int t = mq.dt[i], tx = (t & 63) * 8, ty = (t >> 6) * 8;
        uint32_t t0 = twid(tx, ty, 9);
        uint32_t cell[8];
        uint8_t *c = (uint8_t *)cell;
        const uint16_t *w = (const uint16_t *)mq.dpx[i];
        for (int j = 0; j < 4; j++) {
            /* rows 2j and 2j+1, pixels 0..7 as nibbles: VRAM word 0 = pixels 0..3, high nibble first */
            uint32_t a = (uint32_t)w[j * 4] << 16 | w[j * 4 + 1];
            uint32_t b = (uint32_t)w[j * 4 + 2] << 16 | w[j * 4 + 3];
            const uint8_t *o = &twb[j * 8];
            for (int x = 0; x < 8; x++) {
                int sh = 28 - 4 * x;
                c[o[x]] = (uint8_t)(((a >> sh) & 15) | ((b >> sh) & 15) << 4);
            }
        }
        volatile uint32_t *dst = (volatile uint32_t *)((uint8_t *)atlas + (t0 >> 1));
        for (int k = 0; k < 8; k++) dst[k] = cell[k];
    }
    mq_pvr_tiles += (uint32_t)mq.nd;
}

static inline uint32_t argb8888(uint16_t c, int clear) {
    return (clear ? 0 : 0xff000000u) | (uint32_t)(c >> 11) << 19 | (uint32_t)((c >> 5) & 63) << 10 | (uint32_t)(c & 31) << 3;
}

static void upload_palette(void) {
    for (int e = 0; e < mq.nbanks * 16; e++) {
        uint32_t v = argb8888(mq.col[e], (e & 15) == 0);
        if (v != pal_cur[e]) { pal_cur[e] = v; pvr_set_pal_entry(e, v); }
    }
}

/* -------------------------------------------------------------- scene */
static float OX, OY;

static inline void vtx(uint32_t flags, float x, float y, float z, float u, float v, uint32_t argb) {
    pvr_vertex_t *p = (pvr_vertex_t *)TGT();
    p->flags = flags; p->x = x; p->y = y; p->z = z; p->u = u; p->v = v;
    p->argb = argb; p->oargb = 0;
    COMMIT(p);
}

static inline void header(const pvr_poly_hdr_t *h, uint32_t mode3) {
    pvr_poly_hdr_t *p = (pvr_poly_hdr_t *)TGT();
    *p = *h;
    p->mode3 = mode3;
    COMMIT(p);
    mq_pvr_headers++;
}

static void op_list(int cur, int W) {
    float xr = OX + 2 * W;
    header(&h_col, h_col.mode3);
    for (int s = 0; s < mq.ns; s++) {
        const mq_strip_t *st = &mq.s[s];
        if (st->sw) continue;
        float y0 = OY + 2 * st->y0, y1 = OY + 2 * (st->y1 + 1);
        uint32_t c = argb8888(st->back, 0);
        vtx(PVR_CMD_VERTEX, OX, y0, 0.5f, 0, 0, c);
        vtx(PVR_CMD_VERTEX, xr, y0, 0.5f, 0, 0, c);
        vtx(PVR_CMD_VERTEX, OX, y1, 0.5f, 0, 0, c);
        vtx(PVR_CMD_VERTEX_EOL, xr, y1, 0.5f, 0, 0, c);
    }
    if (!mq.any_sw) return;
    header(&h_scr[cur], h_scr[cur].mode3);
    float ur = W / 512.0f;
    for (int s = 0; s < mq.ns; s++) {
        const mq_strip_t *st = &mq.s[s];
        if (!st->sw) continue;
        float y0 = OY + 2 * st->y0, y1 = OY + 2 * (st->y1 + 1);
        float v0 = st->y0 / 256.0f, v1 = (st->y1 + 1) / 256.0f;
        vtx(PVR_CMD_VERTEX, OX, y0, 200.0f, 0, v0, 0xffffffff);
        vtx(PVR_CMD_VERTEX, xr, y0, 200.0f, ur, v0, 0xffffffff);
        vtx(PVR_CMD_VERTEX, OX, y1, 200.0f, 0, v1, 0xffffffff);
        vtx(PVR_CMD_VERTEX_EOL, xr, y1, 200.0f, ur, v1, 0xffffffff);
    }
}

static uint16_t order[MQ_MAX_QUADS];
static uint16_t cnt[65];

static void pt_list(void) {
    int n = mq.nq;
    memset(cnt, 0, sizeof(cnt));
    for (int i = 0; i < n; i++) cnt[mq.q[i].bank + 1]++;
    for (int k = 1; k <= 64; k++) cnt[k] += cnt[k - 1];
    for (int i = 0; i < n; i++) order[cnt[mq.q[i].bank]++] = (uint16_t)i;
    int last = -1;
    const float su = 1.0f / 512, sv = 1.0f / 512;
    for (int k = 0; k < n; k++) {
        const mq_quad_t *q = &mq.q[order[k]];
        if (q->bank != last) {
            header(&h_atlas, (h_atlas.mode3 & ~(0x3Fu << 21)) | (uint32_t)q->bank << 21);
            last = q->bank;
        }
        float x0 = OX + 2 * q->x0, x1 = OX + 2 * q->x1, y0 = OY + 2 * q->y0, y1 = OY + 2 * q->y1;
        float u0 = q->u0 * su, u1 = q->u1 * su, v0 = q->v0 * sv, v1 = q->v1 * sv;
        float z = 1.0f + q->z * (1.0f / 256);
        vtx(PVR_CMD_VERTEX, x0, y0, z, u0, v0, 0xffffffff);
        vtx(PVR_CMD_VERTEX, x1, y0, z, u1, v0, 0xffffffff);
        vtx(PVR_CMD_VERTEX, x0, y1, z, u0, v1, 0xffffffff);
        vtx(PVR_CMD_VERTEX_EOL, x1, y1, z, u1, v1, 0xffffffff);
    }
    mq_pvr_quads += (uint32_t)n;
}

void mq_pvr_show(const uint16_t *fb, int pitch, int y0) {
    if (!ready) return;
    int W = mq.w ? mq.w : 320, H = mq.h ? mq.h : 224;
    OX = (float)(320 - W);
    OY = (float)(240 - H);
    int cur = scr_cur ^= 1;
#ifndef MQ_PVR_SIM
    pvr_wait_render_done();     /* the last scene may still read the atlas and palette */
#endif
    upload_tiles();
    upload_palette();
    if (mq.any_sw)
        for (int s = 0; s < mq.ns; s++) {
            const mq_strip_t *st = &mq.s[s];
            if (!st->sw) continue;
            for (int y = st->y0; y <= st->y1 && y < 256; y++)
                pvr_txr_load(fb + (y0 + y) * pitch, (pvr_ptr_t)((uint8_t *)scr[cur] + y * 1024), (uint32_t)W * 2);
        }
#ifndef MQ_PVR_SIM
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    op_list(cur, W);
    pvr_list_finish();
    pvr_list_begin(PVR_LIST_PT_POLY);
    pt_list();
    pvr_list_finish();
    pvr_scene_finish();
#else
    mq_sim_words = 0;
    op_list(cur, W);
    pt_list();
#endif
}

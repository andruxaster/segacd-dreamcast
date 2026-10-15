/* mq: Mega Drive VDP line ranges -> tile rectangles (see mq.h) */
#include <string.h>
#include "pico/pico_int.h"
#include "mq.h"

mq_frame_t mq;
int mq_enabled, mq_verify;

static uint32_t tdirty[64];        /* tiles written since the last staging */
static uint8_t  tstate[2048];      /* 0 unknown, 1 blank, 2 has pixels */
static uint16_t cell_of[2048];     /* the atlas cell the tile is drawn from in this frame */
static uint8_t  tused[2048];       /* drawn from cell_of[] in this frame */
static int      next_spare;
static uint16_t vcram[MQ_MAX_VER][64];
static int      nver;
static int      frame_open;

/* PicoDrive's sprite line flags (draw.c) */
#define SPRL_TILE_OVFL   0x04
#define SPRL_HAVE_MASK0  0x02

/* ------------------------------------------------------------ tiles */
void mq_vram_written(uint32_t a, uint32_t n) {
    if (!n) return;
    if (n >= 0x10000) { mq_reset_tiles(); return; }
    uint32_t t = (a & 0xffff) >> 5, e = ((a + n - 1) & 0xffff) >> 5;
    for (;;) {
        if (tused[t]) {
            /* drawn earlier in this frame: those rectangles keep the
               contents of now, the rest of the frame uses a spare cell */
            tused[t] = 0;
            if (next_spare < MQ_CELLS && mq.nd < MQ_MAX_STAGE) {
                mq.dt[mq.nd] = cell_of[t];
                memcpy(mq.dpx[mq.nd], PicoMem.vram + t * 16, 32);
                mq.nd++;
                cell_of[t] = (uint16_t)next_spare++;
                mq.snapshots++;
            } else
                mq.spare_over++;
        }
        tdirty[t >> 5] |= 1u << (t & 31);
        tstate[t] = 0;
        if (t == e) break;
        t = (t + 1) & 2047;
    }
}

void mq_reset_tiles(void) {
    memset(tdirty, 0xff, sizeof(tdirty));
    memset(tstate, 0, sizeof(tstate));
}

static inline int tile_blank(int t) {
    int s = tstate[t];
    if (!s) {
        const uint32_t *p = (const uint32_t *)(PicoMem.vram + t * 16);
        s = (p[0] | p[1] | p[2] | p[3] | p[4] | p[5] | p[6] | p[7]) ? 2 : 1;
        tstate[t] = (uint8_t)s;
    }
    return s == 1;
}

/* ------------------------------------------------------------ frame */
void mq_frame_begin(void) {
    mq.nq = mq.ns = mq.nbanks = mq.any_sw = 0;
    mq.w = 0;
    mq.h = (Pico.video.reg[1] & 8) ? 240 : 224;
    nver = 0;
    frame_open = 1;
    mq.nd = 0;
    for (int t = 0; t < 2048; t++) cell_of[t] = (uint16_t)t;
    memset(tused, 0, sizeof(tused));
    next_spare = 2048;
}

void mq_frame_close(void) {
    if (!frame_open) return;
    frame_open = 0;
    /* staged so far: the snapshots of rewritten tiles. Now every written
       tile into the cell the end of the frame draws it from; a tile moved
       to a spare cell stays dirty, so its own cell gets these contents
       for the next frame */
    int n = mq.nd;
    memset(tused, 0, sizeof(tused));
    for (int w = 0; w < 64; w++) {
        uint32_t b = tdirty[w], keep = 0;
        if (!b) continue;
        for (int i = 0; i < 32; i++)
            if (b & (1u << i)) {
                int t = w * 32 + i;
                if (n >= MQ_MAX_STAGE) { keep |= 1u << i; continue; }
                mq.dt[n] = cell_of[t];
                memcpy(mq.dpx[n], PicoMem.vram + t * 16, 32);
                n++;
                if (cell_of[t] != t) keep |= 1u << i;
            }
        tdirty[w] = keep;
    }
    mq.nd = n;
}

/* ------------------------------------------------------------ strips */
static void add_strip(int from, int to, int sw, int why, uint16_t back) {
    if (mq.ns >= 240) return;
    mq_strip_t *s = &mq.s[mq.ns];
    if (mq.ns && s[-1].y1 == from - 1 && s[-1].sw == sw && (sw || s[-1].back == back)) {
        s[-1].y1 = (int16_t)to;
        return;
    }
    s->y0 = (int16_t)from; s->y1 = (int16_t)to; s->sw = (uint8_t)sw; s->why = (uint8_t)why; s->back = back;
    mq.ns++;
}

static int sw_strip(int from, int to, int why) {
    add_strip(from, to, 1, why, 0);
    mq.any_sw = 1;
    mq.sw_lines += (uint32_t)(to - from + 1);
    mq.why[why]++;
    return 0;
}

/* banks of the current CRAM, a new version when it changed */
static int pal_base(void) {
    if (nver && !memcmp(vcram[nver - 1], PicoMem.cram, 128)) return (nver - 1) * 4;
    if (nver >= MQ_MAX_VER) return -1;
    memcpy(vcram[nver], PicoMem.cram, 128);
    for (int i = 0; i < 64; i++) mq.col[nver * 64 + i] = mq_rgb565(PicoMem.cram[i]);
    nver++;
    mq.nbanks = nver * 4;
    return (nver - 1) * 4;
}

/* ------------------------------------------------------------ rectangles */
static int W, overflow, bankbase, nq_frame0;   /* nq_frame0: no merging into an earlier range */

/* tile t with its left edge at screen x sx, clipped to [cl, cr), lines
   y0..y1-1, r0 = tile row (display order) at y0 */
static inline void emit(int t, int code, int sx, int cl, int cr, int y0, int y1, int r0, int z) {
    int x0 = sx > cl ? sx : cl, x1 = sx + 8 < cr ? sx + 8 : cr;
    if (x0 >= x1) return;
    int c = cell_of[t];
    tused[t] = 1;
    int ax = (c & 63) * 8, ay = (c >> 6) * 8, n = y1 - y0, u0, u1, v0, v1;
    int bank = bankbase + ((code >> 13) & 3);
    if (code & 0x0800) { u0 = ax + 8 - (x0 - sx); u1 = ax + 8 - (x1 - sx); }
    else               { u0 = ax + x0 - sx;       u1 = ax + x1 - sx; }
    if (code & 0x1000) { v0 = ay + 8 - r0;         v1 = ay + 8 - r0 - n; }
    else               { v0 = ay + r0;             v1 = ay + r0 + n; }
    /* the right neighbour of the last rectangle with the next texels of the
       same atlas row (tiles t, t+1 side by side in VRAM order): one wider
       rectangle */
    if (mq.nq > nq_frame0) {
        mq_quad_t *p = &mq.q[mq.nq - 1];
        if (p->x1 == x0 && p->u1 == u0 && p->y0 == y0 && p->y1 == y1 && p->v0 == v0 && p->v1 == v1 &&
            p->z == z && p->bank == bank && ((p->u1 > p->u0) == (u1 > u0))) {
            p->x1 = (int16_t)x1; p->u1 = (int16_t)u1;
            return;
        }
    }
    if (mq.nq >= MQ_MAX_QUADS) { overflow = 1; return; }
    mq_quad_t *q = &mq.q[mq.nq++];
    q->x0 = (int16_t)x0; q->x1 = (int16_t)x1; q->y0 = (int16_t)y0; q->y1 = (int16_t)y1;
    q->u0 = (int16_t)u0; q->u1 = (int16_t)u1; q->v0 = (int16_t)v0; q->v1 = (int16_t)v1;
    q->z = (uint16_t)z;
    q->bank = (uint8_t)bank;
}

static const int shiftw[4] = { 5, 6, 5, 7 };

/* plane A (0) or B (1), lines ya..yb with horizontal scroll hs, cells
   cellskip..maxcells (as DrawLayer / DrawStrip / DrawStripVSRam) */
static void plane_run(int plane, int ya, int yb, int hs, int cellskip, int maxcells) {
    const uint8_t *reg = Pico.video.reg;
    int width = reg[16] & 3, height = (reg[16] >> 4) & 3;
    int xmask = (1 << shiftw[width]) - 1, ymask = (height << 8) | 0xff;
    switch (width) { case 1: ymask &= 0x1ff; break; case 2: ymask = 0x007; break; case 3: ymask = 0x0ff; break; }
    int nametab = plane ? (reg[4] & 7) << 12 : (reg[2] & 0x38) << 9;
    int zlo = plane ? 1 << 8 : 2 << 8, zhi = plane ? 4 << 8 : 5 << 8;
    int cl = cellskip * 8, cr = maxcells * 8;
    int sxs[48], txs[48], vss[48], n = 0;

    if (!(reg[11] & 4)) {
        int tilex = ((-hs) >> 3) + cellskip, dx = ((hs - 1) & 7) + 1 + cellskip * 8;
        int cnt = maxcells - cellskip + ((dx & 7) ? 1 : 0), v = PicoMem.vsram[plane];
        for (int k = 0; k < cnt; k++) { sxs[k] = dx - 8 + 8 * k; txs[k] = tilex + k; vss[k] = v; }
        n = cnt;
    } else {
        /* 2-cell columns: the same walk as DrawStripVSRam */
        int tilex = (-hs) >> 3, dx = ((hs - 1) & 7) + 1, cell = 0, cells = maxcells, left = -1;
        if (hs & 0x0f) {
            int adj = ((hs ^ dx) >> 3) & 1;
            cell -= adj + 1; cells -= adj + 1;
            left = PicoMem.vsram[((reg[12] & 1) ? 0x00 : 0x20) + plane];
        }
        cell += cellskip; tilex += cellskip; dx += cellskip << 3;
#define VS(c) ((((c) & 0x3e) == 0x3e && left >= 0) ? left : PicoMem.vsram[plane + ((c) & 0x3e)])
        int v = 0;
        if ((cell & 1) == 1 || (dx & 7)) v = VS(cell);
        if (dx & 7) { sxs[n] = dx - 8; txs[n] = tilex; vss[n++] = v; dx += 8; tilex++; cell++; }
        for (; cell < cells; dx += 8, tilex++, cell++) {
            if (!(cell & 1)) v = VS(cell);
            sxs[n] = dx - 8; txs[n] = tilex; vss[n++] = v;
        }
        if (dx & 7) { if (!(cell & 1)) v = VS(cell); sxs[n] = dx - 8; txs[n] = tilex; vss[n++] = v; }
#undef VS
    }

    if (!(reg[11] & 4)) {
        /* one vertical scroll: rows outside, columns inside (neighbours merge) */
        int v = vss[0];
        for (int y = ya; y <= yb; ) {
            int py = (y + v) & ymask, r = py & 7, len = 8 - r;
            if (y + len - 1 > yb) len = yb - y + 1;
            int row = nametab + ((py >> 3) << shiftw[width]);
            for (int k = 0; k < n; k++) {
                int sx = sxs[k];
                if (sx + 8 <= cl || sx >= cr) continue;
                int code = PicoMem.vram[(row + (txs[k] & xmask)) & 0x7fff];
                int t = code & 0x7ff;
                if (!tile_blank(t)) emit(t, code, sx, cl, cr, y, y + len, r, (code & 0x8000) ? zhi : zlo);
            }
            y += len;
        }
        return;
    }
    for (int k = 0; k < n; k++) {
        int sx = sxs[k];
        if (sx + 8 <= cl || sx >= cr) continue;
        int tx = txs[k] & xmask, v = vss[k];
        for (int y = ya; y <= yb; ) {
            int py = (y + v) & ymask, r = py & 7, len = 8 - r;
            if (y + len - 1 > yb) len = yb - y + 1;
            int code = PicoMem.vram[(nametab + ((py >> 3) << shiftw[width]) + tx) & 0x7fff];
            int t = code & 0x7ff;
            if (!tile_blank(t)) emit(t, code, sx, cl, cr, y, y + len, r, (code & 0x8000) ? zhi : zlo);
            y += len;
        }
    }
}

/* window tile pairs ts..te-1 on lines ya..yb */
static void window_run(int ya, int yb, int ts, int te) {
    const uint8_t *reg = Pico.video.reg;
    int nametab, sh;
    if (reg[12] & 1) { nametab = (reg[3] & 0x3c) << 9; sh = 6; }
    else             { nametab = (reg[3] & 0x3e) << 9; sh = 5; }
    for (int y = ya; y <= yb; ) {
        int r = y & 7, len = 8 - r;
        if (y + len - 1 > yb) len = yb - y + 1;
        int base = nametab + ((y >> 3) << sh);
        for (int tx = ts * 2; tx < te * 2; tx++) {
            int code = PicoMem.vram[(base + tx) & 0x7fff], t = code & 0x7ff;
            if (!tile_blank(t)) emit(t, code, tx * 8, 0, W, y, y + len, r, (code & 0x8000) ? 5 << 8 : 2 << 8);
        }
        y += len;
    }
}

/* window state of a line: 0 none, 1 whole line, 2 split; A cells / window pairs */
typedef struct { int mode, askip, amax, wts, wte; } winl_t;

static void win_line(int line, int maxcells, winl_t *o) {
    const uint8_t *reg = Pico.video.reg;
    int win = reg[0x12], edge = (win & 0x1f) << 3, hv = 0;
    if (win & 0x80) { if (line >= edge) hv = 1; }
    else            { if (line < edge) hv = 1; }
    if (!hv) {
        win = reg[0x11]; edge = win & 0x1f;
        if (win & 0x80) { if (!edge) hv = 1; else if (edge < (maxcells >> 1)) hv = 2; }
        else            { if (!edge); else if (edge < (maxcells >> 1)) hv = 2; else hv = 1; }
    }
    o->mode = hv;
    o->askip = 0; o->amax = maxcells; o->wts = 0; o->wte = maxcells >> 1;
    if (hv == 2) {
        if (win & 0x80) { o->amax = edge << 1; o->wts = edge; }
        else            { o->askip = edge << 1; o->wte = edge; }
    }
}

/* ------------------------------------------------------------ sprites */
static int16_t sp_y0[256], sp_last[256];
static uint8_t sp_w[256], sp_open[256];
static int     nopen;

static void sprite_span(const int32_t *sp, int key, int ya, int yb, int w) {
    int32_t s0 = sp[0], s1 = sp[1];
    int width = (s0 >> 28) & 0xf, height = (s0 >> 24) & 7, sy = (int16_t)s0;
    int code = s1 & 0xffff, sx = (s1 >> 16) - 8;
    int z = ((code & 0x8000) ? 6 << 8 : 3 << 8) + 255 - (key & 0x7f);
    int delta = height, base = code;
    if (code & 0x0800) { base += delta * (width - 1); delta = -delta; }
    if (w) width = w;
    for (int y = ya; y <= yb; ) {
        int row = y - sy, r = row & 7, len = 8 - r;
        if (y + len - 1 > yb) len = yb - y + 1;
        int rr = (code & 0x1000) ? (height << 3) - 1 - row : row;
        int t0 = base + (rr >> 3);
        for (int c = 0; c < width; c++) {
            int x = sx + 8 * c;
            if (x + 8 <= 0) continue;
            if (x >= W) break;
            int t = (t0 + c * delta) & 0x7ff;
            if (!tile_blank(t)) emit(t, code, x, 0, W, y, y + len, r, z);
        }
        y += len;
    }
}

static void sprites(int from, int to, const int32_t *prespr) {
    nopen = 0;
    for (int l = from; l <= to; l++) {
        const unsigned char *p = HighLnSpr[l];
        int cnt = p[0] & 0x7f, bank = p[0] & 0x80;
        if (!cnt) continue;
        if ((p[1] & (SPRL_TILE_OVFL | SPRL_HAVE_MASK0)) == (SPRL_TILE_OVFL | SPRL_HAVE_MASK0)) continue;
        for (int i = 0; i < cnt; i++) {
            int idx = p[4 + i] & 0x7f, key = bank | idx;
            int w = (i == cnt - 1) ? p[4 + cnt] : 0;
            if (sp_open[key] && sp_last[key] == l - 1 && sp_w[key] == w) { sp_last[key] = (int16_t)l; continue; }
            if (sp_open[key]) sprite_span(prespr + bank * 2 + idx * 2, key, sp_y0[key], sp_last[key], sp_w[key]);
            else { sp_open[key] = 1; nopen++; }
            sp_y0[key] = sp_last[key] = (int16_t)l; sp_w[key] = (uint8_t)w;
        }
    }
    if (!nopen) return;
    for (int key = 0; key < 256; key++)
        if (sp_open[key]) {
            sp_open[key] = 0;
            sprite_span(prespr + (key & 0x80) * 2 + (key & 0x7f) * 2, key, sp_y0[key], sp_last[key], sp_w[key]);
        }
}

/* ------------------------------------------------------------ a line range */
int mq_lines(int from, int to, int off, int on, int rendstatus, const int32_t *prespr) {
    const uint8_t *reg = Pico.video.reg;
    int w = (reg[12] & 1) ? 320 : 256, maxcells = w / 8;
    if (!mq.w) mq.w = w;
    if (reg[12] & 8)                  return sw_strip(from, to, MQ_WHY_SH);
    if ((reg[12] & 6) == 6)           return sw_strip(from, to, MQ_WHY_ILACE);
    if (off | on)                     return sw_strip(from, to, MQ_WHY_ONOFF);
    if (rendstatus & PDRAW_BGC_DMA)   return sw_strip(from, to, MQ_WHY_BGCDMA);
    if (w != mq.w)                    return sw_strip(from, to, MQ_WHY_WIDTH);
    int pb = pal_base();
    if (pb < 0)                       return sw_strip(from, to, MQ_WHY_PAL);
    uint16_t back = mq.col[pb * 16 + (reg[7] & 0x3f)];
    if (!(reg[1] & 0x40)) {           /* display off: backdrop */
        add_strip(from, to, 0, 0, back);
        mq.hw_lines += (uint32_t)(to - from + 1);
        return 1;
    }
    W = w; bankbase = pb; overflow = 0;
    int nq0 = mq.nq;
    nq_frame0 = nq0;

    /* scroll of each line */
    int htab0 = reg[13] << 9, hm = reg[11] & 3;
    int hsa[240], hsb[240];
    winl_t wl[240];
    for (int l = from; l <= to; l++) {
        int ht = htab0;
        switch (hm) { case 1: ht += (l << 1) & 0x0f; break; case 2: ht += (l << 1) & ~0x0f; break; case 3: ht += l << 1; break; }
        hsa[l] = PicoMem.vram[ht & 0x7fff];
        hsb[l] = PicoMem.vram[(ht + 1) & 0x7fff];
        win_line(l, maxcells, &wl[l]);
    }
    /* plane B */
    for (int l = from; l <= to; ) {
        int e = l;
        while (e < to && hsb[e + 1] == hsb[l]) e++;
        plane_run(1, l, e, hsb[l], 0, maxcells);
        l = e + 1;
    }
    /* plane A and window */
    for (int l = from; l <= to; ) {
        int e = l;
        while (e < to && hsa[e + 1] == hsa[l] && wl[e + 1].mode == wl[l].mode) e++;
        const winl_t *o = &wl[l];
        if (o->mode != 1) plane_run(0, l, e, hsa[l], o->askip, o->amax);
        if (o->mode) window_run(l, e, o->wts, o->wte);
        l = e + 1;
    }
    sprites(from, to, prespr);

    if (overflow) { mq.nq = nq0; return sw_strip(from, to, MQ_WHY_QUADS); }
    add_strip(from, to, 0, 0, back);
    mq.hw_lines += (uint32_t)(to - from + 1);
    return 1;
}

/* debugging (host): a tile written since the last staging */
int mq_tile_pending(int t) { return (tdirty[(t >> 5) & 63] >> (t & 31)) & 1; }

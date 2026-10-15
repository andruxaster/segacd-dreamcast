/* mq: the Mega Drive picture as rectangles for a 3D chip (the Dreamcast PVR).

   PicoDrive draws the screen in line ranges: PicoDrawSync(to) is called
   before every VDP change that affects the picture (register, VRAM, CRAM,
   VSRAM write, DMA) and at the end of the active display, so the VDP state
   is constant over each range. With mq_enabled the hook in draw.c gives
   each range to mq_lines() instead of drawing it: one textured rectangle
   per 8x8 tile (planes A and B, window, sprite cells) clipped to the lines
   and columns where it shows, with a depth for its layer:

       backdrop < B lo < A/window lo < sprites lo < B hi < A/window hi < sprites hi
       (sprites: earlier in the sprite list = nearer)

   which is what PicoDrive draws (its default is not POPT_ACC_SPRITES).
   Per-line horizontal scroll, 2-cell vertical scroll, the window split,
   the sprite limits and masking are taken as PicoDrive does them (the
   per-line sprite lists come from its own ParseSprites).

   Tiles: the atlas (4-bit, 512x512) has 4096 cells, cell s at
   ((s & 63) * 8, (s >> 6) * 8). Cells 0..2047 are the VRAM tiles; written
   tiles are staged at the end of the active display (vblank writes are for
   the next frame). When a tile already drawn in this frame is rewritten
   during the display (Comix Zone streams its text line by line), its
   contents at that moment are staged into the cell the earlier
   rectangles use, and the rest of the frame draws the tile from a spare
   cell (2048..4095) that gets the later contents.
   Colours: each CRAM version of the frame gets 4 PVR banks (64 entries,
   colour 0 of each bank transparent), 16 versions a frame.

   Ranges that this cannot show (shadow/highlight, interlace mode 2,
   display switched inside a line, background-colour DMA, too many palette
   versions or rectangles) are drawn by PicoDrive into its line buffer and
   shown from a screen texture. */
#ifndef MQ_H
#define MQ_H
#include <stdint.h>

#define MQ_MAX_QUADS 8192
#define MQ_MAX_VER   16
#define MQ_CELLS     4096          /* atlas cells: 2048 tiles + 2048 spares */
#define MQ_MAX_STAGE 6144

typedef struct {
    int16_t  x0, y0, x1, y1;       /* MD pixels, exclusive ends */
    int16_t  u0, v0, u1, v1;       /* atlas texels at x0/y0 and x1/y1 (flips swap), 512x512 */
    uint16_t z;                    /* layer * 256 + sprite rank bonus */
    uint8_t  bank, pad;            /* PVR palette bank (4 per CRAM version) */
} mq_quad_t;

typedef struct {
    int16_t  y0, y1;               /* lines y0..y1 inclusive */
    uint16_t back;                 /* hardware: backdrop colour, RGB565 */
    uint8_t  sw;                   /* 1: PicoDrive drew it (line buffer) */
    uint8_t  why;                  /* software: reason, see MQ_WHY_* */
} mq_strip_t;

enum { MQ_WHY_SH = 1, MQ_WHY_ILACE, MQ_WHY_ONOFF, MQ_WHY_BGCDMA, MQ_WHY_WIDTH,
       MQ_WHY_PAL, MQ_WHY_QUADS, MQ_WHY_N };

typedef struct {
    mq_quad_t  q[MQ_MAX_QUADS];
    int        nq;
    mq_strip_t s[240];
    int        ns;
    int        w, h;               /* 320/256, 224/240 */
    uint16_t   col[1024];          /* RGB565 of the banks in use (entry 0 of a bank: unused) */
    int        nbanks;
    int        any_sw;
    /* atlas cells to upload: cell, raw VRAM bytes of the tile (PicoMem order) */
    uint16_t   dt[MQ_MAX_STAGE];
    uint8_t    dpx[MQ_MAX_STAGE][32];
    int        nd;
    /* statistics */
    uint32_t   hw_lines, sw_lines, why[MQ_WHY_N];
    uint32_t   snapshots, spare_over;  /* tiles rewritten after use; spares ran out */
} mq_frame_t;

extern mq_frame_t mq;
extern int mq_enabled;             /* rectangles instead of PicoDrive's line renderer */
extern int mq_verify;              /* host: draw in software as well */

/* from the core */
void mq_frame_begin(void);                          /* PicoFrameStart */
int  mq_lines(int from, int to, int off, int on, int rendstatus, const int32_t *prespr);
void mq_frame_close(void);                          /* end of the active display */
void mq_vram_written(uint32_t a, uint32_t n);       /* VRAM bytes a..a+n-1 changed */
void mq_reset_tiles(void);                          /* all tiles again (state load, init) */

/* the RGB565 PicoDrive makes of a CRAM word (PicoDoHighPal555, no s/h) */
static inline uint16_t mq_rgb565(uint16_t c) {
    uint32_t t = ((c & 0x000e) << 12) | ((c & 0x00e0) << 3) | ((c & 0x0e00) >> 7);
    t |= (t >> 4) & 0x0841;
    t |= ((t ^ 0x0841) & (t >> 3 | t >> 2) & 0x0841) << 1;
    return (uint16_t)t;
}
#endif

/***************************************************************************************
 *  Genesis Plus
 *  CD graphics processor
 *
 *  Copyright (C) 2012  Eke-Eke (Genesis Plus GX)
 *
 *  Redistribution and use of this code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *   - Redistributions may not be sold, nor may they be used in a commercial
 *     product or activity.
 *
 *   - Redistributions that are modified from the original source must include the
 *     complete source code, including the source code for all components used by a
 *     binary built from the modified sources. However, as a special exception, the
 *     source code distributed need not include anything that is normally distributed
 *     (in either source or binary form) with the major components (compiler, kernel,
 *     and so on) of the operating system on which the executable runs, unless that
 *     component itself accompanies the executable.
 *
 *   - Redistributions must reproduce the above copyright notice, this list of
 *     conditions and the following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************************/
#include "../pico_int.h"
#include "genplus_macros.h"

typedef struct
{
  //uint32 cycles;                    /* current cycles count for graphics operation */
  //uint32 cyclesPerLine;             /* current graphics operation timings */
  uint32 dotMask;                   /* stamp map size mask */
  uint32 stampMask;                 /* stamp number mask */
  uint16 *tracePtr;                 /* trace vector pointer */
  uint16 *mapPtr;                   /* stamp map table base address */
  uint8 stampShift;                 /* stamp pixel shift value (related to stamp size) */
  uint8 mapShift;                   /* stamp map table shift value (related to stamp map size) */
  uint16 bufferOffset;              /* image buffer column offset */
  uint32 bufferStart;               /* image buffer start index */
  uint32 y_step;                    /* pico: render line step */
  uint8 lut_prio[4][0x10][0x10];    /* WORD-RAM data writes priority lookup table */
  uint8 lut_pixel[0x200];           /* Graphics operation dot offset lookup table */
  uint16 lut_cell2[0x80];           /* Graphics operation stamp offset lookup table */
  uint16 lut_cell4[0x80];           /* Graphics operation stamp offset lookup table */
  /* snatcher-dc: cell and pixel offset in one step, by HFLIP/ROTATION and
     the pixel's row and column in the stamp: hrr | y << 3 | x << 7 (16x16)
     or hrr | y << 3 | x << 8 (32x32) */
  uint16 lut_off16[8 * 16 * 16];
  uint16 lut_off32[8 * 32 * 32];
} gfx_t;

static gfx_t gfx;

static void gfx_schedule(void);

/***************************************************************/
/*      Rotation / Scaling operation (2M Mode)                 */
/***************************************************************/

void gfx_init(void)
{
  int i, j;
  uint8 row, col, temp;

  memset(&gfx, 0, sizeof(gfx));

  /* Initialize priority modes lookup table */
  for (i = 0; i < 0x10; i++)
  {
    for (j = 0; j < 0x10; j++)
    {
      /* normal */
      gfx.lut_prio[0][i][j] = j;
      /* underwrite */
      gfx.lut_prio[1][i][j] = i ? i : j;
      /* overwrite */
      gfx.lut_prio[2][i][j] = j ? j : i;
      /* invalid */
      gfx.lut_prio[3][i][j] = i;
    }
  }

  /* Initialize cell lookup table             */
  /* table entry = yyxxhrr (7 bits)           */
  /* with: yy = cell row (0-3)                */
  /*       xx = cell column (0-3)             */
  /*      hrr = HFLIP & ROTATION bits         */
  for (i=0; i<0x80; i++)
  {
    /* one stamp = 2x2 cells (16x16) or 4x4 cells (32x32) */
    row = (i >> 5) & 3;
    col = (i >> 3) & 3;

    if (i & 4) { col = col ^ 3; }  /* HFLIP (always first)  */ 
    if (i & 2) { col = col ^ 3; row = row ^ 3; }  /* ROLL1 */
    if (i & 1) { temp = col; col = row ^ 3; row = temp; }  /* ROLL0  */

    /* cell offset (0-3 or 0-15) */
    gfx.lut_cell2[i] = ((row&1) + (col&1) * 2) << 6;
    gfx.lut_cell4[i] = ((row&3) + (col&3) * 4) << 6;
  }

  /* Initialize pixel lookup table      */
  /* table entry = yyyxxxhrr (9 bits)   */
  /* with:  yyy = pixel row  (0-7)      */
  /*        xxx = pixel column (0-7)    */
  /*        hrr = HFLIP & ROTATION bits */
  for (i=0; i<0x200; i++)
  {
    /* one cell = 8x8 pixels */
    row = (i >> 6) & 7;
    col = (i >> 3) & 7;

    if (i & 4) { col = col ^ 7; }   /* HFLIP (always first) */ 
    if (i & 2) { col = col ^ 7; row = row ^ 7; }  /* ROLL1 */
    if (i & 1) { temp = col; col = row ^ 7; row = temp; } /* ROLL0 */

    /* pixel offset (0-63) */
    gfx.lut_pixel[i] = col + row * 8;
  }

  /* snatcher-dc: combined tables (same values as lut_cell | lut_pixel) */
  for (i = 0; i < 8; i++)
    for (j = 0; j < 32; j++)
    {
      int k;
      for (k = 0; k < 32; k++)
      {
        uint32 y = j, x = k;
        uint32 c4 = gfx.lut_cell4[i | ((y >> 3) & 3) << 5 | ((x >> 3) & 3) << 3];
        uint32 c2 = gfx.lut_cell2[i | ((y >> 3) & 3) << 5 | ((x >> 3) & 3) << 3];
        uint32 p = gfx.lut_pixel[i | (y & 7) << 6 | (x & 7) << 3];
        gfx.lut_off32[i | j << 3 | k << 8] = (uint16)(c4 | p);
        if (j < 16 && k < 16)
          gfx.lut_off16[i | j << 3 | k << 7] = (uint16)(c2 | p);
      }
    }
}

int gfx_context_save(uint8 *state)
{
  uint32 tmp32;
  int bufferptr = 0;

  //save_param(&gfx.cycles, sizeof(gfx.cycles));
  //save_param(&gfx.cyclesPerLine, sizeof(gfx.cyclesPerLine));
  save_param(&gfx.dotMask, sizeof(gfx.dotMask));
  save_param(&gfx.stampShift, sizeof(gfx.stampShift));
  save_param(&gfx.mapShift, sizeof(gfx.mapShift));
  save_param(&gfx.bufferOffset, sizeof(gfx.bufferOffset));
  save_param(&gfx.bufferStart, sizeof(gfx.bufferStart));

  tmp32 = (uint8 *)(gfx.tracePtr) - Pico_mcd->word_ram2M;
  save_param(&tmp32, 4);

  tmp32 = (uint8 *)(gfx.mapPtr) - Pico_mcd->word_ram2M;
  save_param(&tmp32, 4);

  save_param(&gfx.y_step, sizeof(gfx.y_step));
  save_param(&gfx.stampMask, sizeof(gfx.stampMask));

  return bufferptr;
}

int gfx_context_load(const uint8 *state)
{
  uint32 tmp32;
  int bufferptr = 0;

  //load_param(&gfx.cycles, sizeof(gfx.cycles));
  //load_param(&gfx.cyclesPerLine, sizeof(gfx.cyclesPerLine));
  load_param(&gfx.dotMask, sizeof(gfx.dotMask));
  load_param(&gfx.stampShift, sizeof(gfx.stampShift));
  load_param(&gfx.mapShift, sizeof(gfx.mapShift));
  load_param(&gfx.bufferOffset, sizeof(gfx.bufferOffset));
  load_param(&gfx.bufferStart, sizeof(gfx.bufferStart));

  load_param(&tmp32, 4);
  gfx.tracePtr = (uint16 *)(Pico_mcd->word_ram2M + tmp32);

  load_param(&tmp32, 4);
  gfx.mapPtr = (uint16 *)(Pico_mcd->word_ram2M + tmp32);

  load_param(&gfx.y_step, sizeof(gfx.y_step));
  load_param(&gfx.stampMask, sizeof(gfx.stampMask));

  return bufferptr;
}

/* dc-poker: the gfx state comes in as locals (GFX_LOCALS in gfx_render);
   the byte writes to Word RAM would otherwise make the compiler reload
   every field of gfx for each pixel */
#define gfx_pixel(x, y, c) gfx_pixel_(x, y, c, dotMask, stampMask, mapPtr, \
    stampShift, mapShift, lut_pixel, wram)
static inline int gfx_pixel_(uint32 xpos, uint32 ypos, uint16 *lut_cell,
    uint32 dotMask, uint32 stampMask, const uint16 *mapPtr, int stampShift,
    int mapShift, const uint8 *lut_pixel, const uint8 *wram)
{
  uint16 stamp_data;
  uint32 stamp_index;
  uint8 pixel_out = 0x00;

  /* check if pixel is outside stamp map */
  if (((xpos | ypos) & ~dotMask) == 0)
  {
    /* read stamp map table data */
    stamp_data = mapPtr[(xpos >> stampShift) | ((ypos >> stampShift) << mapShift)];

    /* stamp generator base index                                     */
    /* sss ssssssss ccyyyxxx (16x16) or sss sssssscc ccyyyxxx (32x32) */
    /* with:  s = stamp number (1 stamp = 16x16 or 32x32 pixels)      */
    /*        c = cell offset  (0-3 for 16x16, 0-15 for 32x32)        */
    /*      yyy = line offset  (0-7)                                  */
    /*      xxx = pixel offset (0-7)                                  */
    stamp_index = (stamp_data & stampMask) << 8;

    if (stamp_index)
    {
      /* extract HFLIP & ROTATION bits */
      stamp_data = (stamp_data >> 13) & 7;

      /* cell offset (0-3 or 0-15)                             */
      /* table entry = yyxxhrr (7 bits)                        */
      /* with: yy = cell row  (0-3) = (ypos >> (11 + 3)) & 3   */
      /*       xx = cell column (0-3) = (xpos >> (11 + 3)) & 3 */
      /*      hrr = HFLIP & ROTATION bits                      */
      stamp_index |= lut_cell[stamp_data | ((ypos >> 9) & 0x60) | ((xpos >> 11) & 0x18)];

      /* pixel  offset (0-63)                              */
      /* table entry = yyyxxxhrr (9 bits)                  */
      /* with: yyy = pixel row  (0-7) = (ypos >> 11) & 7   */
      /*       xxx = pixel column (0-7) = (xpos >> 11) & 7 */
      /*       hrr = HFLIP & ROTATION bits                 */
      stamp_index |= lut_pixel[stamp_data | ((ypos >> 5) & 0x1c0) | ((xpos >> 8) & 0x38)];

      /* read pixel pair (2 pixels/byte) */
      pixel_out = READ_BYTE(wram, stamp_index >> 1);

      /* extract left or right pixel */
      pixel_out >>= 4 * !(stamp_index & 1);
      pixel_out &= 0x0f;
    }
  }

  return pixel_out;
}

#define RENDER_LOOP(N, UPDP, COND1, COND2) do {				\
  if (bufferIndex & 1) {						\
    bufferIndex ^= 1;							\
    goto right##N; /* no initial left pixel */				\
  }									\
  /* process all dots */						\
  while (width--)							\
  {									\
    /* left pixel */							\
    xpos &= mask;							\
    ypos &= mask;							\
									\
    if (COND1) {							\
      pixel_out = gfx_pixel(xpos, ypos, lut_cell);			\
      UPDP;								\
    }									\
									\
    if (COND2) {							\
      /* read out paired pixel data */					\
      pixel_in = READ_BYTE(wram, bufferIndex >> 1);			\
									\
      /* priority mode write */						\
      pixel_in = (lut_prio[(pixel_in & 0xf0) >> 4][pixel_out] << 4) |	\
                 (pixel_in & 0x0f);					\
									\
      /* write data to image buffer */					\
      WRITE_BYTE(wram, bufferIndex >> 1, pixel_in);			\
    }									\
									\
    /* increment pixel position */					\
    xpos += xoffset;							\
    ypos += yoffset;							\
									\
right##N:								\
    if (width-- == 0) break;						\
									\
    /* right pixel */							\
    xpos &= mask;							\
    ypos &= mask;							\
									\
    if (COND1) {							\
      pixel_out = gfx_pixel(xpos, ypos, lut_cell);			\
      UPDP;								\
    }									\
									\
    if (COND2) {							\
      /* read out paired pixel data */					\
      pixel_in = READ_BYTE(wram, bufferIndex >> 1);			\
									\
      /* priority mode write */						\
      pixel_in = (lut_prio[pixel_in & 0x0f][pixel_out]) |		\
                 (pixel_in & 0xf0);					\
									\
      /* write data to image buffer */					\
      WRITE_BYTE(wram, bufferIndex >> 1, pixel_in);			\
    }									\
									\
    /* increment pixel position */					\
    xpos += xoffset;							\
    ypos += yoffset;							\
									\
    /* next pixel */							\
    bufferIndex += 2;							\
    /* check current pixel position  */					\
    if ((bufferIndex & 7) == 0)						\
    {									\
      /* next cell: increment buffer offset by one column (minus 8 pixels) */ \
      bufferIndex += bufferOffset-1;					\
    }									\
  }									\
} while (0)

static void gfx_render_ref(uint32 bufferIndex, uint32 width)
{
  uint8 pixel_in, pixel_out;
  uint32 priority;
  uint8 (*lut_prio)[0x10];
  uint16 *lut_cell;
  uint32 mask;
  const uint32 dotMask = gfx.dotMask, stampMask = gfx.stampMask;
  const uint16 *mapPtr = gfx.mapPtr;
  const int stampShift = gfx.stampShift, mapShift = gfx.mapShift;
  const uint8 *lut_pixel = gfx.lut_pixel;
  uint8 *wram = Pico_mcd->word_ram2M;
  const uint32 bufferOffset = gfx.bufferOffset;

  /* pixel map start position for current line (13.3 format converted to 13.11) */
  uint32 xpos = *gfx.tracePtr++ << 8;
  uint32 ypos = *gfx.tracePtr++ << 8;

  /* pixel map offset values for current line (5.11 format) */
  uint32 xoffset = (int16) *gfx.tracePtr++;
  uint32 yoffset = (int16) *gfx.tracePtr++;

  priority = (Pico_mcd->s68k_regs[2] << 8) | Pico_mcd->s68k_regs[3];
  priority = (priority >> 3) & 0x03;
  lut_prio = gfx.lut_prio[priority];

  lut_cell = (Pico_mcd->s68k_regs[0x58+1] & 0x02) ? gfx.lut_cell4 : gfx.lut_cell2;

  /* check if stamp map is repeated */
  mask = 0xffffff; /* 24-bit range */
  if (Pico_mcd->s68k_regs[0x58+1] & 0x01)
  {
    /* stamp map range */
    mask = gfx.dotMask;
  }

  pixel_out = 0;
  if (xoffset+(1U<<10) <= 1U<<11 && yoffset+(1U<<10) <= 1U<<11) {
    /* upscaling >= 2x, test for duplicate pixels to avoid recalculation */
    uint32 oldx, oldy;
    oldx = oldy = ~xpos;
    RENDER_LOOP(1, oldx = xpos;oldy = ypos, (oldx^xpos ^ oldy^ypos) >> 11, (!priority) | pixel_out);
  } else {
    RENDER_LOOP(3,                        , 1, (!priority) | pixel_out);
  }
}

/* snatcher-dc: the same operation, a pixel pair per Word RAM byte (Sega CD
   BIOS logo: ~24000 pixels a frame), same results as gfx_render_ref (kept
   for the PC build check, GFX_REF):
   - the coordinates are kept 8 bits up (24-bit position in the top bits),
     so they wrap at 24 bits by themselves and need no mask per pixel;
   - one combined table gives the cell and pixel offset in the stamp;
   - the destination byte is read (only if the priority mode needs it) and
     written once per pixel pair;
   - a loop per priority mode, stamp size and repeat mode. */
#define GFX_SRC(X, Y, OUT, BIG, REP) do {                                   \
    uint32 x_ = (X), y_ = (Y);                                              \
    OUT = 0;                                                                \
    if (REP) { x_ &= dmask; y_ &= dmask; }                                  \
    if (REP || ((x_ | y_) & outside) == 0) {                                \
      uint32 sd_ = mapPtr[(x_ >> sshift) | ((y_ >> sshift) << mapShift)];   \
      uint32 si_ = (sd_ & stampMask) << 8;                                  \
      if (si_) {                                                            \
        uint32 h_ = (sd_ >> 13) & 7;                                        \
        si_ |= (BIG) ? lut32[h_ | ((y_ >> 16) & 0xf8) | ((x_ >> 11) & 0x1f00)] \
                     : lut16[h_ | ((y_ >> 16) & 0x78) | ((x_ >> 12) & 0x780)]; \
        uint32 b_ = wram[MEM_BE2(si_ >> 1)];                                \
        OUT = (si_ & 1) ? (b_ & 15) : (b_ >> 4);                            \
      }                                                                     \
    }                                                                       \
  } while (0)

/* PRIO: 0 normal, 1 underwrite (keep a non-zero old pixel), 2 overwrite
   (keep the old pixel where the new one is 0) */
#define GFX_COMBINE(PRIO, NEW, OLD) \
  ((PRIO) == 0 ? (NEW) : (PRIO) == 1 ? ((OLD) ? (OLD) : (NEW)) : ((NEW) ? (NEW) : (OLD)))

#define GFX_LOOPS(PRIO, SRC) do {                                       \
  if ((bufferIndex & 1) && width) {         /* a first right pixel alone */ \
    uint32 p;                                                               \
    SRC(p);                                                                 \
    uint8 *d = &wram[MEM_BE2(bufferIndex >> 1)];                            \
    *d = (uint8)((*d & 0xf0) | GFX_COMBINE(PRIO, p, *d & 15));              \
    width--;                                                                \
    bufferIndex += 1;                                                       \
    if ((bufferIndex & 7) == 0) bufferIndex += bufferOffset - 1;            \
  }                                                                         \
  while (width >= 2) {                                                      \
    uint32 pl, pr;                                                          \
    SRC(pl);                                                                \
    SRC(pr);                                                                \
    uint8 *d = &wram[MEM_BE2(bufferIndex >> 1)];                            \
    if (PRIO) {                                                             \
      uint32 o = *d;                                                        \
      pl = GFX_COMBINE(PRIO, pl, o >> 4);                                   \
      pr = GFX_COMBINE(PRIO, pr, o & 15);                                   \
    }                                                                       \
    *d = (uint8)(pl << 4 | pr);                                             \
    width -= 2;                                                             \
    bufferIndex += 2;                                                       \
    if ((bufferIndex & 7) == 0) bufferIndex += bufferOffset - 1;            \
  }                                                                         \
  if (width) {                              /* a last left pixel alone */   \
    uint32 p;                                                               \
    SRC(p);                                                                 \
    uint8 *d = &wram[MEM_BE2(bufferIndex >> 1)];                            \
    *d = (uint8)((*d & 0x0f) | GFX_COMBINE(PRIO, p, *d >> 4) << 4);         \
  }                                                                         \
} while (0)

/* any position and step: a pixel, then the next position */
#define GFX_ANY(OUT) do { GFX_SRC(X, Y, OUT, BIG_, REP_); X += dx; Y += dy; } while (0)
#define GFX_LOOP(PRIO, BIG, REP) do {                                       \
  enum { BIG_ = BIG, REP_ = REP };                                          \
  GFX_LOOPS(PRIO, GFX_ANY);                                                 \
} while (0)

/* 1:1 along the line (step 1.0 in x, 0 in y; the BIOS logo): the stamp
   map entry is fetched once per 16 (32) pixels */
#define GFX_ROW(OUT) do {                                                   \
    OUT = 0;                                                                \
    if (!((X & outside) | yout)) {                                          \
      uint32 sc_ = X >> sshift;                                             \
      if (sc_ != cur_sc) {                                                  \
        uint32 sd_ = mapPtr[maprow | sc_];                                  \
        cur_sc = sc_;                                                       \
        cur_base = (sd_ & stampMask) << 8;                                  \
        cur_h = ((sd_ >> 13) & 7) | ylut;                                   \
      }                                                                     \
      if (cur_base) {                                                       \
        uint32 si_ = cur_base | (BIG_ ? lut32[cur_h | ((X >> 11) & 0x1f00)] \
                                      : lut16[cur_h | ((X >> 12) & 0x780)]); \
        uint32 b_ = wram[MEM_BE2(si_ >> 1)];                                \
        OUT = (si_ & 1) ? (b_ & 15) : (b_ >> 4);                            \
      }                                                                     \
    }                                                                       \
    X += dx;                                                                \
  } while (0)
/* the source pair from one byte: with no flip or rotation (hrr = 0) and an
   even pixel column, the next pixel is the low nibble of the same byte */
#define GFX_ROWPAIR(PL, PR) do {                                            \
    uint32 done_ = 0;                                                       \
    if (!((X & outside) | yout)) {                                          \
      uint32 sc_ = X >> sshift;                                             \
      if (sc_ != cur_sc) {                                                  \
        uint32 sd_ = mapPtr[maprow | sc_];                                  \
        cur_sc = sc_;                                                       \
        cur_base = (sd_ & stampMask) << 8;                                  \
        cur_h = ((sd_ >> 13) & 7) | ylut;                                   \
      }                                                                     \
      if (cur_base && !(cur_h & 7) && !(X & (1u << 19))) {                  \
        uint32 si_ = cur_base | (BIG_ ? lut32[cur_h | ((X >> 11) & 0x1f00)] \
                                      : lut16[cur_h | ((X >> 12) & 0x780)]); \
        uint32 b_ = wram[MEM_BE2(si_ >> 1)];                                \
        PL = b_ >> 4; PR = b_ & 15;                                         \
        X += 2 * dx;                                                        \
        done_ = 1;                                                          \
      }                                                                     \
    }                                                                       \
    if (!done_) { GFX_ROW(PL); GFX_ROW(PR); }                               \
  } while (0)
#define GFX_ROWLOOP(PRIO, BIG) do {                                         \
  enum { BIG_ = BIG };                                                      \
  const uint32 yout = Y & outside;                                          \
  const uint32 ylut = BIG ? ((Y >> 16) & 0xf8) : ((Y >> 16) & 0x78);        \
  const uint32 maprow = (Y >> sshift) << mapShift;                          \
  uint32 cur_sc = ~0u, cur_base = 0, cur_h = 0;                             \
  if ((bufferIndex & 1) && width) {         /* a first right pixel alone */ \
    uint32 p;                                                               \
    GFX_ROW(p);                                                             \
    uint8 *d = &wram[MEM_BE2(bufferIndex >> 1)];                            \
    *d = (uint8)((*d & 0xf0) | GFX_COMBINE(PRIO, p, *d & 15));              \
    width--;                                                                \
    bufferIndex += 1;                                                       \
    if ((bufferIndex & 7) == 0) bufferIndex += bufferOffset - 1;            \
  }                                                                         \
  while (width >= 2) {                                                      \
    uint32 pl, pr;                                                          \
    GFX_ROWPAIR(pl, pr);                                                    \
    uint8 *d = &wram[MEM_BE2(bufferIndex >> 1)];                            \
    if (PRIO) {                                                             \
      uint32 o = *d;                                                        \
      pl = GFX_COMBINE(PRIO, pl, o >> 4);                                   \
      pr = GFX_COMBINE(PRIO, pr, o & 15);                                   \
    }                                                                       \
    *d = (uint8)(pl << 4 | pr);                                             \
    width -= 2;                                                             \
    bufferIndex += 2;                                                       \
    if ((bufferIndex & 7) == 0) bufferIndex += bufferOffset - 1;            \
  }                                                                         \
  if (width) {                              /* a last left pixel alone */   \
    uint32 p;                                                               \
    GFX_ROW(p);                                                             \
    uint8 *d = &wram[MEM_BE2(bufferIndex >> 1)];                            \
    *d = (uint8)((*d & 0x0f) | GFX_COMBINE(PRIO, p, *d >> 4) << 4);         \
  }                                                                         \
} while (0)

static void gfx_render(uint32 bufferIndex, uint32 width)
{
#ifdef GFX_REF
  gfx_render_ref(bufferIndex, width);
  return;
#else
  const uint32 dmask = gfx.dotMask << 8, outside = ~(gfx.dotMask << 8) & 0xffffff00;
  const uint32 stampMask = gfx.stampMask;
  const uint16 *mapPtr = gfx.mapPtr;
  const int sshift = gfx.stampShift + 8, mapShift = gfx.mapShift;
  uint8 *wram = Pico_mcd->word_ram2M;
  const uint32 bufferOffset = gfx.bufferOffset;
  const uint16 *lut16 = gfx.lut_off16, *lut32 = gfx.lut_off32;

  /* 13.3 start position -> 13.11, then 8 bits up; 5.11 steps likewise */
  uint32 X = (uint32)*gfx.tracePtr++ << 16;
  uint32 Y = (uint32)*gfx.tracePtr++ << 16;
  uint32 dx = (uint32)(int32)(int16)*gfx.tracePtr++ << 8;
  uint32 dy = (uint32)(int32)(int16)*gfx.tracePtr++ << 8;

  uint32 priority = (Pico_mcd->s68k_regs[2] << 8) | Pico_mcd->s68k_regs[3];
  priority = (priority >> 3) & 0x03;
  int big = (Pico_mcd->s68k_regs[0x58+1] & 0x02) != 0;
  int rep = (Pico_mcd->s68k_regs[0x58+1] & 0x01) != 0;

  if (dy == 0 && dx == (1u << 19) && !rep) {
    switch (priority * 2 + big) {
    case 0: GFX_ROWLOOP(0, 0); break;
    case 1: GFX_ROWLOOP(0, 1); break;
    case 2: GFX_ROWLOOP(1, 0); break;
    case 3: GFX_ROWLOOP(1, 1); break;
    case 4: GFX_ROWLOOP(2, 0); break;
    case 5: GFX_ROWLOOP(2, 1); break;
    default: break;
    }
    return;
  }
  switch (priority * 4 + big * 2 + rep) {
  case 0:  GFX_LOOP(0, 0, 0); break;
  case 1:  GFX_LOOP(0, 0, 1); break;
  case 2:  GFX_LOOP(0, 1, 0); break;
  case 3:  GFX_LOOP(0, 1, 1); break;
  case 4:  GFX_LOOP(1, 0, 0); break;
  case 5:  GFX_LOOP(1, 0, 1); break;
  case 6:  GFX_LOOP(1, 1, 0); break;
  case 7:  GFX_LOOP(1, 1, 1); break;
  case 8:  GFX_LOOP(2, 0, 0); break;
  case 9:  GFX_LOOP(2, 0, 1); break;
  case 10: GFX_LOOP(2, 1, 0); break;
  case 11: GFX_LOOP(2, 1, 1); break;
  default: break;                           /* mode 3 ("invalid"): the old pixels stay */
  }
#endif
}

void gfx_start(uint32 base)
{
  /* make sure 2M mode is enabled */
  if (!(Pico_mcd->s68k_regs[3] & 0x04))
  {
    uint32 mask = 0;
    uint32 reg;
    
    /* trace vector pointer */
    gfx.tracePtr = (uint16 *)(Pico_mcd->word_ram2M + ((base << 2) & 0x3fff8));

    /* stamps & stamp map size */
    switch ((Pico_mcd->s68k_regs[0x58+1] >> 1) & 0x03)
    {
      case 0:
        gfx.dotMask = 0x07ffff;   /* 256x256 dots/map  */
        gfx.stampMask = 0x7ff;    /* 16x16 dots/stamp  */
        gfx.stampShift = 11 + 4;  /* 16x16 dots/stamp  */
        gfx.mapShift = 4;         /* 16x16 stamps/map  */
        mask = 0x3fe00;           /* 512 bytes/table   */
        break;

      case 1:
        gfx.dotMask = 0x07ffff;   /* 256x256 dots/map  */
        gfx.stampMask = 0x7fc;    /* 16x16 dots/stamp  */
        gfx.stampShift = 11 + 5;  /* 32x32 dots/stamp  */
        gfx.mapShift = 3;         /* 8x8 stamps/map    */
        mask = 0x3ff80;           /* 128 bytes/table   */
        break;

      case 2:
        gfx.dotMask = 0x7fffff;   /* 4096*4096 dots/map */
        gfx.stampMask = 0x7ff;    /* 16x16 dots/stamp   */
        gfx.stampShift = 11 + 4;  /* 16x16 dots/stamp   */
        gfx.mapShift = 8;         /* 256x256 stamps/map */
        mask = 0x20000;           /* 131072 bytes/table */
        break;

      case 3:
        gfx.dotMask = 0x7fffff;   /* 4096*4096 dots/map */
        gfx.stampMask = 0x7fc;    /* 16x16 dots/stamp   */
        gfx.stampShift = 11 + 5;  /* 32x32 dots/stamp   */
        gfx.mapShift = 7;         /* 128x128 stamps/map */
        mask = 0x38000;           /* 32768 bytes/table  */
        break;
    }

    /* stamp map table base address */
    reg = (Pico_mcd->s68k_regs[0x5a] << 8) | Pico_mcd->s68k_regs[0x5b];
    gfx.mapPtr = (uint16 *)(Pico_mcd->word_ram2M + ((reg << 2) & mask));

    /* image buffer column offset (64 pixels/cell, minus 7 pixels to restart at cell beginning) */
    gfx.bufferOffset = (((Pico_mcd->s68k_regs[0x5c+1] & 0x1f) + 1) << 6) - 7;

    /* image buffer start index in dot units (2 pixels/byte) */
    reg = (Pico_mcd->s68k_regs[0x5e] << 8) | Pico_mcd->s68k_regs[0x5f];
    gfx.bufferStart = (reg << 3) & 0x7ffc0;

    /* add image buffer horizontal dot offset */
    gfx.bufferStart += (Pico_mcd->s68k_regs[0x60+1] & 0x3f);

    /* reset GFX chip cycle counter */
    //gfx.cycles = cycles;

    /* update GFX chip timings (see AC3:Thunderhawk / Thunderstrike) */
    //gfx.cyclesPerLine = 4 * 5 * scd.regs[0x62>>1].w;

    /* start graphics operation */
    Pico_mcd->s68k_regs[0x58] = 0x80;
    Pico_mcd->m.state_flags &= ~PCD_ST_S68K_POLL;
    Pico_mcd->m.s68k_poll_cnt = 0;

    gfx_schedule();
  }
}

/* PicoDrive specific */
#define UPDATE_CYCLES 20000

static void gfx_schedule(void)
{
  int w, h, cycles;
  int y_step;

  w = (Pico_mcd->s68k_regs[0x62] << 8) | Pico_mcd->s68k_regs[0x63];
  h = (Pico_mcd->s68k_regs[0x64] << 8) | Pico_mcd->s68k_regs[0x65];

  cycles = 5 * w * h;
  y_step = h;
  if (cycles > UPDATE_CYCLES)
    y_step = (UPDATE_CYCLES + 5 * w - 1) / (5 * w);

  gfx.y_step = y_step;
  pcd_event_schedule_s68k(PCD_EVENT_GFX, 5 * w * y_step);
}

void gfx_update(unsigned int cycles)
{
  int lines, lines_reg;
  int w;

  if (!(Pico_mcd->s68k_regs[0x58] & 0x80))
    return;

  w = (Pico_mcd->s68k_regs[0x62] << 8) | Pico_mcd->s68k_regs[0x63];
  lines = (Pico_mcd->s68k_regs[0x64] << 8) | Pico_mcd->s68k_regs[0x65];
  lines_reg = lines - gfx.y_step;

  if (lines_reg <= 0) {
    Pico_mcd->s68k_regs[0x58] = 0;
    Pico_mcd->s68k_regs[0x64] =
    Pico_mcd->s68k_regs[0x65] = 0;

    Pico_mcd->m.state_flags &= ~PCD_ST_S68K_POLL;
    Pico_mcd->m.s68k_poll_cnt = 0;
    if (Pico_mcd->s68k_regs[0x33] & PCDS_IEN1) {
      elprintf(EL_INTS|EL_CD, "s68k: gfx_cd irq 1");
      pcd_irq_s68k(1, 1);
    }
  }
  else {
    Pico_mcd->s68k_regs[0x64] = lines_reg >> 8;
    Pico_mcd->s68k_regs[0x65] = lines_reg;

    if (lines > gfx.y_step)
      lines = gfx.y_step;

    pcd_event_schedule(cycles, PCD_EVENT_GFX, 5 * w * lines);
  }

  if (PicoIn.opt & POPT_EN_MCD_GFX)
  {
    /* render lines */
    while (lines--)
    {
      /* process dots to image buffer */
      gfx_render(gfx.bufferStart, w);

      /* increment image buffer start index for next line (8 pixels/line) */
      gfx.bufferStart += 8;
    }
  }
}

// vim:shiftwidth=2:ts=2:expandtab

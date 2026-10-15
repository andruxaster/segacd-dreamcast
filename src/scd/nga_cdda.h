/* block source for nga_cdda.c, provided by the platform */
#ifndef NGA_CDDA_H
#define NGA_CDDA_H
#include <stdint.h>
/* start streaming NGA blocks of the track file f (as opened by PicoDrive)
   from block 'block'; called from the emulation */
void nga_src_open(void *f, uint32_t block);
void nga_src_close(void);
/* next block (NGA_BLOCK bytes): 1, or 0 when none is ready / end of track */
int  nga_src_next(uint8_t *blk);
/* set by the platform to get the CD audio at 44.1 kHz instead of mixed:
   4 int16 (two L/R pairs) per output sample, nga_cdda_out_len samples
   written by the frame's ogg_update (0 if it was not called) */
extern int16_t *nga_cdda_out;
extern int nga_cdda_out_len;
#endif

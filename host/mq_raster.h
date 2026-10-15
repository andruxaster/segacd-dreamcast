#ifndef MQ_RASTER_H
#define MQ_RASTER_H
#include <stdint.h>
void mq_raster_tiles(void);                                        /* staged tiles -> atlas model */
void mq_raster(uint16_t *out320x240, const uint16_t *fb, int fb_pitch, int fb_y0);
int  mq_compare(const uint16_t *hw, const uint16_t *fb, int fb_pitch, int fb_y0, int *fx, int *fy);
#endif

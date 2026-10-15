#ifndef MQ_PVR_H
#define MQ_PVR_H
#include <stdint.h>
int  mq_pvr_init(void);           /* after pvr_init; 0 ok */
/* one scene from mq; fb = PicoDrive's line buffer (software strips), line y at fb + (y0 + y) * pitch */
void mq_pvr_show(const uint16_t *fb, int pitch, int y0);
extern uint32_t mq_pvr_headers, mq_pvr_quads, mq_pvr_tiles;
#ifdef MQ_PVR_SIM
#define MQ_SIM_TA_WORDS (8 * 40000)
extern uint8_t  mq_sim_vram[];
extern uint32_t mq_sim_pal[1024], mq_sim_ta[], mq_sim_words, mq_sim_keep;
#endif
#endif

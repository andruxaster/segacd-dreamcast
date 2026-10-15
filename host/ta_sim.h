#ifndef TA_SIM_H
#define TA_SIM_H
#include <stdint.h>
void ta_sim_render(uint16_t *out640x480);
int  ta_sim_compare(const uint16_t *out640, const uint16_t *pic320, int w, int h, int *fx, int *fy);
#endif

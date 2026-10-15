/* Audio from the board core's ring (22050 Hz stereo) to the AICA stream
   (44100 Hz): linear interpolation with a step that follows the backlog
   (the boards run at 59.185 Hz, the Dreamcast shows 59.94 Hz). */
#ifndef RESAMPLE_H
#define RESAMPLE_H
#include <stdint.h>

typedef struct {
    int (*take)(int16_t *dst, int max_frames);   /* interleaved L R */
    int (*avail)(void);
    uint32_t step;                               /* 65536 x in rate / out rate x 59.94 / board fps */
} rs_source_t;

void rs_audio(const rs_source_t *s, int16_t *out, int n);   /* n int16 values (stereo) */
extern uint32_t rs_underruns;
extern volatile int rs_out_peak;                /* largest |sample| sent */
#endif

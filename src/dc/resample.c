/* see resample.h */
#include "resample.h"

uint32_t rs_underruns;
volatile int rs_out_peak;
static int16_t in_buf[1024 * 2];
static int in_have, in_pos;
static int prev_l, prev_r, cur_l, cur_r;
static uint32_t frac;                          /* 16.16 between prev and cur */
static int32_t err_f;                          /* 16 x smoothed backlog error */

static inline void next_in(const rs_source_t *s) {
    prev_l = cur_l; prev_r = cur_r;
    if (in_pos >= in_have) {
        in_have = s->take(in_buf, 1024);
        in_pos = 0;
        if (!in_have) { rs_underruns++; return; }   /* underrun: hold the last sample */
    }
    cur_l = in_buf[2 * in_pos];
    cur_r = in_buf[2 * in_pos + 1];
    in_pos++;
}

void rs_audio(const rs_source_t *s, int16_t *out, int n) {
    int frames = n / 2;
    /* aim at what this callback uses (frames / 2 input samples) + 512
       (23 ms) queued; the board adds ~370 a frame */
    int backlog = s->avail() + (in_have - in_pos);
    int target = frames / 2 + 512;
    int err = backlog - target;
    if (err > 4096) {                          /* far behind: resync */
        while (backlog > target) {
            int k = s->take(in_buf, backlog - target > 1024 ? 1024 : backlog - target);
            if (!k) break;
            backlog -= k;
        }
        in_have = in_pos = 0;
        err = 0;
        err_f = 0;
    }
    /* proportional: adj = err / 2 (err_f is 16 x err, smoothed over ~32
       callbacks), within +-1.3 %. The machine's rate (from the SH4 timer)
       and the AICA's differ by ~0.25 % on a console: with the earlier
       +-0.2 % range the queue grew until the resync above (a click every
       few minutes). Here 0.25 % settles at ~160 samples above target. */
    err_f += (err * 16 - err_f) / 32;
    int32_t adj = err_f / 32;
    if (adj > 440) adj = 440;
    if (adj < -440) adj = -440;
    uint32_t step = (uint32_t)((int32_t)s->step + adj);
    for (int i = 0; i < frames; i++) {
        while (frac >= 65536) { next_in(s); frac -= 65536; }
        int f = (int)(frac >> 1);
        out[2 * i]     = (int16_t)(prev_l + (((cur_l - prev_l) * f) >> 15));
        out[2 * i + 1] = (int16_t)(prev_r + (((cur_r - prev_r) * f) >> 15));
        int a = out[2 * i] < 0 ? -out[2 * i] : out[2 * i];
        if (a > rs_out_peak) rs_out_peak = a;
        frac += step;
    }
}

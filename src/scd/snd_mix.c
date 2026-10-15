/* Sound output of the Mega CD machine, platform independent (used by the
   Dreamcast machine and the host test):

   PicoDrive renders FM, PSG and PCM at SND_MIX_EMU_RATE (22050 Hz: the
   YM2612 is the most expensive part of the machine and costs half at that
   rate). The CD audio is not resampled into it: nga_cdda.c hands it over
   at 44.1 kHz, two L/R pairs per 22 kHz sample. Both go into one ring
   entry per 22 kHz sample, so they stay in step.

   The output (44100 Hz stereo) interpolates PicoDrive's sound and takes
   the CD pairs as they are. The machine produces 60 frames of sound per
   second but is driven at 59.94 Hz on the Dreamcast: the read step
   follows the ring's backlog, smoothed and kept within +-0.2 % so the
   pitch does not wobble. */
#include <string.h>
#include "pico_int.h"
#include "nga_cdda.h"
#include "snd_mix.h"

/* one entry per 22 kHz sample: PicoDrive's L/R and two CD L/R pairs */
typedef struct { int16_t l, r, c0l, c0r, c1l, c1r; } snd_t;
#define SND_RING 8192                         /* entries, power of 2 (0.37 s) */
static int16_t snd_frame[2 * 1024];           /* PicoDrive's buffer for one frame */
static int16_t cd_frame[4 * 1024];            /* CD audio of the frame */
static snd_t snd_ring[SND_RING];
static uint32_t snd_head, snd_tail;           /* entries written / read */
uint32_t snd_mix_underruns, snd_mix_drops;
static int priming = 1;                       /* silence until the cushion is there */
uint32_t snd_mix_fixed;                       /* tests: fixed 16.16 step, no backlog control */

/* PicoIn.writeSound: once per frame, after the frame's CD audio */
static void write_sound(int len) {            /* len in bytes */
    int n = len / 4, ncd = nga_cdda_out_len;
    nga_cdda_out_len = 0;
    for (int i = 0; i < n; i++) {
        if (snd_head - snd_tail >= SND_RING) { snd_mix_drops++; break; }
        snd_t *e = &snd_ring[snd_head++ & (SND_RING - 1)];
        e->l = snd_frame[2 * i];
        e->r = snd_frame[2 * i + 1];
        if (i < ncd) {
            const int16_t *c = &cd_frame[4 * i];
            e->c0l = c[0]; e->c0r = c[1]; e->c1l = c[2]; e->c1r = c[3];
        } else {
            e->c0l = e->c0r = e->c1l = e->c1r = 0;
        }
    }
}

void snd_mix_init(void) {
    PicoIn.sndOut = snd_frame;
    PicoIn.writeSound = write_sound;
    nga_cdda_out = cd_frame;
    nga_cdda_out_len = 0;
    snd_head = snd_tail = 0;
    priming = 1;
}

static snd_t prev_e, cur_e;
static uint32_t snd_frac;                     /* 16.16 between prev and cur */
static int32_t err_f;                         /* 16 x smoothed backlog error */
/* 65536 * 22050 * 59.94/60 / 44100 */
#define SND_STEP_NOMINAL 32735u

static inline void snd_next(void) {
    prev_e = cur_e;
    if (snd_head == snd_tail) {               /* underrun: hold the sound, CD silent */
        snd_mix_underruns++;
        priming = 1;                          /* and build the cushion again */
        cur_e.c0l = cur_e.c0r = cur_e.c1l = cur_e.c1r = 0;
        return;
    }
    cur_e = snd_ring[snd_tail++ & (SND_RING - 1)];
}

static inline int16_t clip(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

void snd_mix_out(int16_t *out, int frames) {
    int backlog = (int)(snd_head - snd_tail);  /* in 22 kHz entries */
    int target = frames / 4 + 1536;           /* ~70 ms of cushion: uneven frames, VMU writes */
    int err = backlog - target;
    if (err > 4096) {                         /* far behind (loading stall): resync */
        snd_tail = snd_head - (uint32_t)target;
        err = 0;
        err_f = 0;
    }
    if (priming) {                            /* start (or after an underrun): wait for the cushion */
        if (backlog < target) {
            memset(out, 0, (size_t)frames * 4);
            return;
        }
        priming = 0;
        err = 0;
        err_f = 0;
    }
    err_f += (err * 16 - err_f) / 32;
    int32_t adj = err_f / 256;
    if (adj > 65) adj = 65;
    if (adj < -65) adj = -65;
    uint32_t step = snd_mix_fixed ? snd_mix_fixed : (uint32_t)((int32_t)SND_STEP_NOMINAL + adj);

    for (int i = 0; i < frames; i++) {
        while (snd_frac >= 65536) { snd_next(); snd_frac -= 65536; }
        int f = (int)(snd_frac >> 1);         /* 0..32767 */
        int l = prev_e.l + (((cur_e.l - prev_e.l) * f) >> 15);
        int r = prev_e.r + (((cur_e.r - prev_e.r) * f) >> 15);
        /* CD: first or second pair of the entry being passed */
        if (f < 16384) { l += prev_e.c0l; r += prev_e.c0r; }
        else           { l += prev_e.c1l; r += prev_e.c1r; }
        out[2 * i]     = clip(l);
        out[2 * i + 1] = clip(r);
        snd_frac += step;
    }
}

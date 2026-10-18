/* sound output of the Mega CD machine (snd_mix.c) */
#ifndef SND_MIX_H
#define SND_MIX_H
#include <stdint.h>
#define SND_MIX_EMU_RATE 22050     /* PicoIn.sndRate to use */
#define SND_MIX_OUT_RATE 44100
/* after PicoLoopPrepare: sets PicoIn.sndOut/writeSound and the CD output
   (then PsndRerate(0)) */
void snd_mix_init(void);
/* 'frames' stereo samples at 44100 Hz */
void snd_mix_out(int16_t *out, int frames);
extern uint32_t snd_mix_underruns, snd_mix_drops;
extern uint32_t snd_mix_fixed; /* tests: fixed 16.16 step */
#endif

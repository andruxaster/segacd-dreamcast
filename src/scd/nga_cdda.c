/* CD audio for PicoDrive from NGA tracks (IMA ADPCM, tools/ngcdconv).
   PicoDrive plays compressed tracks through its ogg_* hooks; cd_parse.c
   maps the .nga extension onto them. The blocks (one per CD sector) come
   from the platform: nga_src_* (a reader thread on the Dreamcast, plain
   reads in the host test). Mixed at 5/8 like PicoDrive's raw CD audio:
   - into PicoDrive's buffer, resampled to PicoIn.sndRate (default), or
   - with nga_cdda_out set (PicoIn.sndRate 22050): kept at 44.1 kHz in
     nga_cdda_out, two pairs per output sample of the frame, for the
     platform to mix after PicoDrive's 22 kHz sound. */
#include <stdio.h>
#include <string.h>
#include "pico_int.h"
#include "nga.h"
#include "nga_cdda.h"

static int playing;
static int16_t pcm[NGA_SAMPLES * 2];
static int pos = NGA_SAMPLES;             /* next sample in pcm */
static int skip;                          /* samples to drop after a seek */
static uint32_t frac;                     /* 16.16 position for resampling */
static int cur_l, cur_r;
int16_t *nga_cdda_out;                    /* NULL: mix into PicoDrive's buffer */
int nga_cdda_out_len;                     /* output samples covered this frame */

/* track length in ms (PicoDrive turns it into sectors: ms * 75 / 1000) */
int ogg_get_length(void *f) {
    uint8_t h[NGA_HEADER];
    FILE *fp = f;
    if (fseek(fp, 0, SEEK_SET) != 0 || fread(h, 1, NGA_HEADER, fp) != NGA_HEADER
        || memcmp(h, "NGA1", 4))
        return -1;
    uint32_t sectors = h[12] | (h[13] << 8) | (h[14] << 16) | ((uint32_t)h[15] << 24);
    return (int)((sectors * 1000u + 74) / 75);   /* rounded up: exact on the way back */
}

void ogg_start_play(void *f, int sample_offset) {
    if (sample_offset < 0) sample_offset = 0;
    nga_src_open(f, (uint32_t)sample_offset / NGA_SAMPLES);
    skip = sample_offset % NGA_SAMPLES;
    pos = NGA_SAMPLES;
    frac = 0;
    cur_l = cur_r = 0;
    playing = 1;
}

void ogg_stop_play(void) {
    if (playing) nga_src_close();
    playing = 0;
}

/* next 44.1 kHz sample pair; silence when the source has nothing */
static inline void next(void) {
    for (;;) {
        if (pos >= NGA_SAMPLES) {
            uint8_t blk[NGA_BLOCK];
            if (!nga_src_next(blk)) { cur_l = cur_r = 0; return; }
            nga_decode_block(blk, pcm);
            pos = 0;
        }
        if (skip) {
            int n = NGA_SAMPLES - pos;
            if (n > skip) n = skip;
            pos += n; skip -= n;
            continue;
        }
        cur_l = pcm[2 * pos];
        cur_r = pcm[2 * pos + 1];
        pos++;
        return;
    }
}

void ogg_update(s32 *buffer, int length, int stereo) {
    if (!playing) return;
    /* as PicoDrive's raw tracks: playback may start within the frame */
    int offs = Pico_mcd->cdda_frame_offs * Pico.snd.cdda_div >> 16;
    Pico_mcd->cdda_frame_offs = 0;
    if (offs > length) offs = length;
    length -= offs;
    buffer += offs * (stereo ? 2 : 1);
    if (nga_cdda_out) {                   /* 2 CD pairs per output sample */
        int16_t *o = nga_cdda_out;
        memset(o, 0, offs * 4 * sizeof(int16_t));
        o += offs * 4;
        for (int i = 0; i < 2 * length; i++) {
            next();
            if (++Pico_mcd->m.cdda_lba_offset >= NGA_SAMPLES)
                Pico_mcd->m.cdda_lba_offset = 0;
            /* 5/8 as above, times the 7/8 PicoDrive's final mix
               (mix_32_to_16) applies to what goes through it */
            *o++ = (int16_t)((cur_l * 35) >> 6);
            *o++ = (int16_t)((cur_r * 35) >> 6);
        }
        nga_cdda_out_len = offs + length;
        return;
    }
    uint32_t step = (uint32_t)((44100ull << 16) / (unsigned)PicoIn.sndRate);
    for (int i = 0; i < length; i++) {
        frac += step;
        while (frac >= 0x10000) {
            frac -= 0x10000;
            next();
            /* position within the sector, kept for savestates/restarts */
            if (++Pico_mcd->m.cdda_lba_offset >= NGA_SAMPLES)
                Pico_mcd->m.cdda_lba_offset = 0;
        }
        int l = (cur_l * 5) >> 3, r = (cur_r * 5) >> 3;
        if (stereo) { buffer[2 * i] += l; buffer[2 * i + 1] += r; }
        else buffer[i] += (l + r) >> 1;
    }
}

/* mp3 tracks are not used */
int  mp3_get_bitrate(void *f, int size) { (void)f; (void)size; return -1; }
void mp3_start_play(void *f, int pos1024) { (void)f; (void)pos1024; }
void mp3_update(s32 *buffer, int length, int stereo) { (void)buffer; (void)length; (void)stereo; }

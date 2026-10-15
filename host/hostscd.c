/* Headless host front end for the PicoDrive Mega CD core (Snatcher).
   usage: scdh bios.bin game.cue
   env: SC_FRAMES (3600), SC_PPM (dir) + SC_EVERY (60), SC_WAV (file),
        SC_SCRIPT (lines "frame keys", keys: udlr ABC S, held until the next line),
        SC_RANDOM=seed (random player: a button every few frames, see rnd_pad),
        SC_BOOT=1 (fast boot as on the Dreamcast, mcd_boot.c),
        SC_NOIDLE=1 (sub CPU idle parking off, mcd_idle.c),
        SC_NOPOLL=1 (PicoDrive's own poll/idle detection off too),
        SC_SAVE=frame (state to SC_STATE, default state.gz), SC_LOAD=file,
        SC_HANG=frames (stop when the picture has not changed for that long
                        while the random player presses buttons; default 7200),
        SC_TRACE=n (every n frames: main/sub PC, picture hash, CD state),
        SC_IOLOG=file (every data sector read: frame, lba),
        SC_HW / SC_VERIFY (tile rectangles, as md-dc's MD_HW / MD_VERIFY) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include "pico/pico_types.h"
#include "pico/pico_port.h"
#include "pico/pico.h"
#include "pico/pico_int.h"
#include "mcd_boot.h"
#ifdef MD_MQ
#include "../src/mq/mq.h"
#include "mq_raster.h"
#include "../src/dc/mq_pvr.h"
#endif
#ifdef MD_MARK
#include <unistd.h>
#include <sys/syscall.h>
#define MARK(k) syscall(20, 0x4E474344, (k))
#else
#define MARK(k)
#endif

/* --- platform hooks the core needs --- */
void cache_flush_d_inval_i(void *start, void *end) { (void)start; (void)end; }
void *plat_mmap(unsigned long addr, size_t size, int need_exec, int is_fixed) {
    (void)addr; (void)need_exec; (void)is_fixed; return calloc(1, size);
}
void *plat_mremap(void *ptr, size_t oldsize, size_t newsize) {
    void *p = realloc(ptr, newsize);
    if (p && newsize > oldsize) memset((char *)p + oldsize, 0, newsize - oldsize);
    return p;
}
void plat_munmap(void *ptr, size_t size) { (void)size; free(ptr); }
void *plat_mem_get_for_drc(size_t size) { (void)size; return NULL; }
int plat_mem_set_exec(void *ptr, size_t size) { (void)ptr; (void)size; return 0; }
static int vid_w = 320, vid_h = 224, vid_y0 = 8, vid_x0 = 0;
void emu_video_mode_change(int start_line, int line_count, int start_col, int col_count) {
    vid_y0 = start_line; vid_h = line_count; vid_x0 = start_col; vid_w = col_count;
}
void emu_32x_startup(void) {}
void lprintf(const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    if (getenv("SC_LOG")) vfprintf(stdout, fmt, a);
    va_end(a);
}

static const char *bios_path;
static const char *get_bios(int *region, const char *cd_fname) { (void)region; (void)cd_fname; return bios_path; }

/* NGA CD audio blocks (nga_cdda.c), straight from the file */
#include "nga.h"
#include "nga_cdda.h"
static FILE *src_f;
static uint32_t src_block, src_blocks;
void nga_src_open(void *f, uint32_t block) {
    uint8_t h[NGA_HEADER];
    src_f = f;
    fseek(src_f, 0, SEEK_SET);
    if (fread(h, 1, NGA_HEADER, src_f) != NGA_HEADER) { src_f = NULL; return; }
    src_blocks = h[12] | (h[13] << 8) | (h[14] << 16) | ((uint32_t)h[15] << 24);
    src_block = block;
    fseek(src_f, NGA_HEADER + (long)block * NGA_BLOCK, SEEK_SET);
}
void nga_src_close(void) { src_f = NULL; }
int nga_src_next(uint8_t *blk) {
    if (!src_f || src_block >= src_blocks) return 0;
    if (fread(blk, 1, NGA_BLOCK, src_f) != NGA_BLOCK) return 0;
    src_block++;
    return 1;
}

/* data track reads (CD_DATA_HOOK in cdd.c): logged, then read by PicoDrive */
static int frame;
static FILE *iolog;
uint32_t io_reads, io_seeks;
static uint32_t io_last = ~0u;
#include "cd_cache.h"
static int use_cache;
static FILE *iso;
uint32_t io_bad;
/* the drive held while "the platform" reads (cdd.c): SC_SLOWIO=n holds it
   n CDD ticks (1/75 s) after every seek, SC_SLOWRND=p also p% of the other
   sectors (a drive that keeps falling behind); SC_CACHE=1 reads through
   cd_cache.c (as the Dreamcast does) and checks every sector against the file */
static int slow_io, slow_rnd, slow_left;
uint32_t io_stalls, io_hints;
static uint32_t srnd = 12345;
void cd_data_hook_seek(int lba) {
    io_hints++;
    slow_left = slow_io;
    if (use_cache) cd_cache_seek(lba);
}
int cd_data_hook_ready(int lba) {
    if (slow_left > 0) { slow_left--; io_stalls++; return 0; }
    if (slow_rnd) {
        srnd = srnd * 1103515245u + 12345u;
        if ((srnd >> 16) % 100 < (unsigned)slow_rnd) { io_stalls++; return 0; }
    }
    if (use_cache && !cd_cache_ready(lba)) { io_stalls++; return 0; }
    return 1;
}
int cd_data_hook_read(unsigned int pos, uint8_t *dst) {
    uint32_t lba = pos / 2048;
    io_reads++;
    if (lba != io_last + 1) io_seeks++;
    io_last = lba;
    if (iolog) fprintf(iolog, "%d %u\n", frame, lba);
    if (!use_cache) return 0;
    int got = cd_cache_read(pos, dst);
    uint8_t ref[2048];
    if (!got || fseek(iso, pos, SEEK_SET) || fread(ref, 1, 2048, iso) != 2048 || memcmp(ref, dst, 2048)) io_bad++;
    return got;
}

static int nframes = 3600, every = 60;
static const char *ppmdir;
static FILE *wav; static long wavbytes; static int rate = 44100;
static struct { int f; uint16_t b; } scr[65536]; static int nscr, sidx;
static uint16_t fb[328 * 256];
static int16_t sndbuf[4096];
#ifdef MD_MQ
static uint16_t hwpic[320 * 240];
#endif
static int use_hw;

static uint16_t keys(const char *s) {
    uint16_t b = 0;
    for (; *s; s++) switch (*s) {
    case 'u': b |= 1; break; case 'd': b |= 2; break; case 'l': b |= 4; break; case 'r': b |= 8; break;
    case 'B': b |= 0x10; break; case 'C': b |= 0x20; break; case 'A': b |= 0x40; break; case 'S': b |= 0x80; break; }
    return b;
}

/* random player: every 12..40 frames one button for 6 frames; mostly C
   (choose), the cursor keys, sometimes B (back), A (blaster) and Start */
static uint32_t rnd_state;
static uint32_t rnd(void) { rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 17; rnd_state ^= rnd_state << 5; return rnd_state; }
static int rnd_next, rnd_until, rnd_mode = 1; static uint16_t rnd_b;
static uint16_t rnd_pad(void) {
    if (frame >= rnd_next) {
        uint32_t r = rnd() % 100;
        if (rnd_mode == 2)                    /* more cursor moves, rarely back */
            rnd_b = r < 34 ? 0x20 : r < 62 ? 2 : r < 76 ? 1 : r < 82 ? 4 : r < 88 ? 8 : r < 92 ? 0x10 : r < 98 ? 0x40 : 0x80;
        else
            rnd_b = r < 45 ? 0x20 : r < 60 ? 2 : r < 70 ? 1 : r < 76 ? 4 : r < 82 ? 8 : r < 90 ? 0x10 : r < 97 ? 0x40 : 0x80;
        rnd_until = frame + 6;
        rnd_next = frame + 12 + rnd() % 29;
    }
    return frame < rnd_until ? rnd_b : 0;
}

static void snd_write(int len) {
    if (wav) { fwrite(PicoIn.sndOut, 1, len, wav); wavbytes += len; }
}
static void wav_hdr(void) {
    uint8_t h[44]; uint32_t v; uint16_t w;
    memcpy(h, "RIFF", 4); v = 36 + wavbytes; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4); w = 1; memcpy(h + 20, &w, 2); w = 2; memcpy(h + 22, &w, 2);
    v = rate; memcpy(h + 24, &v, 4); v = rate * 4; memcpy(h + 28, &v, 4); w = 4; memcpy(h + 32, &w, 2); w = 16; memcpy(h + 34, &w, 2);
    memcpy(h + 36, "data", 4); v = wavbytes; memcpy(h + 40, &v, 4);
    fseek(wav, 0, SEEK_SET); fwrite(h, 1, 44, wav); fseek(wav, 0, SEEK_END);
}
static uint16_t pix(int x, int y) {
#ifdef MD_MQ
    if (use_hw) return hwpic[y * 320 + x];
#endif
    return fb[(y + vid_y0) * 328 + x + vid_x0];
}
static void dump_ppm(const char *dir, int n) {
    char p[512]; sprintf(p, "%s/f%06d.ppm", dir, n);
    FILE *f = fopen(p, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", vid_w, vid_h);
    for (int y = 0; y < vid_h; y++) for (int x = 0; x < vid_w; x++) {
        uint16_t c = pix(x, y);
        uint8_t rgb[3] = { (uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
static uint32_t pic_hash(void) {
    uint32_t h = 2166136261u;
    for (int y = 0; y < vid_h; y++) for (int x = 0; x < vid_w; x++) h = (h ^ pix(x, y)) * 16777619u;
    return h;
}

extern int mcd_idle_on;
extern uint32_t mcd_idle_parked;

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s bios.bin game.cue\n", argv[0]); return 1; }
    bios_path = argv[1];
    if (getenv("SC_FRAMES")) nframes = atoi(getenv("SC_FRAMES"));
    if (getenv("SC_EVERY")) every = atoi(getenv("SC_EVERY"));
    if (getenv("SC_RATE")) rate = atoi(getenv("SC_RATE"));
    ppmdir = getenv("SC_PPM");
    if (getenv("SC_WAV")) { wav = fopen(getenv("SC_WAV"), "wb"); uint8_t z[44] = { 0 }; fwrite(z, 1, 44, wav); }
    if (getenv("SC_IOLOG")) iolog = fopen(getenv("SC_IOLOG"), "w");
    if (getenv("SC_SCRIPT")) {
        FILE *f = fopen(getenv("SC_SCRIPT"), "r"); char l[256];
        while (f && fgets(l, 256, f) && nscr < 65536) { int fr; char k[200] = ""; if (sscanf(l, "%d %199s", &fr, k) >= 1) { scr[nscr].f = fr; scr[nscr].b = keys(k); nscr++; } }
        if (f) fclose(f);
    }
    int random = getenv("SC_RANDOM") != NULL;
    if (getenv("SC_RMODE")) rnd_mode = atoi(getenv("SC_RMODE"));
    setvbuf(stdout, NULL, _IOLBF, 0);
    rnd_state = random ? (uint32_t)atoi(getenv("SC_RANDOM")) * 2654435761u + 1 : 1;
    int hang = getenv("SC_HANG") ? atoi(getenv("SC_HANG")) : 7200;
    int trace = getenv("SC_TRACE") ? atoi(getenv("SC_TRACE")) : 0;
    mcd_idle_on = !getenv("SC_NOIDLE");
    slow_io = getenv("SC_SLOWIO") ? atoi(getenv("SC_SLOWIO")) : 0;
    slow_rnd = getenv("SC_SLOWRND") ? atoi(getenv("SC_SLOWRND")) : 0;

    PicoIn.opt = POPT_EN_STEREO | POPT_EN_FM | POPT_EN_PSG | POPT_EN_Z80
               | POPT_EN_MCD_PCM | POPT_EN_MCD_CDDA | POPT_EN_MCD_GFX | POPT_DIS_32C_BORDER;
    if (getenv("SC_NOPOLL")) PicoIn.opt |= POPT_DIS_IDLE_DET;
    if (getenv("SC_NOSND")) PicoIn.opt &= ~(POPT_EN_FM | POPT_EN_PSG | POPT_EN_MCD_PCM | POPT_EN_MCD_CDDA);
    PicoIn.sndRate = rate;
    PicoIn.autoRgnOrder = 0x184;
    PicoInit();
    PicoSetInputDevice(0, PICO_INPUT_PAD_3BTN);
    PicoSetInputDevice(1, PICO_INPUT_NOTHING);
    enum media_type_e t = PicoLoadMedia(argv[2], NULL, 0, NULL, get_bios, NULL, NULL);
    if (t != PM_CD) { printf("load failed (%d)\n", (int)t); return 1; }
    if (getenv("SC_CACHE")) {                 /* DATA.ISO next to the cue */
        char iso_p[512]; strncpy(iso_p, argv[2], sizeof iso_p - 16); iso_p[sizeof iso_p - 16] = 0;
        char *sl = strrchr(iso_p, '/'); strcpy(sl ? sl + 1 : iso_p, "DATA.ISO");
        use_cache = cd_cache_open(iso_p);
        iso = fopen(iso_p, "rb");
        printf("cache %s: %d\n", iso_p, use_cache);
    }
    PicoDrawSetOutFormat(PDF_RGB555, 0);
    PicoDrawSetOutBuf(fb, 328 * 2);
    PicoLoopPrepare();
    PicoIn.writeSound = snd_write;
    PicoIn.sndOut = sndbuf;
    PsndRerate(0);
#ifdef MD_MQ
    use_hw = getenv("SC_HW") || getenv("SC_VERIFY");
    mq_enabled = use_hw;
    mq_verify = getenv("SC_VERIFY") != NULL;
    mq_reset_tiles();
    if (use_hw) mq_pvr_init();
    long bad_frames = 0, bad_px = 0, quads = 0, maxq = 0;
#endif
    int f0 = 1;
    if (getenv("SC_LOAD")) {
        if (PicoState(getenv("SC_LOAD"), 0)) { printf("state load failed\n"); return 1; }
#ifdef MD_MQ
        mq_reset_tiles();
#endif
        f0 = getenv("SC_LOADF") ? atoi(getenv("SC_LOADF")) : 1;
    } else if (getenv("SC_BOOT")) {
        mcd_boot_begin();
        while (mcd_boot_frame()) ;
        f0 = mcd_boot_frames() + 1;
        PicoIn.skipFrame = 0;
        printf("fast boot: game at frame %d\n", f0);
    }
    int save_at = getenv("SC_SAVE") ? atoi(getenv("SC_SAVE")) : -1;
    uint32_t last_hash = 0; int same_since = f0, pic_changes = 0;
    clock_t t0 = clock();
    for (frame = f0; frame <= nframes; frame++) {
        if (frame == save_at) PicoState(getenv("SC_STATE") ? getenv("SC_STATE") : "state.gz", 1);
        while (sidx < nscr && scr[sidx].f <= frame) { PicoIn.pad[0] = scr[sidx].b; sidx++; }
        if (random) PicoIn.pad[0] = rnd_pad();
        MARK(0);
        PicoFrame();
#ifdef MD_MQ
        if (use_hw) mq_pvr_show(fb, 328, vid_y0);
#endif
        MARK(1);
#ifdef MD_MQ
        if (use_hw) {
            mq_raster_tiles();
            mq_raster(hwpic, fb, 328, vid_y0);
            quads += mq.nq; if (mq.nq > maxq) maxq = mq.nq;
            if (mq_verify) {
                int fx, fy, n = mq_compare(hwpic, fb, 328, vid_y0, &fx, &fy);
                if (n) { bad_frames++; bad_px += n;
                    if (bad_frames <= 20) printf("frame %d: %d pixels differ, first (%d,%d)\n", frame, n, fx, fy);
                    if (getenv("SC_BADPPM") && bad_frames <= 8) {
                        dump_ppm(getenv("SC_BADPPM"), frame); use_hw = 0; dump_ppm(getenv("SC_BADPPM"), frame + 1000000); use_hw = 1; } }
            }
        }
#endif
        if (ppmdir && every && frame % every == 0) dump_ppm(ppmdir, frame);
        if (random || trace) {
            uint32_t h = pic_hash();
            if (h != last_hash) { last_hash = h; same_since = frame; pic_changes++; }
            if (random && hang && frame - same_since >= hang) {
                printf("HANG? frame %d: picture unchanged since frame %d; main %06x sub %06x sub flags %02x%02x\n",
                       frame, same_since, SekPc & 0xffffff, SekPcS68k & 0xffffff,
                       Pico_mcd->s68k_regs[0x0e], Pico_mcd->s68k_regs[0x0f]);
                if (ppmdir) dump_ppm(ppmdir, frame);
                break;
            }
        }
        if (trace && frame % trace == 0)
            printf("f%d main %06x sub %06x hash %08x flags %02x/%02x cdd %02x parked %u io %u/%u pad %02x\n", frame,
                   SekPc & 0xffffff, SekPcS68k & 0xffffff, last_hash, Pico_mcd->s68k_regs[0x0e], Pico_mcd->s68k_regs[0x0f],
                   Pico_mcd->s68k_regs[0x38], mcd_idle_parked, io_reads, io_seeks, PicoIn.pad[0]);
    }
    double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    if (wav) { wav_hdr(); fclose(wav); }
    if (iolog) fclose(iolog);
#ifdef MD_MQ
    if (use_hw) {
        printf("mq: %ld quads/frame (max %ld), hw lines %u, sw lines %u (", quads / (frame - f0 + 1), maxq, mq.hw_lines, mq.sw_lines);
        for (int i = 1; i < MQ_WHY_N; i++) printf("%s%u", i > 1 ? " " : "", mq.why[i]);
        printf(")\n");
        if (mq_verify) printf("verify: %ld frames differ, %ld pixels\n", bad_frames, bad_px);
    }
#endif
    printf("done: frames %d..%d in %.1f s (%.0f fps), picture changes %d, parked %u, data reads %u (%u seeks, %u hints, %u stalls)\n",
           f0, frame - 1, s, (frame - f0) / s, pic_changes, mcd_idle_parked, io_reads, io_seeks, io_hints, io_stalls);
    if (use_cache) printf("cache: %u chunks read, %u in the emulation, %u ticks held, %u bad sectors\n", cd_cache_loads, cd_cache_sync, cd_cache_waits, io_bad);
    return 0;
}

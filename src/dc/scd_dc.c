/* snatcher-dc: Dreamcast / KallistiOS front end for PicoDrive's Mega CD
   (Sega CD) core, made for Snatcher (USA).

   Files in the game folder (the disc root, or the folder the anthology menu
   gives; tools/ngcdconv makes the last three from the cue/bin image):
     BIOS.BIN     Sega CD BIOS, 128 KB: the disc's region (Snatcher USA:
                  bios_CD_U.bin); or one per region, BIOS_U.BIN /
                  BIOS_J.BIN (Mega-CD, Japanese discs: bios_CD_J.bin) /
                  BIOS_E.BIN, tried before BIOS.BIN; in the game
                  folder or the disc root
     DISC.CUE     cue sheet over the files below
     DATA.ISO     data track, 2048-byte sectors
     TRKnn.NGA    CD audio tracks, IMA ADPCM

   Per frame: pad -> PicoFrame() (main 68000, sub 68000 on FAME SH4, Z80,
   FM/PSG/PCM; the VDP line ranges become tile rectangles, src/mq) -> wait
   for the display -> mq_pvr_show().

   Nothing in the emulation waits on the GD-ROM:
   - the data track is read by a thread (src/scd/cd_cache.c), started when
     the emulated drive is told to seek; a sector that has not arrived yet
     holds the emulated drive for a tick (cdd.c), not the machine;
   - CD audio comes from a second thread through a ring of NGA blocks.
   Sound: PicoDrive at 22050 Hz plus the CD audio at 44.1 kHz, mixed to
   44.1 kHz for the AICA (src/scd/snd_mix.c).
   The 8 KB backup RAM is kept on the first VMU (file SCDBRAM). */
#include <kos.h>
#include <dc/sound/stream.h>
#include <dc/biosfont.h>
#include <dc/maple/controller.h>
#include <dc/vmu_pkg.h>
#include <dc/vmufs.h>
#include <dc/pvr.h>
#include <arch/timer.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>

#include "pico/pico_int.h"
#include "../mq/mq.h"
#include "mq_pvr.h"
#include "menu_return.h"
#include "nga.h"
#include "nga_cdda.h"
#include "snd_mix.h"
#include "mcd_boot.h"
#include "cd_cache.h"

#ifndef GAME_TITLE
#define GAME_TITLE "snatcher"
#endif

#ifdef SCD_RELEASE
KOS_INIT_FLAGS(INIT_DEFAULT | INIT_QUIET);
#else
KOS_INIT_FLAGS(INIT_DEFAULT);
#endif

/* ---- game folder (the marker of the other -dc programs, so the anthology
   menu can start this one from a sub folder) ---- */
#define DIR_MARKER "NGCD-DISC-DIR="
volatile char disc_dir_patch[64] __attribute__((used, aligned(4))) = DIR_MARKER "/cd/";
static const char *disc_dir = "/cd/";
static char disc_dir_buf[48];

static void pick_disc_dir(void) {
    const volatile char *p = disc_dir_patch + sizeof(DIR_MARKER) - 1;
    size_t n = 0;
    while (n < sizeof(disc_dir_buf) - 2 && p[n] && p[n] != ' ') { disc_dir_buf[n] = p[n]; n++; }
    disc_dir_buf[n] = 0;
    if (n < 5 || strncmp(disc_dir_buf, "/cd/", 4) != 0) return;
    if (disc_dir_buf[n - 1] != '/') { disc_dir_buf[n++] = '/'; disc_dir_buf[n] = 0; }
    disc_dir = disc_dir_buf;
}

static void fatal(const char *msg) {
    dbglog(DBG_ERROR, "%s: %s\n", GAME_TITLE, msg);
    vid_set_mode(DM_640x480, PM_RGB565);
    vid_clear(0, 0, 64);
    char line[64];
    int y = 24;
    for (const char *s = msg; *s && y < 456; y += 28) {      /* '\n' starts a line */
        size_t n = strcspn(s, "\n");
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, s, n); line[n] = 0;
        bfont_draw_str(vram_s + y * vid_mode->width + 8, vid_mode->width, true, line);
        s += n;
        if (*s == '\n') s++;
    }
    for (;;)
        thd_sleep(1000);
}

/* ------------------------------------------------- what the core needs */
void cache_flush_d_inval_i(void *start, void *end) { (void)start; (void)end; }
void *plat_mmap(unsigned long addr, size_t size, int need_exec, int is_fixed) {
    (void)addr; (void)need_exec; (void)is_fixed;
    void *p = memalign(32, size);
    if (p) memset(p, 0, size);
    return p;
}
void *plat_mremap(void *ptr, size_t oldsize, size_t newsize) {
    void *p = realloc(ptr, newsize);
    if (p && newsize > oldsize) memset((char *)p + oldsize, 0, newsize - oldsize);
    return p;
}
void plat_munmap(void *ptr, size_t size) { (void)size; free(ptr); }
void *plat_mem_get_for_drc(size_t size) { (void)size; return NULL; }
int plat_mem_set_exec(void *ptr, size_t size) { (void)ptr; (void)size; return 0; }
static int vid_y0 = 8, vid_h = 224, vid_w = 320;
void emu_video_mode_change(int start_line, int line_count, int start_col, int col_count) {
    (void)start_col;
    vid_y0 = start_line; vid_h = line_count; vid_w = col_count;
}
void emu_32x_startup(void) {}
void lprintf(const char *fmt, ...) {
#ifndef SCD_RELEASE
    char b[256];
    va_list a; va_start(a, fmt);
    vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    dbglog(DBG_INFO, "pico: %s", b);
#else
    (void)fmt;
#endif
}
/* The BIOS for the disc's region (PicoDrive reads it from the disc's header:
   1 Japan, 4 USA, 8 Europe): BIOS_J/U/E.BIN, else BIOS.BIN. A BIOS only
   boots discs of its own region (the security code at the start of the
   disc differs): a Japanese disc with an American BIOS ends in the BIOS's
   CD player, so a mismatch is said on screen instead. */
static char bios_path[96];
static int disc_region;
static const char *get_bios(int *region, const char *cd_fname) {
    (void)cd_fname;
    disc_region = *region;
    const char *name = *region == 1 ? "BIOS_J.BIN" : *region == 8 ? "BIOS_E.BIN" : "BIOS_U.BIN";
    /* the game's folder first, then the disc root (one set of BIOSes for
       all the games of an anthology disc) */
    const char *dirs[2] = { disc_dir, "/cd/" };
    for (int pass = 0; pass < 2; pass++)
        for (int d = 0; d < 2; d++) {
            if (d == 1 && !strcmp(disc_dir, "/cd/")) continue;
            snprintf(bios_path, sizeof bios_path, "%s%s", dirs[d], pass ? "BIOS.BIN" : name);
            FILE *f = fopen(bios_path, "rb");
            if (f) { fclose(f); return bios_path; }
        }
    snprintf(bios_path, sizeof bios_path, "%s%s", disc_dir, name);
    return bios_path;
}

/* the region letter of a Mega CD BIOS (its Mega Drive header, 0x1f0) */
static char bios_region(const char *path) {
    char c = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        if (fseek(f, 0x1f0, SEEK_SET) == 0 && fread(&c, 1, 1, f) != 1) c = 0;
        fclose(f);
    }
    return c;
}

/* ------------------------------------------------- data track (cd_cache.c) */
void cd_data_hook_seek(int lba) { cd_cache_seek(lba); }
int  cd_data_hook_ready(int lba) { return cd_cache_ready(lba); }
int  cd_data_hook_read(unsigned int pos, uint8_t *dst) { return cd_cache_read(pos, dst); }

/* ------------------------------------------------- CD audio reader (NGA) */
#define RING_BLOCKS 96                        /* 1.3 s */
#define READ_BATCH  16
static uint8_t cring[RING_BLOCKS][NGA_BLOCK];
static volatile uint32_t cring_head, cring_tail;   /* blocks written / consumed */
static mutex_t cring_lock = MUTEX_INITIALIZER;
static semaphore_t cwake;
static volatile int req_gen, req_play;
static volatile file_t req_fd = FILEHND_INVALID;
static volatile uint32_t req_block;
static uint32_t cd_underruns;

/* PicoDrive opened the track with fopen; the reader thread uses its KOS
   handle (PicoDrive does not touch it while it plays) */
void nga_src_open(void *f, uint32_t block) {
    mutex_lock(&cring_lock);
    req_fd = (file_t)fileno((FILE *)f);
    req_block = block;
    req_play = 1;
    req_gen++;
    cring_head = cring_tail = 0;
    mutex_unlock(&cring_lock);
    sem_signal(&cwake);
}

void nga_src_close(void) {
    mutex_lock(&cring_lock);
    req_play = 0;
    req_gen++;
    cring_head = cring_tail = 0;
    mutex_unlock(&cring_lock);
}

int nga_src_next(uint8_t *blk) {
    int have = 0;
    mutex_lock(&cring_lock);
    if (cring_head != cring_tail) {
        memcpy(blk, cring[cring_tail % RING_BLOCKS], NGA_BLOCK);
        cring_tail++;
        have = 1;
    } else if (req_play) {
        cd_underruns++;
    }
    mutex_unlock(&cring_lock);
    if (have && cring_head - cring_tail < RING_BLOCKS / 2) sem_signal(&cwake);
    return have;
}

static void *cdda_thread(void *arg) {
    (void)arg;
    static uint8_t tmp[READ_BATCH * NGA_BLOCK];
    int gen = -1, play = 0;
    file_t fd = FILEHND_INVALID;
    uint32_t block = 0, nblocks = 0;
    for (;;) {
        mutex_lock(&cring_lock);
        int g = req_gen, want = req_play;
        file_t rfd = req_fd;
        uint32_t rblock = req_block;
        uint32_t room = RING_BLOCKS - (cring_head - cring_tail);
        mutex_unlock(&cring_lock);

        if (g != gen) {                       /* new request */
            gen = g;
            play = 0;
            if (want && rfd >= 0) {
                uint8_t h[NGA_HEADER];
                fd = rfd;
                if (fs_seek(fd, 0, SEEK_SET) >= 0 && fs_read(fd, h, NGA_HEADER) == NGA_HEADER
                    && !memcmp(h, "NGA1", 4)) {
                    nblocks = h[12] | (h[13] << 8) | (h[14] << 16) | ((uint32_t)h[15] << 24);
                    block = rblock;
                    if (block < nblocks &&
                        fs_seek(fd, NGA_HEADER + (off_t)block * NGA_BLOCK, SEEK_SET) >= 0)
                        play = 1;
                }
            }
            continue;
        }
        if (!play || room < READ_BATCH) { sem_wait_timed(&cwake, 20); continue; }

        uint32_t n = nblocks - block;
        if (n > READ_BATCH) n = READ_BATCH;
        if (n == 0 || fs_read(fd, tmp, n * NGA_BLOCK) != (ssize_t)(n * NGA_BLOCK)) {
            play = 0;                         /* end of track */
            continue;
        }
        block += n;
        mutex_lock(&cring_lock);
        if (req_gen == gen) {
            for (uint32_t i = 0; i < n; i++)
                memcpy(cring[(cring_head + i) % RING_BLOCKS], tmp + i * NGA_BLOCK, NGA_BLOCK);
            cring_head += n;
        }
        mutex_unlock(&cring_lock);
    }
    return NULL;
}

/* ------------------------------------------------- backup RAM on the VMU */
#define VMU_FILE  "SCDBRAM"
#define BRAM_SIZE sizeof(Pico_mcd->bram)
static uint8_t saved_bram[0x2000], save_buf[0x2000], last_bram[0x2000];
static volatile int saving;
static uint32_t bram_check_frames;
static int bram_dirty_checks;

static maple_device_t *first_vmu(void) { return maple_enum_type(0, MAPLE_FUNC_MEMCARD); }

static void bram_load(void) {
    maple_device_t *vmu = first_vmu();
    void *data = NULL;
    int size = 0;
    if (vmu && vmufs_read(vmu, VMU_FILE, &data, &size) == 0 && data) {
        /* VMU file header (0x80 bytes): icon_cnt @0x40, eyecatch @0x44,
           data_len @0x48; icons (512 bytes each) follow, then the payload */
        const uint8_t *h = data;
        if (size >= 0x80) {
            uint32_t icons = h[0x40] | (h[0x41] << 8);
            uint32_t len = h[0x48] | (h[0x49] << 8) | (h[0x4A] << 16) | ((uint32_t)h[0x4B] << 24);
            uint32_t off = 0x80 + icons * 512;
            if (h[0x44] == 0 && len == BRAM_SIZE && off + len <= (uint32_t)size) {
                memcpy(Pico_mcd->bram, h + off, BRAM_SIZE);
                dbglog(DBG_INFO, "%s: backup RAM loaded from the VMU\n", GAME_TITLE);
            }
        }
        free(data);
    }
    memcpy(saved_bram, Pico_mcd->bram, BRAM_SIZE);
    memcpy(last_bram, Pico_mcd->bram, BRAM_SIZE);
}

static void *save_thread(void *arg) {
    (void)arg;
    maple_device_t *vmu = first_vmu();
    if (vmu) {
        vmu_pkg_t pkg;
        memset(&pkg, 0, sizeof pkg);
        strcpy(pkg.desc_short, "Sega CD save");
        strcpy(pkg.desc_long, "Snatcher: Sega CD backup RAM");
        strcpy(pkg.app_id, "snatcher-dc");
        pkg.icon_cnt = 0;
        pkg.eyecatch_type = VMUPKG_EC_NONE;
        pkg.data_len = BRAM_SIZE;
        pkg.data = save_buf;
        uint8_t *out = NULL;
        int out_size = 0;
        if (vmu_pkg_build(&pkg, &out, &out_size) == 0 && out) {
            if (vmufs_write(vmu, VMU_FILE, out, out_size, VMUFS_OVERWRITE) == 0)
                dbglog(DBG_INFO, "%s: backup RAM saved to the VMU\n", GAME_TITLE);
            else
                dbglog(DBG_WARNING, "%s: VMU write failed\n", GAME_TITLE);
            free(out);
        }
    }
    saving = 0;
    return NULL;
}

/* once a second: save when the backup RAM changed and then stayed the same */
static void bram_poll(void) {
    if (++bram_check_frames < 60) return;
    bram_check_frames = 0;
    int changed = memcmp(Pico_mcd->bram, saved_bram, BRAM_SIZE) != 0;
    int stable = memcmp(Pico_mcd->bram, last_bram, BRAM_SIZE) == 0;
    memcpy(last_bram, Pico_mcd->bram, BRAM_SIZE);
    if (!changed || !stable) { bram_dirty_checks = 0; return; }
    if (++bram_dirty_checks < 2 || saving) return;
    bram_dirty_checks = 0;
    memcpy(save_buf, Pico_mcd->bram, BRAM_SIZE);
    memcpy(saved_bram, Pico_mcd->bram, BRAM_SIZE);
    saving = 1;
    thd_create(1, save_thread, NULL);
}

/* ---------------------------------------------------------------- audio */
#define AUDIO_BUF    8192
#define STREAM_BYTES 4096
static int16_t abuf[AUDIO_BUF / 2] __attribute__((aligned(32)));

static void *audio_cb(snd_stream_hnd_t hnd, int req_bytes, int *got_bytes) {
    (void)hnd;
    int n = (req_bytes / 4);                         /* stereo frames */
    if (n > AUDIO_BUF / 4) n = AUDIO_BUF / 4;
    snd_mix_out(abuf, n);
    *got_bytes = n * 4;
    return abuf;
}

static volatile int audio_run;
static void *audio_thread(void *arg) {
    snd_stream_hnd_t snd = (snd_stream_hnd_t)(intptr_t)arg;
    while (audio_run) {
        snd_stream_poll(snd);
        thd_sleep(5);
    }
    return NULL;
}

/* ---------------------------------------------------------------- video */
static uint16_t fb[328 * 256] __attribute__((aligned(32)));

static void video_init(void) {
    vid_set_mode(DM_640x480, PM_RGB565);
    pvr_init_params_t params;
    memset(&params, 0, sizeof(params));
    params.opb_sizes[PVR_LIST_OP_POLY] = PVR_BINSIZE_16;
    params.opb_sizes[PVR_LIST_PT_POLY] = PVR_BINSIZE_32;   /* the tile rectangles */
    params.vertex_buf_size = 1280 * 1024;
    params.opb_overflow_count = 4;
    pvr_init(&params);
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    if (mq_pvr_init() != 0)
        fatal("out of video memory");
}

/* ----------------------------------------------------------------- pads */
static int quit;

/* MD pad bits: MXYZ SACB RLDU. Snatcher: C chooses, B goes back, A draws
   the blaster. Dreamcast A = C, B = B, X / Y = A. */
static uint16_t map_pad(const cont_state_t *s) {
    uint16_t b = 0;
    uint32_t k = s->buttons;
    if (k & CONT_DPAD_UP)    b |= 0x001;
    if (k & CONT_DPAD_DOWN)  b |= 0x002;
    if (k & CONT_DPAD_LEFT)  b |= 0x004;
    if (k & CONT_DPAD_RIGHT) b |= 0x008;
    if (s->joyx < -64) b |= 0x004;
    if (s->joyx > 64)  b |= 0x008;
    if (s->joyy < -64) b |= 0x001;
    if (s->joyy > 64)  b |= 0x002;
    if (k & CONT_A) b |= 0x020;                    /* C: choose */
    if (k & CONT_B) b |= 0x010;                    /* B: back */
    if (k & (CONT_X | CONT_Y)) b |= 0x040;         /* A: blaster */
    if (k & CONT_START) b |= 0x080;
    if ((b & 0x004) && (b & 0x008)) b &= ~0x00c;
    if ((b & 0x001) && (b & 0x002)) b &= ~0x003;
    return b;
}

static int b_held_at_start;

static void poll_pads(void) {
    maple_device_t *d = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *s = d ? (cont_state_t *)maple_dev_status(d) : NULL;
    PicoIn.pad[0] = s ? map_pad(s) : 0;
    if (s) {
        const uint32_t exit_combo = CONT_START | CONT_A | CONT_B | CONT_X | CONT_Y;
        if ((s->buttons & exit_combo) == exit_combo) quit = 1;
    }
}

/* ----------------------------------------------------------------- main */
static snd_stream_hnd_t snd;
static kthread_t *athd;

static void shutdown_and_exit(void) {
    while (saving) thd_sleep(10);                  /* a VMU save under way */
    audio_run = 0;
    thd_join(athd, NULL);
    snd_stream_stop(snd);
    snd_stream_destroy(snd);
    snd_stream_shutdown();
    pvr_shutdown();
    menu_return(disc_dir);                         /* started by the anthology menu: back to it */
    arch_exit();
}

#define MAX_SKIP 3

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    pick_disc_dir();
    dbglog(DBG_INFO, "%s: game folder %s\n", GAME_TITLE, disc_dir);
    sem_init(&cwake, 0);

    PicoIn.opt = POPT_EN_STEREO | POPT_EN_FM | POPT_EN_PSG | POPT_EN_Z80
               | POPT_EN_MCD_PCM | POPT_EN_MCD_CDDA | POPT_EN_MCD_GFX | POPT_DIS_32C_BORDER;
    PicoIn.sndRate = SND_MIX_EMU_RATE;
    PicoIn.autoRgnOrder = 0x184;                   /* US, Europe, Japan */
    PicoInit();
    PicoSetInputDevice(0, PICO_INPUT_PAD_3BTN);
    PicoSetInputDevice(1, PICO_INPUT_NOTHING);

    char path[96];
    snprintf(path, sizeof path, "%sDISC.CUE", disc_dir);
    int r = PicoLoadMedia(path, NULL, 0, NULL, get_bios, NULL, NULL);
    if (r == PM_BAD_CD_NO_BIOS) {
        char m[160];
        snprintf(m, sizeof m, "No Sega CD BIOS:\n%s (128 KB)", bios_path);
        fatal(m);
    }
    {
        char want = disc_region == 1 ? 'J' : disc_region == 8 ? 'E' : 'U';
        char have = bios_region(bios_path);
        if ((have == 'J' || have == 'U' || have == 'E') && have != want) {
            static const char *const need[3] = {
                "Japanese disc: needs the Mega-CD BIOS\n(bios_CD_J.bin) in the game folder\nas BIOS_J.BIN",
                "US disc: needs the Sega CD BIOS\n(bios_CD_U.bin) in the game folder\nas BIOS_U.BIN",
                "European disc: needs the Mega-CD BIOS\n(bios_CD_E.bin) in the game folder\nas BIOS_E.BIN" };
            fatal(need[want == 'J' ? 0 : want == 'U' ? 1 : 2]);
        }
    }
    if (r != PM_CD) { char m[128]; snprintf(m, sizeof m, "%s: no Sega CD disc (tools/ngcdconv)", path); fatal(m); }
    snprintf(path, sizeof path, "%sDATA.ISO", disc_dir);
    if (!cd_cache_open(path)) fatal("DATA.ISO missing");

    PicoDrawSetOutFormat(PDF_RGB555, 0);           /* RGB565 in this build */
    PicoDrawSetOutBuf(fb, 328 * 2);
    PicoLoopPrepare();
    snd_mix_init();                                /* sndOut, writeSound, CD output */
    PsndRerate(0);
    bram_load();
    thd_create(1, cdda_thread, NULL);

    video_init();
#ifndef SCD_SOFT
    mq_enabled = 1;
#endif
    mq_reset_tiles();

    snd_stream_init();
    snd = snd_stream_alloc(audio_cb, STREAM_BYTES);
    if (snd == SND_STREAM_INVALID)
        fatal("snd_stream_alloc failed");
    snd_stream_start(snd, 44100, 1);
    audio_run = 1;
    athd = thd_create(0, audio_thread, (void *)(intptr_t)snd);
    if (!athd) fatal("audio thread");
    thd_set_prio(athd, PRIO_DEFAULT - 1);

    /* fast boot (mcd_boot.c): the BIOS logo, title and licence screens at
       full speed behind a black screen; B held: the BIOS as it is (its CD
       player and backup RAM manager) */
    poll_pads();
    b_held_at_start = (PicoIn.pad[0] & 0x010) != 0;
    if (!b_held_at_start) {
        mcd_boot_begin();
        while (mcd_boot_frame()) {
            if ((mcd_boot_frames() & 31) == 0) {   /* keep the display and the pads alive */
                pvr_wait_ready();
                pvr_scene_begin();
                pvr_scene_finish();
                poll_pads();
                if (quit) shutdown_and_exit();
            }
        }
        PicoIn.skipFrame = 0;
        snd_mix_init();                            /* drop the boot's silence */
        mq_reset_tiles();
        dbglog(DBG_INFO, "%s: fast boot, game at frame %d\n", GAME_TITLE, mcd_boot_frames());
    }

    uint64_t t_base = timer_us_gettime64(), frames = 0;
    int skipped = 0;
#ifndef SCD_RELEASE
    extern uint32_t mcd_idle_parked;
    uint64_t t_emu = 0, t_wait = 0, t_show = 0, t_emu_max = 0;
    int perf_frames = 0, perf_shown = 0;
    uint32_t under0 = 0, cdu0 = 0, park0 = 0, held0 = 0, sync0 = 0, loads0 = 0, swl0 = 0, hwl0 = 0;
#endif
    for (;;) {
        poll_pads();
        if (quit) shutdown_and_exit();

        /* frames due at 59.94 Hz; behind -> skip the drawing of this one
           (at most MAX_SKIP in a row) */
        uint64_t now = timer_us_gettime64();
        frames++;
        uint64_t due = (now - t_base) * 5994 / 100000000;
        if (due > frames + 30) {
            t_base = now - frames * 100000000 / 5994;
            due = frames;
        }
        int skip = frames < due && skipped < MAX_SKIP;
        skipped = skip ? skipped + 1 : 0;
        PicoIn.skipFrame = (unsigned short)skip;

        PicoFrame();
        bram_poll();
#ifndef SCD_RELEASE
        uint64_t t1 = timer_us_gettime64();
        t_emu += t1 - now;
        if (t1 - now > t_emu_max) t_emu_max = t1 - now;
#endif
        if (!skip) {
            pvr_wait_ready();                      /* the last scene is done: paces to the display */
#ifndef SCD_RELEASE
            uint64_t t2 = timer_us_gettime64();
            t_wait += t2 - t1;
#endif
            if (!mq_enabled) {
                /* software only: the whole picture from the line buffer */
                mq.ns = 1; mq.nq = 0; mq.nd = 0; mq.nbanks = 0; mq.any_sw = 1;
                mq.w = vid_w; mq.h = vid_h;
                mq.s[0].y0 = 0; mq.s[0].y1 = (int16_t)(vid_h - 1); mq.s[0].sw = 1;
            }
            mq_pvr_show(fb, 328, vid_y0);
#ifndef SCD_RELEASE
            t_show += timer_us_gettime64() - t2;
            perf_shown++;
#endif
        }
#ifndef SCD_RELEASE
        if (++perf_frames == 120) {
            /* emu: emulation incl. the rectangles (avg, max); wait/show per shown frame;
               cd: drive ticks held for the data track, chunks read, reads in the emulation
               (should stay 0), CD audio blocks missing; sub idle: parked sub CPU loops */
            dbglog(DBG_INFO, "perf/frame: emu %u us (max %u), wait %u us, show %u us, shown %d/120, "
                   "sound underruns %u; cd: held %u, chunks %u, sync %u, cdda under %u; sub idle %u; "
                   "rects %u/shown, sw lines %u/%u\n",
                   (unsigned)(t_emu / 120), (unsigned)t_emu_max,
                   perf_shown ? (unsigned)(t_wait / perf_shown) : 0,
                   perf_shown ? (unsigned)(t_show / perf_shown) : 0,
                   perf_shown, (unsigned)(snd_mix_underruns - under0),
                   (unsigned)(cd_cache_waits - held0), (unsigned)(cd_cache_loads - loads0),
                   (unsigned)(cd_cache_sync - sync0), (unsigned)(cd_underruns - cdu0),
                   (unsigned)(mcd_idle_parked - park0),
                   perf_shown ? (unsigned)(mq_pvr_quads / perf_shown) : 0,
                   (unsigned)(mq.sw_lines - swl0), (unsigned)(mq.sw_lines + mq.hw_lines - swl0 - hwl0));
            mq_pvr_quads = mq_pvr_tiles = 0;
            under0 = snd_mix_underruns; cdu0 = cd_underruns; park0 = mcd_idle_parked;
            held0 = cd_cache_waits; sync0 = cd_cache_sync; loads0 = cd_cache_loads;
            swl0 = mq.sw_lines; hwl0 = mq.hw_lines;
            t_emu = t_wait = t_show = t_emu_max = 0;
            perf_frames = perf_shown = 0;
        }
#endif
    }
    return 0;
}

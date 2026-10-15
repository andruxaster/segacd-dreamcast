/* Data track (DATA.ISO) for PicoDrive's CD drive, read in the background.

   The Mega CD reads its data track at 75 sectors a second, and a game
   asks the drive to seek before it reads (the drive takes a while: PicoDrive
   counts ~11 CDD ticks plus the seek distance). Here:

   - cd_cache_seek() (cd_data_hook_seek): the drive was told to play from a sector of the
     data track (cdd.c); the reader thread starts on the chunk (64 KB) with
     that sector at once, while the emulated drive is still "seeking";
   - cd_cache_ready() (cd_data_hook_ready): the drive is about to read a sector; when its
     chunk is not in memory yet, the answer is "not yet" and the emulated
     drive waits a tick (cdd.c holds it: no sector, no decoder interrupt),
     as a slower drive would. The machine itself never waits on the disc:
     sound, picture and both CPUs run on;
   - cd_cache_read() (cd_data_hook_read): the sector, from memory (the drive only reads
     sectors it was told are ready).

   The reader keeps the chunk after the one being read, and the one after
   that, ahead of the drive. 16 chunks (1 MB), least recently used out.
   The host test build (no threads) reads a chunk the moment it is asked
   for. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _arch_dreamcast
#include <kos/fs.h>
#include <kos/mutex.h>
#include <kos/thread.h>
#include <kos/sem.h>
#else
#include <stdio.h>
#endif
#include "cd_cache.h"

#define CH_SECT  32                           /* sectors per chunk */
#define CH_BYTES (CH_SECT * 2048)
#define CH_N     16                           /* 1 MB */
#define AHEAD    2                            /* chunks read ahead of the drive */
#define WANT_N   4                            /* queued requests */

enum { EMPTY, LOADING, READY };
typedef struct {
    volatile int state;
    uint32_t first;                           /* first sector */
    uint32_t len;                             /* bytes read */
    uint32_t used;                            /* LRU tick */
    uint8_t *buf;
} chunk_t;

static chunk_t ch[CH_N];
static int ok;
static uint32_t tick, total_sectors;
static uint32_t want[WANT_N];                 /* chunk first sectors for the reader */
static volatile uint32_t want_w, want_r;
uint32_t cd_cache_waits;                      /* drive ticks held (statistics) */
uint32_t cd_cache_sync;                       /* reads done by the emulation itself */
uint32_t cd_cache_loads;                      /* chunks read */

#ifdef _arch_dreamcast
static file_t fd = FILEHND_INVALID;
static mutex_t lock = MUTEX_INITIALIZER;
static semaphore_t wake;
#define LOCK()   mutex_lock(&lock)
#define UNLOCK() mutex_unlock(&lock)
static int file_read(uint32_t off, void *buf, uint32_t n) {
    if (fs_seek(fd, off, SEEK_SET) < 0) return -1;
    return fs_read(fd, buf, n);
}
#else
static FILE *fp;
#define LOCK()
#define UNLOCK()
static int file_read(uint32_t off, void *buf, uint32_t n) {
    if (fseek(fp, off, SEEK_SET) != 0) return -1;
    return (int)fread(buf, 1, n, fp);
}
#endif

static chunk_t *find(uint32_t first) {
    for (int i = 0; i < CH_N; i++)
        if (ch[i].state != EMPTY && ch[i].first == first)
            return &ch[i];
    return NULL;
}

/* an empty chunk, else the least recently used ready one */
static chunk_t *victim(void) {
    chunk_t *best = NULL;
    for (int i = 0; i < CH_N; i++) {
        if (ch[i].state == EMPTY) return &ch[i];
        if (ch[i].state == READY && (!best || ch[i].used < best->used))
            best = &ch[i];
    }
    return best;
}

/* locked: claim a chunk for 'first' (NULL: there already, or none free) */
static chunk_t *claim(uint32_t first) {
    if (first >= total_sectors || find(first)) return NULL;
    chunk_t *c = victim();
    if (!c) return NULL;
    c->state = LOADING;
    c->first = first;
    c->used = ++tick;
    return c;
}

/* c: LOADING, claimed; read without the lock */
static void load(chunk_t *c) {
    int r = file_read(c->first * 2048, c->buf, CH_BYTES);
    LOCK();
    c->len = r > 0 ? (uint32_t)r : 0;
    c->used = ++tick;
    c->state = READY;
    cd_cache_loads++;
    UNLOCK();
}

/* ask for the chunk of sector s (and AHEAD after it) */
static void request(uint32_t s) {
    uint32_t first = s - s % CH_SECT;
#ifdef _arch_dreamcast
    LOCK();
    for (int k = 0; k <= AHEAD; k++) {
        uint32_t f = first + k * CH_SECT;
        if (f >= total_sectors || find(f)) continue;
        int dup = 0;
        for (uint32_t i = want_r; i != want_w; i++)
            if (want[i % WANT_N] == f) dup = 1;
        if (dup) continue;
        if (want_w - want_r >= WANT_N) want_r++;   /* oldest request dropped */
        want[want_w % WANT_N] = f;
        want_w++;
    }
    UNLOCK();
    sem_signal(&wake);
#else
    for (int k = 0; k <= AHEAD; k++) {
        chunk_t *c = claim(first + k * CH_SECT);
        if (c) load(c);
    }
#endif
}

#ifdef _arch_dreamcast
static void *reader_thread(void *arg) {
    (void)arg;
    for (;;) {
        sem_wait(&wake);
        for (;;) {
            chunk_t *c = NULL;
            LOCK();
            while (!c && want_r != want_w) {
                uint32_t f = want[want_r % WANT_N];
                want_r++;
                c = claim(f);
            }
            UNLOCK();
            if (!c) break;
            load(c);
        }
    }
    return NULL;
}
#endif

int cd_cache_open(const char *path) {
#ifdef _arch_dreamcast
    fd = fs_open(path, O_RDONLY);
    if (fd == FILEHND_INVALID) return 0;
    total_sectors = (uint32_t)(fs_total(fd) / 2048);
#else
    fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    total_sectors = (uint32_t)(ftell(fp) / 2048);
#endif
    for (int i = 0; i < CH_N; i++) {
        ch[i].buf = malloc(CH_BYTES);
        if (!ch[i].buf) return 0;
        ch[i].state = EMPTY;
    }
#ifdef _arch_dreamcast
    sem_init(&wake, 0);
    thd_create(1, reader_thread, NULL);
#endif
    ok = 1;
    return 1;
}

int cd_cache_active(void) { return ok; }

void cd_cache_seek(int lba) {
    if (!ok || lba < 0) return;
    request((uint32_t)lba);
}

int cd_cache_ready(int lba) {
    if (!ok || lba < 0 || (uint32_t)lba >= total_sectors) return 1;
    uint32_t s = (uint32_t)lba, first = s - s % CH_SECT;
    LOCK();
    chunk_t *c = find(first);
    int ready = c && c->state == READY;
    if (ready) c->used = ++tick;
    UNLOCK();
    if (!ready) cd_cache_waits++;
    /* keep the reader ahead: from the middle of a chunk on, the next ones */
    if (!ready || s - first >= CH_SECT / 4)
        request(ready ? first + CH_SECT : s);
    return ready;
}

/* PicoDrive: sector at byte offset pos of the data track; 0: not here,
   PicoDrive reads it from the file itself */
int cd_cache_read(unsigned int pos, uint8_t *dst) {
    if (!ok || (pos & 2047)) return 0;
    uint32_t s = pos / 2048, first = s - s % CH_SECT;
    for (;;) {
        LOCK();
        chunk_t *c = find(first);
        if (c && c->state == READY) {
            uint32_t off = (s - first) * 2048;
            int got = off + 2048 <= c->len;
            if (got) { memcpy(dst, c->buf + off, 2048); c->used = ++tick; }
            UNLOCK();
            return got;
        }
        if (!c) {                             /* not asked for: read it here */
            c = claim(first);
            UNLOCK();
            if (!c) return 0;
            cd_cache_sync++;
            load(c);
            continue;
        }
        UNLOCK();                             /* being read by the thread */
#ifdef _arch_dreamcast
        thd_sleep(1);
#endif
    }
}

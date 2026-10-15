/* PicoDrive's FAME C interface (cpu/fame/fame.h: fm68k_*, M68K_CONTEXT) on
   top of the FAME SH4 assembler core (src/ffight/fame/fame.s), so the
   Mega CD's two 68000s run on the fast core. PicoDrive itself only gains a
   hook in its memory map setup (fame_pico_map, called from memory.c).

   Memory: the core resolves addresses through 4 KB page tables (fetch,
   read byte/word, write byte/word) reached through five pointers
   (m68k_page_tables). Each CPU here owns its tables, derived from
   PicoDrive's 64 KB maps: memory pages map directly (host = data + address,
   words in host order, bytes at a ^ 1, PicoDrive's layout too), handler
   pages call a trampoline that runs PicoDrive's read/write function with
   its cycle counter (io_cycle_counter: SekCyclesDone, SekEndRun) in step
   with the core's. Code fetch follows FAME C's Fetch[]: set for memory
   mapped through cpu68k_map_set / _read_mem / _all_ram.

   CPU switches copy the registers in and out of the core's global context.
   A handler of one CPU may run the other (pcd_sync_s68k): the core's global
   state of the interrupted run is then saved around the nested run (its
   SH4 registers are kept by the calling convention).

   Interrupts: PicoDrive keeps the wanted level in interrupts[0] and acks
   through iack_handler; the core keeps a pending bit per level and only
   looks at it at the start of a run and when SR changes. The level is
   applied before each run, after each acknowledge and after each handler
   call; a run is cut short (and resumed) when a newly raised level is
   above the current mask.

   From dc-poker (src/snatcher/fame_pico.c, Mega CD Snatcher); md-dc adds
   idle_wait(). */
#include <string.h>
#include "pico_int.h"
#include "memory.h"

#ifdef FAME_LINUX_TEST
#define ASM(x) __asm__("_" #x)
#else
#define ASM(x)
#endif

/* --- the assembler core (see src/ffight/fame/fame_api.h) --- */
struct sh4_data { u32 lo, hi; void *handler; void *data; };
struct sh4_prg  { u32 lo, hi; u32 offset; };
struct sh4_ctx {
    void *lists[15];                  /* memory lists, unused here */
    void (*reset_handler)(void);
    void (*iack_handler)(unsigned unused, unsigned level);
    unsigned *icust_handler;
    u32 dreg[8], areg[8], asp, pc, cycles_counter;
    u8  interrupts[8];
    u16 sr, execinfo;
};
extern struct sh4_ctx m68kcontext ASM(m68kcontext);
extern void *m68k_page_tables[5] ASM(m68k_page_tables);
/* cycles left in the run (fame.s: __io_cycle_counter), current while
   the core calls a handler and reloaded after it */
#ifdef FAME_LINUX_TEST
extern s32 core_left ASM(_io_cycle_counter);
#define CORE_LEFT core_left
#else
extern s32 _io_cycle_counter;
#define CORE_LEFT _io_cycle_counter
#endif
unsigned m68k_emulate(int n) ASM(m68k_emulate);
int m68k_raise_irq(int level, int vector) ASM(m68k_raise_irq);

#define PAGES 4096                    /* 16 MB / 4 KB */
#define SH4_STOPPED 0x80
/* global state of a run: context .. page table pointers (fame.s .data) */
#define STATE_START ((u8 *)&m68kcontext.dreg)
#define STATE_END   ((u8 *)(m68k_page_tables + 5))
#define STATE_MAX   256

typedef struct {
    M68K_CONTEXT *ctx;
    uptr *r8, *r16, *w8, *w16;        /* PicoDrive's maps for this CPU */
    void *tbl[5][PAGES];              /* fetch, rb, rw, wb, ww */
    struct sh4_prg  fetch[PAGES];
    struct sh4_data rb[PAGES], rw[PAGES], wb[PAGES], ww[PAGES];
    void *h_rb, *h_rw, *h_wb, *h_ww;  /* trampolines */
    int level;                        /* level applied to the core */
    int forced;                       /* run cut short for an interrupt */
    s32 forced_left;
} cpu_t;

static cpu_t cpus[2];
static cpu_t *running;                /* CPU inside m68k_emulate */
static int ready;
/* a jump to where no memory is mapped for fetching lands in "bra *" */
static u16 trap_page[0x800] __attribute__((aligned(4)));
static struct sh4_prg trap_prg[PAGES];

static void apply_level(cpu_t *c, int level) {
    c->level = level;
    m68kcontext.interrupts[0] = 0;
    if (level) m68k_raise_irq(level, -1);   /* autovector; ends a STOP */
}

/* after PicoDrive code ran from inside the core */
static inline void io_after(cpu_t *c) {
    M68K_CONTEXT *ctx = c->ctx;
    s32 left = ctx->io_cycle_counter;
    if (ctx->interrupts[0] != c->level) {
        apply_level(c, ctx->interrupts[0]);
        if (c->level > ((m68kcontext.sr >> 8) & 7)) c->forced = 1;
    }
    if (c->forced) { c->forced_left = left; left = 0; }
    CORE_LEFT = left;
}

#define TRAMPOLINES(n, C) \
static u32 rb_##n(u32 a) { C.ctx->io_cycle_counter = CORE_LEFT; u32 v = C.ctx->read_byte(a); io_after(&C); return v; } \
static u32 rw_##n(u32 a) { C.ctx->io_cycle_counter = CORE_LEFT; u32 v = C.ctx->read_word(a); io_after(&C); return v; } \
static void wb_##n(u32 a, u32 d) { C.ctx->io_cycle_counter = CORE_LEFT; C.ctx->write_byte(a, d); io_after(&C); } \
static void ww_##n(u32 a, u32 d) { C.ctx->io_cycle_counter = CORE_LEFT; C.ctx->write_word(a, d); io_after(&C); }
TRAMPOLINES(m, cpus[0])
TRAMPOLINES(s, cpus[1])

/* called by the core after taking an interrupt (r5 = level); the core has
   cleared that level's pending bit */
static void iack(unsigned unused, unsigned level) {
    cpu_t *c = running;
    M68K_CONTEXT *ctx = c->ctx;
    ctx->io_cycle_counter = CORE_LEFT;
    if (ctx->iack_handler) ctx->iack_handler(level);
    else ctx->interrupts[0] = 0;
    apply_level(c, ctx->interrupts[0]);
    s32 left = ctx->io_cycle_counter;
    if (c->forced) { c->forced_left = left; left = 0; }
    CORE_LEFT = left;
}

/* handler value for the core (fame.s): write-protected memory, written
   directly at and above the address in the entry's lo word */
#define WP_DIRECT ((void *)1)

/* rebuild the core's page entries for [start, end] of one CPU from
   PicoDrive's maps; fetch_ptr: host address of start when the range was
   mapped as memory for reading (FAME C's Fetch[]), else NULL */
void fame_pico_map(u32 start, u32 end, int is_sub, const void *fetch_ptr) {
    cpu_t *c = &cpus[is_sub ? 1 : 0];
    if (!ready) return;
    start &= 0xFFFFFF; end &= 0xFFFFFF;
    for (u32 p = start >> 12; p <= end >> 12; p++) {
        u32 a = p << 12, m = a >> M68K_MEM_SHIFT;
        uptr v;
#define ENTRY(tab, map, h) \
        v = c->map[m]; \
        c->tab[p].lo = a; c->tab[p].hi = a + 0xFFF; \
        if (map_flag_set(v)) { c->tab[p].handler = c->h; c->tab[p].data = NULL; } \
        else { c->tab[p].handler = NULL; c->tab[p].data = (void *)(v << 1); }
        ENTRY(rb, r8, h_rb)
        ENTRY(rw, r16, h_rw)
        ENTRY(wb, w8, h_wb)
        ENTRY(ww, w16, h_ww)
        /* Mega CD PRG RAM 000000-01ffff: PicoDrive checks the write
           protection (s68k_regs[2], 512-byte units) on every write. Pages
           above it are written directly, the page holding the boundary
           too with the core doing the same check (WP_DIRECT); refreshed
           by fame_pico_prg_wp */
        if (c == &cpus[1] && a < 0x20000 && (PicoIn.AHW & PAHW_MCD) && Pico_mcd
            && map_flag_set(c->w16[m]) && !map_flag_set(c->r16[m])) {
            u32 wp = (u32)Pico_mcd->s68k_regs[2] << 9;
            u8 *prg_base = (u8 *)(c->r16[m] << 1);     /* sub CPU address 0 */
            if (a >= wp) {
                c->wb[p].handler = c->ww[p].handler = NULL;
                c->wb[p].data = c->ww[p].data = prg_base;
            } else if (a + 0xFFF >= wp) {
                c->wb[p].handler = c->ww[p].handler = WP_DIRECT;
                c->wb[p].data = c->ww[p].data = prg_base;
                c->wb[p].lo = c->ww[p].lo = wp;
            }
        }
        if (fetch_ptr) {
            c->fetch[p].lo = a; c->fetch[p].hi = a + 0xFFF;
            c->fetch[p].offset = (u32)((uptr)fetch_ptr - start);
            c->tbl[0][p] = &c->fetch[p];
        }
    }
}

void fame_pico_prg_wp(void) { fame_pico_map(0, 0x1FFFF, 1, NULL); }

static void cpu_setup(cpu_t *c, M68K_CONTEXT *ctx, int sub) {
    c->ctx = ctx;
    if (!sub) {
        c->r8 = m68k_read8_map; c->r16 = m68k_read16_map;
        c->w8 = m68k_write8_map; c->w16 = m68k_write16_map;
        c->h_rb = rb_m; c->h_rw = rw_m; c->h_wb = wb_m; c->h_ww = ww_m;
    } else {
        c->r8 = s68k_read8_map; c->r16 = s68k_read16_map;
        c->w8 = s68k_write8_map; c->w16 = s68k_write16_map;
        c->h_rb = rb_s; c->h_rw = rw_s; c->h_wb = wb_s; c->h_ww = ww_s;
    }
    for (int p = 0; p < PAGES; p++) {
        c->tbl[0][p] = &trap_prg[p];
        c->tbl[1][p] = &c->rb[p]; c->tbl[2][p] = &c->rw[p];
        c->tbl[3][p] = &c->wb[p]; c->tbl[4][p] = &c->ww[p];
    }
}

void fm68k_init(void) {
    if (ready) return;
    for (int i = 0; i < 0x800; i++) trap_page[i] = 0x60FE;
    for (int p = 0; p < PAGES; p++) {           /* host = trap page + (a & 0xFFF) */
        trap_prg[p].lo = p << 12; trap_prg[p].hi = (p << 12) + 0xFFF;
        trap_prg[p].offset = (u32)(uptr)trap_page - (p << 12);
    }
    cpu_setup(&cpus[0], &PicoCpuFM68k, 0);
    cpu_setup(&cpus[1], &PicoCpuFS68k, 1);
    ready = 1;
    /* maps set up before this call */
    fame_pico_map(0, 0xFFFFFF, 0, NULL);
    fame_pico_map(0, 0xFFFFFF, 1, NULL);
    for (int s = 0; s < 2; s++) {               /* and their fetch areas */
        M68K_CONTEXT *ctx = cpus[s].ctx;
        int shiftout = 24 - FAMEC_FETCHBITS;
        for (int i = 0; i < M68K_FETCHBANK1; i++)
            if (ctx->Fetch[i]) {
                u32 a = (u32)i << shiftout;
                fame_pico_map(a, a + (1 << shiftout) - 1, s,
                              (void *)(ctx->Fetch[i] + a));
            }
    }
}

int fm68k_reset(M68K_CONTEXT *ctx) {
    fm68k_init();
    memset(ctx->dreg, 0, sizeof(ctx->dreg));
    memset(ctx->areg, 0, sizeof(ctx->areg));
    ctx->asp = 0;
    ctx->interrupts[0] = 0;
    ctx->execinfo = 0;
    ctx->sr = 0x2700;
    ctx->areg[7].D = ctx->read_long(0);
    ctx->pc = ctx->read_long(4) & 0xFFFFFF;
    return 0;
}

u32 fm68k_get_pc(const M68K_CONTEXT *ctx) {
    extern unsigned m68k_get_pc(void) ASM(m68k_get_pc);
    if (running && running->ctx == ctx) return m68k_get_pc() & 0xFFFFFF;
    return ctx->pc & 0xFFFFFF;
}

int fm68k_would_interrupt(M68K_CONTEXT *ctx) {
    u32 sr = (running && running->ctx == ctx) ? m68kcontext.sr : ctx->sr;
    return ctx->interrupts[0] > ((sr >> 8) & 7);
}

/* idle loop patching relies on FAME C opcodes: not available */
int fm68k_idle_install(void) { return 0; }
int fm68k_idle_remove(void) { return 0; }

/* md-dc: the main CPU waiting for its vblank interrupt in ROM code
       tst.b/tst.w (abs).w ; bne.s/beq.s *-4      (all three Streets of Rage)
   on a RAM flag that only the interrupt changes. Taking the rest of the
   run at once is what FAME C's idle patches do (fm68k_idle_install is not
   available here). PC at either instruction; never with an interrupt due. */
static int idle_wait(const M68K_CONTEXT *ctx) {
    if (PicoIn.AHW || ctx != &PicoCpuFM68k) return 0;
    if (ctx->interrupts[0] > ((ctx->sr >> 8) & 7)) return 0;
    u32 pc = ctx->pc & 0xFFFFFF;
    const u16 *rom = (const u16 *)Pico.rom;
    for (int k = 0; k < 2; k++, pc -= 4) {
        if (pc + 6 > Pico.romsize || (pc & 1)) return 0;
        u16 op = rom[pc >> 1], br = rom[(pc >> 1) + 2];
        if ((op != 0x4A38 && op != 0x4A78) || (br != 0x66FA && br != 0x67FA)) continue;
        u32 a = (u32)(s32)(s16)rom[(pc >> 1) + 1] & 0xFFFFFF;
        if (a < 0xFF0000) return 0;
        u32 v = op == 0x4A38 ? ((u8 *)PicoMem.ram)[(a & 0xFFFF) ^ 1] : ((u16 *)PicoMem.ram)[(a & 0xFFFF) >> 1];
        return br == 0x66FA ? v != 0 : v == 0;
    }
    return 0;
}
u32 fame_pico_idle_runs;

int fm68k_emulate(M68K_CONTEXT *ctx, int cycles, fm68k_call_reason reason) {
    if (reason != fm68k_reason_emulate) {
        if (reason == fm68k_reason_init) fm68k_init();
        return 0;
    }
    if (idle_wait(ctx)) {
        fame_pico_idle_runs++;
        ctx->io_cycle_counter = 0;
        return cycles;
    }
    cpu_t *c = ctx == &PicoCpuFS68k ? &cpus[1] : &cpus[0];
    if (ctx->execinfo & FM68K_HALTED) {
        if (ctx->interrupts[0] <= ((ctx->sr >> 8) & 7)) {
            ctx->io_cycle_counter = 0;
            return cycles;
        }
        ctx->execinfo &= ~FM68K_HALTED;
    }

    /* nested in a handler of the other CPU: keep its run's global state */
    cpu_t *outer = running;
    u32 saved[STATE_MAX / 4];
    if (outer) memcpy(saved, STATE_START, STATE_END - STATE_START);

    for (int i = 0; i < 5; i++) m68k_page_tables[i] = c->tbl[i];
    for (int i = 0; i < 8; i++) {
        m68kcontext.dreg[i] = ctx->dreg[i].D;
        m68kcontext.areg[i] = ctx->areg[i].D;
    }
    m68kcontext.asp = ctx->asp;
    m68kcontext.pc = ctx->pc;
    m68kcontext.sr = ctx->sr;
    m68kcontext.execinfo = 0;
    m68kcontext.iack_handler = iack;
    apply_level(c, ctx->interrupts[0]);
    running = c;

    s32 left = cycles;
    for (;;) {
        s32 req = left;
        c->forced = 0;
        ctx->io_cycle_counter = req;
        u32 before = m68kcontext.cycles_counter;
        m68k_emulate(req);
        s32 done = (s32)(m68kcontext.cycles_counter - before);
        left = req - done;                  /* the core's own count */
        if (!c->forced) break;
        /* cut short at counter 0: resume with what PicoDrive left */
        left += c->forced_left;
        if (left <= 0 || (ctx->execinfo & FM68K_HALTED)
            || (m68kcontext.execinfo & SH4_STOPPED)) break;
        apply_level(c, ctx->interrupts[0]);
    }
    ctx->io_cycle_counter = left;

    for (int i = 0; i < 8; i++) {
        ctx->dreg[i].D = m68kcontext.dreg[i];
        ctx->areg[i].D = m68kcontext.areg[i];
    }
    ctx->asp = m68kcontext.asp;
    ctx->pc = m68kcontext.pc;
    ctx->sr = m68kcontext.sr;
    if (m68kcontext.execinfo & SH4_STOPPED) ctx->execinfo |= FM68K_HALTED;

    running = outer;
    if (outer) memcpy(STATE_START, saved, STATE_END - STATE_START);
    return cycles - left;
}

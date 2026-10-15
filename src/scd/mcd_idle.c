/* Snatcher (USA): the sub CPU's idle loops.

   Most of the time the sub CPU program waits in one of three loops for
   its interrupt handlers to change a few flags. Loop 1 (gameplay):

     01681e  tst.b   $ff802f         ; own status register, 0
             bne.w   $16952
             bsr.w   $1510c          ; -> $15130: movem, bsr $16364
                                     ;    ($16364: tst.b $7f80f / beq rts)
             move.b  $1511e,d0
             andi.b  #7,d0
             bne.s   $1681e
             tst.b   $9b44
             bne.s   $1681e

   Loops 2 (intro and other sequences) and 3 (menus) are described at
   sig2 and sig3 below. That is
   12.5 MHz of 68000 emulation doing nothing but reading PRG RAM (a third
   of the frame's CPU time on the Dreamcast). While its flags say
   it keeps looping, nothing but an interrupt can change them, so the CPU
   is parked the way PicoDrive parks a CPU polling a register
   (PCD_ST_S68K_POLL: its cycles are skipped, any interrupt wakes it).
   Hooked into the sub CPU's read of $ff802e/f (cd/memory.c,
   MCD_IDLE_HOOK); the code is checked word by word first. */
#include "pico_int.h"

typedef struct { u32 a; u16 w; } idle_sig_t;

/* the common part: $1510c -> $15130 -> $16364 (nothing when $7f80f = 0) */
static const idle_sig_t sig_sub[] = {
    { 0x1510c, 0x4ef9 }, { 0x1510e, 0x0001 }, { 0x15110, 0x5130 },
    { 0x15130, 0x48e7 }, { 0x15132, 0xf0c0 }, { 0x15134, 0x6100 },
    { 0x15136, 0x122e }, { 0x15138, 0x4cdf }, { 0x1513a, 0x030f },
    { 0x1513c, 0x4e75 },
    { 0x16364, 0x4a39 }, { 0x16366, 0x0007 }, { 0x16368, 0xf80f },
    { 0x1636a, 0x6700 }, { 0x1636c, 0x025c }, { 0x165c8, 0x4e75 },
};
/* loop 1 (gameplay), above */
static const idle_sig_t sig1[] = {
    { 0x1681e, 0x4a39 }, { 0x16820, 0x00ff }, { 0x16822, 0x802f },
    { 0x16824, 0x6600 }, { 0x16826, 0x012c }, { 0x16828, 0x6100 },
    { 0x1682a, 0xe8e2 }, { 0x1682c, 0x1039 }, { 0x1682e, 0x0001 },
    { 0x16830, 0x511e }, { 0x16832, 0x0200 }, { 0x16834, 0x0007 },
    { 0x16836, 0x66e6 }, { 0x16838, 0x4a39 }, { 0x1683a, 0x0000 },
    { 0x1683c, 0x9b44 }, { 0x1683e, 0x66de },
};
/* loop 2 (intro and other sequences):
     0166ba  tst.b   $ff802f
             bne.w   $16952
             move.w  $9808,d0        ; step of the sequence
             add.w   d0,d0
             move.w  $166d4(pc,d0.w),d0
             jmp     $166d4(pc,d0.w) ; step 0 -> $166e0:
     0166e0  tst.b   $78ad.w         ; < 0: next step
             bmi.s   $166ee
             bne.s   $166ec
             bsr.w   $1510c
     0166ec  bra.s   $166ba
   idle while the step is 0 and $78ad is not negative */
static const idle_sig_t sig2[] = {
    { 0x166ba, 0x4a39 }, { 0x166bc, 0x00ff }, { 0x166be, 0x802f },
    { 0x166c0, 0x6600 }, { 0x166c2, 0x0290 }, { 0x166c4, 0x3039 },
    { 0x166c6, 0x0000 }, { 0x166c8, 0x9808 }, { 0x166ca, 0xd040 },
    { 0x166cc, 0x303b }, { 0x166ce, 0x0006 }, { 0x166d0, 0x4efb },
    { 0x166d2, 0x0002 }, { 0x166d4, 0x000c }, { 0x166e0, 0x4a38 },
    { 0x166e2, 0x78ad }, { 0x166e4, 0x6b08 }, { 0x166e6, 0x6604 },
    { 0x166e8, 0x6100 }, { 0x166ea, 0xea22 }, { 0x166ec, 0x60cc },
};

/* loop 3 (menus: waiting for a button):
     01105e  bsr.s   $11078          ; the menu's input step
             tst.b   $ff802f
             bne.s   $1106e
             tst.b   $79f4.w
             beq.s   $1105e
   With $79fe = 0 the step is $110b0 + $11150: with no new button ($7991
   = 0) it runs the auto-repeat of the held button ($7990) from the timer
   $7848 (state $9713, last time $9714) and returns the button to act on,
   or 0. Idle while a pass would return 0 and change nothing (the case by
   case conditions below); the timer and the buttons change only in the
   interrupt handlers. */
static const idle_sig_t sig3[] = {
    { 0x1105e, 0x6118 }, { 0x11060, 0x4a39 }, { 0x11062, 0x00ff },
    { 0x11064, 0x802f }, { 0x11066, 0x6606 }, { 0x11068, 0x4a38 },
    { 0x1106a, 0x79f4 }, { 0x1106c, 0x67f0 }, { 0x1106e, 0x11fc },
    { 0x11070, 0x0003 }, { 0x11072, 0x7a25 }, { 0x11074, 0x6000 },
    { 0x11076, 0x0d58 }, { 0x11078, 0x0c78 }, { 0x1107a, 0x0001 },
    { 0x1107c, 0x79fe }, { 0x1107e, 0x670e }, { 0x11080, 0x0c78 },
    { 0x11082, 0x0002 }, { 0x11084, 0x79fe }, { 0x11086, 0x641a },
    { 0x11088, 0x6126 }, { 0x1108a, 0x6000 }, { 0x1108c, 0x00c4 },
    { 0x1108e, 0x6100 }, { 0x11090, 0xf7b6 }, { 0x11092, 0x6100 },
    { 0x11094, 0xfef0 }, { 0x11096, 0x66f0 }, { 0x11098, 0x0838 },
    { 0x1109a, 0x0004 }, { 0x1109c, 0x7990 }, { 0x1109e, 0x67e8 },
    { 0x110a0, 0x4e75 }, { 0x110a2, 0x6100 }, { 0x110a4, 0xc408 },
    { 0x110a6, 0x6100 }, { 0x110a8, 0x0ad2 }, { 0x110aa, 0x6104 },
    { 0x110ac, 0x6000 }, { 0x110ae, 0x05d6 }, { 0x110b0, 0x1038 },
    { 0x110b2, 0x7991 }, { 0x110b4, 0x6600 }, { 0x110b6, 0x0092 },
    { 0x110b8, 0x4a39 }, { 0x110ba, 0x0000 }, { 0x110bc, 0x9713 },
    { 0x110be, 0x671c }, { 0x110c0, 0x0c39 }, { 0x110c2, 0x0001 },
    { 0x110c4, 0x0000 }, { 0x110c6, 0x9713 }, { 0x110c8, 0x672e },
    { 0x110ca, 0x0c78 }, { 0x110cc, 0x0003 }, { 0x110ce, 0x79fe },
    { 0x110d0, 0x6754 }, { 0x110d2, 0x0c78 }, { 0x110d4, 0x0002 },
    { 0x110d6, 0x79fe }, { 0x110d8, 0x6452 }, { 0x110da, 0x6036 },
    { 0x110dc, 0x3038 }, { 0x110de, 0x7848 }, { 0x110e0, 0x13c0 },
    { 0x110e2, 0x0000 }, { 0x110e4, 0x9714 }, { 0x110e6, 0x4a38 },
    { 0x110e8, 0x7990 }, { 0x110ea, 0x6758 }, { 0x110ec, 0x5239 },
    { 0x110ee, 0x0000 }, { 0x110f0, 0x9713 }, { 0x110f2, 0x5339 },
    { 0x110f4, 0x0000 }, { 0x110f6, 0x9714 }, { 0x110f8, 0x3038 },
    { 0x110fa, 0x7848 }, { 0x110fc, 0x1239 }, { 0x110fe, 0x0000 },
    { 0x11100, 0x9714 }, { 0x11102, 0xb300 }, { 0x11104, 0x0200 },
    { 0x11106, 0x001f }, { 0x11108, 0x663a }, { 0x1110a, 0x5239 },
    { 0x1110c, 0x0000 }, { 0x1110e, 0x9713 }, { 0x11110, 0x602c },
    { 0x11112, 0x3038 }, { 0x11114, 0x7848 }, { 0x11116, 0x1239 },
    { 0x11118, 0x0000 }, { 0x1111a, 0x9714 }, { 0x1111c, 0xb300 },
    { 0x1111e, 0x0200 }, { 0x11120, 0x000f }, { 0x11122, 0x6620 },
    { 0x11124, 0x6018 }, { 0x11126, 0x1038 }, { 0x11128, 0x7992 },
    { 0x1112a, 0x4e75 }, { 0x1112c, 0x3038 }, { 0x1112e, 0x7848 },
    { 0x11130, 0x1239 }, { 0x11132, 0x0000 }, { 0x11134, 0x9714 },
    { 0x11136, 0xb300 }, { 0x11138, 0x0200 }, { 0x1113a, 0x0007 },
    { 0x1113c, 0x6606 }, { 0x1113e, 0x1038 }, { 0x11140, 0x7990 },
    { 0x11142, 0x4e75 }, { 0x11144, 0x7000 }, { 0x11146, 0x4e75 },
    { 0x11148, 0x4239 }, { 0x1114a, 0x0000 }, { 0x1114c, 0x9713 },
    { 0x1114e, 0x4e75 }, { 0x11150, 0x0c39 }, { 0x11152, 0x0001 },
    { 0x11154, 0x0000 }, { 0x11156, 0x9853 }, { 0x11158, 0x671a },
    { 0x1115a, 0x0c00 }, { 0x1115c, 0x0001 }, { 0x1115e, 0x672a },
    { 0x11160, 0x0c00 }, { 0x11162, 0x0002 }, { 0x11164, 0x6772 },
    { 0x11166, 0x0c00 }, { 0x11168, 0x0004 }, { 0x1116a, 0x6700 },
    { 0x1116c, 0x001e }, { 0x1116e, 0x0c00 }, { 0x11170, 0x0008 },
    { 0x11172, 0x6764 }, { 0x11174, 0x0c38 }, { 0x11176, 0x0010 },
    { 0x11178, 0x7991 }, { 0x1117a, 0x6700 }, { 0x1117c, 0x00c0 },
    { 0x1117e, 0x0c38 }, { 0x11180, 0x0020 }, { 0x11182, 0x7991 },
    { 0x11184, 0x6700 }, { 0x11186, 0x00c4 }, { 0x11188, 0x4e75 },
};

static int sig_ok(const idle_sig_t *sg, unsigned n) {
    const u16 *w = (const u16 *)Pico_mcd->prg_ram;
    for (unsigned i = 0; i < n; i++)
        if (w[sg[i].a >> 1] != sg[i].w)
            return 0;
    return 1;
}
#define SIG_OK(sg) sig_ok(sg, sizeof sg / sizeof sg[0])

uint32_t mcd_idle_parked;                     /* statistics */
int mcd_idle_on = 1;                          /* tests: 0 = never park */

void mcd_idle_s68k_read(u32 a) {
    const u8 *b = Pico_mcd->prg_ram;
    const u16 *w = (const u16 *)Pico_mcd->prg_ram;
    u32 pc;
    if (!mcd_idle_on || a != 0x2e || Pico_mcd->s68k_regs[0x2f])
        return;
    pc = SekPcS68k;
    if (pc >= 0x1681e && pc <= 0x16828) {
        /* loop 1 goes on: $16364 does nothing, a flag is still set */
        if (b[MEM_BE2(0x7f80f)] || !((b[MEM_BE2(0x1511e)] & 7) || b[MEM_BE2(0x9b44)]))
            return;
        if (!SIG_OK(sig1) || !SIG_OK(sig_sub))
            return;
    } else if (pc >= 0x166ba && pc <= 0x166c4) {
        /* loop 2 goes on: step 0, $78ad not negative, $16364 idle if called */
        int f = (signed char)b[MEM_BE2(0x78ad)];
        if (w[0x9808 >> 1] || f < 0 || (f == 0 && b[MEM_BE2(0x7f80f)]))
            return;
        if (!SIG_OK(sig2) || !SIG_OK(sig_sub))
            return;
    } else if (pc >= 0x11060 && pc <= 0x11066) {
        /* one pass of the step, as long as it changes nothing */
        u32 t = b[MEM_BE2(0x9713)], x = (w[0x7848 >> 1] ^ b[MEM_BE2(0x9714)]) & 0xff;
        if (b[MEM_BE2(0x79f4)] || w[0x79fe >> 1] || b[MEM_BE2(0x7991)])
            return;
        if (t == 0) {                   /* $9714 = timer, $7990 = 0: 0 */
            if (x || b[MEM_BE2(0x7990)])
                return;
        } else if (t == 1) {            /* repeat delay not over: 0 */
            if (!(x & 0x1f))
                return;
        } else {                        /* repeating: 0 or $7990 */
            if (!(x & 0x0f) && b[MEM_BE2(0x7990)])
                return;
        }
        if (!SIG_OK(sig3))
            return;
    } else
        return;
    Pico_mcd->m.state_flags |= PCD_ST_S68K_POLL;
    Pico_mcd->m.s68k_poll_a = 0xfffe;         /* no register write releases it */
    Pico_mcd->m.s68k_poll_cnt = 0;
    mcd_idle_parked++;
    SekEndRunS68k(8);
}

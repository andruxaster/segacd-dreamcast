/* Fast boot: the Mega CD BIOS runs at full speed behind a black screen.

   From power-on to the game there are ~620 frames (10 s) the player can
   only wait through: the BIOS logo animation, its title screen (Start)
   and the licence screen of the disc's security code. They cannot be
   skipped without the BIOS (it stays the game's sub CPU kernel), so they
   are emulated as fast as possible instead: nothing drawn, no graphics
   ASIC rendering (nothing shows its output), no FM / PSG / PCM / CD audio
   rendering (silent anyway), Start pressed every half second. Several
   frames run per Dreamcast frame; it ends at the first frame the main
   CPU runs from work RAM (the game's initial program), below $fffd00
   where the BIOS keeps its interrupt jump table. */
#include "pico_int.h"
#include "mcd_boot.h"

#define BOOT_OPTS (POPT_EN_MCD_GFX | POPT_EN_FM | POPT_EN_PSG | POPT_EN_MCD_PCM | POPT_EN_MCD_CDDA)
#define BOOT_MAX  3600                        /* give up after a minute */

static int active, frame;
static unsigned saved_opts;

void mcd_boot_begin(void) {
    saved_opts = PicoIn.opt & BOOT_OPTS;
    PicoIn.opt &= ~BOOT_OPTS;
    active = 1;
    frame = 0;
}

void mcd_boot_cancel(void) {
    if (!active) return;
    PicoIn.opt |= saved_opts;
    active = 0;
}

int mcd_boot_running(void) { return active; }

int mcd_boot_frame(void) {
    u32 pc;
    if (!active) return 0;
    PicoIn.pad[0] = (frame % 30) < 8 ? 1 << 7 : 0;    /* Start */
    PicoIn.skipFrame = 1;
    PicoFrame();
    frame++;
    pc = SekPc & 0xffffff;
    if ((pc >= 0xff0000 && pc < 0xfffd00) || frame >= BOOT_MAX)
        mcd_boot_cancel();
    return active;
}

int mcd_boot_frames(void) { return frame; }

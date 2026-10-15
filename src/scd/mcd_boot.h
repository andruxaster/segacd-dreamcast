/* Fast boot through the Mega CD BIOS (mcd_boot.c) */
#ifndef MCD_BOOT_H
#define MCD_BOOT_H
void mcd_boot_begin(void);      /* after PicoLoadMedia / PicoLoopPrepare */
void mcd_boot_cancel(void);     /* back to normal emulation (BIOS shown) */
int  mcd_boot_running(void);
int  mcd_boot_frame(void);      /* one emulated frame; 0 once the game runs */
int  mcd_boot_frames(void);     /* frames run so far */
#endif

/* md-dc: what PicoDrive references from the parts left out (32X, MP3 CD audio) */
#include "pico/pico_int.h"

void Pico32xStartup(void) {}
void Pico32xShutdown(void) {}
void Pico32xPrepare(void) {}
u32 PicoRead8_32x(u32 a) { (void)a; return 0; }
u32 PicoRead16_32x(u32 a) { (void)a; return 0; }
void PicoWrite8_32x(u32 a, u32 d) { (void)a; (void)d; }
void PicoWrite16_32x(u32 a, u32 d) { (void)a; (void)d; }
void PicoDrawSetOutFormat32x(pdso_t which, int use_32x_line_mode) { (void)which; (void)use_32x_line_mode; }
void PicoDrawSetOutBuf32X(void *dest, int increment) { (void)dest; (void)increment; }
void p32x_sync_sh2s(unsigned int m68k_target) { (void)m68k_target; }
#ifdef NO_SMS
void PicoPrepareMS(void) {}
#endif

/* Back to the anthology menu (anthology-dc), for the game programs.

   Call it when the player quits (Start+A+B+X+Y), after the sound stream
   and the PVR are shut down. When the program was started by the menu
   (its NGCD-DISC-DIR= folder is not the disc root) and /cd/MENU.BIN
   exists, it loads the menu, writes this game's folder into the menu's
   own marker (the menu puts its cursor there) and starts it; it does not
   return then. Otherwise it returns and the program exits as before.

   The marker text is copied from the program's disc_dir_patch, so the
   image holds no second copy of it for the menu to find by mistake.
   A copy of this file lives in each game project (src/dc/menu_return.c). */
#include <kos.h>
#include <stdlib.h>
#include <string.h>
#include "menu_return.h"

#define MARKER_LEN  14                     /* "NGCD-DISC-DIR=" */
#define MARKER_ROOM 50

extern volatile char disc_dir_patch[64];

void menu_return(const char *disc_dir) {
    if (!disc_dir || !strcmp(disc_dir, "/cd/") || strlen(disc_dir) >= MARKER_ROOM) return;
    char pat[MARKER_LEN];
    for (int k = 0; k < MARKER_LEN; k++) pat[k] = disc_dir_patch[k];

    file_t f = fs_open("/cd/MENU.BIN", O_RDONLY);
    if (f == FILEHND_INVALID) return;
    size_t len = fs_total(f);
    uint8_t *buf = len > MARKER_LEN + MARKER_ROOM ? malloc(len) : NULL;
    int ok = buf && fs_read(f, buf, len) == (ssize_t)len;
    fs_close(f);
    if (!ok) { free(buf); return; }

    uint8_t *m = NULL;
    for (size_t i = 0; i + MARKER_LEN + MARKER_ROOM <= len; i++)
        if (buf[i] == (uint8_t)pat[0] && !memcmp(buf + i, pat, MARKER_LEN)) { m = buf + i; break; }
    if (!m) { free(buf); return; }
    m += MARKER_LEN;
    memset(m, 0, MARKER_ROOM);
    memcpy(m, disc_dir, strlen(disc_dir));
    arch_exec(buf, len);                    /* does not return */
}

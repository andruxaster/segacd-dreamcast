/* Data track read in the background (cd_cache.c); the platform's
   cd_data_hook_* (cdd.c, CD_DATA_HOOK) call these */
#ifndef CD_CACHE_H
#define CD_CACHE_H
#include <stdint.h>
int  cd_cache_open(const char *path);         /* DATA.ISO; 0: not used */
int  cd_cache_active(void);
void cd_cache_seek(int lba);                  /* the drive seeks to sector lba */
int  cd_cache_ready(int lba);                 /* 1: sector lba can be read now */
int  cd_cache_read(unsigned int pos, uint8_t *dst);   /* 1: copied */
extern uint32_t cd_cache_waits, cd_cache_sync, cd_cache_loads;
#endif

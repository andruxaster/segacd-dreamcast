# PC test build of the Mega CD machine (headless, see hostscd.c):
#   make        -> scdh (FAME C 68000s, sub CPU idle hook, rectangles model)
P = ../src/pd
DEFS = -DEMU_F68K -D_USE_CZ80 -DNO_32X -DNO_SMS -DMD_MQ -DMQ_PVR_SIM -DMD_FASTFM -DMCD_IDLE_HOOK -DCD_DATA_HOOK \
       -D_FILE_OFFSET_BITS=64
CFLAGS = -O2 -g -w -I$(P) -I$(P)/pico -I$(P)/zlib -I../src/scd $(DEFS) $(XF)
SRC = $(addprefix $(P)/pico/, pico.c cart.c memory.c state.c sek.c z80if.c videoport.c draw2.c draw.c \
        mode4.c misc.c eeprom.c patch.c debug.c media.c) \
      $(wildcard $(P)/pico/cd/*.c) \
      $(addprefix $(P)/pico/pico/, pico.c memory.c xpcm.c) \
      $(addprefix $(P)/pico/carthw/, carthw.c eeprom_spi.c svp/svp.c svp/memory.c svp/ssp16.c) \
      $(addprefix $(P)/pico/sound/, sound.c resampler.c sn76496.c ym2612.c ym2413.c mix.c vgm.c) \
      $(P)/cpu/fame/famec.c $(P)/cpu/cz80/cz80.c $(P)/unzip/unzip.c $(P)/platform/common/upscale.c $(P)/md_stubs.c \
      ../src/mq/mq.c mq_raster.c ta_sim.c ../src/dc/mq_pvr.c \
      ../src/scd/mcd_idle.c ../src/scd/mcd_boot.c ../src/scd/nga_cdda.c ../src/scd/cd_cache.c hostscd.c
scdh: $(SRC) $(wildcard ../src/mq/*.h ../src/scd/*.h) mq_raster.h ta_sim.h ../src/dc/mq_pvr.h
	$(CC) $(CFLAGS) -o $@ $(SRC) -lz -lm
clean:
	rm -f scdh

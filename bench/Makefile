# SH4 build of the host front end (hostscd.c) for qemu-sh4: the FAME SH4
# assembler core for both 68000s, instruction counts per frame (MD_MARK)
P = ../src/pd
DEFS = -DEMU_F68K -D_USE_CZ80 -DNO_32X -DNO_SMS -DMD_MARK -DMD_MQ -DMQ_PVR_SIM -DMD_FASTFM \
       -DMCD_IDLE_HOOK -DCD_DATA_HOOK $(CPUDEF)
CFLAGS = -ml -m4 -O2 -U_FORTIFY_SOURCE -fno-stack-protector -fomit-frame-pointer -w \
         -I$(P) -I$(P)/pico -I$(P)/zlib -I../src/scd $(DEFS) $(XF)
ZL = $(addprefix $(P)/zlib/, inflate.c inftrees.c inffast.c adler32.c crc32.c zutil.c uncompr.c gzio.c deflate.c trees.c compress.c)
SRC = $(addprefix $(P)/pico/, pico.c cart.c memory.c state.c sek.c z80if.c videoport.c draw2.c draw.c \
        mode4.c misc.c eeprom.c patch.c debug.c media.c) \
      $(wildcard $(P)/pico/cd/*.c) \
      $(addprefix $(P)/pico/pico/, pico.c memory.c xpcm.c) \
      $(addprefix $(P)/pico/carthw/, carthw.c eeprom_spi.c svp/svp.c svp/memory.c svp/ssp16.c) \
      $(addprefix $(P)/pico/sound/, sound.c resampler.c sn76496.c ym2612.c ym2413.c mix.c vgm.c) \
      $(CPUSRC) $(P)/cpu/cz80/cz80.c $(P)/unzip/unzip.c $(P)/platform/common/upscale.c \
      $(P)/md_stubs.c $(ZL) ../src/mq/mq.c ../src/dc/mq_pvr.c ../host/mq_raster.c ../host/ta_sim.c \
      ../src/scd/mcd_idle.c ../src/scd/mcd_boot.c ../src/scd/nga_cdda.c ../host/hostscd.c
.DEFAULT_GOAL := scdsh4
# FAME=c: PicoDrive's FAME C instead of the FAME SH4 assembler core
ifeq ($(FAME),c)
CPUSRC = $(P)/cpu/fame/famec.c
else
CPUDEF = -DFAME_PICO -DFAME_LINUX_TEST
CPUSRC = ../src/fame/fame_pico.c fame_sh4.o
endif
fame_sh4.o: ../src/fame/fame.s
	sh4-linux-gnu-as --little -o $@ $<
scdsh4: $(SRC) $(wildcard ../src/mq/*.h ../src/dc/*.h ../host/*.h ../src/scd/*.h)
	sh4-linux-gnu-gcc $(CFLAGS) -static -Wl,-Ttext-segment=0x8c010000 -o $@ $(SRC) -lm
clean:
	rm -f scdsh4 fame_sh4.o

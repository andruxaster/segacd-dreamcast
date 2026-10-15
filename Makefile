# snatcher-dc: Snatcher (Sega CD) for the Dreamcast (KallistiOS), with
# PicoDrive's Mega CD core: both 68000s on the FAME SH4 assembler core
# (src/fame), Z80 on CZ80, the picture drawn by the PVR from tile
# rectangles (src/mq), the data track and the CD audio read by threads
# (src/scd), the BIOS run at full speed behind a black screen.
#   make               -> snatcher.elf (debug: KOS banner, perf log every 2 s)
#   make RELEASE=1     -> the same without any console output
#   make SOFT=1        -> picture drawn by PicoDrive's line renderer only
# Run `make clean` when switching between RELEASE / SOFT and the default.
# The PC test build lives in host/, the SH4 bench in bench/ (see README),
# the disc converter in tools/ngcdconv.

TARGETS = snatcher.elf

P = src/pd
CORE_SRC = $(addprefix $(P)/pico/, pico.c cart.c memory.c state.c sek.c z80if.c videoport.c draw2.c draw.c \
             mode4.c misc.c eeprom.c patch.c debug.c media.c) \
           $(wildcard $(P)/pico/cd/*.c) \
           $(addprefix $(P)/pico/pico/, pico.c memory.c xpcm.c) \
           $(addprefix $(P)/pico/carthw/, carthw.c eeprom_spi.c svp/svp.c svp/memory.c svp/ssp16.c) \
           $(addprefix $(P)/pico/sound/, sound.c resampler.c sn76496.c ym2612.c ym2413.c mix.c vgm.c) \
           $(P)/cpu/cz80/cz80.c $(P)/unzip/unzip.c $(P)/platform/common/upscale.c \
           $(P)/md_stubs.c \
           $(addprefix $(P)/zlib/, inflate.c inftrees.c inffast.c adler32.c crc32.c zutil.c uncompr.c gzio.c \
             deflate.c trees.c compress.c)
FE_SRC   = src/mq/mq.c src/dc/mq_pvr.c src/dc/menu_return.c \
           src/scd/cd_cache.c src/scd/mcd_idle.c src/scd/mcd_boot.c src/scd/nga_cdda.c src/scd/snd_mix.c \
           src/fame/fame_pico.c
OBJS     = $(patsubst %.c,%.o,$(CORE_SRC) $(FE_SRC)) src/fame/fame.o

# NO_32X / NO_SMS: no 32X, no Master System; MD_MQ: the rectangle hooks
# (src/mq); MD_FASTFM: the faster YM2612 loops (same output); FAME_PICO:
# page-table hooks for the assembler core; MCD_IDLE_HOOK: the sub CPU's
# idle loops (mcd_idle.c); CD_DATA_HOOK: the data track from cd_cache.c
DEFS  = -DEMU_F68K -D_USE_CZ80 -DNO_32X -DNO_SMS -DMD_MQ -DMD_FASTFM -DFAME_PICO -DMCD_IDLE_HOOK -DCD_DATA_HOOK
INC   = -I$(P) -I$(P)/pico -I$(P)/zlib -Isrc/dc -Isrc/scd
# gnu11: the bundled zlib has K&R definitions, which C23 (GCC 15's default) rejects
CORE_FLAGS = -std=gnu11 -O2 -fomit-frame-pointer -fno-strict-aliasing -w $(INC) $(DEFS)

ifeq ($(RELEASE),1)
FE_DEFS = -DSCD_RELEASE -DNDEBUG
endif
ifeq ($(SOFT),1)
FE_DEFS += -DSCD_SOFT
endif

all: $(TARGETS)

include $(KOS_BASE)/Makefile.rules

$(P)/%.o: $(P)/%.c
	kos-cc $(CORE_FLAGS) -c $< -o $@

# FAME SH4 68000 core (MAME4ALL DC, page-table version from dc-poker)
src/fame/fame.o: src/fame/fame.s
	$(KOS_AS) --little -o $@ $<

src/fame/%.o: src/fame/%.c
	kos-cc $(CORE_FLAGS) -c $< -o $@

src/mq/%.o: src/mq/%.c src/mq/mq.h
	kos-cc $(CORE_FLAGS) -c $< -o $@

src/scd/%.o: src/scd/%.c $(wildcard src/scd/*.h)
	kos-cc $(CORE_FLAGS) -c $< -o $@

src/dc/%.o: src/dc/%.c src/mq/mq.h src/dc/mq_pvr.h
	kos-cc $(CORE_FLAGS) $(FE_DEFS) -c $< -o $@

snatcher.elf: $(OBJS) src/dc/scd_dc.o
	kos-cc -o $@ $(OBJS) src/dc/scd_dc.o -lm

clean:
	-rm -f $(TARGETS) $(OBJS) src/dc/scd_dc.o

.PHONY: all clean

# snatcher-dc changes

## r3

- The BIOS (logo, title screen, CD player, memory manager) is shown by
  default; B held at power-on: the fast boot of r1/r2.
- Graphics ASIC (pico/cd/gfx.c): new renderer with offset tables, 8-bit
  fixed point and a fast path for unrotated rows; 2.5x faster, identical to
  the old one (kept as -DGFX_REF) over the BIOS and 60k game frames. BIOS
  logo frame on qemu-sh4: 5.3M -> 4.2M instructions.
- Sound: elastic mode until the game's own program runs (pitch follows
  the machine down to ~85 % speed, integral control); then the exact
  rate as before.
- bench/Makefile: cd_cache.c linked.

## r2

- Sound: ~70 ms cushion in the output ring (was ~23 ms), built before the
  sound starts and again after an underrun: frames that come late (scene
  changes, VMU writes) no longer crackle. Model with 3 % of the frames
  12 ms late: 0 underruns (r1: 492 per minute).
- GCC 15: PicoPrepareMS / p32x_sync_sh2s declared without SMS / 32X.

## r1

- New project from dc-poker's snatcher.elf and md-dc: PicoDrive 8b5e91b with
  md-dc's hooks (tile rectangles, FAME SH4, fast YM2612) and dc-poker's
  Mega CD changes (NGA tracks, sub CPU idle loops, PRG RAM protection).
- Disc: the data track is read ahead by a thread from the drive's seek
  command on (cd_cache.c); a sector not read yet holds the emulated drive
  for a tick (cdd.c) instead of the emulation waiting on the GD-ROM.
- Picture by the PVR from tile rectangles (0 differences against the
  software renderer); sound 22 kHz + CD audio 44.1 kHz.
- Folder marker and menu_return for anthology-dc.
- host/: PC build with a random player and hang detection, slow-drive
  simulation, cache check; bench/: qemu-sh4 build with FAME SH4.

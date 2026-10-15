# snatcher-dc changes

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

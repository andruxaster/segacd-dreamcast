# segacd-dc changes

## r6

- A plain Sega CD emulator: the anthology-dc parts are gone (the folder
  marker the menu patched, the return to the menu on quit). Every file is
  read from the disc root; Start+A+B+X+Y quits to the Dreamcast.

## r5

- The project is now segacd-dc (repository segacd-dreamcast): the program
  is `segacd.elf`, the VMU file keeps its name (`SCDBRAM`, old saves stay).
  README.md rewritten in English as a Sega CD emulator with Snatcher as
  its first game.

## r4

- BIOS by the disc's region: `BIOS_J.BIN` / `BIOS_U.BIN` / `BIOS_E.BIN`
  (game folder, then disc root), else `BIOS.BIN`. A BIOS whose region does
  not match the disc is reported on screen (it would only reach its CD
  player after a minute of black fast boot).
- Other Sega CD games: Captain Tsubasa (Japan) with the Mega-CD 1.01 J BIOS
  boots and plays in the PC build and the SH4 code under qemu.
- PC build: warns about a BIOS / disc region mismatch. bench/Makefile:
  cd_cache.c linked (lost with the r3 revert).

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

# snatcher-dc

Snatcher (Sega CD, USA) on the Dreamcast (KallistiOS 2.3): PicoDrive's Mega
CD core with the real BIOS, both 68000s on the FAME SH4 assembler core, the
picture drawn by the PVR from tile rectangles (as md-dc). The data track is
read by a thread that starts when the emulated drive is told to seek; a
sector that has not arrived holds the emulated drive, never the machine.
CD audio (IMA ADPCM, tools/ngcdconv) comes from a second thread.
`make` / `make RELEASE=1`. Details in Russian: README.ru.md. Licence:
PicoDrive's (non-commercial, with sources; src/pd/COPYING).

## Files

`tools/ngcdconv` turns a cue/bin image into `DISC.CUE`, `DATA.ISO` and
`TRKnn.NGA`; with them, in the disc root or a game folder of the
anthology menu, goes the BIOS:

- `BIOS_J.BIN` (Mega-CD, Japanese discs: `bios_CD_J.bin`), `BIOS_U.BIN`
  (Sega CD, US discs: `bios_CD_U.bin`) or `BIOS_E.BIN` (European discs),
  chosen by the region in the disc's header; `BIOS.BIN` if that one is
  missing. The game's folder is looked at first, then the disc root.
- A BIOS only boots discs of its own region (the security code at the start
  of the disc differs); a Japanese disc with the US BIOS ends in the BIOS's
  CD player. When the BIOS's region letter does not match the disc, the
  screen says which BIOS is needed.

## Other Sega CD games

Nothing here is tied to Snatcher except the sub CPU wait-loop parking
(`src/scd/mcd_idle.c`, which simply does not trigger in other games).
Tried in the PC build and the SH4 code under qemu (not yet on a console):

| Game | BIOS | Result |
|---|---|---|
| Captain Tsubasa (Japan) | Mega-CD 1.01 J | boots in ~8 s; menus, team select, matches, cut scenes; random input, 2 x 60k frames, no hang. A match frame costs ~3.1M SH4 instructions (~15 ms of 16.7, 1M of it FM music), so frames may be skipped now and then |

## Controls

| Dreamcast | Sega CD |
|---|---|
| A / B / X, Y | C / B / A |
| Start | Start |
| D-pad, stick | D-pad |
| B held at power-on | the BIOS as it is (CD player, memory manager) |
| Start+A+B+X+Y | quit (to the anthology menu) |

Saves (the 8 KB Sega CD backup RAM) go to the first VMU, file `SCDBRAM`.

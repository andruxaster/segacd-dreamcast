# segacd-dc — Sega CD / Mega-CD emulator for the Dreamcast

[Русская версия](README.ru.md)

A Sega CD (Mega-CD) emulator for the Sega Dreamcast, built with KallistiOS
(tested with 2.3.0 and GCC 15.2). The core is PicoDrive's Mega Drive / Mega
CD emulator (irixxxx/picodrive) running the real Sega CD BIOS. Both 68000s
run on the FAME SH4 assembler core, the Z80 on CZ80, and the picture is
drawn by the Dreamcast's PVR from tile rectangles (as in md-dc).

The project started as a single-game port of **Snatcher (USA)** and is
tuned for it, but nothing ties it to that game: any Sega CD disc converted
with `tools/ngcdconv` can be started with the BIOS of the disc's region.

BIOS files and games are not included.

## Status

| Game | BIOS | Status |
|---|---|---|
| Snatcher (USA) | Sega CD 2.11 U | runs on a Dreamcast: 4–6 ms a frame, 9–12 ms in scenes with FM music; no stalls on disc access |
| Captain Tsubasa (Japan) | Mega-CD 1.01 J | boots and plays in the PC build and the SH4 code under qemu (menus, team select, matches, cut scenes); not yet tried on a Dreamcast. A match frame costs ~3.1M SH4 instructions (~15 ms of 16.7), so frames may be skipped now and then |

## Features

- Real Sega CD BIOS, chosen by the region of the disc.
- **No stalls on disc access.** The data track is read by a background
  thread that starts as soon as the emulated drive is told to seek; a
  sector that has not arrived yet holds the emulated drive for a tick, as a
  slower drive would, never the machine: CPUs, picture, sound and music
  keep running. CD audio tracks (IMA ADPCM `.NGA` files) come from a second
  thread through a 1.3 s ring.
- PVR rendering: the VDP's planes and sprites become textured rectangles
  (lines the PVR cannot do, such as shadow / highlight or interlace, fall
  back to PicoDrive's line renderer).
- Sound: PicoDrive at 22050 Hz (FM, PSG, PCM) plus the CD audio at
  44.1 kHz, mixed with a ~70 ms cushion.
- Fast boot: the BIOS logo, title screen and licence screen run at full
  speed behind a black screen (~8–10 s instead of a minute of waiting).
- Saves: the 8 KB Sega CD backup RAM goes to the first VMU (`SCDBRAM`),
  ~2 s after it changes; shared by all games, as on the console.

## Building

Requires KallistiOS with its environment set up (`environ.sh`).

    make            # segacd.elf, debug: KOS banner, perf log every 2 s
    make RELEASE=1  # no console output
    make SOFT=1     # picture by PicoDrive's line renderer only (for comparison)

Run `make clean` when switching between RELEASE / SOFT and the default.
Then `segacd.elf` → scramble → `1ST_READ.BIN`.

## Preparing a disc

A Sega CD image (CUE + BIN: a MODE1/2352 data track and CD audio tracks) is
converted by `tools/ngcdconv`:

    cc -O2 -o ngcdconv tools/ngcdconv/ngcdconv.c
    ./ngcdconv "Snatcher (USA).cue" out

| File (disc root) | From | Size |
|---|---|---|
| `1ST_READ.BIN` | `segacd.elf`, scrambled | |
| `BIOS_U.BIN` / `BIOS_J.BIN` / `BIOS_E.BIN`, or `BIOS.BIN` | the Sega CD / Mega-CD BIOS of the disc's region | 128 KB |
| `DISC.CUE`, `DATA.ISO`, `TRKnn.NGA` | `ngcdconv` | Snatcher: 115 + 112 MB |

### BIOS and the disc's region

A BIOS only boots discs of its own region: the security code at the start
of the disc differs between Japan, the USA and Europe, and a Japanese disc
with the US BIOS ends in the BIOS's CD player. The emulator reads the
region from the disc's header and loads `BIOS_J.BIN` (Mega-CD, e.g.
`bios_CD_J.bin`), `BIOS_U.BIN` (Sega CD, `bios_CD_U.bin`) or `BIOS_E.BIN`
(Mega-CD, `bios_CD_E.bin`); if that file is missing, `BIOS.BIN`. When
the BIOS's own region letter does not match the disc, the screen says
which BIOS is needed.

## Controls

| Dreamcast | Sega CD | Snatcher |
|---|---|---|
| A | C | choose |
| B | B | back |
| X, Y | A | blaster |
| Start | Start | pause / menu |
| D-pad, analog stick | D-pad | |
| B held at power-on | | the BIOS as it is (CD player, memory manager) |
| Start+A+B+X+Y | | quit |

## How it was tested (without a Dreamcast)

- `host/` — PC build (FAME C), `bench/` — an SH4 build with the same FAME
  SH4 assembler core as on the Dreamcast, run under qemu-sh4.
- Snatcher with random input (`SC_RANDOM`) and a hang detector (picture
  unchanged for 2 minutes while buttons are pressed): ~3.5M frames (~16
  hours of play) in the PC build and 0.5M on FAME SH4, no hang. The random
  player gets through part of the first act.
- A slow drive (`SC_SLOWIO=40 SC_SLOWRND=30`: +0.5 s per seek, 30 % of the
  sectors late): the game goes on, only loading takes longer.
- Through the cache (`SC_CACHE=1`, as on the Dreamcast): every sector
  matched the file, no read from the emulation thread.
- Rectangles against the software renderer: no difference over 60k frames.
- Speed under qemu-sh4 (Snatcher, game frames, sound at 22 kHz, picture as
  rectangles): 0.94M instructions on average (~5 ms of 16.7), 99 % of the
  frames at most 1.8M.

First run on a console (r1 log): the data track never read from the
emulation thread, frames of 4–6 ms (9–12 ms with FM music), 118–120 of 120
frames shown.

## Development

    cd host && make
    SC_BOOT=1 SC_FRAMES=60000 SC_RANDOM=1 SC_CACHE=1 ./scdh BIOS.BIN out/DISC.CUE

The environment variables are listed at the top of `host/hostscd.c`
(`SC_PPM`, `SC_WAV`, `SC_SCRIPT`, `SC_HW`/`SC_VERIFY`, `SC_SAVE`/`SC_LOAD`,
`SC_TRACE`, `SC_IOLOG`, `SC_NOIDLE`, `SC_SLOWIO`, `SC_SLOWRND`). The PC
build warns when the BIOS and the disc are of different regions. `bench/`:
`qemu-sh4 -plugin mark.so bench/scdsh4 BIOS.BIN out/DISC.CUE`.

## Source layout

```
src/pd/     PicoDrive 8b5e91b with the changes below
src/dc/     Dreamcast front end (scd_dc.c: files, BIOS choice, video, pads,
            VMU, threads), PVR rectangles (mq_pvr.c)
src/scd/    cd_cache.c (data track reader), nga_cdda.c (CD audio),
            snd_mix.c (sound mixing), mcd_boot.c (fast boot),
            mcd_idle.c (Snatcher's sub CPU wait loops)
src/mq/     VDP lines to tile rectangles
src/fame/   FAME SH4 68000 core (fame.s) and its PicoDrive glue
host/       PC build; bench/ SH4 build for qemu-sh4
tools/      ngcdconv
```

## Changes to PicoDrive

Base: irixxxx/picodrive 8b5e91b (as in dc-poker), plus:
- md-dc's hooks: tile rectangles (`MD_MQ`), FAME SH4 (`FAME_PICO`), fast
  YM2612 (`MD_FASTFM`);
- from dc-poker: NGA tracks in the cue, `CD_DATA_HOOK`, `MCD_IDLE_HOOK`,
  the PRG RAM protection boundary for FAME SH4, the graphics ASIC without
  re-reading its state;
- new: the drive waits for a sector (`cd_data_hook_ready`) and reports
  seeks (`cd_data_hook_seek`).

## Known limitations

- The sub CPU wait-loop parking (`mcd_idle.c`) only knows Snatcher's
  loops; other games run their wait loops in full (it costs speed, not
  correctness).
- The BIOS logo shown as it is (B at power-on) runs below full speed: the
  graphics ASIC's rotation is expensive on the SH4.
- Only games tried are listed in [Status](#status); other Sega CD games may
  need more speed than the Dreamcast has.

## Licence

PicoDrive's licence (non-commercial, with sources; `src/pd/COPYING`); this
build is distributed on the same terms. FAME SH4 from the MAME4ALL
Dreamcast sources. See [CHANGELOG.md](CHANGELOG.md) for the history.

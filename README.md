# snatcher-dc

Snatcher (Sega CD, USA) on the Dreamcast (KallistiOS 2.3): PicoDrive's Mega
CD core with the real BIOS, both 68000s on the FAME SH4 assembler core, the
picture drawn by the PVR from tile rectangles (as md-dc). The data track is
read by a thread that starts when the emulated drive is told to seek; a
sector that has not arrived holds the emulated drive, never the machine.
CD audio (IMA ADPCM, tools/ngcdconv) comes from a second thread.
`make` / `make RELEASE=1`. Details in Russian: README.ru.md. Licence:
PicoDrive's (non-commercial, with sources; src/pd/COPYING).

# tools/ngcdconv

Converts the cue/bin image into the disc layout of snatcher-dc:

    cc -O2 -o ngcdconv ngcdconv/ngcdconv.c
    ngcdconv "Snatcher (USA).cue" out

`out/` gets `DATA.ISO`, `TRK02.NGA` … `TRK21.NGA` and `DISC.CUE`
(`TOC.BIN` is for the Neo Geo CD emulator, not needed here).
The same tool as in dc-poker / ngcd-dc: an already converted disc works as is.

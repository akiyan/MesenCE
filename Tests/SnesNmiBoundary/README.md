# SNES: where the VBlank NMI is taken around a read-modify-write

A title-independent test ROM, offered as a reference. **The generator and the script were written with an AI assistant (Claude)**, so this is not meant to be merged as is.

## Why

In Kamaitachi no Yoru (SFC), a scheduler updates a signal word without holding off the NMI that sets a bit in it:

```
$00:8213  STA $22,X
$00:8215  LDA $32,X     ; read the signal word
$00:8217  AND #$FFFC
$00:821A  STA $32,X     ; write it back
```

When the NMI is taken after the read and before the write, the bit is lost and the scenario stops (the staff roll of one ending loses its last three pictures). On MesenCE this happens or not depending on the input timing and on 4 master clocks between two NMIs. Video: https://youtu.be/T-8i9yBZ-nw

## What the ROM does

`nmi_boundary_rom.py` generates a 32 KiB LoROM. For the frames of each field ($213F bit 7) it syncs to the NMI with WAI, burns about a frame, then runs the four instructions above at 96 start positions 2 master clocks apart (pads of `LDY #0` = 16 clocks and `NOP` = 14 clocks). The NMI handler keeps its return address and sets bit 3 of the word. The table is stored at $1000 and shown on screen.

## Running it

```sh
python3 Tests/SnesNmiBoundary/nmi_boundary_rom.py nmi-boundary.sfc
Mesen --testrunner nmi-boundary.sfc Tests/SnesNmiBoundary/nmi_boundary.lua    # stop code 0: the table below
```

`nmi_boundary.lua` checks that the bit is lost exactly where the NMI was taken after the LDA or the AND, and that the table is MesenCE's (2.2.1 and master a8e5d023 give the same):

```
field 0 AAAA99999999999999999997777777777774444444444444444444222222222222222222000000000000------------
field 1 AAAAA9A99999999999999999997777777777774444444444444444444222222222222222222000000000000---------
```

`-` before the sequence, `0` before its first STA, `2` after it, `4` after the LDA, `7` after the AND, `9` after the last STA, `A`-`F` in the NOPs after it. The screen shows the same table for each field, followed by the bit (1 kept, 0 lost).

Investigation notes: https://github.com/akiyan/MesenCE-ops/tree/main/docs/investigations/2026-11-26-nmi-lost-wakeup

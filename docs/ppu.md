# SNES PPU observation

The native headless runner can call the SNES PPU observation functions from a
Lua probe. Use the functions during an emulation callback or while the machine
is held.

## Snapshots

Call `emu.enableSnesFrameCapture(true)` before running the frames you want to
inspect. It keeps the latest completed, rendered PPU image. Disable capture with
`emu.enableSnesFrameCapture(false)` to discard the stored image.

`emu.getSnesPpuSnapshot([wramOffset, wramLength])` returns a Lua table.
WRAM offsets are relative to the start of WRAM. Both arguments default to
zero. The maximum length is 4,096 bytes.

| Field | Value |
| --- | --- |
| `live` | Timing of this observation and a `boundary` label |
| `cpu` | 65816 registers, 24-bit PC, CPU cycle count and emulation-mode flag |
| `ppu` | Mode, brightness, display flags, layer settings and decoded CGRAM palette |
| `wram` | Offset and the requested array of bytes |
| `completedFrame` | Timing, dimensions and a binary PNG string for the most recent completed image, when available |

The live boundary can be `debugger` (execution held), `frameStart`,
`vblankStart`, or `emulation`. Each timing object contains `frame`,
`masterClock`, `scanline` and `cycle`. The `cycle` is the master-clock
offset within the scanline. The `ppu.dot` uses the PPU's dot counter.

The completed image uses the native PPU dimensions and excludes host display
filters. The PPU publishes it after the end-frame callback, so a snapshot in
that callback can contain the preceding image. A debugger stop during drawing
keeps the previously completed image. Use the image's own timestamp when
aligning it with live observations. Reset and state restoration clear the
completed image until the next rendered frame.

Layer addresses are VRAM word addresses. The Lua palette array starts at one,
corresponding to CGRAM color zero. Each value is a BGR555 word with red in the
lowest five bits.

## Raster trace

`emu.startSnesPpuTrace(capacity)` starts a fresh trace. The capacity is
1–65,536 events. `emu.stopSnesPpuTrace()` returns `events` in recording
order and `dropped`, the number of later events beyond capacity.
Reset and state restoration clear the captured events and drop count while
preserving the configured capacity.

Each event includes the four timing fields used by snapshots.

| `kind` | Meaning and extra fields |
| --- | --- |
| `ppu` | PPU register bus access at `$2100–$213F`, with `access`, `address` and `value` |
| `dma` | DMA bus access, with `access`, `address`, `value` and zero-based `channel` |
| `hdma` | HDMA bus access including table reads, with the same fields as DMA |
| `nmi`, `irq` | CPU interrupt dispatch |

Addresses are the bus addresses reported by the debugger. DMA to a PPU
register appears as a DMA or HDMA event. A write is timestamped when the
debugger observes the bus operation.

## Verification

```sh
make USE_GCC=true LTO=false headless
python3 tests/headless_ppu.py
```

The generated LoROM exercises CPU writes to CGRAM, DMA, HDMA, IRQ and NMI. Two
independent profiles must produce the same decoded palette, event order,
completed PNG and debugger timestamps. The fixture also verifies that a
one-event trace reports later events as dropped and that a Lua state load
clears the preceding frame and trace.

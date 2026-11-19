#!/usr/bin/env python3
"""Exercise the SNES PPU observation API with a generated LoROM."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile

from headless_snes import expect_exit, make_rom, run


ROOT = Path(__file__).resolve().parents[1]


def make_raster_rom(path):
    make_rom(path)
    rom = bytearray(path.read_bytes())
    code = bytes.fromhex(
        "78 d8 64 10 64 11 "  # SEI, CLD, zero NMI and IRQ counters.
        "a9 80 8d 00 21 "  # Force blank while populating CGRAM.
        "9c 21 21 a9 1f 8d 22 21 9c 22 21 9c 21 21 "  # Write red, then rewind CGRAM address.
        "9c 00 43 a9 22 8d 01 43 "  # DMA channel 0, mode 0, destination $2122.
        "9c 02 43 a9 90 8d 03 43 9c 04 43 a9 02 8d 05 43 9c 06 43 "
        "a9 01 8d 0b 42 "  # Transfer two palette bytes from $009000.
        "9c 10 43 9c 11 43 a9 04 8d 12 43 a9 90 8d 13 43 9c 14 43 "
        "a9 02 8d 0c 42 "  # HDMA channel 1 writes $2100 from a table at $009004.
        "a9 0f 8d 00 21 "  # Turn on display.
        "a9 40 8d 09 42 9c 0a 42 a9 a0 8d 00 42 "  # V-IRQ at scanline 64 and NMI.
        "58 cb 80 fd"  # CLI, WAI, loop.
    )
    rom[:len(code)] = code
    rom[0x100:0x103] = bytes.fromhex("e6 10 40")  # Count NMI.
    rom[0x110:0x116] = bytes.fromhex("ad 11 42 e6 11 40")  # Acknowledge and count IRQ.
    rom[0x1000:0x1007] = bytes.fromhex("e0 03 00 00 02 0a 00")
    struct.pack_into("<HHH", rom, 0x7FFA, 0x8100, 0x8000, 0x8110)
    rom[0x7FC0:0x7FD5] = b"HEADLESS RASTER".ljust(21)
    rom[0x7FDC:0x7FE0] = bytes(4)
    checksum = (sum(rom) + 0x1FE) & 0xFFFF
    struct.pack_into("<HH", rom, 0x7FDC, checksum ^ 0xFFFF, checksum)
    path.write_bytes(rom)


def check_probe(directory):
    output = json.loads((directory / "raster.json").read_text())
    png = (directory / "completed.png").read_bytes()
    if png[:16] != b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR":
        raise SystemExit("completed frame was not encoded as PNG")
    if struct.unpack_from(">II", png, 16) != (output["width"], output["height"]):
        raise SystemExit("PNG dimensions did not match the completed frame metadata")
    if not 0 < output["completedClock"] < output["debuggerClock"]:
        raise SystemExit(f"PNG timestamp did not precede the live debugger position: {output}")
    if output["palette0"] != 0x03E0 or output["events"] <= 0 or output["dropped"] <= 0:
        raise SystemExit(f"unexpected PPU observation: {output}")
    timeline = (directory / "events.tsv").read_bytes()
    return output, hashlib.sha256(png).hexdigest(), hashlib.sha256(timeline).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "bin" / "mesen-headless")
    parser.add_argument("--work-dir", type=Path, default=ROOT / "bin" / "test-work")
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error(f"runner is not executable: {binary}")
    parent = args.work_dir.resolve()
    parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="mesen-raster-test-", dir=parent) as temporary:
        root = Path(temporary)
        rom = root / "raster.sfc"
        make_raster_rom(rom)
        probe = ROOT / "tests" / "headless_ppu.lua"
        results = []
        for repeat in range(2):
            trial = root / f"repeat-{repeat}"
            trial.mkdir()
            expect_exit(run(binary, rom, probe, trial / "profile", trial, 5), 0, "raster probe")
            results.append(check_probe(trial))
        if results[0] != results[1]:
            raise SystemExit(f"repeated raster observations disagreed: {results}")
        evidence, png_digest, trace_digest = results[0]
        print(json.dumps({"raster": evidence, "png_sha256": png_digest,
                          "trace_sha256": trace_digest, "repeats": len(results)}))


if __name__ == "__main__":
    main()

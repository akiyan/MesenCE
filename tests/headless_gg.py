#!/usr/bin/env python3
"""Exercise the native headless runner with a generated Game Gear ROM."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib


ROOT = Path(__file__).resolve().parents[1]

# Mode 4 with the display off, name table $3800, sprite table $3F00, no line interrupt.
REGISTERS = bytes.fromhex("0680 8081 ff82 ff83 ff84 ff85 fb86 0087 0088 0089 ff8a")


def make_rom(path):
    """Generate a 32 KiB Game Gear ROM.

    Each frame it waits for VBlank, counts the frame at $C010, records the pad
    port ($DC) and the Start port ($00) at $C020 and $C040 + (count & 15), and
    sets the backdrop (color 0) to red on even counts and blue on odd ones.
    """
    rom = bytearray(32768)
    rom[0x0000:0x0009] = bytes.fromhex("f3 ed56 31f0df c30001")      # DI, IM 1, SP=$DFF0, JP $0100
    rom[0x0066:0x0068] = bytes.fromhex("ed45")                         # RETN
    code = bytes.fromhex(
        "210002 0616 0ebf edb3 "          # Write the VDP registers from $0200.
        "af d3bf 3e40 d3bf 010040 "       # VRAM address $0000 for writing, 16 KiB.
        "af d3be 0b 78 b1 20f8 "          # Clear VRAM.
        "3ec0 d3bf 3e81 d3bf "            # Display on.
        "dbbf e680 28fa "                 # Wait for the VBlank flag.
        "2110c0 34 7e e60f 5f 1600 "      # Count the frame; DE = count & 15.
        "2120c0 19 dbdc 77 "              # Record the pad port.
        "2140c0 19 db00 77 "              # Record the Start port.
        "af d3bf 3ec0 d3bf "              # CRAM address 0.
        "3a10c0 e601 280a "               # Even count: red.
        "af d3be 3e0f d3be c32301 "       # Blue ($0F00), next frame.
        "3e0f d3be af d3be c32301"        # Red ($000F), next frame.
    )
    rom[0x0100:0x0100 + len(code)] = code
    rom[0x0200:0x0200 + len(REGISTERS)] = REGISTERS
    rom[0x7FF0:0x7FF8] = b"TMR SEGA"
    struct.pack_into("<H", rom, 0x7FFA, sum(rom[:0x7FF0]) & 0xFFFF)
    rom[0x7FFF] = 0x6C                                                 # Game Gear export, 32 KiB.
    path.write_bytes(rom)


def run(binary, rom, script, home, evidence_dir, timeout):
    env = os.environ.copy()
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    env["MESEN_HEADLESS_TEST_EVIDENCE"] = str(evidence_dir)
    return subprocess.run(
        [str(binary), "--home", str(home), "--timeout", str(timeout), str(rom), str(script)],
        env=env, cwd=evidence_dir, capture_output=True, text=True, timeout=timeout + 10,
    )


def expect_exit(result, code, case):
    if result.returncode != code:
        raise SystemExit(f"{case} exited with {result.returncode}, expected {code}\n"
                         f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")


def png_pixels(data):
    """Width, height and the set of RGB colors of an 8-bit RGB or RGBA PNG."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("not a PNG screenshot")
    offset, idat, header = 8, b"", None
    while offset < len(data):
        length, kind = struct.unpack_from(">I4s", data, offset)
        body = data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        offset += 12 + length
    width, height, depth, color = header[:4]
    if depth != 8 or color not in (2, 6):
        raise SystemExit(f"unexpected PNG format: depth {depth}, color type {color}")
    channels = 3 if color == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels
    previous = bytearray(stride)
    colors = set()
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 255
            elif kind == 2:
                line[i] = (line[i] + up) & 255
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                line[i] = (line[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 255
        colors |= {bytes(line[x * channels:x * channels + 3]) for x in range(width)}
        previous = line
    return width, height, colors


def check_probe(directory):
    evidence = json.loads((directory / "observation.json").read_text())
    # Clearing VRAM takes the first 10 frames; samples 1-12 follow, one per frame.
    # Button 1 is pressed in frame 15 (sample 6) and Start in frame 17 (sample 8).
    pads, starts = evidence["pads"][1:13], evidence["starts"][1:13]
    if (
        evidence["count"] != 12
        or evidence["zero"] != [0] * 16
        or pads != [0xFF] * 5 + [0xEF] + [0xFF] * 6
        or [i + 1 for i, v in enumerate(starts) if v & 0x80 == 0] != [8]
    ):
        raise SystemExit(f"unexpected ROM report: {evidence}")
    shots = []
    for name in ("first", "second"):
        png = (directory / f"{name}.png").read_bytes()
        width, height, colors = png_pixels(png)
        if (width, height) != (160, 144) or len(colors) != 1:
            raise SystemExit(f"{name} screenshot: {width}x{height} with {len(colors)} colors")
        shots.append((colors.pop(), hashlib.sha256(png).hexdigest()))
    # Consecutive frames show one backdrop each: red, then blue (no blending).
    (first, _), (second, _) = shots
    if first != b"\xff\x00\x00" or second != b"\x00\x00\xff":
        raise SystemExit(f"blended or wrong frame colors: {first.hex()} {second.hex()}")
    return evidence, [digest for _, digest in shots]


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

    with tempfile.TemporaryDirectory(prefix="mesen-headless-gg-", dir=parent) as temporary:
        root = Path(temporary)
        rom = root / "generated.gg"
        make_rom(rom)
        probe = ROOT / "tests" / "headless_gg.lua"
        observations = []
        for repeat in range(2):
            trial = root / f"repeat-{repeat}"
            trial.mkdir()
            expect_exit(run(binary, rom, probe, trial / "profile", trial, 10), 0, "Game Gear probe")
            observations.append(check_probe(trial))
        if observations[0] != observations[1]:
            raise SystemExit(f"repeated runs disagreed: {observations}")
        evidence, screenshots = observations[0]
        print(json.dumps({"game_gear": evidence, "png_sha256": screenshots, "repeats": 2}))


if __name__ == "__main__":
    main()

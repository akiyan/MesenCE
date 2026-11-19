#!/usr/bin/env python3
"""Verify raw screen bytes with generated SNES and Game Gear ROMs."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import tempfile

import headless_gg


ROOT = Path(__file__).resolve().parents[1]


def make_snes_rom(path):
    """Generate a LoROM that sets another backdrop color in each VBlank."""
    rom = bytearray(32768)
    code = bytes.fromhex(
        "78 d8 a9 80 8d 00 21 a2 00 "                # Display off; X = color.
        "9c 21 21 9c 22 21 9c 22 21 "                # Backdrop black.
        "a9 0f 8d 00 21 "                            # Display on.
        "ad 12 42 30 fb "                            # Wait for the end of VBlank.
        "ad 12 42 10 fb "                            # Wait for VBlank.
        "e8 9c 21 21 8e 22 21 9c 22 21 80 ea"        # Next color.
    )
    rom[:len(code)] = code
    rom[0x7FC0:0x7FD5] = b"HEADLESS VIDEO TEST".ljust(21)
    rom[0x7FD5:0x7FDC] = bytes((0x20, 0, 5, 0, 1, 0, 0))
    struct.pack_into("<HHH", rom, 0x7FFA, 0x8000, 0x8000, 0x8000)
    checksum = (sum(rom) + 0x1FE) & 0xFFFF
    struct.pack_into("<HH", rom, 0x7FDC, checksum ^ 0xFFFF, checksum)
    path.write_bytes(rom)


def check(directory):
    sizes = json.loads((directory / "sizes.json").read_text())
    video = (directory / "frames.bgr0").read_bytes()
    assert len(video) == sum(width * height * 4 for width, height in sizes), "wrong stream size"
    frames, offset = [], 0
    for width, height in sizes:
        pixels = [word for word, in struct.iter_unpack("<I", video[offset:offset + width * height * 4])]
        offset += width * height * 4
        assert all(pixel >> 24 == 0 for pixel in pixels), "the fourth byte is not zero"
        # The middle row lies inside the picture whatever the overscan.
        row = set(pixels[height // 2 * width:(height // 2 + 1) * width])
        assert len(row) == 1, "the generated screen is a single color"
        frames.append(((width, height), row.pop()))
    return frames, hashlib.sha256(video).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "bin" / "mesen-headless")
    parser.add_argument("--work-dir", type=Path, default=ROOT / "bin" / "test-work")
    args = parser.parse_args()
    binary, parent = args.binary.resolve(), args.work_dir.resolve()
    parent.mkdir(parents=True, exist_ok=True)

    report = {}
    with tempfile.TemporaryDirectory(prefix="mesen-headless-video-", dir=parent) as temporary:
        root = Path(temporary)
        for name, suffix, make_rom in (("snes", "sfc", make_snes_rom), ("game_gear", "gg", headless_gg.make_rom)):
            rom = root / f"{name}.{suffix}"
            make_rom(rom)
            observations = []
            for repeat in range(2):
                trial = root / f"{name}-{repeat}"
                trial.mkdir()
                result = headless_gg.run(binary, rom, ROOT / "tests" / "headless_video.lua",
                                         trial / "profile", trial, 20)
                headless_gg.expect_exit(result, 0, f"{name} video")
                observations.append(check(trial))
            assert observations[0] == observations[1], f"{name}: repeated runs disagreed"
            frames, digest = observations[0]
            settled = [color for _, color in frames[16:]]
            assert all(a != b for a, b in zip(settled, settled[1:])), f"{name}: a frame was repeated"
            if name == "game_gear":
                # Like getScreenBuffer, the whole 256x240 frame without the overscan crop.
                assert frames[-1][0] == (256, 240) and set(settled) == {0xFF0000, 0x0000FF}, frames
            else:
                assert len(set(settled)) == len(settled), frames
            report[name] = dict(frames=len(frames), size=frames[-1][0], colors=len(set(settled)), sha256=digest)
    print(json.dumps({**report, "repeats": 2}))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Check that emu.takeScreenshot() does not depend on the host's load.

The video decoder converts each frame on its own thread. In an endFrame callback
the frame that just ended has not reached it yet, so a screenshot shows the
frame before; on a loaded host the decoder could still be converting that one,
and the screenshot showed the frame before it. The generated Game Gear ROM
(headless_gg.py) alternates its backdrop between red and blue every frame. With
busy processes on every core, every screenshot taken in endFrame must show the
color emu.getScreenBytes() gave at the end of the frame before.
"""

import argparse
import multiprocessing
import os
from pathlib import Path
import tempfile

from headless_gg import ROOT, expect_exit, make_rom, png_pixels, run

FIRST, LAST = 21, 620           # the ROM clears VRAM in the first 10 frames


def busy():
    while True:
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, default=ROOT / "bin" / "mesen-headless")
    parser.add_argument("--work-dir", type=Path, default=ROOT / "bin" / "test-work")
    parser.add_argument("--load", type=int, default=2 * (os.cpu_count() or 1),
                        help="busy processes during the run (default: two a core)")
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error(f"runner is not executable: {binary}")
    parent = args.work_dir.resolve()
    parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="mesen-headless-screenshot-", dir=parent) as temporary:
        root = Path(temporary)
        rom = root / "generated.gg"
        make_rom(rom)
        os.environ["SCREENSHOT_FIRST"], os.environ["SCREENSHOT_LAST"] = str(FIRST), str(LAST)
        workers = [multiprocessing.Process(target=busy, daemon=True) for _ in range(args.load)]
        for worker in workers:
            worker.start()
        try:
            result = run(binary, rom, ROOT / "tests" / "headless_screenshot.lua", root / "profile", root, 60)
        finally:
            for worker in workers:
                worker.terminate()
        expect_exit(result, 0, "screenshots under load")
        live = dict(line.split("\t") for line in (root / "frames.tsv").read_text().splitlines())
        late = []
        for frame in range(FIRST + 1, LAST + 1):
            width, height, colors = png_pixels((root / f"shot-{frame:04d}.png").read_bytes())
            if (width, height) != (160, 144) or len(colors) != 1:
                raise SystemExit(f"frame {frame}: {width}x{height} with {len(colors)} colors")
            if colors.pop().hex() != live[str(frame - 1)]:
                late.append(frame)
        if late:
            raise SystemExit(f"{len(late)} of {LAST - FIRST} screenshots were not the frame before: {late[:10]}")
        print(f"{LAST - FIRST} screenshots, each the frame before, with {args.load} busy processes")


if __name__ == "__main__":
    main()

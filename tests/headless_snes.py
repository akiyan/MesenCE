#!/usr/bin/env python3
"""Exercise the native headless runner with a generated SNES ROM."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def make_rom(path):
    """Generate a 32 KiB LoROM that records the automatic joypad result."""
    rom = bytearray(32768)
    code = bytes.fromhex(
        "78 d8 a2 00 86 10 86 11 "  # Set 8-bit emulation defaults and zero counters.
        "a9 01 8d 00 42 "  # Enable auto joypad read.
        "a9 80 8d 00 21 9c 21 21 a9 1f 8d 22 21 9c 22 21 "  # Set a red backdrop.
        "a9 0f 8d 00 21 "  # Enable display.
        "ad 12 42 30 fb "  # Wait for the end of the previous VBlank.
        "ad 12 42 10 fb "  # Wait for the next VBlank.
        "ad 12 42 4a 90 fa "  # Wait for the automatic joypad read to start.
        "ad 12 42 4a b0 fa "  # Wait for the automatic joypad read to finish.
        "a6 11 ad 19 42 29 80 95 20 "  # Record the B bit from JOY1H.
        "f0 02 e6 10 e6 11 80 d9"  # Count pressed samples and total samples.
    )
    rom[:len(code)] = code
    rom[0x7FC0:0x7FD5] = b"HEADLESS INPUT CHECK".ljust(21)
    rom[0x7FD5:0x7FDC] = bytes((0x20, 0, 5, 0, 1, 0, 0))
    for vector in (0x7FFA, 0x7FFC, 0x7FFE):
        struct.pack_into("<H", rom, vector, 0x8000)
    checksum = (sum(rom) + 0x1FE) & 0xFFFF
    struct.pack_into("<HH", rom, 0x7FDC, checksum ^ 0xFFFF, checksum)
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


def check_probe(directory):
    evidence = json.loads((directory / "observation.json").read_text())
    if (
        evidence["frame"] != 5
        or evidence["pressed"] != 2
        or evidence["polls"] != 5
        or evidence["samples"] != [0, 0, 128, 128, 0]
        or evidence["masterClock"] <= 0
    ):
        raise SystemExit(f"unexpected ROM input report: {evidence}")
    png = (directory / "screen.png").read_bytes()
    if len(png) < 33 or png[:16] != b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR":
        raise SystemExit("the runner did not generate a PNG screenshot")
    width, height = struct.unpack_from(">II", png, 16)
    if not width or not height:
        raise SystemExit(f"empty screenshot: {width}x{height}")
    return evidence, hashlib.sha256(png).hexdigest()


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

    with tempfile.TemporaryDirectory(prefix="mesen-headless-test-", dir=parent) as temporary:
        root = Path(temporary)
        rom = root / "generated.sfc"
        make_rom(rom)
        probe = ROOT / "tests" / "headless_snes.lua"
        observations = []
        for repeat in range(2):
            trial = root / f"repeat-{repeat}"
            trial.mkdir()
            expect_exit(run(binary, rom, probe, trial / "profile", trial, 5), 0, "input probe")
            observations.append(check_probe(trial))
        if observations[0] != observations[1]:
            raise SystemExit(f"repeated runs disagreed: {observations}")

        stop = root / "stop.lua"
        stop.write_text("emu.addEventCallback(function() emu.stop(23) end, emu.eventType.endFrame)\n")
        expect_exit(run(binary, rom, stop, root / "exit-profile", root, 5), 23, "Lua exit status")

        idle = root / "idle.lua"
        idle.write_text("-- Let the runner enforce the wall-clock timeout.\n")
        expect_exit(run(binary, rom, idle, root / "timeout-profile", root, 1), 124, "timeout")
        expect_exit(run(binary, root / "missing.sfc", idle, root / "missing-profile", root, 1),
                    1, "missing ROM")
        invalid = root / "invalid.lua"
        invalid.write_text("local =\n")
        failed = run(binary, rom, invalid, root / "invalid-profile", root, 5)
        expect_exit(failed, 1, "invalid Lua")
        if "could not load Lua script" not in failed.stderr:
            raise SystemExit(f"missing Lua diagnostic:\n{failed.stderr}")

        evidence, screenshot = observations[0]
        print(json.dumps({"input": evidence, "png_sha256": screenshot, "repeats": 2,
                          "lua_exit": 23, "timeout_exit": 124, "missing_rom_exit": 1,
                          "invalid_lua_exit": 1}))


if __name__ == "__main__":
    main()

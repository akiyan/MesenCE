#!/usr/bin/env python3
"""Verify frame-bounded Game Gear mixer recording, PSG write events and Lua I/O port callbacks."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import wave


ROOT = Path(__file__).resolve().parents[1]

# Mode 4 with the display off, name table $3800, sprite table $3F00, no line interrupt.
REGISTERS = bytes.fromhex("0680 8081 ff82 ff83 ff84 ff85 fb86 0087 0088 0089 ff8a")


def make_rom(path):
    """Generate a 32 KiB Game Gear ROM.

    It sets tone 0 to period $0FE at full volume on the left output only (stereo
    port $06 = $F0), then every frame waits for the VBlank flag, counts the frame
    at $C010 and writes tone 0's attenuation: 0 on even counts, 4 on odd ones.
    """
    rom = bytearray(32768)
    rom[0x0000:0x0009] = bytes.fromhex("f3 ed56 31f0df c30001")      # DI, IM 1, SP=$DFF0, JP $0100
    rom[0x0066:0x0068] = bytes.fromhex("ed45")                         # RETN
    code = bytes.fromhex(
        "210002 0616 0ebf edb3 "          # Write the VDP registers from $0200.
        "3e8e d37f 3e0f d37f "            # Tone 0 period $0FE (latch, then data).
        "3e90 d37f "                      # Tone 0 attenuation 0.
        "3ef0 d306 "                      # Every channel on the left only.
        "dbbf e680 28fa "                 # $0119: wait for the VBlank flag.
        "2110c0 34 7e e601 07 07 f690 d37f "  # Count the frame; attenuation 0 or 4.
        "c31901"                          # Next frame.
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


def check(directory):
    status = json.loads((directory / "status.json").read_text())
    assert status["state"] == "complete" and status["frames"] == 8, status
    assert status["second"] == "an audio capture is already active", status
    assert status["alias"] == "complete", status
    assert status["stereo"] == 0xF0, status
    with wave.open(str(directory / "mix.wav"), "rb") as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (2, 2, status["sampleRate"])
        assert wav.getnframes() == status["samples"]
        pcm = wav.readframes(wav.getnframes())
    left = struct.unpack_from(f"<{len(pcm) // 2}h", pcm)[0::2]
    right = struct.unpack_from(f"<{len(pcm) // 2}h", pcm)[1::2]
    # The tone is on the left output only.
    assert max(abs(v) for v in left) > 1000 and max(abs(v) for v in right) == 0
    rows = [json.loads(line) for line in (directory / "psg.jsonl").read_text().splitlines()]
    start, end = rows[0], rows[-1]
    frames = [row for row in rows if row["kind"] == "frame"]
    writes = [row for row in rows if row["kind"] == "psg_write"]
    assert start["kind"] == "start" and end["kind"] == "end"
    assert end["frames"] == status["frames"] == len(frames) == 8
    assert end["samples"] == status["samples"] == frames[-1]["samples"]
    assert end["events"] == status["events"] == len(writes) and end["droppedEvents"] == 0
    # Sample counts follow the master clock.
    for row in frames:
        expected = (row["masterClock"] - start["masterClock"]) * status["sampleRate"] / status["clockRate"]
        assert abs(row["samples"] - expected) <= 2, (row, expected)
    # One attenuation write a frame, alternating, at the PSG port, inside the recording.
    assert len(writes) == 8, writes
    assert all(w["address"] == 0x7F and "spcCycle" not in w for w in writes)
    assert {w["value"] for w in writes} == {0x90, 0x94}
    assert all(start["masterClock"] < w["masterClock"] <= frames[-1]["masterClock"] for w in writes)
    assert all(a["masterClock"] < b["masterClock"] for a, b in zip(writes, writes[1:]))
    # The Lua callbacks saw the 3 set-up writes and one a frame (the first VBlank flag is set in frame 2).
    assert status["psgWrites"] == 3 + status["frame"] - 1, status
    return status, hashlib.sha256(pcm).hexdigest(), hashlib.sha256((directory / "psg.jsonl").read_bytes()).hexdigest()


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

    with tempfile.TemporaryDirectory(prefix="mesen-headless-gg-audio-", dir=parent) as temporary:
        root = Path(temporary)
        rom = root / "generated.gg"
        make_rom(rom)
        script = ROOT / "tests" / "headless_gg_audio.lua"
        results = []
        for repeat in range(2):
            trial = root / f"repeat-{repeat}"
            trial.mkdir()
            result = run(binary, rom, script, trial / "profile", trial, 10)
            if result.returncode != 0:
                raise SystemExit(f"Game Gear audio probe exited with {result.returncode}\n{result.stdout}\n{result.stderr}")
            results.append(check(trial))
        if results[0] != results[1]:
            raise SystemExit(f"repeated runs disagreed: {results}")
        status, wav, events = results[0]
        print(json.dumps({"game_gear_audio": status, "wav_sha256": wav, "events_sha256": events, "repeats": 2}))


if __name__ == "__main__":
    main()

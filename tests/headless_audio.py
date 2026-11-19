#!/usr/bin/env python3
"""Verify frame-bounded SNES mixer recording and timestamped APU events."""

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import wave


ROOT = Path(__file__).resolve().parents[1]


def make_rom(path):
    """Build a LoROM whose CPU uploads an SPC noise generator and exchanges a byte."""
    spc = bytearray()
    for register, value in (
        (0x6C, 0x3F),  # Unmute and set the noise rate.
        (0x0C, 0x7F), (0x1C, 0x7F),  # Master volume, left and right.
        (0x00, 0x50), (0x01, 0x50),  # Voice 0 volume.
        (0x03, 0x10), (0x07, 0x7F),  # Pitch and direct gain.
        (0x3D, 0x01), (0x4C, 0x01),  # Noise and key on, voice 0.
    ):
        spc.extend((0x8F, register, 0xF2, 0x8F, value, 0xF3))
    spc.extend(bytes.fromhex(
        "8f 52 f6 "      # SPC ready flag, visible to the CPU at $2142.
        "e4 f4 68 34 d0 fa "  # Wait for command $34 at SPC port 0.
        "c4 f5 "        # Echo the command to the CPU at $2141.
        "8f 00 f2 c4 f3 "  # Set voice 0 left volume from the command.
        "2f fe"         # Keep the DSP running.
    ))

    rom = bytearray(32768)
    code = bytearray(bytes.fromhex(
        "78 d8 "  # SEI, CLD.
        "a2 02 "  # Start the upload after two VBlanks so all setup is captured.
        "ad 12 42 30 fb ad 12 42 10 fb ca d0 f3 "
        "a9 aa cd 40 21 d0 fb a9 bb cd 41 21 d0 fb "  # Wait for SPC IPL.
        "9c 42 21 a9 02 8d 43 21 8d 41 21 "  # Transfer to $0200.
        "a9 cc 8d 40 21 cd 40 21 d0 fb "  # Request transfer, wait for echo.
        "a2 00"
    ))
    loop = len(code)
    code.extend((0xBD, 0, 0))  # LDA SPC code,X. Address is filled in below.
    code.extend(bytes.fromhex("8d 41 21 8a 8d 40 21 cd 40 21 d0 fb e8"))
    code.extend((0xE0, len(spc), 0xD0, (loop - len(code) - 4) & 0xFF))
    code.extend(bytes.fromhex(
        "9c 41 21 e8 8a 8d 40 21 "  # End transfer and execute $0200.
        "a9 52 cd 42 21 d0 fb "      # Wait until the program is ready.
        "a9 34 8d 40 21 cd 41 21 d0 fb "  # Send the command and wait for echo.
        "80 fe"
    ))
    struct.pack_into("<H", code, loop + 1, 0x8000 + len(code))
    rom[:len(code) + len(spc)] = code + spc
    rom[0x7FC0:0x7FD5] = b"HEADLESS AUDIO TEST".ljust(21)
    rom[0x7FD5:0x7FDC] = bytes((0x20, 0, 5, 0, 1, 0, 0))
    struct.pack_into("<HHH", rom, 0x7FFA, 0x8000, 0x8000, 0x8000)
    checksum = (sum(rom) + 0x1FE) & 0xFFFF
    struct.pack_into("<HH", rom, 0x7FDC, checksum ^ 0xFFFF, checksum)
    path.write_bytes(rom)


def inspect(trial):
    status = json.loads((trial / "result.json").read_text())
    wav_path = trial / "capture.wav"
    with wave.open(str(wav_path), "rb") as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (2, 2, status["sampleRate"])
        assert wav.getnframes() == status["samples"]
        samples = wav.readframes(wav.getnframes())
        assert len(samples) == wav.getnframes() * 4
        assert any(samples), "the synthetic APU should produce nonzero audio"

    rows = [json.loads(line) for line in (trial / "events.jsonl").read_text().splitlines()]
    start, end = rows[0], rows[-1]
    boundaries = [row for row in rows if row["kind"] == "frame"]
    assert start["kind"] == "start" and start["masterClock"] == status["startClock"]
    assert start["clockRate"] == status["clockRate"]
    assert end["kind"] == "end" and end["samples"] == status["samples"]
    assert end["frames"] == status["frames"] == len(boundaries) == 8
    assert end["droppedEvents"] == status["droppedEvents"] == 0
    assert [row["frame"] for row in boundaries] == list(range(start["frame"] + 1, start["frame"] + 9))
    for previous, current in zip([start] + boundaries, boundaries):
        assert current["masterClock"] > previous["masterClock"]
        assert current["samples"] > previous.get("samples", 0)
    assert boundaries[-1]["masterClock"] == status["endClock"]
    duration = (status["endClock"] - status["startClock"]) / status["clockRate"]
    assert abs(status["samples"] - duration * status["sampleRate"]) <= 4, status

    bus = [row for row in rows if "spcCycle" in row]
    assert len(bus) == status["events"] == end["events"]
    assert all(status["startClock"] <= row["masterClock"] <= status["endClock"] for row in bus)
    assert all(a["masterClock"] <= b["masterClock"] for a, b in zip(bus, bus[1:]))
    assert all(a["spcCycle"] <= b["spcCycle"] for a, b in zip(bus, bus[1:]))
    def find_event(kind, address, value, start_index=0):
        return next(index for index in range(start_index, len(bus))
                    if (bus[index]["kind"], bus[index]["address"], bus[index]["value"]) == (kind, address, value))

    ready = find_event("spc_write", 0xF6, 0x52)
    command = find_event("cpu_write", 0x2140, 0x34, ready + 1)
    received = find_event("spc_read", 0xF4, 0x34, command + 1)
    echo = find_event("spc_write", 0xF5, 0x34, received + 1)
    find_event("dsp_write", 0x00, 0x34, echo + 1)
    find_event("cpu_read", 0x2141, 0x34, echo + 1)

    return {
        **status,
        "eventKinds": dict(Counter(row["kind"] for row in bus)),
        "wavSha256": hashlib.sha256(wav_path.read_bytes()).hexdigest(),
        "eventsSha256": hashlib.sha256((trial / "events.jsonl").read_bytes()).hexdigest(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "bin" / "mesen-headless")
    parser.add_argument("--work-dir", type=Path, default=ROOT / "bin" / "test-work")
    args = parser.parse_args()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mesen-audio-test-", dir=args.work_dir.resolve()) as temp:
        root = Path(temp)
        rom = root / "generated.sfc"
        make_rom(rom)
        env = {key: value for key, value in os.environ.items() if key not in ("DISPLAY", "WAYLAND_DISPLAY")}
        env["SDL_AUDIODRIVER"] = "mesen-test-no-device"
        results = []
        for repeat in range(2):
            trial = root / f"repeat-{repeat}"
            result = subprocess.run(
                [str(args.binary.resolve()), "--home", str(trial), "--timeout", "10",
                 str(rom), str(ROOT / "tests" / "headless_audio.lua")],
                env=env, capture_output=True, text=True, timeout=15,
            )
            if result.returncode:
                raise SystemExit(f"audio test exited {result.returncode}\n{result.stdout}\n{result.stderr}")
            results.append(inspect(trial))
        assert results[0] == results[1], results
        trial = root / "stopped"
        result = subprocess.run(
            [str(args.binary.resolve()), "--home", str(trial), "--timeout", "10",
             str(rom), str(ROOT / "tests" / "headless_audio.lua")],
            env={**env, "MESEN_AUDIO_STOPPED": "1"}, capture_output=True, text=True, timeout=15,
        )
        if result.returncode:
            raise SystemExit(f"stopped capture exited {result.returncode}\n{result.stdout}\n{result.stderr}")
        status = json.loads((trial / "result.json").read_text())
        for key in ("frames", "samples", "startClock", "endClock"):
            assert status[key] == results[0][key], (key, status)
        assert status["events"] == status["droppedEvents"] == 0, status
        assert not (trial / "events.jsonl").exists(), "a WAV-only capture wrote an event log"
        assert hashlib.sha256((trial / "capture.wav").read_bytes()).hexdigest() == results[0]["wavSha256"]
        print(json.dumps({**results[0], "repeats": 2, "stoppedWithoutEvents": "identical"}))


if __name__ == "__main__":
    main()

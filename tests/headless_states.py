#!/usr/bin/env python3
"""Check named checkpoints and exact controller replay on a generated LoROM."""

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from headless_control import Client
from headless_snes import ROOT, make_rom


class StateClient(Client):
    def command(self, text, success=True):
        response = self.receive(self.send(text))
        assert response["ok"] is success, (text, response)
        return response

    def observation(self):
        state = self.command("status")
        ram = self.command("read 16 64")["data"]
        return {key: state[key] for key in ("frame", "boundary", "masterClock", "pad")} | {"ram": ram}


@contextmanager
def session(binary, rom, root, label):
    address = root / f"{label}.sock"
    env = os.environ.copy()
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    with (root / f"{label}.log").open("w+") as log:
        process = subprocess.Popen(
            [str(binary), "--home", str(root / f"{label}-home"),
             "--state-dir", str(root / "states"), "--control", str(address), str(rom)],
            env=env, cwd=root, stdout=log, stderr=subprocess.STDOUT,
        )
        try:
            with StateClient(address, process) as client:
                yield client
                client.command("quit")
            assert process.wait(timeout=8) == 0
        except BaseException:
            log.seek(0)
            print(log.read(), end="")
            raise
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=8)


def replay(client, baseline):
    assert client.observation() == baseline
    client.command("run 3")
    client.command("pad 0")
    last = client.command("run 4")
    assert last["reason"] == "completed" and last["advanced"] == 4
    observation = client.observation()
    assert observation["frame"] == baseline["frame"] + 7
    assert observation["masterClock"] > baseline["masterClock"]
    assert bytes.fromhex(observation["ram"])[0] > bytes.fromhex(baseline["ram"])[0]
    return observation


def check_states(binary, root):
    rom = root / "fixture.sfc"
    make_rom(rom)
    with session(binary, rom, root, "initial") as client:
        client.command("run 3")
        client.command("pad 2")
        saved = client.command("save checkpoint")
        assert saved["frame"] == 3 and saved["pad"] == 2 and saved["boundary"] == "frame"
        baseline = client.observation()
        expected = replay(client, baseline)
        client.command("pad 1")
        client.command("run 2")
        client.command("load checkpoint")
        assert replay(client, baseline) == expected

        stable = client.observation()
        state = (root / "states" / "checkpoint.mcs").read_bytes()
        bad = root / "states" / "damaged.mcs"
        bad.write_bytes(state[:-1])
        assert "checksum" in client.command("load damaged", False)["error"]
        incompatible = root / "states" / "build-mismatch.mcs"
        incompatible.write_bytes(state.replace(b"\nrunner ", b"\nunknown ", 1))
        assert "incompatible" in client.command("load build-mismatch", False)["error"]
        different_settings = root / "states" / "settings-mismatch.mcs"
        settings_offset = state.index(b"\nsettings ") + len(b"\nsettings ")
        changed_hex = b"1" if state[settings_offset:settings_offset + 1] == b"0" else b"0"
        different_settings.write_bytes(state[:settings_offset] + changed_hex + state[settings_offset + 1:])
        assert "incompatible" in client.command("load settings-mismatch", False)["error"]
        client.command("load missing", False)
        client.command("save ../outside", False)
        assert client.observation() == stable
        assert not (root / "outside.mcs").exists()

    with session(binary, rom, root, "restored") as client:
        client.command("load checkpoint")
        assert replay(client, baseline) == expected

    altered = root / "changed.sfc"
    data = bytearray(rom.read_bytes())
    data[0x4000] ^= 1
    altered.write_bytes(data)
    with session(binary, altered, root, "other-rom") as client:
        stable = client.observation()
        assert "incompatible" in client.command("load checkpoint", False)["error"]
        assert client.observation() == stable

    return {"checkpoint_frame": baseline["frame"], "checkpoint_pad": baseline["pad"],
            "replayed_frame": expected["frame"], "master_clock": expected["masterClock"],
            "identical_replays": 3, "bad_states_rejected": 5}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "bin" / "mesen-headless")
    parser.add_argument("--work-dir", type=Path, default=ROOT / "bin" / "test-work")
    args = parser.parse_args()
    parent = args.work_dir.resolve()
    parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mesen-states-", dir=parent) as temporary:
        print(json.dumps(check_states(args.binary.resolve(), Path(temporary)), sort_keys=True))


if __name__ == "__main__":
    main()

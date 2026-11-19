#!/usr/bin/env python3
"""Check external frame control with a generated SNES ROM."""

import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

from headless_snes import make_rom


ROOT = Path(__file__).resolve().parents[1]


class Client:
    def __init__(self, path, process):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(5)
        self.responses = {}
        self.next_id = 1
        previous = Path.cwd()
        deadline = time.monotonic() + 5
        try:
            # Keep the Unix socket address short even under a deep build directory.
            os.chdir(path.parent)
            while True:
                try:
                    self.socket.connect(path.name)
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise
                    time.sleep(0.01)
            self.stream = self.socket.makefile("rb")
        except BaseException:
            self.socket.close()
            raise
        finally:
            os.chdir(previous)

    def send(self, command):
        request = self.next_id
        self.next_id += 1
        self.socket.sendall(f"{request} {command}\n".encode())
        return request

    def receive(self, request):
        while request not in self.responses:
            line = self.stream.readline()
            if not line:
                raise RuntimeError(f"control socket closed while waiting for request {request}")
            result = json.loads(line)
            self.responses[result["id"]] = result
        return self.responses.pop(request)

    def call(self, command):
        result = self.receive(self.send(command))
        if not result["ok"]:
            raise RuntimeError(f"{command}: {result}")
        return result

    def close(self):
        self.stream.close()
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def position(reply):
    if reply.get("boundary") != "frame":
        raise RuntimeError(f"unexpected stop boundary: {reply}")
    return reply["frame"], reply["masterClock"]


def wait_until_running(client, start):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        result = client.call("status")
        if result["running"] and result["frame"] > start:
            return
    raise RuntimeError("frame run did not advance before cancellation")


def run_checks(binary, root):
    rom = root / "generated.sfc"
    make_rom(rom)
    path = root / "control.sock"
    environment = os.environ.copy()
    environment.pop("DISPLAY", None)
    environment.pop("WAYLAND_DISPLAY", None)
    with (root / "runner.log").open("w+") as log:
        process = subprocess.Popen(
            [str(binary), "--home", str(root / "profile"), "--control", str(path), str(rom)],
            env=environment, stdout=log, stderr=subprocess.STDOUT,
        )
        try:
            with Client(path, process) as client:
                if path.stat().st_mode & 0o077:
                    raise RuntimeError("control socket must belong to the session owner")
                initial = client.call("status")
                if initial["frame"] != 0 or initial["running"]:
                    raise RuntimeError(f"session ran before its first request: {initial}")

                first = client.call("run 3")
                if first["frame"] != 3 or first["advanced"] != 3 or first["reason"] != "completed":
                    raise RuntimeError(f"bounded run finished unexpectedly: {first}")
                if first["masterClock"] <= initial["masterClock"] or client.call("read 0x11 1")["data"] == "00":
                    raise RuntimeError("the fixture did not execute during the frame run")

                run = client.send("run 10000000 10000")
                wait_until_running(client, first["frame"])
                cancelled = client.call("cancel")
                completion = client.receive(run)
                if completion["reason"] != "cancelled" or completion["advanced"] == 0:
                    raise RuntimeError(f"cancel did not stop an active run: {completion}")
                if position(cancelled) != position(completion):
                    raise RuntimeError(f"cancel acknowledgement disagreed with run: {cancelled}")
                before = client.call("read 0x10 16")
                time.sleep(0.05)
                after = client.call("read 0x10 16")
                if position(before) != position(after) or before["data"] != after["data"]:
                    raise RuntimeError("emulation advanced while held")

                resumed = client.call("run 5")
                if resumed["frame"] != after["frame"] + 5 or resumed["reason"] != "completed":
                    raise RuntimeError(f"resume did not advance exactly five frames: {resumed}")
                polls = client.call("read 0x11 1")["data"]
                if int(polls, 16) != (int(after["data"][2:4], 16) + 5) % 256:
                    raise RuntimeError("the fixture did not poll the joypad five times after resume")
                expired = client.call("run 10000000 2")
                if expired["reason"] != "deadline" or expired["frame"] - resumed["frame"] != expired["advanced"]:
                    raise RuntimeError(f"deadline did not hold at a frame boundary: {expired}")
                if position(client.call("status")) != position(expired):
                    raise RuntimeError("session advanced past its deadline response")
                client.send("run 10000000 10000")
                wait_until_running(client, expired["frame"])
                # Disconnect while emulating, then reconnect on the same socket.

            with Client(path, process) as next_client:
                disconnected = next_client.call("status")
                if disconnected["lastStop"] != "disconnected" or disconnected["running"]:
                    raise RuntimeError(f"disconnect did not leave a held session: {disconnected}")
                resumed = next_client.call("run 2")
                if resumed["frame"] != disconnected["frame"] + 2:
                    raise RuntimeError(f"reconnected session did not resume: {resumed}")
                next_client.call("quit")
            if process.wait(timeout=5) != 0 or path.exists():
                raise RuntimeError("resident runner did not exit cleanly and remove its socket")
        except BaseException:
            log.seek(0)
            print(log.read(), end="")
            raise
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "bin" / "mesen-headless")
    parser.add_argument("--work-dir", type=Path, default=ROOT / "bin" / "test-work")
    args = parser.parse_args()
    parent = args.work_dir.resolve()
    parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mesen-control-test-", dir=parent) as directory:
        run_checks(args.binary.resolve(), Path(directory))
    print("control: completed, cancelled, deadline, disconnected, resumed, quit")


if __name__ == "__main__":
    main()

# Resident SNES control

The Linux headless runner can hold a SNES emulation session at a frame boundary
and accept commands on a local Unix socket. Use a new or empty profile and an
absolute socket path. The socket must be new and is accessible to the process
owner.

```sh
USE_GCC=true make -j4 headless
bin/mesen-headless --home /absolute/path/to/profile \
  --control /absolute/path/to/session.sock input.sfc
```

Connect one client at a time. Send a decimal request ID, the command and any
arguments separated by whitespace, followed by a newline. Responses are
newline-delimited JSON objects with the request's `id` and an `ok` flag. Use a
different ID for each outstanding request. Numeric arguments accept decimal
and `0x`-prefixed hexadecimal.

| Command | Behavior |
| --- | --- |
| `status` | Report the bridge frame, master clock, pad mask, whether a run is active, and its last stop reason. |
| `run FRAMES [DEADLINE_MS]` | Advance up to the requested number of frames. The default wall-clock deadline is 30,000 ms. |
| `cancel` | Stop the active run and acknowledge its stop position. |
| `pad MASK` | Set the SNES port 1 latch while held. Bits 0 through 11 are A, B, X, Y, L, R, Select, Start, Up, Down, Left, Right. |
| `read OFFSET COUNT` | Read up to 4,096 bytes from SNES WRAM while held. Offsets start at zero. The response's `data` field is lowercase hexadecimal. |
| `save NAME` | Write a named checkpoint while held. |
| `load NAME` | Restore a compatible named checkpoint while held. |
| `quit` | End the session and remove its socket. |

The runner holds before the first frame with `frame` equal to zero. A completed
run replies with `advanced`, `frame`, `masterClock`, `pad` and `boundary: "frame"`.
Its `reason` is `completed`, `cancelled` or `deadline`. The emulator loop returns
from the current frame before checking the socket and the deadline.

Send `cancel` over the current connection while the `run` request is pending.
The two requests get separate replies with the same stop position. A client
disconnect stops the run at a frame boundary and leaves the runner available
for the next client, whose `status` reports `lastStop: "disconnected"`.

This exchange advances ten frames, holds the B button, then advances two more.

```text
1 run 10
2 pad 2
3 run 2
4 read 0x10 16
```

Wait for each `run` reply before sending a command that requires a held session.
Pass `--state-dir /absolute/path/to/states` before `--control` to reuse named
checkpoints across fresh profiles. [Named checkpoints](states.md)

```sh
python3 tests/headless_control.py
```

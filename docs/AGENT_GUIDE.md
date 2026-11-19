# MesenCE headless guide for AI agents

Use this guide to run and verify SNES software with the Linux headless runner.
Start with numbers from WRAM, timing, and event logs. Save PNG or WAV files when
you need to inspect pixels or sound.

| Task | Entry point |
| --- | --- |
| Drive a game, examine WRAM, or retry a section from a checkpoint | Resident session with `--control` |
| Stop at a ROM-specific condition, inspect the CPU or PPU, or capture raster and audio events | Batch run with a Lua probe |

Keep ROM-specific addresses, expected values, and input sequences in the project
under test. The WRAM offsets in this guide are examples.

## 1. Build and check the runner

Build from the MesenCE checkout on Linux with GCC and SDL2 development files.

```sh
make USE_GCC=true LTO=false -j4 headless
python3 tests/headless_snes.py
```

The test generates its own ROM and verifies input, frame timing, a PNG, and exit
codes. Use a new or empty absolute profile directory for each runner invocation.

## 2. Drive a resident session

Create a local working directory, then launch the runner in one terminal.
Replace the paths with paths on your machine.

```sh
bin/mesen-headless --home /absolute/run/profile \
  --state-dir /absolute/run/checkpoints \
  --control /absolute/run/mesen.sock /absolute/game.sfc
```

The process opens an owner-only Unix socket and starts held at frame 0.
Connect one client at a time. Send a request ID, a command and its arguments,
separated by whitespace, followed by a newline. Read each newline-delimited
JSON reply and match its `id` to the request.

| Command | When to use it |
| --- | --- |
| `status` | Read `frame`, `masterClock`, `pad`, `running` and `lastStop`. Wait for a held state before using the position as final evidence. |
| `pad 2` | Hold B on port 1. A is mask 1, B is mask 2, and mask 0 releases all buttons. A mask remains latched across runs. |
| `run 3 5000` | Advance up to three frames, with a 5,000 ms wall-clock deadline. Wait for the reply before sending a command that requires a held session. |
| `read 0x10 16` | Read 16 bytes at WRAM offset `0x10` while held. `data` contains lowercase hex. Each read supports up to 4,096 bytes. |
| `save NAME` / `load NAME` | Save or restore a named checkpoint while held. |
| `cancel` | Stop an active run at the next available frame boundary. |
| `quit` | End the session and remove its socket. |

Check both `ok` and `reason` on a run reply. For an exact advance, require
`reason == "completed"` and the expected `advanced` count. The bridge `frame`
counts completed advances, and `masterClock` measures emulated time. Use these
fields to compare runs. The deadline uses wall time and is checked at frame
boundaries.

Send `cancel` on the current connection while a `run` request is pending.
Read both replies by ID. They report the same stop position with
`boundary: "frame"`. If the client disconnects during a run, reconnect and
check `status`. It reports `lastStop: "disconnected"` and the position remains
held until another request advances it. [Resident protocol](control.md)

## 3. Replay from a named checkpoint

When the socket exists, run this client in another terminal. It warms up for
three frames, saves a checkpoint with B latched, and compares two replays.

```sh
python3 - /absolute/run/mesen.sock <<'PY'
import itertools
import json
import socket
import sys

ids = itertools.count(1)
with socket.socket(socket.AF_UNIX) as connection:
    connection.settimeout(10)
    connection.connect(sys.argv[1])
    with connection.makefile("rb") as replies:
        def call(command):
            request = next(ids)
            connection.sendall(f"{request} {command}\n".encode())
            reply = json.loads(replies.readline())
            assert reply["id"] == request and reply["ok"], reply
            return reply

        def advance(frames):
            reply = call(f"run {frames} 5000")
            assert reply["reason"] == "completed", reply
            assert reply["advanced"] == frames, reply

        def replay():
            assert call("status")["pad"] == 2
            advance(3)
            call("pad 0")
            advance(4)
            reply = call("read 0x10 16")
            return {key: reply[key] for key in ("frame", "masterClock", "data")}

        try:
            advance(3)
            call("pad 2")
            call("save before-input")
            first = replay()
            call("load before-input")
            second = replay()
            assert first == second, (first, second)
            print(json.dumps(second))
        finally:
            call("quit")
PY
```

Checkpoints include the bridge frame and pad latch. To replay in a fresh
process, start with a new profile and point `--state-dir` at the same checkpoint
directory. Match the ROM and patch, runner and core build, and emulation
settings. Rejected loads leave the session held. For a changed ROM or build,
start a fresh session. [Checkpoint format and compatibility](states.md)

## 4. Observe CPU, WRAM, and PPU state with Lua

Use a batch probe for frame conditions and instruction-level observations.
Count frames in `startFrame` and apply scheduled pad inputs in `inputPolled`.
Use `endFrame` for sampled WRAM checks or an `exec` memory callback for a
specific CPU address. Use a bounded frame count or wall-clock timeout.

Save this probe as `probe.lua`. It holds B on frames 2 and 3, writes timing and
palette data after frame 5, and keeps the latest completed PNG.

```lua
local frame = 0
emu.enableSnesFrameCapture(true)
emu.startSnesPpuTrace(4096)

emu.addEventCallback(function()
    frame = frame + 1
end, emu.eventType.startFrame)

emu.addEventCallback(function()
    emu.setInput({b = frame == 2 or frame == 3}, 0)
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
    if frame ~= 5 then return end
    local s = emu.getSnesPpuSnapshot(0x10, 4)
    local trace = emu.stopSnesPpuTrace()
    local report = assert(io.open("probe.txt", "w"))
    assert(report:write(string.format(
        "frame=%d clock=%d boundary=%s pc=%06x mode=%d palette0=%04x events=%d dropped=%d\n",
        s.live.frame, s.live.masterClock, s.live.boundary, s.cpu.pc,
        s.ppu.bgMode, s.ppu.palette[1], #trace.events, trace.dropped)))
    assert(report:write("wram=" .. table.concat(s.wram.bytes, ",") .. "\n"))
    if s.completedFrame then
        assert(report:write("image_clock=" .. s.completedFrame.masterClock .. "\n"))
        local png = assert(io.open("frame.png", "wb"))
        assert(png:write(s.completedFrame.png))
        assert(png:close())
    end
    assert(report:close())
    emu.stop(0)
end, emu.eventType.endFrame)
```

```sh
bin/mesen-headless --home /absolute/run/probe-profile \
  --timeout 30 /absolute/game.sfc probe.lua
```

- Relative Lua output paths resolve inside the profile. Inspect output files
  and the process exit status. A batch wall-clock timeout returns 124.
- `live` describes the observation point. Use `completedFrame.masterClock` for
  the image, which can be the preceding frame at `endFrame` or a debugger
  stop. Reset and state restoration clear cached frame and trace data.
- `ppu.palette[1]` describes CGRAM color zero. The first element of
  `wram.bytes` is at the requested offset. Decode colors as BGR555 words.
- For raster regressions, compare ordered event fields `kind`, `frame`,
  `masterClock`, `scanline`, and `cycle`, plus address and value where present.
  The recorder handles PPU access, DMA, HDMA, NMI, and IRQ. A positive
  `dropped` count marks a partial trace. Limit the interval or increase capacity
  up to 65,536 events.
- To observe an instruction-level stop, take a snapshot inside `codeBreak`
  after a scanline step and resume when done.
  [Raster fixture](../tests/headless_ppu.lua) · [Snapshot and trace fields](ppu.md)

## 5. Capture and compare audio

Save this script as `audio.lua` and run it in batch mode with a fresh profile.
The recorder starts at the next VBlank mixer boundary and records eight
complete intervals in this example.

```lua
assert(emu.startSnesAudioCapture("mix.wav", "apu.jsonl", 8))
emu.addEventCallback(function()
    local result = emu.getSnesAudioCapture()
    if result.state == "complete" then
        emu.stop(0)
    elseif result.state == "error" then
        emu.stop(1)
    end
end, emu.eventType.startFrame)
```

Compare the sample count and rate, the start and end master clocks, and the
order of CPU, SPC and DSP events in the JSONL. Check `droppedEvents` before
using the bus trace as complete evidence. Use new output paths for each
capture. [Audio formats and limits](audio.md)

## 6. Verify the workflow against generated ROMs

```sh
python3 tests/headless_snes.py
python3 tests/headless_control.py
python3 tests/headless_states.py
python3 tests/headless_ppu.py
python3 tests/headless_audio.py
```

For comparisons, record the ROM and runner revisions, input sequence,
checkpoint name if used, stopping position, and the numerical or file evidence
checked. Compare runs from the same initial state and report any capture loss.

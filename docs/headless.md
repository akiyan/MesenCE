# Native headless runner

On Linux, build the runner using a C++17 compiler and SDL2 development files.
The `headless` make target builds the runner with the MesenCE core.

```sh
USE_GCC=true make -j4 headless
bin/mesen-headless --home /absolute/path/to/profile --timeout 30 rom.sfc probe.lua
```

Use an absolute profile path. The runner accepts an empty directory or creates
a new directory. Use a fresh profile for every run to isolate settings and
output from other MesenCE sessions. ROM and Lua script paths may be relative
to the caller's current directory. Relative paths opened by Lua resolve inside
the profile.

The CLI runs without a display server, audio device, or physical input, and
emulates as fast as the host allows (no frame pacing).
It configures a standard SNES controller on port 1, initializes SNES RAM to
zero, and disables SNES frame skipping. Lua file I/O is enabled for writing
observations. Run trusted Lua scripts.

Batch runs also accept Master System and Game Gear ROMs; the `.gg` extension
selects the Game Gear. Port 1 is a standard pad (`up`, `down`, `left`, `right`,
`one`, `two`, and `pause`, which is Start on the Game Gear). Work RAM, VRAM and
palette RAM start at zero, and so does the VDP's scanline, which the
interactive emulator randomizes at power-on; repeated runs therefore match
frame for frame. Screenshots show the visible screen: 256x192 on the Master
System and the 160x144 window on the Game Gear. Game Gear frames are not
blended with the previous frame. The PSG channels play at full volume (the
interactive emulator's defaults; the core's own are silent). Lua memory
callbacks on `emu.memType.smsPort` see the I/O port accesses, such as the PSG
writes to ports 0x40-0x7F. The resident `--control` mode remains SNES-only.

A script can apply controller input in an `inputPolled` callback, inspect
memory in an `endFrame` callback, and store the PNG bytes returned by
`emu.takeScreenshot()` or, for every frame, `emu.getScreenBytes()`. Call `emu.stop(code)` to finish with a chosen process
exit status. The wall-clock timeout defaults to 30 seconds and returns 124.

- [Headless guide for AI agents](AGENT_GUIDE.md)
- [Resident frame control](control.md)
- [Named checkpoints and input replay](states.md)
- [SNES PPU snapshots and raster trace](ppu.md)
- [Audio capture with APU and PSG events](audio.md)

## Verification

```sh
python3 tests/headless_snes.py
python3 tests/headless_gg.py
python3 tests/headless_screenshot.py
```

The first test writes a small, generated SNES cartridge image. It runs the image in
two isolated profiles and compares the CPU's recorded joypad samples, the
condition at which the script stopped, the master clock, and the screenshot
hash. It also checks an explicit Lua exit status, a timeout, and a missing ROM.
The test checks that a malformed Lua script produces an immediate diagnostic.
Use `--binary` to select a runner or `--work-dir` to select the parent
directory for temporary test files.

`tests/headless_gg.py` generates a Game Gear ROM that records the pad and
Start ports every frame and alternates the backdrop between red and blue. It
checks the recorded input, the frame count, zeroed work RAM, and that two
consecutive 160x144 screenshots each show a single color, in two profiles.
`tests/headless_screenshot.py` runs the same ROM with busy processes on every
core and checks that each of 599 screenshots taken in `endFrame` shows the frame
before, as at rest: `emu.takeScreenshot()` waits for the video decoder's thread.

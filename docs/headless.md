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

The CLI runs without a display server, audio device, or physical input.
It configures a standard SNES controller on port 1, initializes SNES RAM to
zero, and disables SNES frame skipping. Lua file I/O is enabled for writing
observations. Run trusted Lua scripts.

A script can apply controller input in an `inputPolled` callback, inspect
memory in an `endFrame` callback, and store the PNG bytes returned by
`emu.takeScreenshot()`. Call `emu.stop(code)` to finish with a chosen process
exit status. The wall-clock timeout defaults to 30 seconds and returns 124.

- [Headless guide for AI agents](AGENT_GUIDE.md)
- [Resident frame control](control.md)
- [Named checkpoints and input replay](states.md)
- [SNES PPU snapshots and raster trace](ppu.md)
- [SNES audio and APU event capture](audio.md)

## Verification

```sh
python3 tests/headless_snes.py
```

The test writes a small, generated SNES cartridge image. It runs the image in
two isolated profiles and compares the CPU's recorded joypad samples, the
condition at which the script stopped, the master clock, and the screenshot
hash. It also checks an explicit Lua exit status, a timeout, and a missing ROM.
The test checks that a malformed Lua script produces an immediate diagnostic.
Use `--binary` to select a runner or `--work-dir` to select the parent
directory for temporary test files.

# Audio capture

`emu.startAudioCapture(wavPath, eventsPath, frames)` arms a recording for 1
to 216,000 frame intervals of a SNES, Master System or Game Gear game.
`emu.startSnesAudioCapture` is the same function under its earlier name.
Give it two new output paths and enable Lua file I/O. The headless runner
enables file I/O by default and runs the mixer without opening a host audio
device.

The recording begins at the next frame end after the console's audio
buffer has been flushed (the SNES at VBlank, the Master System and Game Gear
when the VDP finishes the visible lines). It ends after the specified number
of subsequent frame ends. The WAV contains stereo signed 16-bit PCM at the
mixer output sample rate. `emu.getAudioCapture()` (or
`emu.getSnesAudioCapture()`) reports the state, total completed frames and
stereo sample frames, start and end master clocks, clock and sample rates,
and event counts. A completed recording has state `complete` and both output
files are closed. Invalid output paths and a second active capture return
`nil` and an error string. Write errors and discontinuous frame clocks set
the state to `error`.

```lua
assert(emu.startAudioCapture("mix.wav", "apu.jsonl", 120))
emu.addEventCallback(function()
    local capture = emu.getAudioCapture()
    if capture.state == "complete" then
        emu.stop(0)
    elseif capture.state == "error" then
        emu.stop(1)
    end
end, emu.eventType.startFrame)
```

The trace is JSON Lines. Its `start` row gives the initial master clock,
frame, master clock rate in Hz and WAV sample rate. Each `frame` row
records the master clock and cumulative stereo sample frames at that
boundary. The `end` row gives the final frame, sample and event counts.

Bus event rows carry `kind`, `masterClock`, `spcCycle`, `address` and
`value`. CPU port reads and writes use addresses 0x2140–0x2143.
SPC port operations use addresses 0xF4–0xF7. DSP writes report the DSP
register selected through SPC port 0xF2. CPU events use the actual master
clock. SPC events carry the SPC half-cycle counter and its projection
onto the master clock, including when the SPC catches up inside a single
CPU tick. On the Master System and Game Gear, `psg_write` rows record the
PSG writes (ports 0x40-0x7F) and `psg_stereo` rows the Game Gear's stereo
register (port 0x06), with `masterClock`, `address` (the port) and `value`.
The trace retains at most 1,000,000 bus rows per recording and reports
further rows in `droppedEvents`. Frame markers are always retained.

Run `python3 tests/headless_audio.py` with a built headless runner. The
test generates a SNES ROM that uploads a short SPC program, exchanges a
command and generates a noise waveform. Two isolated runs verify sample
counts against master clocks, the order of the CPU, SPC and DSP events, and
identical WAV and JSONL files.

`python3 tests/headless_gg_audio.py` generates a Game Gear ROM that plays a
tone on the left output only and changes its volume every frame. Two
isolated runs verify the sample counts against master clocks, one PSG write
row a frame, a silent right channel, the Lua port callbacks, the refusal of a
second capture, and identical WAV and JSONL files. A third run records
across frame 65,536 (the Master System frame count is 32 bits).

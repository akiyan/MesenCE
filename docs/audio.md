# SNES audio capture

`emu.startSnesAudioCapture(wavPath, eventsPath, frames)` arms a recording
for 1 to 3,600 SNES frame intervals. Give it two new output paths and enable
Lua file I/O. The headless runner enables file I/O by default and runs the
mixer without opening a host audio device.

The recording begins at the next VBlank after the SPC audio buffer has been
flushed. It ends after the specified number of subsequent VBlanks. The WAV
contains stereo signed 16-bit PCM at the mixer output sample rate.
`emu.getSnesAudioCapture()` reports the state, total completed frames and
stereo sample frames, start and end master clocks, clock and sample rates,
and event counts. A completed recording has state `complete` and both output
files are closed. Invalid output paths and a second active capture return
`nil` and an error string. Write errors and discontinuous frame clocks set
the state to `error`.

```lua
assert(emu.startSnesAudioCapture("mix.wav", "apu.jsonl", 120))
emu.addEventCallback(function()
    local capture = emu.getSnesAudioCapture()
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
CPU tick. The trace retains at most 1,000,000 bus rows per recording and
reports further rows in `droppedEvents`. Frame markers are always retained.

Run `python3 tests/headless_audio.py` with a built headless runner. The
test generates a SNES ROM that uploads a short SPC program, exchanges a
command and generates a noise waveform. Two isolated runs verify sample
counts against master clocks, the order of the CPU, SPC and DSP events, and
identical WAV and JSONL files.

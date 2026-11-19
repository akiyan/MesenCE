# Named checkpoints

A resident runner accepts `save NAME` and `load NAME` while held at a completed
frame boundary. These commands save and restore the emulated machine, the
completed-frame counter and the current controller latch as one checkpoint.
The next controller poll uses the restored latch. Run commands can then replay
the same sequence of controller updates and frame budgets.

State names use up to 64 ASCII letters, digits, dots, underscores or hyphens.
Begin the name with a letter or digit. The runner writes `NAME.mcs` into the
profile's `NamedStates` directory by default. Pass `--state-dir ABS_DIR` to reuse
checkpoints from another fresh profile. A save of the same name replaces the
file atomically.

On load, the runner checks the ROM and patch identities, the serialized
emulation settings, the runner and loaded core build hashes, and the state
format before restoring the machine. It checks the payload digest and lengths
before calling the core deserializer. A rejected load leaves execution held.
The bridge only changes the frame counter and controller latch after the core
restore succeeds.

```sh
python3 tests/headless_states.py
```

The test saves a checkpoint with a held button and compares WRAM, the stop frame
and the master clock over three replays, including a new process. It checks
missing and corrupt files and rejects state files for a different ROM, build or
configuration.

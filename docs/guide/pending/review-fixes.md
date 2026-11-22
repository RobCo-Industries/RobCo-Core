# Review fixes

## Input held-key state (`include/robco/input.h`)

- `input_hold_init(h)`: clears the per-source held keys and the repeat key.
- `input_hold_update(h, ev)`: applies a KEY event. A key is down while any source holds it. A release clears only that source's hold. Returns 1 on a press so the caller restarts the repeat delay.
- `input_hold_down(h, code)`: non-zero while any source holds `code`. `input_key_down()` now uses it, and auto-repeat goes on while any source holds the key.

## Unmount errors (`include/robco/fs.h`)

- `fs_unmount(volume)`: flushes the device before it detaches the volume. If the flush fails, it returns the device error and the volume stays mounted, so the caller can call it again.
- `fs_close(f)`: returns a flush error, and the handle is closed anyway.

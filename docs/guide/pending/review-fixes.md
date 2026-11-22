# Review fixes

## Input held-key state (`include/robco/input.h`)

- `input_hold_init(h)`: clears the per-source held keys and the repeat key.
- `input_hold_update(h, ev)`: applies a KEY event. A key is down while any source holds it. A release clears only that source's hold. Returns 1 on a press so the caller restarts the repeat delay.
- `input_hold_down(h, code)`: non-zero while any source holds `code`. `input_key_down()` now uses it, and auto-repeat goes on while any source holds the key.

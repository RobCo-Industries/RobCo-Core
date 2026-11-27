# Sound effect models

A sound effect model describes short sounds as layers of tones and filtered noise. `tools/sfx_table.py` renders a model into 8-bit tables that a program plays at the model's rate. The synthesis is original and works from the numbers in the model alone.

A program checks in its model file and the generated table. The table records the model's SHA-256 and the tool's revision, so a build can tell when the table is out of date.

## The file

A model is one JSON object in UTF-8. Every number is a JSON integer, so a file reads the same on every machine. The type counts, not the value: `1.0`, `1e0` and `-0.0` are refused while the file is parsed, even where `1` or `0` would do. `NaN`, `Infinity`, a number of more than 15 digits and a duplicated field are refused too. `true` and `false` are not numbers, and a number is not `true` or `false`.

The tables below list every field of the format. The tool checks each one through the same typed check, and a test fails if its list of fields and these tables differ.

```json
{ "format": 1, "rate_hz": 37500, "ceiling_steps": 90,
  "sounds": { "blip": { "len": 1200, "peak_steps": 60, "layers": [
      { "kind": "noise", "seed": 2, "level_mdb": -6000, "hits": [[0, 0]],
        "env": [[0, -30000], [500, 0], [20000, -40000]],
        "bands": [[0, -20000], [3000, 0], [9000, -12000]] } ] } } }
```

### Top level

| Field | Range | Meaning |
|---|---|---|
| `format` | 1 | The format version |
| `rate_hz` | 8000 to 48000 | Samples per second of every table |
| `ceiling_steps` | 1 to 127 | No sample may sit further than this from 128 |
| `sounds` | 1 or more | Sound name to sound. A name is `a` to `z`, `0` to `9` and `_`, starts with a letter, at most 32 characters. |

### A sound

| Field | Range | Meaning |
|---|---|---|
| `len` | 1 to 262144 | Length of the table in samples |
| `peak_steps` | 1 to `ceiling_steps` | The loudest sample sits exactly this far from 128 |
| `layers` | 1 to 16 | Tone and noise layers, summed in order |
| `loop` | `true` or `false`, optional | What runs past `len` wraps to the start |
| `split` | 1 to `len - 1`, optional | Length of the first of two parts. The tables are named `<name>_1` and `<name>_2`. |

### A tone layer

| Field | Range | Meaning |
|---|---|---|
| `kind` | `"tone"` | |
| `hits` | 1 to 64 pairs | `[start, level]`: start in samples, 0 to `len - 1` of the sound, level in millidecibels |
| `len` | 1 to 262144 | Length of one hit in samples |
| `pitch_mhz` | `[from, to]`, 1 to `rate_hz * 500` | Pitch in millihertz. It glides from `from` to `to` by a constant ratio. |
| `level_mdb` | -120000 to 20000 | Level of the layer in millidecibels |
| `partials` | 1 to 16 pairs | `[multiple, level]`: the multiple of the pitch times 10000, 1 to 1000000, and the level in millidecibels added to `level_mdb` |
| `attack_us`, `release_us` | 0 to the hit's length, optional | Linear ramps at the start and the end of a hit, in microseconds |
| `env` | 1 to 64 pairs, optional | `[time, level]`: time in microseconds from the hit, 0 to 32768000, level in millidecibels. Times must not go down. |

### A noise layer

| Field | Range | Meaning |
|---|---|---|
| `kind` | `"noise"` | |
| `hits` | 1 to 64 pairs | As for a tone |
| `seed` | 0 to 4294967295 | Seed of the noise generator |
| `level_mdb` | -120000 to 20000 | The rms level of the layer where the envelope is 0 dB |
| `env` | 2 to 64 pairs | As for a tone. The last time sets the length of a hit. |
| `bands` | 2 to 24 pairs | `[frequency, level]`: the filter curve. Frequency in hertz, 0 to `rate_hz / 2`, each above the one before. Level in millidecibels. |
| `fold` | 0 to half the hit's length, optional | Samples faded in and out at each end of a hit |
| `repeat` | `true` or `false`, optional | Every hit replays the noise of the first hit |

Millidecibels and microseconds convert to the units below by a division by 1000 and by 1000000. A time `t` sits at sample `t * rate_hz / 1000000`, a fraction.

## The arithmetic

Every computation uses IEEE 754 doubles with add, subtract, multiply, divide and compare, in the order this document gives. Sums run from the first term to the last with one rounding per addition. A platform's own sine or exponential may differ in its last bit, and one bit can change the rounding of a sample, so the tool brings its own four functions:

| Function | Method | Error bound |
|---|---|---|
| `sin(x)`, `cos(x)` | `k = round(x * 2/pi)`, `r = x - k*P1 - k*P2 - k*P3` with pi/2 split into three 33-bit parts. Then the Taylor sum of sin or cos of `r` to the term of degree 19 or 20, in Horner form on `r*r`. `k mod 4` picks the function and the sign. For abs(x) up to 1600000. | Absolute error under 5e-16 |
| `ln(x)` | Scale `x` by exact powers of 2 to `m` in `[sqrt(1/2), sqrt(2))`, then `2 atanh(s)` with `s = (m-1)/(m+1)`, summed to `s^25/25`. Add `e * ln 2` with ln 2 in two parts. | Under 4 ulp plus 2e-16 |
| `exp(x)` | `k = round(x / ln 2)`, `r = (x - k*LN2_HI) - k*LN2_LO`, the Taylor sum of `e^r` to degree 20 in Horner form, then `k` exact doublings or halvings. 0 under -745. | Under 3 ulp for abs(x) up to 700 |
| `sqrt(x)` | Scale `x` by exact powers of 4 to `m` in `[1, 4)`, start at `(1+m)/2`, take 6 Newton steps `y = (y + m/y) / 2`, scale back. | Under 1 ulp. Exact on the square of a short integer. |

The constants are the decimal literals in `tools/sfx_table.py`. A rounding to an integer takes the nearest, with a half away from zero. A level `d` in dB becomes the gain `db_gain(d) = exp(d * ln(10) / 20)`, with `ln(10)` from the tool's own `ln`.

The sine reproduces the library's 1024-entry table: `floor(32767 * sin(2 pi k / 1024) + 0.5)` for `k` from 0 to 1023, as 2048 little-endian bytes, has CRC-32 `05067686`.

## The algorithm

Every sound starts as `len` zeros, `out`. Each layer adds its hits. A hit that runs past `len` stops there, or wraps to the start in a loop.

### The envelope

`env` is straight lines in dB between its points. A point sits at sample `s = t * rate_hz / 1000000`. Sample `i` belongs to the segment from point `a` to point `b` when `ceil(s_a) <= i < ceil(s_b)`. Its gain is `db_gain(d_a + (d_b - d_a) * (i - s_a) / (s_b - s_a))`. The ceilings are exact integer divisions. Two points at the same time make an empty segment.

Before the first point a hit is silent. After the last point a tone holds the last level, and a noise hit is silent.

### A tone layer

With `n` the hit's `len`, `f0 = from / 1000` and `f1 = to / 1000`:

1. `ratio = exp(ln(f1 / f0) / n)`.
2. In a loop, a tone whose `len` equals the sound's `len` moves to whole cycles. `c` is the sum of `f / rate_hz` over the `n` samples, with `f` stepping as below. `w = round(c)`. The tool refuses a move `w / c - 1` over 0.05 % either way, then sets `f0 = f0 * (w / c)`. A shorter tone does not move.
3. The ramps are `att = attack_us * rate_hz / 1000000` and `rel = release_us * rate_hz / 1000000`.
4. Each partial has `mult = multiple / 10000` and `amp = db_gain((level_mdb + level) / 1000)`.
5. With `phase = 0` and `f = f0`, for each `i` from 0 to `n - 1`:
   - `g = 1`. If `i < att`, `g = (i + 0.5) / att`. If `n - i < rel`, `g` takes the smaller of itself and `(n - i - 0.5) / rel`. With `env`, `g = g * env[i]`.
   - If `g` is not 0, `v` is the sum, in the order of `partials`, of `amp * sin(phase * mult)` for each partial with `f * mult < rate_hz / 2`. Else `v = 0`.
   - `one[i] = v * g`, then `phase = phase + (2 pi * f) / rate_hz` and `f = f * ratio`.
6. Each hit `[start, level]` adds `one[i] * db_gain(level / 1000)` to `out[start + i]` for every `i` where `one[i]` is not 0.

### The noise generator

The generator is xorshift32: `s ^= s << 13`, `s ^= s >> 17`, `s ^= s << 5`, each on 32 bits. Hit `k` of a layer, counted from 0, starts from `s = (seed * 2654435761 + (k + 1) * 0x9E3779B9) mod 2^32`. A zero `s` becomes `0x1234567`. The generator then steps four times. Each sample steps it once more and gives `((s >> 8 & 0xFFFF) - 32768) / (65536 / sqrt(12))`, which has unit rms.

| Seed, hit 0 | Start after four steps | Next four states | Their 16-bit values |
|---|---|---|---|
| 1 | `5c5329b6` | `15db5584`, `9216ac11`, `b50154bb`, `6d591570` | 23381, -26964, -32428, -9963 |
| 2 | `787289ee` | `00a4f469`, `5b1af4dd`, `b5ab95bd`, `24969053` | 9460, -25868, 11157, 5776 |

### The filter

The filter has 511 taps, built from `bands`:

1. For `k` from 0 to 512 the bin frequency is `k * rate_hz / 1024`. Its level is the curve: straight lines in dB between points, and no level below the first or above the last point. `amp[k] = db_gain(level)`, or 0 with no level.
2. `c[j] = cos(2 pi * j / 1024)` for `j` from 0 to 1023.
3. For `m` from 0 to 255: `v = amp[0] + amp[512] * c[512 m mod 1024]`, then for `k` from 1 to 511 `v = v + 2 * amp[k] * c[k m mod 1024]`. The Hann window gives `v = v * (0.5 + 0.5 * cos(pi * m / 256))`. Tap `255 + m` and tap `255 - m` take `v`.
4. `e = sqrt(h[0]^2 + h[1]^2 + ... + h[510]^2)`, summed in that order, and every tap becomes `h[j] / e`. The filter has unit power. A curve that gives no bin is refused.

The output sample `i` of a hit is `h[0] * w[i] + h[1] * w[i + 1] + ... + h[510] * w[i + 510]`, added in that order, where `w` is `n + 510` generator samples. The tool runs the loops the other way round for speed. Each sum still adds the same products in the same order, and a test proves the bits equal.

### A noise layer

1. `n = ceil(t_last * rate_hz / 1000000)`, with `t_last` the last `env` time.
2. `gain[i]` is the envelope for `i` from 0 to `n - 1`.
3. The fold: for `i` from 0 to `fold - 1`, `u = (i + 0.5) / fold`, `gain[i] = gain[i] * sin(pi/2 * u)` and `gain[n - fold + i] = gain[n - fold + i] * cos(pi/2 * u)`. A hit `fold` samples longer than its loop then overlaps itself at constant power.
4. For hit `k`: with `repeat` and `k > 0`, reuse hit 0's samples. Else `one[i] = filtered[i] * gain[i]`, or 0 where `gain[i]` is 0.
5. Each hit adds `one[i] * db_gain((level_mdb + level) / 1000)` as a tone does.

In a one-shot, the samples of a hit past the sound's end never sound, so the tool does not make them. It filters only the `m = min(n, len - at)` samples that sound, from the first `m + 510` generator values. The generator runs in order, so those values are the same as in a whole run, and each kept sample adds the same products in the same order. With `repeat`, hit 0 is made as long as the longest kept hit. A loop wraps what runs past its end, so every sample of its hits sounds and is made. A test compares the bits with the whole render.

### Scaling and rounding

`peak` is the largest absolute value in `out`. A silent sound is refused. Each sample becomes `128 + round(out[i] * (peak_steps / peak))`. A tool that wants the values before rounding takes `out[i] * (peak_steps / peak)`.

## The checks

The tool refuses a sound, with one message per fault, when:

- a sample sits further than `ceiling_steps` from 128;
- the loudest sample is not exactly `peak_steps` from 128;
- a loop has `peak_steps` over 32;
- a loop's join, from its last sample to its first, steps further than the largest step between two neighbours inside it;
- a loop tone needs a move over 0.05 % to reach whole cycles;
- a one-shot's first sample sits more than 2 steps from 128, or its last more than 8.

The tool also refuses a tone whose sine can pass its limit of 1600000. It bounds the argument `phase * mult` of each partial on its own, from the model alone, with `n` the hit's `len`, `f0 = from / 1000` and `f1 = to / 1000`. The largest multiple is not enough: in a falling glide it can stay above `rate_hz / 2` while a lower one sounds.

- With a pitch that rises or holds, a partial only sounds below `rate_hz / 2`, and every earlier step was no higher. The bound is `pi * n`, always under the limit. A partial that goes silent part way never sounds again.
- With a falling pitch, a partial can start silent and sound from some sample on, so the bound takes the whole phase sum. A partial with `f1 * mult * 0.99` at or above `rate_hz / 2` never sounds. Else the bound is `1.01 * mult * 2 pi * (f0 + n * (f0 - f1) / ln(f0 / f1)) / rate_hz`. The sum of a falling geometric series is at most its first term plus its integral. The 1 % covers the loop move and rounding.

Each partial past the limit gets its own line, which names it, such as `partials[0]`.

The tool also refuses a model whose render costs more than 1000000000 units, with one line. The cost is:

- 511 for each kept noise sample, one per filter tap, counted once for a `repeat` layer;
- 32 for each tone sample of each partial, since a sine takes about 18 filter products of time;
- 32 for each sample of each sound;
- 131072 for each noise layer, for its filter design.

On the reference machine, a 2.3 s noise loop at 37500 Hz costs 44073750 units and renders in 1.9 s, about 42 ns a unit. A model near the budget renders in 41 s with noise and in 22 s with tones, well under two minutes. The largest made-up test sound costs 982920 units. A 3 s loop at 48000 Hz of two noise layers and a 16-partial tone costs 225766144 units, under a quarter of the budget.

Every other function call stays in range for any model the file rules allow: `exp` sees -28 to 5, `ln` a positive pitch ratio, `sqrt` a positive sum.

Behind these checks is a backstop. `sin`, `cos`, `ln`, `exp` and `sqrt` refuse an argument out of their range. The tool then refuses the model with one line that names the sound and the layer and says the arithmetic left its range, exit 2. A valid model never reaches it: a test feeds the tool a few hundred random models and checks that validation or a check refuses each one first. Any other unexpected error prints one line, `internal error`, with the model and the error, exit 70. The tool never prints a Python traceback.

Each fault in the file gets its own line too: an unknown field, a missing field, a wrong type, a number out of range, a list too long or too short. Each line names the sound and the field. A missing or bad field hides no other fault: every field that can be judged on its own is judged. Only a check that needs the bad value is skipped, such as the sine bound without a tone's `len`, or the render cost while any fault stands. A bad `format` or a layer's bad `kind` stops the checks below it, since the other fields then have no meaning. The exit code is 2.

## The commands

```
tools/sfx_table.py MODEL --c OUT.c --h OUT.h --prefix NAME_sfx_
tools/sfx_table.py MODEL --c OUT.c --h OUT.h --prefix NAME_sfx_ --check [--full]
tools/sfx_table.py MODEL --dump DIR
tools/sfx_table.py MODEL --wav DIR
```

| Option | Does |
|---|---|
| `--c`, `--h`, `--prefix` | Writes the tables in C. The header defines `NAME_SFX_MODEL_SHA256`, `NAME_SFX_TOOL_REVISION`, `NAME_SFX_RATE_HZ`, and for each sound `_PEAK` and `_LOOP`. For each table it defines `_LEN` and `_CRC32` and declares `const uint8_t name_sfx_<table>[]`. Both files start with a comment that says they are generated and names the model file. |
| `--check` | Compares the SHA-256 of `MODEL` and the tool revision with the lines in `OUT.h`, and with the opening comment of both files. Fast: it renders nothing. It first checks the paths as a write does, and refuses with exit 64 the paths a write would refuse. |
| `--check --full` | Also renders every sound again and compares both files byte for byte |
| `--dump DIR` | Writes each table as a raw `<table>.u8` file |
| `--wav DIR` | Writes each sound as an 8-bit mono WAV at `rate_hz`, whole, not split. A loop plays three times so its join is heard. |

Every name the tool writes carries the marker `sfx`. No C or POSIX keyword, type, function, macro or reserved pattern holds it, so the tool needs no list of taken names. Earlier lists of keywords, `<stdint.h>` names and standard headers each missed a name, such as `size_t` from the sound `t` and the prefix `size_`.

`--prefix` matches `([a-z][a-z0-9]*_)*sfx_`, at most 32 characters, such as `sfx_`, `uos_sfx_` or `dx_sfx_`. So every table name holds `sfx_` and every macro `SFX_`.

The tool checks file names as written, before it reads the model:

- The file name of `OUT.c` is a lower-case letter, then lower-case letters, digits or `_`, holds `sfx`, and ends in `.c`, such as `sfx_table.c`. It has no capital, no `-` and only the one `.` before `c`.
- The file name of `OUT.h` follows the same rule and ends in `.h`, such as `uos_sfx_table.h`. It goes into `#include "..."` in `OUT.c`, so it needs no escape and is ASCII.
- The file name of `MODEL` holds letters, digits, `_`, `.` and `-` only, and starts with a letter or a digit, because it goes into the opening comment as written.

No standard or system header has a name that holds `sfx`. So a generated header never takes the place of one on the include path, and the tool keeps no list of header names.

A bad prefix or file name gets one line, exit 64. The include guard is `SFX_GUARD_` followed by the file name of `OUT.h` in capitals with the `.` as `_`, such as `SFX_GUARD_UOS_SFX_TABLE_H`, so it is never a name C reserves.

Two different file names give two different guards. The name rule leaves no capital and one `.` only, so the guard is the name, and a project can include any two generated headers. Two generated headers with the same file name in two folders share a guard, as with any C header: start the file name with the prefix, such as `uos_sfx_table.h`.

The tool then checks that the names it writes do not clash with each other: two sounds that give one table name, or the `_1` and `_2` tables of a split against another sound. The guard cannot clash: no other name starts with `SFX_GUARD_` and ends with `_H`. Each clash gets one line that names it and the options and sound it comes from, exit 64. Two sounds whose table names clash are a model fault, exit 2.

`--dump` and `--wav` can join the first form. They name each file after a table or a sound, such as `beep_1.u8` or `beep.wav`. A sound name is already `a` to `z`, `0` to `9` and `_`, so these names need no other check.

### The plan

Before it writes anything, the tool checks its plan as one set of nodes. Each node is a path and a kind, file or folder:

- files: `MODEL`, which it reads, and `OUT.c`, each `.u8` file, each `.wav` file and `OUT.h`, which it writes;
- folders: the `--dump` and `--wav` folders, and the folder each file lies in.

It compares two paths by one rule. Two paths are the same when:

- both exist and are the same file to the system, such as through a hard link or a symbolic link;
- they resolve to the same path, through relative and absolute spellings, `.`, `..` or a symbolic link;
- their resolved paths differ only by case.

The case rule is strict on every system, also where the file system tells case apart. A run that passes on one system then passes on all.

The tool takes a path by where it resolves. It resolves each path once in the plan, and the plan, the folders it makes, the temporary and backup files, the renames, loading `MODEL` and `--check` all use that resolved path. The tool checks the plan it resolved and never resolves a path a second time. It cannot protect against another program that changes the folders while it runs. A missing folder in the middle, as in `missing/../out`, resolves past it as text, so that run writes in `out`. A symbolic link output keeps its link and the tool writes its target. Messages show each path as written.

It refuses the run, exit 64, with one line that names the paths and the rule, when:

- two files are the same, which covers an output that is `MODEL`;
- one path is both a file and a folder, such as `--c out_sfx.c` with `--dump out_sfx.c`, or the folder of `MODEL` as `--c`;
- a node lies under a file, such as `--dump d/sfx.c/x` with `--c d/sfx.c`;
- an output is a directory, or a folder is a file;
- the folder of an output does not exist and is not a `--dump` or `--wav` folder the run makes;
- a `--dump` or `--wav` folder lies in a folder that does not exist and that the run does not make;
- two new folders differ only by case: where the file system tells case apart they are two folders.

It makes a missing `--dump` or `--wav` folder itself. `--dump` and `--wav` may name the same folder: their files end in `.u8` and `.wav`, so they never clash. Two spellings of one folder, such as `out`, `./out`, `out/` or one through a symbolic link, make one folder. A new `--dump` folder may lie in a new `--wav` folder, and the other way round: the tool makes the outer one first. A folder that another process makes between the plan and the run is used as it is, and a roll back never removes it. The only file a run reads is `MODEL`, and it reads it before it writes, so the check that no output is `MODEL` covers every read.

### How the files are written

A run publishes all its files as one. The tool writes each file to a temporary name `.<name>.<pid>.tmp` in the same folder. It then keeps the old content of each file that exists under `.<name>.<pid>.bak`, a hard link or else a copy. Then it renames each temporary file over its target: `OUT.c`, each `.u8` file, each `.wav` file, and `OUT.h` last, because `OUT.h` carries the hash that `--check` reads. A file is never left half written.

The tool records each temporary file, backup and folder the moment it exists, before it writes into it. A failure at any step, even in the middle of a backup copy, then finds it.

On a failure the tool rolls back. Roll back means: each file this run already renamed gets its old content back from its backup, or is removed if it did not exist before; every temporary and backup file is removed; and a `--dump` or `--wav` folder this run made is removed. A test makes each file operation of a run fail in turn and checks that the folders hold exactly what they held before. The tool then exits 1 with one message. On success it removes the backups.

The table below is for the full run, `--c --h --dump --wav` together. "Old" means the content from before the run, or no file if there was none. `--check` reads only `OUT.h` and `OUT.c`, which are always old after a failure, so it answers as it did before the run: stale if the model changed.

| Failure | `OUT.c` | `.u8` files | `.wav` files | `OUT.h` | `--check` |
|---|---|---|---|---|---|
| A temporary file cannot be made or written | old | old | old | old | stale if the model changed |
| A backup cannot be made as a hard link | the tool makes a copy and goes on | | | | |
| A new folder cannot be made | old | old | old | old | stale if the model changed |
| A backup copy cannot be made or written | old | old | old | old | stale if the model changed |
| The rename of `OUT.c` fails | old | old | old | old | stale if the model changed |
| The rename of a `.u8` or `.wav` file fails | old | old | old | old | stale if the model changed |
| The rename of `OUT.h` fails | old | old | old | old | stale if the model changed |
| A restore fails during the roll back | new for that file, and its `.bak` file stays with the old content | | | old | stale if the model changed: `OUT.h` is never renamed when a later step fails |
| The tool is killed after some renames | new up to the kill | | | old | stale if the model changed; `.tmp` and `.bak` files stay, and the `.bak` files hold the old content |

Files from an older model under other names stay in the `--dump` and `--wav` folders.

### What the checks prove

The fast check proves that `OUT.h` and the opening comment of `OUT.c` both name the SHA-256 of `MODEL` and this tool revision. It reads lines, so it accepts a CRLF copy. It does not prove the table bytes: an edited table passes it.

`--check --full` renders every sound again and compares both files byte for byte. The tool always writes LF line ends and ASCII text. A CRLF copy fails the full check. Git can make such a copy: with `core.autocrlf` on, a Windows checkout turns text files into CRLF. This repository's `.gitattributes` keeps line ends only for test vectors and fixtures, not for generated C. A repository that commits the generated files and runs `--check --full` on Windows should mark them `-text`, or the check fails there. That failure is honest: the bytes differ. Exit codes: 0 success, 1 a table is out of date or a file cannot be read or written, 2 the model is refused, 64 a usage error, 70 an internal error.

The tool revision is 1. It goes up whenever a byte of any table can change.

A script can import the tool. `load_model(path)` returns the checked model and its SHA-256. `render_sound(model, name)` returns the bytes of one sound before the checks. `render_steps(model, name)` returns the scaled values before rounding. `check_sound(model, name, data)` returns the faults.

## Checking a table

```
tools/sfx_check.py MODEL DIR
```

`tools/sfx_check.py` checks that the `.u8` tables in `DIR`, as `--dump` writes them, sound like `MODEL`. It reads the model and the table files, nothing else. It shares no code with `tools/sfx_table.py` and uses Python's `math` module: it judges by tolerance, not by bytes.

For each sound it:

1. Joins the `_1` and `_2` tables of a split sound.
2. Checks the length equals `len` and the loudest sample sits exactly `peak_steps` from 128.
3. Measures the table: the power in bands over the whole table, and the rms level in frames. A sound of tones only uses 250 Hz bands and 5 ms frames. A sound with noise gets its own widths, as below. It keeps only the bands and frames within 30 dB of the strongest one and ignores everything above 15 kHz.
4. Predicts the same two measurements from the model. It renders tone layers by the steps above. It takes a noise layer as its expected power and spectrum, as below. It draws no random numbers. A loop wraps what runs past `len`, so a `fold` cross-fades a hit with itself in the prediction too.
5. Scales the prediction and compares it with the measure in dB. The bands and the frames each get their own gain. The band gain matches the power under 15 kHz. The frame gain matches the power of the whole spectrum, so noise above 15 kHz still counts in the frames.

For one noise layer, `h` is its 511 filter taps at unit power, `g[i]` the gain of one hit at its sample `i` without the hit's level, `a_k` the linear level of hit `k`, `s_k` its start, `n` the table length and `M` the FFT size of a one-shot's measurement. The gain `g` is the envelope times the fold fade. The generator filters a longer run of noise than the hit, so every sample of a hit holds the full filter and `g` needs no edge term.

A short envelope smears the spectrum. The tool computes `rh[d] = sum h[m] h[m+d]` and `rg[d] = sum g[i] g[i+d]` for the lags -510 to 510. The expected spectrum of one hit of level 1 is the DFT of `rh[d] * rg[d]` placed circularly, real part, clamped at 0. The tool scales it as it scales the measured spectrum, so its bins sum to `rh[0] * rg[0]`.

A one-shot uses `M` points: the table length rounded up to a power of 2, with zero padding. A loop is periodic, so its spectrum lives on the `n` bins of an `n`-point DFT, with no padding. The tool measures the loop's table on the same `n` bins. There a wrapped hit is only a phase. The tool takes `rg` from the whole unwrapped hit, however often it wraps, adds `rh[d] * rg[d]` into `c[d mod n]` and takes the `n`-point DFT of `c`. An `n`-point DFT of any `n` uses Bluestein's chirp method on a power-of-2 FFT.

In a `repeat` layer, hits with the same start play one waveform. The tool merges them into one hit whose level is the sum of their linear levels. It does this once, before it predicts the spectrum or the power.

Hits of a layer without `repeat` are unrelated: their spectra add, each times `a_k^2`. In a one-shot, each such hit takes `rg` of the part that lands in the table. In a `repeat` layer every hit replays one waveform, so the sum has a comb spectrum. The tool multiplies the spectrum of one hit at bin `j` by `|sum_k a_k * exp(-2 pi i j s_k / P)|^2`, where `P` is `M` for a one-shot and `n` for a loop. Layers add as powers, and so do noise and tones.

A one-shot cuts a hit that runs past the table end. In a `repeat` layer the cut hits keep different parts of one waveform, so the comb no longer holds. When the table end cuts any hit of a `repeat` layer, the tool does not judge the bands of that sound. Its line says `bands not judged, repeated hits cut by the table end`. The tool still judges the frames.

The expected power of a table sample sums over every pair of stream samples that land on it and share a noise stream. Two samples share a stream when they come from the same hit, or from any two hits of a `repeat` layer. With stream indexes `j` and `j'`, at most 510 apart, the pair adds `a a' g[j] g[j'] rh[j' - j]`. A loop wraps each hit, so a hit longer than the loop pairs with itself. A one-shot drops what runs past the end. The frames and `pmax` use this power.

The tool counts the steps of this sum, of the autocorrelations of `g` and of the combs before it does the work. When one sound needs more than 200000000 steps, the tool stops with exit code 2 and says `sounds.NAME: too costly to check, N steps, the limit is 200000000`.

A fixed tolerance needs enough independent values in each band and each frame. Let `p[i]` be the expected noise power of sample `i` and `S[j]` the expected noise spectrum up to half the rate, both summed over the noise layers. Then:

- The effective duration is `T = (sum p)^2 / (sum p^2) / rate` seconds.
- The effective bandwidth is `B = (sum S)^2 / (sum S^2) * rate / P` Hz.
- The band width is `W = max(250, K_band / T)` Hz and the frame length `F = max(0.005, K_frame / B)` seconds.
- `K_band` and `K_frame` count the independent complex values a judged value must hold. A one-shot with noise takes `K_band = 10` and `K_frame = 20`. A loop takes `K_band = 200` and `K_frame = 20`.

Each limit sits about five standard deviations out, so a table with dozens of bands or hundreds of frames does not fail by chance. A 10 dB limit needs 20 independent real numbers, a 6 or 7 dB limit 40, and a 1.5 dB limit about 400.

A band or frame whose noise fills only part of it holds fewer values than its width says. So the tool counts the values of each one:

- Band `b` holds `N_b = T * (rate / P) * (sum S[j])^2 / (sum S[j]^2)` over the bins `j` of the band. `P` is `M` for a one-shot and `n` for a loop.
- Frame `f` holds `N_f = (B / rate) * (sum p[i])^2 / (sum p[i]^2)` over the samples `i` of the frame.
- Tones are fixed, not random. With `q` the noise part of the predicted value, before the rounding floor, the value counts `N_eff = N / (q * (2 - q))`. A value with no noise counts as infinite.

The tool judges every band and frame within 30 dB of the strongest, in one of two tiers:

- A tight value has `N_eff` at or above `K_band` or `K_frame`. The tolerance table judges it. Only tight values enter the mean, median and worst.
- A thin value has `N_eff` under the count. Chance bounds judge it.

The tool models a thin value as a gamma variable. Its mean is the whole prediction: the model part times the gain, plus the rounding floor. Its shape is `k = max(N_eff / 2, 0.5)`. The halving leaves room for what the count ignores. With `r` the measured value over the predicted one, the two tails are `P(k, k * r)` and `Q(k, k * r)`, the regularized lower and upper incomplete gamma functions. The value fails when either tail is under one in a million.

Bands run from 0 in steps of `W` up to 15 kHz, and the tool drops a last partial band. Frames run from sample 0 in steps of `F`, rounded to whole samples. A last frame shorter than half of `F` joins the frame before it. The line prints the widths after the kind, as `bands 250 Hz, frames 5.0 ms`.

The prediction of a sound with a noise layer includes the rounding of each sample to a whole step. It adds this rounding as white noise of power 1/12 step squared per sample, spread evenly from 0 to half the rate, and each gain matches the model part only. A quiet table needs it: its weak bands sit close to the rounding noise, so a prediction without it reads them too low. The white floor is only an average, since at a low level the rounding error follows the waveform. So the tool does not judge a band or frame whose floor is more than half of its whole prediction, as it skips a value more than 30 dB down.

A sound of tones only rounds along its waveform, so its rounding is not white noise. For such a sound the tool predicts the rounded table itself. It scales the render so its loudest sample sits at `peak_steps`, rounds each sample to a whole step with a half away from zero, and measures that. It adds no white floor and applies no gain.

The tool compares the share of the power under 15 kHz in the table with the share the model predicts over the whole spectrum. The predicted share takes the model part, scaled by the frame gain, plus the rounding floor where the floor applies. When only one of the two shares is under 1 %, the sound fails with `power under 15 kHz is M % of the table, the model predicts P %`.

A sound with a noise layer also gets a level check, since the frame gain hides the level of a one-frame sound. The model fixes the peak at `peak_steps`, so the tool computes `level_db = 10 * log10(peak_steps^2 / (frame_gain * pmax))`. Here `pmax` is the largest predicted power of one sample, model part only, before the gain. It says how far the peak sits above the strongest expected rms. The line prints it as `level L.LL dB` after the frame numbers. The sound fails when `level_db` is under -6 dB or over 14 dB. A sound of tones only prints no level.

A sound where the table and the model both hold less than 1 % of the power under 15 kHz is judged without its bands. Its line says `bands not judged, no power under 15 kHz`. The line still prints the band numbers it measured.

It checks the fields it needs and their types, and bounds every number it reads with the ranges of `tools/sfx_table.py`. It also checks the rules its prediction relies on: band frequencies that rise, a `fold` of at most half the samples of the noise envelope, a noise envelope of 1 to 262144 samples, an attack and a release inside the tone, envelope times that do not go down, hits inside the sound, a pitch above zero, lengths above zero, and at least one layer and one hit. An invalid model exits with code 2 and names the sound and the field. It does not repeat every check of `tools/sfx_table.py`.

A loop is a sound with `loop` set. A one-shot with noise has a noise layer and no `loop`.

| Kind | Band tolerance | Frame tolerance |
|---|---|---|
| Tones only | 0.5 dB each | 0.5 dB each |
| One-shot with noise | Mean 3 dB, worst 10 dB | Mean 2 dB, worst 6 dB |
| Loop | 1.5 dB each | Median 1.5 dB, worst 7 dB |

It prints one line per sound: its name and kind, the band width and frame length, the mean, median and worst band error and frame error in dB over the tight values, then `thin N bands, M frames` when either count is not 0, the level for a sound with noise, then any note, then `ok` or `FAIL` with the reason. With no tight value, the mean, median and worst print as `-` and the tolerance table judges nothing for that kind. A thin value beyond chance adds `band at F Hz is E.EE dB off, beyond chance` or `frame at T.T ms is E.EE dB off, beyond chance`, with a signed error. Each kind gives at most one such reason, the worst.

Exit codes: 0 every sound passes, 1 a sound fails, 2 the model is not valid, a table is missing or has the wrong length, or a sound is too costly to check, 64 a usage error, 70 an internal error.

## A worked example

A 2000 Hz tone at 8000 Hz turns a quarter turn per sample:

```json
{ "format": 1, "rate_hz": 8000, "ceiling_steps": 90,
  "sounds": { "beep": { "len": 8, "peak_steps": 8, "layers": [
      { "kind": "tone", "hits": [[0, 0]], "len": 8, "pitch_mhz": [2000000, 2000000],
        "level_mdb": 0, "partials": [[10000, 0]], "attack_us": 500 } ] } } }
```

- `att = 500 * 8000 / 1000000 = 4` samples, so the gains of samples 0 to 3 are 0.125, 0.375, 0.625 and 0.875.
- The sines are about 0, 1, 0, -1, repeated.
- The peak is sample 5, at 1. The scale is 8.
- Sample 1 is `0.375 * 8 = 3`, sample 3 is `-0.875 * 8 = -7`.

The table is `128, 131, 128, 121, 128, 136, 128, 120`.

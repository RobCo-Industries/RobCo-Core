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
| `env` | 1 to 64 pairs, optional | `[time, level]`: time in microseconds from the hit, level in millidecibels. Times must not go down. |

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

Every other function call stays in range for any model the file rules allow: `exp` sees -28 to 5, `ln` a positive pitch ratio, `sqrt` a positive sum.

Behind these checks is a backstop. `sin`, `cos`, `ln`, `exp` and `sqrt` refuse an argument out of their range. The tool then refuses the model with one line that names the sound and the layer and says the arithmetic left its range, exit 2. A valid model never reaches it: a test feeds the tool a few hundred random models and checks that validation or a check refuses each one first. Any other unexpected error prints one line, `internal error`, with the model and the error, exit 70. The tool never prints a Python traceback.

Each fault in the file gets its own line too: an unknown field, a missing field, a wrong type, a number out of range, a list too long or too short. Each line names the sound and the field. The exit code is 2.

## The commands

```
tools/sfx_table.py MODEL --c OUT.c --h OUT.h --prefix NAME_
tools/sfx_table.py MODEL --c OUT.c --h OUT.h --prefix NAME_ --check [--full]
tools/sfx_table.py MODEL --dump DIR
tools/sfx_table.py MODEL --wav DIR
```

| Option | Does |
|---|---|
| `--c`, `--h`, `--prefix` | Writes the tables in C. The header defines `NAME_MODEL_SHA256`, `NAME_TOOL_REVISION`, `NAME_RATE_HZ`, and for each sound `_PEAK` and `_LOOP`. For each table it defines `_LEN` and `_CRC32` and declares `const uint8_t name_<table>[]`. Both files start with a comment that says they are generated and names the model file. |
| `--check` | Compares the SHA-256 of `MODEL` and the tool revision with the lines in `OUT.h`, and with the opening comment of both files. Fast: it renders nothing. |
| `--check --full` | Also renders every sound again and compares both files byte for byte |
| `--dump DIR` | Writes each table as a raw `<table>.u8` file |
| `--wav DIR` | Writes each sound as an 8-bit mono WAV at `rate_hz`, whole, not split. A loop plays three times so its join is heard. |

`NAME_` is `a` to `z`, `0` to `9` and `_`, starts with a letter, at most 32 characters. A leading `_` would make reserved C names. The include guard is the file name of `OUT.h` in capitals with every other character as `_`. The tool refuses a file name whose guard does not start with a letter.

The tool then checks every name it would write, after it joins the parts: each table name, each macro and the include guard. It refuses a name that is:

- a C keyword of any standard through C23, with the C23 spellings such as `bool`, `static_assert`, `thread_local`, `constexpr`, `nullptr` and `typeof`, and the extensions `asm` and `fortran`;
- a reserved name, one that starts with `_`;
- a name of `<stdint.h>` or a pattern C reserves for it, such as `uint8_t`, `int32_t` or `UINT8_MAX`;
- a name the same run writes twice, such as a guard equal to a table macro.

Each refused name gets one line that names it and the options and sound it comes from. The exit code is 64: a change of `--prefix` or `--h` fixes it. Two sounds whose table names clash are a model fault, exit 2. The file name of `MODEL` must be printable ASCII, because it goes into the opening comment.

`--dump` and `--wav` can join the first form.

### How the files are written

The tool writes each file to a temporary name in the same folder, then renames it over the target. A file is never left half written. It writes `OUT.c` first and `OUT.h` last, because `OUT.h` carries the hash that `--check` reads. On a failure it removes its temporary files and exits 1 with one message.

| Failure | `OUT.c` | `OUT.h` | `--check` |
|---|---|---|---|
| A temporary file cannot be made | old | old | stale if the model changed |
| A write fails | old | old | stale if the model changed |
| The rename of `OUT.c` fails | old | old | stale if the model changed |
| The rename of `OUT.h` fails | new | old | stale: the header names the old hash |
| The tool is killed between the two renames | new | old | stale, and a `.OUT.h.<pid>.tmp` file stays |

`--dump` and `--wav` write their files the same way. On a failure the tool also removes every file it already renamed in that run. The folder then lacks the files of this run, so it does not look complete. Files from an older model under other names stay.

### What the checks prove

The fast check proves that `OUT.h` and the opening comment of `OUT.c` both name the SHA-256 of `MODEL` and this tool revision. It reads lines, so it accepts a CRLF copy. It does not prove the table bytes: an edited table passes it.

`--check --full` renders every sound again and compares both files byte for byte. The tool always writes LF line ends and ASCII text. A CRLF copy fails the full check. Git can make such a copy: with `core.autocrlf` on, a Windows checkout turns text files into CRLF. This repository's `.gitattributes` keeps line ends only for test vectors and fixtures, not for generated C. A repository that commits the generated files and runs `--check --full` on Windows should mark them `-text`, or the check fails there. That failure is honest: the bytes differ. Exit codes: 0 success, 1 a table is out of date or a file cannot be read or written, 2 the model is refused, 64 a usage error, 70 an internal error.

The tool revision is 1. It goes up whenever a byte of any table can change.

A script can import the tool. `load_model(path)` returns the checked model and its SHA-256. `render_sound(model, name)` returns the bytes of one sound before the checks. `render_steps(model, name)` returns the scaled values before rounding. `check_sound(model, name, data)` returns the faults.

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

# Changelog

## Unreleased

- `audio_rest_mid()`: the first call plays a 40 ms ramp from duty 0 to duty 128, and from then on every silence the PWM backend writes is duty 128 at the PCM range. A sample that starts near the midpoint then makes no jump. A kernel that never calls it keeps today's pin levels and trace. The idle pin then carries the 37.5 kHz carrier all the time. None of this ran on a board.
- `audio_pcm_mix()` and `audio_pcm_mix_busy()`: one overlay buffer added to the PCM that plays, clamped, before the volume. It also takes a play that waits for its tick. The trace prints `op=pcm_mix len crc t_ms` at its start. Not heard on a board yet.
- The audio guide allows `audio_play_pcm()` and `audio_pcm_mix()` in a tick callback for short buffers, and says why the buffer must be short.
- `examples/audio` gains the `pcm_mix` and `rest` cases, and `tests/target/test_audio.py` checks them.
- `audio_pcm_scale()` rounds to the nearest step, a half step away from 128. Before, it cut toward 128, which flattened a quiet sine at any volume under 100. Every sample play at a volume under 100 now puts slightly different duty values on the pin. `audio_pcm_scale(255, 50)` is now `192`, was `191`.
- `robco-ci.yml` marks the checkout and every submodule folder, at any depth, as safe for git in both jobs, then checks that git works in each. Before, git refused the checkout in the container with "dubious ownership".
- The harness plugin removes a passing test's `tmp_path` when the test ends. A test that fails or errors in setup, call or teardown keeps its folder. Set `ROBCO_KEEP_TMP=1` to keep every folder. A project that sets `tmp_path_retention_policy` itself keeps its own value.
- `rimg_seal()` and `rimg_pack()` in `src/lib/rimg_seal.c`: the sealer and the packer in C. A host test runs every vector of `tests/vectors/rimg/vectors.ini` through them.
- `tools/rimg.py` reads a PBM icon width or height as 1 to 9 ASCII digits, as the spec asks. It now refuses `+32`, `-32`, `3_2`, `0000000032` and a token with a vertical tab or a form feed with `io`, as the C sealer does. Seven new vectors check it.
- `.gitattributes` turns off end-of-line conversion for test vectors, golden images and byte-compared fixtures, so a Windows checkout with `core.autocrlf=true` keeps their bytes. `tests/tools/test_vector_eol.py` checks it for `tests/vectors/`.

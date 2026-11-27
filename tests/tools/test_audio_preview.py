"""Tests of tools/audio_preview.py on hand-written serial logs.

Standard library only: `make test-tools` runs them with pytest. Most
checks read the duty the tool computes per carrier period, before the
high-pass, so the numbers are exact.
"""

import contextlib
import importlib.util
import io
import os
import tempfile
import time
import unittest
import wave
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "audio_preview.py")

_spec = importlib.util.spec_from_file_location("audio_preview", TOOL)
bs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bs)

RATE = 22050
FR = bs.OUT_RATE


def crc(data):
    return "%08x" % zlib.crc32(data)


def pcm(t, data, vol=100, rate=RATE, loop=False):
    return ("@RC stage=test ev=audio op=pcm rate=%d len=%d vol=%d crc=%s t_ms=%d%s"
            % (rate, len(data), vol, crc(data), t, " loop=1" if loop else ""))


def mix(t, data):
    return "@RC stage=test ev=audio op=pcm_mix len=%d crc=%s t_ms=%d" % (len(data), crc(data), t)


def nxt(t, data):
    return "@RC stage=test ev=audio op=pcm_next len=%d crc=%s t_ms=%d" % (len(data), crc(data), t)


def end(t, n=0):
    return "@RC stage=test ev=audio op=pcm_end n=%d last=0 t_ms=%d" % (n, t)


def tone(t, hz, vol=100):
    return "@RC stage=test ev=audio op=tone hz=%d vol=%d t_ms=%d" % (hz, vol, t)


def stop(t):
    return "@RC stage=test ev=audio op=stop hz=0 vol=100 t_ms=%d" % t


def frame(ms):
    return bs.frame_of(ms)


class Case(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.pcm_dir = os.path.join(self.dir, "pcm")
        os.mkdir(self.pcm_dir)

    def tearDown(self):
        self.tmp.cleanup()

    def sound(self, name, data):
        with open(os.path.join(self.pcm_dir, name + ".u8"), "wb") as f:
            f.write(data)
        return data

    def log(self, lines):
        path = os.path.join(self.dir, "serial.log")
        with open(path, "w") as f:
            f.write("\n".join(lines) + "\n")
        return path

    def render(self, lines, scale="cut"):
        """The duty per carrier period and the warnings."""
        events, _ = bs.read_log(self.log(lines))
        warnings = []
        sc = bs.scale_nearest if scale == "nearest" else bs.scale_cut
        r = bs.Renderer(bs.load_pcm([self.pcm_dir]), sc, warnings.append)
        r.run(events)
        return r.duty, warnings

    def main(self, lines, *extra):
        out = os.path.join(self.dir, "out.wav")
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            rc = bs.main([self.log(lines), out, "--pcm", self.pcm_dir] + list(extra))
        return rc, err.getvalue(), out

    def assertFails(self, lines, text):
        rc, err, _ = self.main(lines)
        self.assertEqual(rc, 2, err)
        self.assertIn(text, err)
        self.assertNotIn("Traceback", err)
        self.assertEqual(len(err.strip().splitlines()), 1, err)


class Placement(Case):
    def test_buffer_lands_at_its_time(self):
        d = self.sound("a", bytes([200]) * 441)       # 20 ms
        duty, w = self.render(["kernel text", pcm(100, d), end(120)])
        self.assertEqual(w, [])
        self.assertEqual(duty[frame(100) - 1], 0.0)
        self.assertEqual(duty[frame(100)], 200 / 256)
        self.assertEqual(duty[frame(119)], 200 / 256)
        self.assertEqual(duty[frame(121)], 0.0)

    def test_samples_are_held_per_carrier_period(self):
        d = self.sound("ramp", bytes(range(100, 200)))
        duty, _ = self.render([pcm(0, d), end(5)])
        for f in range(0, 150):
            i = f * RATE // FR
            self.assertEqual(duty[f], (100 + i) / 256 if i < 100 else 0.0)

    def test_queued_buffer_follows_without_gap(self):
        a = self.sound("a", bytes([150]) * 220)
        b = self.sound("b", bytes([170]) * 220)
        duty, _ = self.render([pcm(0, a), nxt(10, b), end(20)])
        self.assertEqual(duty[frame(9)], 150 / 256)
        self.assertEqual(duty[(225 * FR) // RATE], 170 / 256)
        self.assertEqual(duty[frame(21)], 0.0)

    def test_play_at_the_output_rate_is_one_sample_per_frame(self):
        d = self.sound("fast", bytes((60 + 7 * i) % 256 for i in range(750)))   # 20 ms
        duty, w = self.render([pcm(40, d, rate=FR), end(60)])
        self.assertEqual(w, [])
        start = frame(40)
        self.assertEqual(start, 40 * FR // 1000)
        self.assertEqual(duty[start - 1], 0.0)
        for i in range(750):
            self.assertEqual(duty[start + i], d[i] / 256)
        self.assertEqual(duty[start + 750], 0.0)


class Volume(Case):
    def test_cut_toward_128(self):
        self.sound("a", bytes([200, 129, 50]) * 100)
        duty, _ = self.render([pcm(0, bytes([200, 129, 50]) * 100, vol=50)])
        self.assertEqual([round(duty[f * FR // RATE + 1] * 256) for f in range(3)],
                         [164, 128, 89])

    def test_nearest_step(self):
        self.sound("a", bytes([200, 129, 50]) * 100)
        duty, _ = self.render([pcm(0, bytes([200, 129, 50]) * 100, vol=50)], scale="nearest")
        self.assertEqual([round(duty[f * FR // RATE + 1] * 256) for f in range(3)],
                         [164, 129, 89])

    def test_volume_100_is_full_scale(self):
        d = self.sound("a", bytes([255]) * 100)
        duty, _ = self.render([pcm(0, d, vol=100)])
        self.assertEqual(duty[0], 255 / 256)

    def test_default_scale_is_nearest(self):
        d = self.sound("a", bytes([129]) * 2205)
        o = self.sound("o", bytes([128]) * 10)
        for lines in ([pcm(0, d, vol=50)],
                      [pcm(0, d, vol=50), mix(0, o)],
                      [pcm(0, d, vol=50, loop=True), stop(100)]):
            out = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                bs.main([self.log(lines), os.path.join(self.dir, "o.wav"),
                         "--pcm", self.pcm_dir])
            self.assertIn("scale nearest", out.getvalue())

    def test_plain_play_rounds_as_the_driver(self):
        d = self.sound("a", bytes([129]) * 2205)
        self.assertEqual(bs.scale_nearest(129, 50), 129)
        self.assertEqual(bs.scale_cut(129, 50), 128)
        wavs = {}
        for extra in ((), ("--scale", "nearest"), ("--scale", "cut")):
            rc, err, out = self.main([pcm(0, d, vol=50)], *extra)
            self.assertEqual(rc, 0, err)
            with open(out, "rb") as f:
                wavs[extra] = f.read()
        self.assertEqual(wavs[()], wavs[("--scale", "nearest")])
        self.assertNotEqual(wavs[()], wavs[("--scale", "cut")])
        duty, _ = self.render([pcm(0, d, vol=50)], scale="nearest")
        self.assertEqual(duty[0], 129 / 256)
        duty, _ = self.render([pcm(0, d, vol=50)], scale="cut")
        self.assertEqual(duty[0], 128 / 256)

    def test_scale_auto_is_refused(self):
        with contextlib.redirect_stderr(io.StringIO()) as err, self.assertRaises(SystemExit) as e:
            bs.main([self.log([stop(0)]), os.path.join(self.dir, "o.wav"),
                     "--pcm", self.pcm_dir, "--scale", "auto"])
        self.assertEqual(e.exception.code, 2)
        self.assertIn("invalid choice", err.getvalue())


class Mix(Case):
    def test_overlay_adds_then_scales(self):
        play = self.sound("p", bytes([140]) * 2205)    # 100 ms
        over = self.sound("o", bytes([150]) * 441)     # 20 ms
        duty, _ = self.render([pcm(0, play, vol=50), mix(40, over), end(100)])
        self.assertEqual(duty[frame(39)], 134 / 256)   # 128 + 12 * 50 / 100
        self.assertEqual(duty[frame(41)], 145 / 256)   # 128 + 34 * 50 / 100
        self.assertEqual(duty[frame(61)], 134 / 256)

    def test_overlay_clamps(self):
        play = self.sound("p", bytes([250]) * 2205)
        over = self.sound("o", bytes([250]) * 441)
        duty, _ = self.render([pcm(0, play), mix(10, over)])
        self.assertEqual(duty[frame(11)], 255 / 256)

    def test_overlay_starts_on_the_play_grid(self):
        play = self.sound("p", bytes([128]) * 2205)
        over = self.sound("o", bytes([200]) * 441)
        duty, _ = self.render([pcm(0, play), mix(10, over)])
        first = next(f for f, v in enumerate(duty) if v > 0.6)
        self.assertEqual(first * RATE // FR, 221)      # ceil(10 ms * 22050 Hz)

    def test_overlay_ends_with_the_play(self):
        play = self.sound("p", bytes([128]) * 441)
        over = self.sound("o", bytes([200]) * 2205)
        duty, _ = self.render([pcm(0, play), mix(5, over), stop(15)])
        self.assertEqual(duty[frame(14)], 200 / 256)
        self.assertEqual(duty[frame(16)], 0.0)

    def overlays(self, times):
        play = self.sound("p", bytes([128]) * (RATE * 2))      # 2 s
        over = self.sound("o", bytes([200]) * 441)             # 20 ms
        lines = [pcm(0, play)] + [mix(t, over) for t in times] + [end(2000)]
        duty, w = self.render(lines)
        self.assertEqual(w, [])
        for t in times:
            self.assertEqual(duty[frame(t + 1)], 200 / 256, t)
            self.assertEqual(duty[frame(t + 19)], 200 / 256, t)
            self.assertEqual(duty[frame(t + 21)], 0.5, t)
        self.assertEqual(duty[frame(times[0] - 1)], 0.5)

    def test_two_overlays(self):
        self.overlays([100, 300])

    def test_three_overlays(self):
        self.overlays([100, 130, 500])

    def test_nine_overlays(self):
        self.overlays([100 + 60 * k for k in range(9)])

    def test_overlay_while_one_plays_is_a_bad_log(self):
        play = self.sound("p", bytes([128]) * RATE)
        over = self.sound("o", bytes([200]) * 441)
        self.assertFails([pcm(0, play), mix(100, over), mix(110, over)],
                         "serial.log:3: op=pcm_mix while an overlay plays")

    def test_overlay_right_after_the_last_one(self):
        # 441 samples from 100 ms end at 120 ms. t_ms drops the fraction, so
        # a line at 119 ms may be one that started at the end: it follows on.
        play = self.sound("p", bytes([128]) * RATE)
        a = self.sound("a", bytes([200]) * 441)
        b = self.sound("b", bytes([100]) * 441)
        duty, _ = self.render([pcm(0, play), mix(100, a), mix(119, b)])
        self.assertEqual(duty[frame(119) + 10], 200 / 256)
        self.assertEqual(duty[frame(121)], 100 / 256)
        self.assertEqual(duty[frame(139)], 100 / 256)

    def test_overlay_across_queued_buffer(self):
        a = self.sound("a", bytes([128]) * 220)                # 10 ms
        b = self.sound("b", bytes([140]) * 2205)
        over = self.sound("o", bytes([200]) * 441)
        duty, _ = self.render([pcm(0, a), mix(5, over), nxt(10, b), end(110)])
        self.assertEqual(duty[frame(7)], 200 / 256)
        self.assertEqual(duty[frame(15)], 212 / 256)           # 140 + 200 - 128
        self.assertEqual(duty[frame(26)], 140 / 256)

    def test_overlay_on_a_cut_play(self):
        a = self.sound("a", bytes([128]) * RATE)
        b = self.sound("b", bytes([128]) * RATE)
        over = self.sound("o", bytes([200]) * 2205)            # 100 ms
        duty, _ = self.render([pcm(0, a), mix(10, over), stop(30), pcm(40, b), end(1040)])
        self.assertEqual(duty[frame(29)], 200 / 256)
        self.assertEqual(duty[frame(31)], 0.0)
        self.assertEqual(set(duty[frame(41):frame(200)]), {0.5})

    def test_mix_without_play_fails(self):
        over = self.sound("o", bytes([200]) * 10)
        self.assertFails([mix(5, over)], "serial.log:1: op=pcm_mix with no sample play")


class Rest(Case):
    def test_duty_0_without_the_ramp(self):
        d = self.sound("a", bytes([128]) * 441)
        duty, _ = self.render([pcm(0, d), end(20)])
        self.assertEqual(duty[frame(10)], 0.5)
        self.assertEqual(duty[-1], 0.0)

    def test_ramp_moves_rest_to_midpoint(self):
        d = self.sound("a", bytes([128]) * 441)
        duty, w = self.render([pcm(0, bs.RAMP), end(40), pcm(100, d), end(120)])
        self.assertEqual(w, [])
        self.assertEqual(bs.RAMP[0], 0)
        self.assertEqual(bs.RAMP[-1], 128)
        self.assertEqual(duty[0], 0.0)
        self.assertEqual(duty[frame(70)], 0.5)
        self.assertEqual(duty[-1], 0.5)

    def test_ramp_bytes_at_another_volume_are_a_play(self):
        duty, _ = self.render([pcm(0, bs.RAMP, vol=10), end(40), stop(100)])
        self.assertEqual(duty[frame(39)], bs.scale_cut(bs.RAMP[(39 * RATE) // 1000 + 1], 10) / 256)
        self.assertEqual(duty[frame(60)], 0.0)
        self.assertEqual(duty[-1], 0.0)

    def test_ramp_bytes_at_another_rate_are_a_play(self):
        duty, _ = self.render([pcm(0, bs.RAMP, rate=11025), end(80), stop(200)])
        self.assertEqual(duty[frame(79)], bs.RAMP[(79 * 11025) // 1000 + 1] / 256)
        self.assertEqual(duty[frame(100)], 0.0)
        self.assertEqual(duty[-1], 0.0)

    def test_ramp_cut_early_leaves_duty_0(self):
        d = self.sound("a", bytes([128]) * 441)
        for cut in (stop(20), tone(20, 1000, vol=0), pcm(20, d)):
            duty, _ = self.render([pcm(0, bs.RAMP), cut, stop(60), pcm(100, d), end(120)])
            self.assertEqual(duty[frame(19)], bs.RAMP[(19 * RATE) // 1000 + 1] / 256)
            self.assertEqual(duty[frame(80)], 0.0, cut)
            self.assertEqual(duty[-1], 0.0, cut)

    def test_second_ramp_cut_early_leaves_duty_0(self):
        duty, _ = self.render([pcm(0, bs.RAMP), end(40), pcm(100, bs.RAMP), stop(120), stop(200)])
        self.assertEqual(duty[frame(80)], 0.5)
        self.assertEqual(duty[frame(160)], 0.0)
        self.assertEqual(duty[-1], 0.0)

    def test_ramp_cut_after_its_last_sample_leaves_duty_0(self):
        # The last sample sounded at 40 ms, but the stop came before the
        # ramp's own end line: the library never set the midpoint rest.
        duty, _ = self.render([pcm(0, bs.RAMP), stop(100)])
        self.assertEqual(duty[frame(60)], 0.5)      # the pin holds the last sample
        self.assertEqual(duty[-1], 0.0)

    def test_ramp_that_ends_by_itself_rests_at_the_midpoint(self):
        duty, _ = self.render([pcm(0, bs.RAMP), end(40), stop(100)])
        self.assertEqual(duty[-1], 0.5)
        # QEMU's sample interrupt is slow: the end line can come much later.
        duty, _ = self.render([pcm(0, bs.RAMP), end(216), stop(300)])
        self.assertEqual(duty[frame(100)], 0.5)
        self.assertEqual(duty[-1], 0.5)

    def test_tone_after_a_ramp_cut_after_its_last_sample_is_a_pulse(self):
        cut, _ = self.render([pcm(0, bs.RAMP), stop(100), tone(200, 1000), stop(260)])
        plain, _ = self.render([tone(200, 1000), stop(260)])
        self.assertEqual(list(cut[frame(200):frame(260)]), list(plain[frame(200):frame(260)]))

    def test_mix_or_queue_on_the_ramp_is_a_bad_log(self):
        o = self.sound("o", bytes([200]) * 10)
        self.assertFails([pcm(0, bs.RAMP), mix(5, o)], "serial.log:2: op=pcm_mix on the rest ramp")
        self.assertFails([pcm(0, bs.RAMP), nxt(5, o)], "serial.log:2: op=pcm_next on the rest ramp")

    def test_default_scale_and_the_ramp(self):
        for lines, want in (([pcm(0, bs.RAMP), end(40)], "nearest"),
                            ([pcm(0, bs.RAMP), stop(20)], "nearest"),
                            ([pcm(0, bs.RAMP, vol=10), end(40)], "nearest"),
                            ([pcm(0, bs.RAMP, rate=11025), end(80)], "nearest")):
            out = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                bs.main([self.log(lines), os.path.join(self.dir, "o.wav"), "--pcm", self.pcm_dir])
            self.assertIn("scale " + want, out.getvalue(), lines)

    def test_thump_reaches_the_file(self):
        d = self.sound("a", bytes([128]) * 4410)
        rc, err, out = self.main([pcm(100, d), end(300)])
        self.assertEqual(rc, 0, err)
        with wave.open(out) as w:
            self.assertEqual((w.getnchannels(), w.getsampwidth(), w.getframerate()),
                             (1, 2, FR))
            raw = w.readframes(w.getnframes())
        s = [int.from_bytes(raw[i:i + 2], "little", signed=True) for i in range(0, len(raw), 2)]
        self.assertGreater(max(s[frame(100):frame(110)]), 16000)
        self.assertLess(min(s[frame(300):frame(310)]), -16000)
        self.assertEqual(s[0], 0)


def same_crc(prefix, target):
    """`prefix` plus four bytes whose CRC-32 is `target`."""
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
        table.append(c)
    top = {v >> 24: i for i, v in enumerate(table)}
    state = target ^ 0xFFFFFFFF
    for _ in range(4):
        i = top[state >> 24]
        state = ((state ^ table[i]) << 8 | i) & 0xFFFFFFFF
    start = zlib.crc32(prefix) ^ 0xFFFFFFFF
    return prefix + (state ^ start).to_bytes(4, "little")


class Unknown(Case):
    def test_two_files_with_one_crc_are_told_apart_by_length(self):
        a = self.sound("a", bytes([200]) * 441)
        b = self.sound("b", same_crc(bytes([60]) * 216, zlib.crc32(a)))
        self.assertEqual(zlib.crc32(a), zlib.crc32(b))
        self.assertNotEqual(len(a), len(b))
        duty, w = self.render([pcm(0, a), end(20), pcm(50, b), end(60)])
        self.assertEqual(w, [])
        self.assertEqual(duty[frame(10)], 200 / 256)
        self.assertEqual(duty[frame(55)], 60 / 256)

    def test_unknown_crc_plays_silence_with_one_warning(self):
        d = bytes([200]) * 441
        duty, w = self.render([pcm(0, d), end(20), pcm(50, d), end(70)])
        self.assertEqual(len(w), 1)
        self.assertIn("crc=" + crc(d), w[0])
        self.assertEqual(duty[frame(10)], 0.5)

    def test_same_crc_other_length_is_unknown(self):
        self.sound("a", bytes([200]) * 441)
        line = pcm(0, bytes([200]) * 441).replace("len=441", "len=400")
        _, w = self.render([line])
        self.assertEqual(len(w), 1)


class ToneCase(Case):
    def test_tone_is_the_pin_pulse(self):
        duty, _ = self.render([tone(0, 1000), stop(100)])
        # 9600 counts at 9.6 MHz, high for 4800: the mean over 1 ms is one half.
        self.assertAlmostEqual(sum(duty[0:FR // 10]) / (FR // 10), 0.5, places=3)
        self.assertEqual(max(duty[0:frame(10)]), 1.0)
        self.assertEqual(min(duty[0:frame(10)]), 0.0)
        self.assertEqual(duty[frame(101)], 0.0)

    def test_tone_volume_sets_the_duty(self):
        duty, _ = self.render([tone(0, 1000, vol=20), stop(100)])
        # (4800 * 20 + 50) / 100 = 960 counts of 9600.
        self.assertAlmostEqual(sum(duty[0:FR // 10]) / (FR // 10), 0.1, places=3)

    def test_tone_at_volume_0_rests(self):
        duty, _ = self.render([pcm(0, bs.RAMP), end(40), tone(50, 1000, vol=0), stop(80)])
        self.assertEqual(set(duty[frame(50):frame(80)]), {0.5})


class Cut(Case):
    def test_stop_cuts_the_play(self):
        d = self.sound("a", bytes([200]) * 2205)
        duty, _ = self.render([pcm(0, d), stop(30)])
        self.assertEqual(duty[frame(29)], 200 / 256)
        self.assertEqual(duty[frame(31)], 0.0)

    def test_new_play_cuts_the_old(self):
        a = self.sound("a", bytes([200]) * 2205)
        b = self.sound("b", bytes([100]) * 441)
        duty, _ = self.render([pcm(0, a), stop(30), pcm(30, b), end(50)])
        self.assertEqual(duty[frame(31)], 100 / 256)

    def test_play_that_ends(self):
        d = self.sound("a", bytes([200]) * 441)
        duty, _ = self.render([pcm(0, d), end(20), stop(500)])
        self.assertEqual(duty[frame(19)], 200 / 256)
        self.assertEqual(set(duty[frame(21):]), {0.0})

    def test_play_without_end_line_runs_to_its_length(self):
        d = self.sound("a", bytes([200]) * 441)
        duty, _ = self.render([pcm(0, d)])
        self.assertEqual(duty[frame(19)], 200 / 256)
        self.assertEqual(len(duty), frame(20 + bs.TAIL_MS))


class Loop(Case):
    def test_a_loop_repeats_until_its_stop(self):
        d = self.sound("a", bytes(range(28, 228)))     # 200 bytes, 25 ms at 8 kHz
        duty, w = self.render([pcm(0, d, rate=8000, loop=True), stop(1000)])
        self.assertEqual(w, [])
        n = frame(1000)
        self.assertEqual(n, 37500)
        for f in range(n):
            self.assertEqual(duty[f], d[(f * 8000 // FR) % 200] / 256)
        self.assertEqual(duty[n], 0.0)
        self.assertEqual(len(duty), frame(1000 + bs.TAIL_MS))

    def test_a_new_play_cuts_the_loop(self):
        a = self.sound("a", bytes([150]) * 200)
        b = self.sound("b", bytes([170]) * 220)
        duty, _ = self.render([pcm(0, a, rate=8000, loop=True), stop(300), pcm(300, b), end(310)])
        self.assertEqual(duty[frame(300) - 1], 150 / 256)
        self.assertEqual(duty[frame(300)], 170 / 256)
        self.assertEqual(duty[frame(311)], 0.0)

    def test_a_loop_left_playing_stops_at_the_last_marker(self):
        a = self.sound("a", bytes([150]) * 200)
        duty, _ = self.render([pcm(0, a, rate=8000, loop=True), "@RC stage=test ev=audio_drop n=0",
                               mix(400, self.sound("o", bytes([140]) * 8))])
        self.assertEqual(duty[frame(400) - 1], 150 / 256)
        self.assertEqual(duty[frame(400)], 0.0)
        self.assertEqual(len(duty), frame(400 + bs.TAIL_MS))

    def test_an_overlay_over_a_loop_sounds_across_the_wrap(self):
        a = self.sound("a", bytes([150]) * 200)
        o = self.sound("o", bytes([140]) * 400)         # 50 ms: two passes of the loop
        duty, _ = self.render([pcm(0, a, rate=8000, loop=True), mix(100, o), stop(300)])
        self.assertEqual(duty[frame(100) - 1], 150 / 256)
        self.assertEqual(duty[frame(100)], 162 / 256)
        self.assertEqual(duty[frame(149)], 162 / 256)
        self.assertEqual(duty[frame(151)], 150 / 256)

    def test_the_volume_applies_to_a_loop(self):
        a = self.sound("a", bytes([228]) * 200)
        duty, _ = self.render([pcm(0, a, vol=50, rate=8000, loop=True), stop(100)])
        self.assertEqual(duty[frame(60)], 178 / 256)

    def test_a_queue_or_an_end_on_a_loop_is_a_bad_log(self):
        a = self.sound("a", bytes([150]) * 200)
        self.assertFails([pcm(0, a, rate=8000, loop=True), nxt(5, a)], "op=pcm_next on a loop")
        self.assertFails([pcm(0, a, rate=8000, loop=True), end(5)], "op=pcm_end on a loop")

    def test_loop_on_another_op_is_a_bad_log(self):
        self.assertFails([stop(5) + " loop=1"], "only op=pcm takes loop=1")
        a = self.sound("a", bytes([150]) * 200)
        self.assertFails([pcm(0, a).replace("t_ms=0", "t_ms=0 loop=2")], "loop=2")

    def test_the_ramp_bytes_with_loop_are_no_ramp(self):
        self.assertFalse(bs.is_ramp("pcm", {"rate": bs.RAMP_RATE, "vol": 100, "len": bs.RAMP_LEN,
                                            "crc": bs.RAMP_CRC, "loop": 1}))


class StatusMarker(Case):
    def test_an_audio_marker_without_op_is_skipped(self):
        d = self.sound("a", bytes([200]) * 441)
        lines = [pcm(100, d), end(120)]
        plain, w0 = self.render(lines)
        mixed, w1 = self.render(["@RC stage=hello ev=audio tick=5 rc=0"] + lines
                                + ["@RC stage=hello ev=audio tick=900 rc=0"])
        self.assertEqual((plain, w0), (mixed, w1))
        rc, err, _ = self.main(["@RC stage=hello ev=audio tick=5 rc=0"] + lines)
        self.assertEqual((rc, err), (0, ""))


class BadLogs(Case):
    def test_out_of_order(self):
        self.assertFails([stop(500), stop(100)],
                         "serial.log:2: t_ms=100 comes before t_ms=500")

    def test_small_step_back_is_allowed(self):
        rc, err, _ = self.main([stop(500), stop(495)])
        self.assertEqual(rc, 0, err)

    def test_missing_field(self):
        self.assertFails(["@RC stage=test ev=audio op=tone hz=1000 t_ms=5"],
                         "serial.log:1: op=tone has no vol=")

    def test_bad_number(self):
        self.assertFails(["@RC stage=test ev=audio op=tone hz=1k vol=5 t_ms=5"],
                         "hz=1k is not a whole number")

    def test_bad_crc(self):
        self.assertFails(["@RC stage=test ev=audio op=pcm rate=22050 len=4 vol=5 crc=xyz t_ms=5"],
                         "crc=xyz is not 8 hex digits")

    def test_unknown_op(self):
        self.assertFails(["@RC stage=test ev=audio op=sing t_ms=5"], "unknown audio op 'sing'")

    def test_token_without_equals(self):
        self.assertFails(["@RC stage=test ev=audio op=stop junk"], "a field without '='")

    def test_end_without_play(self):
        self.assertFails([end(5)], "op=pcm_end with no sample play")

    def test_next_without_play(self):
        self.assertFails([nxt(5, b"\x80")], "op=pcm_next with no sample play")

    def test_stop_with_hz(self):
        self.assertFails(["@RC stage=test ev=audio op=stop hz=5 vol=5 t_ms=5"],
                         "op=stop with hz=5")

    def test_other_lines_are_skipped(self):
        rc, err, _ = self.main(["garbage \x00\xff", "@RC stage=test ev=ready",
                                "@RC stage=test ev=audio_drop n=3", stop(5)])
        self.assertEqual(rc, 0, err)
        self.assertIn("lost 3 records", err)

    def test_missing_log(self):
        rc, err, _ = self.main([])
        os.remove(os.path.join(self.dir, "serial.log"))
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rc = bs.main([os.path.join(self.dir, "serial.log"), "o.wav", "--pcm", self.pcm_dir])
        self.assertEqual(rc, 2)
        self.assertNotIn("Traceback", err.getvalue())


class Lost(Case):
    """Other markers and lost records."""

    DROP = "@RC stage=test ev=audio_drop n=2"

    def test_other_marker_with_a_quoted_string(self):
        a = self.sound("a", bytes([200]) * 441)
        other = '@RC stage=test ev=errors n=1 str="no acknowledge"'
        with_it, w1 = self.render([pcm(0, a), other, end(20)])
        without, w2 = self.render([pcm(0, a), end(20)])
        self.assertEqual(with_it, without)
        self.assertEqual(w1, w2)
        rc, err, _ = self.main([pcm(0, a), other, end(20)])
        self.assertEqual(rc, 0, err)

    def test_malformed_audio_line_is_an_error(self):
        self.assertFails(['@RC stage=test ev=audio op=stop hz=0 vol=5 t_ms=5 "no ack"'],
                         "a field without '='")

    def test_end_after_a_loss_is_skipped(self):
        a = self.sound("a", bytes([200]) * 441)
        rc, err, out = self.main([self.DROP, end(10), pcm(20, a), end(40)])
        self.assertEqual(rc, 0, err)
        self.assertEqual(err.count("1 markers had no start"), 1, err)
        self.assertIn("lost 2 records", err)
        duty, _ = self.render([self.DROP, end(10), pcm(20, a), end(40)])
        self.assertEqual(duty[frame(20)], 200 / 256)
        self.assertEqual(duty[frame(41)], 0.0)
        with open(out, "rb") as f:
            self.assertGreater(len(f.read()), 44)

    def test_end_without_a_loss_is_an_error(self):
        a = self.sound("a", bytes([200]) * 441)
        self.assertFails([end(10), pcm(20, a), end(40)],
                         "op=pcm_end with no sample play or voice")

    def test_overlay_queue_and_stop_after_a_loss_are_counted(self):
        a = self.sound("a", bytes([200]) * 441)
        o = self.sound("o", bytes([140]) * 8)
        rc, err, _ = self.main([self.DROP, mix(5, o), nxt(6, a), stop(7), tone(10, 440), stop(20)])
        self.assertEqual(rc, 0, err)
        self.assertEqual(err.count("3 markers had no start"), 1, err)
        self.assertNotIn("markers had no start", err.replace("3 markers had no start", ""))

    def test_the_ramp_end_after_a_loss_is_applied(self):
        lines = [pcm(0, bs.RAMP), end(40), tone(100, 1000), stop(160)]
        plain, w0 = self.render(lines)
        lost, w1 = self.render(lines[:1] + [self.DROP] + lines[1:])
        self.assertEqual(lost[frame(60)], 0.5)
        self.assertEqual(plain, lost)
        self.assertEqual((w0, w1), ([], []))
        pulse, _ = self.render([tone(100, 1000), stop(160)])
        self.assertNotEqual(list(lost[frame(100):frame(160)]),
                            list(pulse[frame(100):frame(160)]))

    def test_an_overlay_on_a_loop_after_a_loss_is_applied(self):
        a = self.sound("a", bytes([150]) * 200)
        o = self.sound("o", bytes([140]) * 400)
        lines = [pcm(0, a, rate=8000, loop=True), mix(100, o), stop(300)]
        plain, w0 = self.render(lines)
        lost, w1 = self.render(lines[:1] + [self.DROP] + lines[1:])
        self.assertEqual(lost[frame(100)], 162 / 256)
        self.assertEqual(plain, lost)
        self.assertEqual((w0, w1), ([], []))

    def test_a_queue_and_end_of_a_play_after_a_loss_are_applied(self):
        a = self.sound("a", bytes([200]) * 441)
        b = self.sound("b", bytes([100]) * 441)
        lines = [pcm(0, a), nxt(10, b), end(40), stop(60)]
        plain, w0 = self.render(lines)
        lost, w1 = self.render(lines[:1] + [self.DROP] + lines[1:])
        self.assertEqual(lost[frame(30)], 100 / 256)
        self.assertEqual(plain, lost)
        self.assertEqual((w0, w1), ([], []))
        rc, err, _ = self.main(lines[:1] + [self.DROP] + lines[1:])
        self.assertEqual(rc, 0, err)
        self.assertNotIn("markers had no start", err)

    def test_the_loss_ends_at_the_next_start(self):
        a = self.sound("a", bytes([200]) * 441)
        rc, err, _ = self.main([self.DROP, pcm(0, a), end(20), end(30)])
        self.assertEqual(rc, 2, err)
        self.assertIn("serial.log:4: op=pcm_end with no sample play or voice",
                      err.splitlines()[-1])


class Bounds(Case):
    """Each number of a line is bounded when read, before anything uses it."""

    def fast_fail(self, line, text):
        t = time.monotonic()
        self.assertFails([line], text)
        self.assertLess(time.monotonic() - t, 0.5)

    def test_huge_len_with_unknown_crc(self):
        self.fast_fail("@RC stage=test ev=audio op=pcm rate=22050 len=9999999999 vol=100 "
                       "crc=12345678 t_ms=0", "serial.log:1: len=9999999999 is outside 1 to 11250000")

    def test_len_one_past_the_bound(self):
        for op in ("pcm rate=22050 vol=100", "pcm_next", "pcm_mix"):
            self.fast_fail("@RC stage=test ev=audio op=%s len=11250001 crc=12345678 t_ms=0" % op,
                           "len=11250001 is outside 1 to 11250000")

    def test_len_zero(self):
        self.fast_fail("@RC stage=test ev=audio op=pcm_mix len=0 crc=12345678 t_ms=0",
                       "len=0 is outside")

    def test_len_at_the_bound_is_read(self):
        events, _ = bs.read_log(self.log(
            ["@RC stage=test ev=audio op=pcm rate=22050 len=11250000 vol=100 crc=12345678 t_ms=0"]))
        self.assertEqual(events[0][2]["len"], bs.MAX_PCM_LEN)

    def test_chained_buffers_past_the_bound(self):
        line = "@RC stage=test ev=audio op=%s len=6000000 crc=12345678 t_ms=0"
        rc, err, _ = self.main([line.replace("%s", "pcm rate=22050 vol=100"), line % "pcm_next"])
        self.assertEqual(rc, 2, err)
        self.assertIn("serial.log:2: the play's buffers pass 11250000", err.splitlines()[-1])

    def test_rate(self):
        for rate in (3999, 37501, 9999999999):
            self.fast_fail("@RC stage=test ev=audio op=pcm rate=%d len=4 vol=100 crc=12345678 "
                           "t_ms=0" % rate, "rate=%d is outside 4000 to 37500" % rate)

    def test_vol(self):
        self.fast_fail("@RC stage=test ev=audio op=pcm rate=8000 len=4 vol=101 crc=12345678 t_ms=0",
                       "vol=101 is past 100")
        self.fast_fail("@RC stage=test ev=audio op=tone hz=440 vol=9999999999 t_ms=0",
                       "vol=9999999999 is past 100")
        self.fast_fail("@RC stage=test ev=audio op=stop hz=0 vol=101 t_ms=0", "vol=101 is past 100")

    def test_hz(self):
        for hz in (19, 20001, 9999999999):
            self.fast_fail("@RC stage=test ev=audio op=tone hz=%d vol=5 t_ms=0" % hz,
                           "hz=%d is outside 20 to 20000" % hz)

    def test_end_counts(self):
        self.fast_fail("@RC stage=test ev=audio op=pcm_end n=11250001 last=0 t_ms=0",
                       "n=11250001 is past 11250000")
        self.fast_fail("@RC stage=test ev=audio op=pcm_end n=1 last=11250000 t_ms=0",
                       "last=11250000 is past 11249999")

    def test_t_ms(self):
        self.fast_fail("@RC stage=test ev=audio op=stop hz=0 vol=5 t_ms=9999999999",
                       "t_ms=9999999999 is past the 300 s limit")

    def test_drop_count(self):
        for n in ("n=4294967296", "n=x", "m=3"):
            self.fast_fail("@RC stage=test ev=audio_drop " + n,
                           "serial.log:1: ev=audio_drop needs n= from 0 to 4294967295")


class Limits(Case):
    def test_long_line(self):
        self.assertFails(["x" * (bs.MAX_LINE + 1)], "longer than 4096 bytes")

    def test_long_line_is_not_read_whole(self):
        path = os.path.join(self.dir, "serial.log")
        with open(path, "wb") as f:
            f.write(b"x" * (64 << 20))
        seen = []
        real = io.BufferedReader.readline
        class Spy(io.BufferedReader):
            def readline(self, size=-1):
                line = real(self, size)
                seen.append(len(line))
                return line
        old = bs.open if hasattr(bs, "open") else None
        bs.open = lambda p, m: Spy(io.FileIO(p, "r"))
        try:
            with self.assertRaises(bs.LogError) as e:
                bs.read_log(path)
        finally:
            if old is None:
                del bs.open
            else:
                bs.open = old
        self.assertIn("longer than 4096 bytes", str(e.exception))
        self.assertLessEqual(max(seen), bs.MAX_LINE + 2)

    def test_files_together_past_limit(self):
        old = bs.MAX_PCM_BYTES
        bs.MAX_PCM_BYTES = 1000
        try:
            self.sound("a", bytes([1]) * 600)
            self.sound("b", bytes([2]) * 600)
            self.assertFails(["x"], "--pcm: the .u8 and .voice files pass 1000 bytes together")
        finally:
            bs.MAX_PCM_BYTES = old

    def test_time_past_limit(self):
        self.assertFails([stop(bs.MAX_SECONDS * 1000 + 1)], "past the 300 s limit")

    def test_play_past_limit(self):
        d = self.sound("a", bytes([128]) * 2000000)
        self.assertFails([pcm(0, d, rate=4000)], "the output would pass 300 s")

    def test_too_many_lines(self):
        old = bs.MAX_LINES
        bs.MAX_LINES = 3
        try:
            self.assertFails([stop(1)] * 4, "more than 3 lines")
        finally:
            bs.MAX_LINES = old

    def test_help_states_limits_and_line_forms(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaises(SystemExit):
            bs.main(["--help"])
        text = out.getvalue()
        for s in ("4096 bytes", "1000000 lines", "300 s", "op=pcm_mix", "4000 to 37500",
                  "20 to 20000", "11250000", "4294967295",
                  "every version", "newer versions only", "op=voice", "149796", ".voice"):
            self.assertIn(s, text)



# ---- Centred tones -----------------------------------------------------------

SQ = bs.square
RAMPED = [pcm(0, bs.RAMP), end(40)]


def as_bytes(duty):
    out = [round(d * 256) for d in duty]
    for o, d in zip(out, duty):
        assert o == d * 256, d
    return out


def pulse(hz, vol, n):
    """The pin's pulse of a tone from frame 0, by the pulse path."""
    r = bs.Renderer({}, bs.scale_cut, lambda m: None)
    r.render_tone(bs.Tone(0, hz, vol), 0, n)
    return list(r.duty)


class CentredTone(Case):
    def test_mean_is_128_and_ends_step_by_its_height(self):
        duty, _ = self.render(RAMPED + [tone(50, 1000), stop(52)])
        a, b = frame(50), frame(52)
        x = as_bytes(duty[a:b])
        # half 4800 units, a period of 37.5 samples: two periods are 75.
        self.assertEqual(sum(x[:75]), 128 * 75)
        self.assertEqual(duty[a - 1], 0.5)
        self.assertEqual(duty[b], 0.5)
        self.assertLessEqual(abs(x[0] - 128), 127)
        self.assertLessEqual(abs(x[-1] - 128), 127)
        self.assertEqual(x[0], 255)

    def test_without_the_ramp_the_tone_is_the_pulse(self):
        duty, _ = self.render([tone(0, 1000, vol=60), stop(100)])
        self.assertEqual(list(duty[:frame(100)]), pulse(1000, 60, frame(100)))

    def test_a_ramp_cut_early_keeps_the_pulse(self):
        duty, _ = self.render([pcm(0, bs.RAMP), stop(20), tone(40, 1000), stop(140)])
        a = frame(40)
        n = frame(140) - a
        want = pulse(1000, 100, n)
        self.assertEqual(len(duty[a:a + n]), n)
        for i in range(n):
            # The pulse path works in float time from the tone's start.
            self.assertAlmostEqual(duty[a + i], want[i], places=9)
        self.assertEqual(duty[a + n], 0.0)

    def test_height_follows_the_volume(self):
        for vol, amp in ((100, 127), (80, 102), (20, 25)):
            duty, _ = self.render(RAMPED + [tone(50, 1000, vol=vol), stop(60)])
            x = as_bytes(duty[frame(50):frame(60)])
            self.assertEqual((max(x), min(x)), (128 + amp, 128 - amp), vol)

    def test_tone_peak_sets_the_height(self):
        events, _ = bs.read_log(self.log(RAMPED + [tone(50, 1000), stop(60)]))
        r = bs.Renderer({}, bs.scale_cut, lambda m: None, peak=64)
        r.run(events)
        x = as_bytes(r.duty[frame(50):frame(60)])
        self.assertEqual((max(x), min(x)), (192, 64))

    def test_a_pitch_above_half_the_grid_plays_half_the_grid(self):
        duty, _ = self.render(RAMPED + [tone(50, 20000), stop(52)])
        x = as_bytes(duty[frame(50):frame(52)])
        self.assertEqual(x, [255 if i % 2 == 0 else 1 for i in range(len(x))])

    def test_sequence_with_a_rest(self):
        duty, _ = self.render(RAMPED + [tone(50, 1000), stop(60), tone(70, 500, vol=50),
                                        stop(90)])
        self.assertEqual(set(duty[frame(60):frame(70)]), {0.5})
        x = as_bytes(duty[frame(70):frame(90)])
        # A new note: an edge going up, at the volume of its own line.
        self.assertEqual(x[0], 128 + 64)
        self.assertEqual(min(x), 128 - 64)
        self.assertEqual(x, list(SQ.render(500, 64, len(x))))

    def test_help_states_the_tone_rule(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaises(SystemExit):
            bs.main(["--help"])
        text = " ".join(out.getvalue().split())
        for s in ("centred square", "rest ramp reached its end", "cut early",
                  "max(256, (37500 * 128 + hz / 2) / hz)", "18750 Hz",
                  "(peak * vol + 50) / 100", "edge going up", "next note",
                  "no late IRQ", "assumes the kernel's peak", "--tone-peak"):
            self.assertIn(s, text)

    def test_tone_peak_out_of_range_fails(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as e:
            bs.main([self.log([stop(0)]), os.path.join(self.dir, "o.wav"),
                     "--pcm", self.pcm_dir, "--tone-peak", "128"])
        self.assertEqual(e.exception.code, 2)
        self.assertIn("--tone-peak must be 0 to 127", err.getvalue())


SQUARE_C = os.path.join(ROOT, "src", "lib", "audio_square.c")
SQUARE_MAIN = r"""
#include <stdio.h>
#include <stdlib.h>
#include <robco/audio.h>

int main(int argc, char **argv)
{
    uint32_t hz = (uint32_t)strtoul(argv[1], 0, 10);
    unsigned vol = (unsigned)strtoul(argv[2], 0, 10);
    uint32_t n = (uint32_t)strtoul(argv[3], 0, 10);
    audio_square_t q;

    audio_square_start(&q, hz, 37500, audio_square_amp(127, vol));
    for (uint32_t i = 0; i < n; i++) {
        putchar(audio_square_at(&q, i));
    }
    return 0;
}
"""


def c_compiler():
    for cc in ("cc", "gcc", "clang"):
        for d in os.environ.get("PATH", "").split(os.pathsep):
            if d and os.access(os.path.join(d, cc), os.X_OK):
                return os.path.join(d, cc)
    return None


class SquareAgainstLibrary(unittest.TestCase):
    """The library's own square, built with the host compiler, gives the
    same samples as tools/tone_square.py."""

    def test_same_bytes_as_the_c_square(self):
        cc = c_compiler()
        if cc is None:
            self.skipTest("no host C compiler")
        import subprocess
        with tempfile.TemporaryDirectory() as d:
            main = os.path.join(d, "main.c")
            with open(main, "w", encoding="utf-8") as f:
                f.write(SQUARE_MAIN)
            exe = os.path.join(d, "square")
            subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Werror",
                            "-I", os.path.join(ROOT, "include"),
                            "-o", exe, main, SQUARE_C], check=True)
            for hz in (0, 20, 440, 1000, 2637, 7902, 18750, 20000):
                for vol in (0, 1, 20, 50, 80, 100):
                    got = subprocess.run([exe, str(hz), str(vol), "4000"], check=True,
                                         stdout=subprocess.PIPE).stdout
                    want = SQ.render(hz, SQ.amp(SQ.TONE_PEAK, vol), 4000)
                    self.assertEqual(got, want, (hz, vol))


# ---- Voices ----------------------------------------------------------------

V = bs.voice
SINE_TOP = 256 << 22            # the phase of the sine table's peak


def voice_line(t, spans, vol=100, rate=FR, length=None, count=None):
    data = V.pack(spans)
    return ("@RC stage=test ev=audio op=voice rate=%d spans=%d len=%d vol=%d crc=%s t_ms=%d"
            % (rate, len(spans) if count is None else count,
               V.length(spans) if length is None else length, vol, crc(data), t))


class Voice(Case):
    def spans(self, name, spans):
        with open(os.path.join(self.pcm_dir, name + ".voice"), "wb") as f:
            f.write(V.pack(spans))
        return spans

    def test_voice_overlay_and_end(self):
        sp = self.spans("v", [(1000, 0, 0x10000000, 0, 20 << 16, 0, 0),
                              (1000, 0, 0x01000000, 50, 20 << 16, -300, 2603)])
        over = self.sound("o", bytes([140]) * 300)
        duty, w = self.render([voice_line(40, sp, vol=80), mix(50, over), end(94)])
        self.assertEqual(w, [])
        v0 = frame(40)
        m0 = frame(50) - v0
        voc = V.Voice(sp)
        for i in range(2000):
            o = over[i - m0] if 0 <= i - m0 < 300 else 128
            self.assertEqual(duty[v0 + i], V.out_byte(voc.at(i), o, 80) / 256, i)
        self.assertEqual(duty[v0 - 1], 0.0)
        self.assertEqual(duty[v0 + 2000], 0.0)
        # The test vector of the engine: sample 4 is 5119, sample 5 4730.
        self.assertEqual(duty[v0 + 4], V.out_byte(5119, 128, 80) / 256)

    def test_one_rounding(self):
        # The voice is 100 on 1/256 of a step, the overlay +1 step, the
        # volume 40: (100 + 256) * 40 / 25600 rounds once to 1 step. Two
        # roundings, the voice first, would give 0.
        amp = 101 << 8
        sp = self.spans("v", [(3750, SINE_TOP, 0, 0, amp, 0, 0)])
        self.assertEqual(V.span_at(sp[0], 0), 100)
        over = self.sound("o", bytes([129]) * 375)
        duty, _ = self.render([voice_line(0, sp, vol=40), mix(20, over), end(100)])
        self.assertEqual(duty[frame(10)], 128 / 256)
        self.assertEqual(duty[frame(21)], 129 / 256)
        self.assertEqual(bs.scale_nearest(128 + 0, 40) + bs.scale_nearest(129, 40) - 128, 128)

    def test_scale_option_does_not_change_a_voice(self):
        sp = self.spans("v", [(3750, SINE_TOP, 0, 0, 30 << 16, 0, 0)])
        a, _ = self.render([voice_line(0, sp, vol=50)], scale="cut")
        b, _ = self.render([voice_line(0, sp, vol=50)], scale="nearest")
        self.assertEqual(a, b)
        self.assertEqual(a[5], V.out_byte(V.span_at(sp[0], 5), 128, 50) / 256)

    def test_stop_cuts_a_voice(self):
        sp = self.spans("v", [(3750, SINE_TOP, 0, 0, 30 << 16, 0, 0)])
        duty, _ = self.render([voice_line(0, sp), stop(40)])
        self.assertNotEqual(duty[frame(39)], 0.0)
        self.assertEqual(duty[frame(41)], 0.0)

    def test_unknown_voice_is_silence_with_one_warning(self):
        sp = [(3750, SINE_TOP, 0, 0, 30 << 16, 0, 0)]
        duty, w = self.render([voice_line(0, sp), end(100), voice_line(200, sp), end(300)])
        self.assertEqual(len(w), 1)
        self.assertIn("no .voice file has crc=", w[0])
        self.assertEqual(duty[frame(50)], 128 / 256)
        self.assertEqual(duty[frame(250)], 128 / 256)

    def test_found_by_crc_and_length(self):
        sp = self.spans("v", [(3750, SINE_TOP, 0, 0, 30 << 16, 0, 0)])
        _, w = self.render([voice_line(0, sp, count=2, length=3750)])
        self.assertEqual(len(w), 1)

    def test_queue_on_a_voice_is_a_bad_log(self):
        sp = self.spans("v", [(3750, SINE_TOP, 0, 0, 30 << 16, 0, 0)])
        d = self.sound("a", bytes([140]) * 100)
        self.assertFails([voice_line(0, sp), nxt(10, d)], "serial.log:2: op=pcm_next on a voice")

    def test_len_must_match_the_spans(self):
        sp = self.spans("v", [(3750, SINE_TOP, 0, 0, 30 << 16, 0, 0)])
        self.assertFails([voice_line(0, sp, length=3751)],
                         "len=3751, but the spans of crc=")

    def test_bad_voice_file(self):
        with open(os.path.join(self.pcm_dir, "odd.voice"), "wb") as f:
            f.write(bytes(27))
        self.assertFails([end(0)], "27 bytes is not a whole number of spans")

    def test_voice_file_outside_the_limits(self):
        self.spans("v", [(10, 0, 0, 0, 0, -1, 0)])
        self.assertFails([end(0)], "span 0: amp leaves 0 to")

    def test_empty_voice_file_is_an_error(self):
        open(os.path.join(self.pcm_dir, "v.voice"), "wb").close()
        self.assertFails([end(0)], "v.voice: no span")

    def test_empty_sample_file_is_skipped(self):
        open(os.path.join(self.pcm_dir, "e.u8"), "wb").close()
        rc, err, _ = self.main([stop(0)])
        self.assertEqual(rc, 0, err)

    def test_default_scale_on_a_voice(self):
        sp = self.spans("v", [(3750, SINE_TOP, 0, 0, 30 << 16, 0, 0)])
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            bs.main([self.log([voice_line(0, sp)]), os.path.join(self.dir, "o.wav"),
                     "--pcm", self.pcm_dir])
        self.assertIn("scale nearest", out.getvalue())

    def test_bounds(self):
        base = "@RC stage=test ev=audio op=voice rate=%s spans=%s len=%s vol=%s crc=12345678 t_ms=0"
        for args, text in (((37501, 1, 1, 100), "rate=37501 is outside 4000 to 37500"),
                           ((3999, 1, 1, 100), "rate=3999 is outside"),
                           ((37500, 0, 1, 100), "spans=0 is outside 1 to 149796"),
                           ((37500, 149797, 1, 100), "spans=149797 is outside 1 to 149796"),
                           ((37500, 1, 0, 100), "len=0 is outside"),
                           ((37500, 1, 11250001, 100), "len=11250001 is outside"),
                           ((37500, 1, 1, 101), "vol=101 is past 100")):
            t = time.monotonic()
            self.assertFails([base % args], text)
            self.assertLess(time.monotonic() - t, 0.5)
        self.assertFails(["@RC stage=test ev=audio op=voice rate=37500 len=1 vol=1 "
                          "crc=12345678 t_ms=0"], "op=voice has no spans=")

    def test_voice_at_the_bound_is_read(self):
        _, w = self.render(["@RC stage=test ev=audio op=voice rate=37500 spans=149796 "
                            "len=1 vol=100 crc=12345678 t_ms=0", stop(1)])
        self.assertEqual(len(w), 1)


if __name__ == "__main__":
    unittest.main()


def staged(line, name):
    return line.replace("stage=test", "stage=" + name)


class Stages(Case):
    def test_second_stage_starts_lower(self):
        a = self.sound("a", bytes([200]) * 441)       # 20 ms at 22050
        b = self.sound("b", bytes([60]) * 441)
        lines = [pcm(1000, a), end(1020),
                 staged(pcm(0, b), "next"), staged(end(20), "next")]
        rc, err, _ = self.main(lines)
        self.assertEqual(rc, 0, err)
        self.assertEqual(err.count("warning"), 1, err)
        self.assertIn("stage next follows stage test: the time between them is not in the log",
                      err)
        duty, _ = self.render(lines, scale="cut")
        self.assertEqual(duty[frame(1010)], 200 / 256)
        self.assertEqual(duty[frame(1030)], 60 / 256)

    def test_play_running_at_the_change_is_cut(self):
        a = self.sound("a", bytes([200]) * 22050)     # 1 s
        b = self.sound("b", bytes([60]) * 441)
        lines = [pcm(0, a), staged(pcm(300, b), "next"), staged(end(320), "next")]
        duty, w = self.render(lines, scale="cut")
        self.assertEqual(len(w), 1, w)
        # The first play is cut at its stage's last marker, t_ms=0.
        self.assertEqual(duty[frame(5)], 60 / 256)
        self.assertEqual(duty[frame(25)], 0.0)

    def test_rest_level_does_not_cross_stages(self):
        b = self.sound("b", bytes([60]) * 441)
        lines = [pcm(0, bs.RAMP), end(40), stop(50),
                 staged(pcm(10, b), "next"), staged(end(30), "next")]
        duty, _ = self.render(lines, scale="nearest")
        self.assertEqual(duty[frame(45)], 0.5)
        self.assertEqual(duty[frame(75)], 0.0)

    def test_time_back_inside_a_stage_is_an_error(self):
        rc, err, _ = self.main([stop(1000), staged(stop(0), "next"), staged(stop(500), "next"),
                                staged(stop(100), "next")])
        self.assertEqual(rc, 2, err)
        self.assertIn("serial.log:4: t_ms=100 comes before t_ms=500", err)
        # A reboot of the same image repeats its name with a lower time.
        self.assertFails([stop(1000), stop(10)], "serial.log:2: t_ms=10 comes before t_ms=1000")


class EndOfLog(Case):
    """What the log leaves playing stops at its last marker."""

    DROP = "@RC stage=test ev=audio_drop n=2"
    LONG = (37500, 0, 0x10000000, 0, 20 << 16, 0, 0)    # 1 s at 37500 Hz

    def spans(self, name, spans):
        with open(os.path.join(self.pcm_dir, name + ".voice"), "wb") as f:
            f.write(V.pack(spans))
        return spans

    def assertRestsFrom(self, duty, ms, total_ms):
        self.assertEqual(set(duty[frame(ms):]), {0.0})
        self.assertEqual(len(duty), frame(total_ms))

    def test_a_tone_stops_at_the_last_marker(self):
        duty, _ = self.render([tone(0, 1000), tone(200, 1000)])
        self.assertNotEqual(set(duty[:frame(200)]), {0.0})
        self.assertRestsFrom(duty, 200, 200 + bs.TAIL_MS)

    def test_a_loop_stops_at_the_last_marker(self):
        a = self.sound("a", bytes([150]) * 200)
        duty, _ = self.render([pcm(0, a, rate=8000, loop=True),
                               pcm(200, a, rate=8000, loop=True)])
        self.assertEqual(duty[frame(200) - 1], 150 / 256)
        self.assertRestsFrom(duty, 200, 200 + bs.TAIL_MS)

    def test_a_voice_plays_to_its_end(self):
        sp = self.spans("v", [self.LONG])
        o = self.sound("o", bytes([140]) * 8)
        duty, _ = self.render([voice_line(0, sp), mix(100, o)])
        self.assertNotEqual(set(duty[frame(900):frame(1000)]), {0.0})
        self.assertRestsFrom(duty, 1000, 1000 + bs.TAIL_MS)

    def test_a_play_plays_to_its_end(self):
        a = self.sound("a", bytes([200]) * 22050)     # 1 s
        o = self.sound("o", bytes([128]) * 8)
        duty, _ = self.render([pcm(0, a), mix(100, o)])
        self.assertEqual(duty[frame(999)], 200 / 256)
        self.assertRestsFrom(duty, 1001, 1000 + bs.TAIL_MS)

    def test_a_skipped_marker_moves_the_time(self):
        duty, w = self.render([self.DROP, end(200)])
        self.assertEqual(len(w), 1, w)
        self.assertRestsFrom(duty, 0, 200 + bs.TAIL_MS)

    def test_a_skipped_marker_moves_the_next_stage(self):
        a = self.sound("a", bytes([200]) * 441)
        b = self.sound("b", bytes([60]) * 441)
        duty, _ = self.render([pcm(0, a), end(20), self.DROP, end(200),
                               staged(pcm(0, b), "next"), staged(end(20), "next")])
        self.assertEqual(duty[frame(150)], 0.0)
        self.assertEqual(duty[frame(210)], 60 / 256)


def gain(t, g):
    return "@RC stage=test ev=audio op=gain gain=%d t_ms=%d" % (g, t)


def with_gain(line, g):
    return line + " gain=%d" % g


class Gain(Case):
    """Logs with a gain, each checked against samples worked out by hand:
    a sample is 128 + (s - 128) * gain / 10000, a half step away from
    128, and a change moves the gain by (500000 + rate - 1) / rate per
    sample, from the first sample at or after its time."""

    def test_start_line_gain(self):
        d = self.sound("a", bytes([255, 50, 200]) * 100)
        line = with_gain(pcm(0, d, vol=12, rate=FR), 1234)
        duty, w = self.render([line], scale="nearest")
        self.assertEqual(w, [])
        # 255: 127 * 1234 = 156718, + 5000 = 161718, / 10000 = 16, so 144.
        #      vol=12 alone would give (127 * 12 + 50) / 100 = 15.
        # 50:  -78 * 1234 = -96252, -(5000 + 96252) / 10000 = -10, so 118.
        # 200: 72 * 1234 = 88848, + 5000 = 93848, / 10000 = 9, so 137.
        self.assertEqual(as_bytes(duty[0:3]), [144, 118, 137])

    def test_gain_of_vol_times_100_is_the_volume(self):
        d = self.sound("a", bytes(range(256)))
        for vol in (0, 1, 37, 50, 99, 100):
            plain, _ = self.render([pcm(0, d, vol=vol, rate=FR)], scale="nearest")
            g, _ = self.render([with_gain(pcm(0, d, vol=vol, rate=FR), vol * 100)],
                               scale="nearest")
            self.assertEqual(list(plain), list(g), vol)

    def test_change_in_a_sample_play(self):
        # 37500 Hz, one sample per frame. step = (500000 + 37499) / 37500 = 14.
        d = self.sound("a", bytes([255]) * 1500)
        duty, w = self.render([pcm(0, d, rate=FR), gain(4, 0)], scale="nearest")
        self.assertEqual(w, [])
        x = as_bytes(duty[0:1500])
        # The change names sample 4 * 37500 / 1000 = 150.
        self.assertEqual(x[149], 255)           # gain 10000
        # Sample 150: 10000 - 14 = 9986, 127 * 9986 = 1268222, + 5000, / 10000 = 127.
        self.assertEqual(x[150], 255)
        # Sample 249: 10000 - 14 * 100 = 8600, 127 * 8600 + 5000 = 1097200, so 109.
        self.assertEqual(x[249], 128 + 109)
        # Sample 860: 10000 - 14 * 711 = 46, 127 * 46 + 5000 = 10842, so 1.
        self.assertEqual(x[860], 129)
        # Sample 864: 10000 - 14 * 715 = -10, held at the target 0.
        self.assertEqual(x[864], 128)
        self.assertEqual(set(x[864:]), {128})
        # Never more than one step of gain per sample: the output moves by
        # at most 1 here, 127 * 14 / 10000 rounded up.
        self.assertTrue(all(abs(a - b) <= 1 for a, b in zip(x, x[1:])))

    def test_change_in_a_tone(self):
        # The tone grid is 37500 Hz, step 14. 1000 Hz: half = (37500 * 128
        # + 500) / 1000 = 4800 units, 18.75 samples.
        duty, w = self.render(RAMPED + [tone(50, 1000), gain(52, 5000), stop(70)])
        self.assertEqual(w, [])
        x = as_bytes(duty[frame(50):frame(70)])
        # The change names grid sample frame(52) - frame(50) = 1950 - 1875 = 75.
        # Sample 74 ends on an edge, 74 * 256 = 18944 to 19200: low, amp 127.
        self.assertEqual(x[74], 128 - 127)
        # Sample 75 starts the 5th period, high. Gain 9986:
        # (127 * 9986 + 5000) / 10000 = 127.
        self.assertEqual(x[75], 128 + 127)
        # Sample 175: units 44800 to 45056, after 9 edges, low. Gain
        # 10000 - 14 * 101 = 8586: (127 * 8586 + 5000) / 10000 = 109.
        self.assertEqual(x[175], 128 - 109)
        # Sample 450: units 115200, on the 24th edge, high. The gain reached
        # 5000 at sample 75 + 357: (127 * 5000 + 5000) / 10000 = 64.
        self.assertEqual(x[450], 128 + 64)
        self.assertEqual((max(x[450:]), min(x[450:])), (128 + 64, 128 - 64))

    def test_change_at_8000_hz(self):
        # step = (500000 + 7999) / 8000 = 63, once per sample, not per frame.
        d = self.sound("a", bytes([0]) * 800)      # 100 ms at 8000 Hz
        duty, w = self.render([pcm(0, d, rate=8000), gain(10, 2000)], scale="nearest")
        self.assertEqual(w, [])

        def at(i):
            """The byte of sample i, at its first frame, ceil(i * 37500 / 8000)."""
            return as_bytes([duty[-(-i * FR // 8000)]])[0]

        # The change names sample 10 * 8000 / 1000 = 80.
        self.assertEqual(at(79), 0)              # -128 at gain 10000
        # Sample 80: 9937, -128 * 9937 = -1271936, -(5000 + 1271936) / 10000 = -127.
        self.assertEqual(at(80), 1)
        # Frames 375 to 379 hold sample 80: one step of gain, not five.
        self.assertEqual(set(as_bytes(duty[375:380])), {1})
        # Sample 150: 10000 - 63 * 71 = 5527, -128 * 5527 = -707456, so -71.
        self.assertEqual(at(150), 57)
        # Sample 205: 10000 - 63 * 126 = 2062, -263936, -(268936) / 10000 = -26.
        self.assertEqual(at(205), 102)
        # Sample 206 would be 1999: held at 2000, -256000, -(261000) / 10000 = -26.
        self.assertEqual(at(206), 102)
        self.assertEqual(at(799), 102)

    def test_pulse_keeps_its_width(self):
        plain, _ = self.render([tone(0, 1000, vol=60), stop(100)])
        g, _ = self.render([tone(0, 1000, vol=60), gain(10, 1000), stop(100)])
        self.assertEqual(list(plain), list(g))

    def test_gain_with_nothing_playing_is_a_bad_log(self):
        self.assertFails([gain(10, 5000)], "op=gain with nothing playing")

    def test_gain_past_10000_is_a_bad_log(self):
        d = self.sound("a", bytes([200]) * 100)
        self.assertFails([with_gain(pcm(0, d), 10001)], "gain=10001 is past 10000")
        self.assertFails([pcm(0, d), gain(1, 10001)], "gain=10001 is past 10000")

    def test_a_later_field_does_not_matter(self):
        d = self.sound("a", bytes([255]) * 1500)
        a, _ = self.render([pcm(0, d, rate=FR), gain(4, 0)])
        b, _ = self.render([pcm(0, d, rate=FR), gain(4, 0) + " kind=1"])
        self.assertEqual(list(a), list(b))


class GainAcrossNotes(Case):
    """The gain across the notes of one centred play. Grid 37500 Hz, step
    14, one sample per frame. A change at 52 ms names frame(52) = 1950:
    frame f holds gain 10000 - 14 * (f - 1949) until it reaches 5000 at
    frame 1950 + 357 = 2307. 1000 Hz has half 4800 units, 2000 Hz 2400."""

    def test_next_note_keeps_the_move(self):
        duty, w = self.render(RAMPED + [tone(50, 1000), gain(52, 5000), tone(60, 2000, vol=50),
                                        stop(100)])
        self.assertEqual(w, [])
        x = as_bytes(duty)
        # Frame 2249, sample 374 of the first note: units 95744 to 96000,
        # before the 20th edge, low. Gain 10000 - 14 * 300 = 5800:
        # (127 * 5800 + 5000) / 10000 = 74.
        self.assertEqual(x[2249], 128 - 74)
        # Frame 2250 starts the second note at an edge going up. The gain
        # goes on: 10000 - 14 * 301 = 5786, (127 * 5786 + 5000) / 10000 = 73.
        self.assertEqual(x[2250], 128 + 73)
        # Frame 2260, sample 10 of the note: units 2560 to 2816, after the
        # edge at 2400, low. Gain 10000 - 14 * 311 = 5646: 72.
        self.assertEqual(x[2260], 128 - 72)
        # Frame 2320, sample 70: units 17920 to 18176, after 7 edges, low.
        # The gain reached 5000 at frame 2307: (127 * 5000 + 5000) / 10000 = 64.
        self.assertEqual(x[2320], 128 - 64)
        self.assertEqual((max(x[2307:2625]), min(x[2307:2625])), (128 + 64, 128 - 64))
        # No frame of the move stands above the gain it holds.
        for f in range(1950, 2307):
            g = 10000 - 14 * (f - 1949)
            self.assertLessEqual(abs(x[f] - 128), (127 * g + 5000) // 10000, f)

    def test_note_after_a_rest_starts_at_its_gain(self):
        duty, w = self.render(RAMPED + [tone(50, 1000), gain(52, 5000), stop(60),
                                        tone(70, 2000, vol=50), stop(100)])
        self.assertEqual(w, [])
        x = as_bytes(duty)
        # The rest holds 128.
        self.assertEqual(set(x[frame(60):frame(70)]), {128})
        # Frame 2625 starts the note at its own gain 5000, high: 64.
        self.assertEqual(x[frame(70)], 128 + 64)
        # Frame 2635, sample 10: low, 64.
        self.assertEqual(x[frame(70) + 10], 128 - 64)

    def test_a_line_off_the_target_starts_at_its_gain(self):
        # An older driver printed no op=gain: the note takes its own vol.
        duty, w = self.render(RAMPED + [tone(50, 1000), tone(60, 2000, vol=50), stop(100)])
        self.assertEqual(w, [])
        x = as_bytes(duty)
        self.assertEqual(x[2249], 128 - 127)
        self.assertEqual(x[2250], 128 + 64)

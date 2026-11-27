"""Tests of tools/voice_engine.py and tools/tone_square.py against the C.

The two engines copy the integer steps of src/lib/audio_voice.c and
src/lib/audio_square.c. These tests read the C sources and headers, so a
change in the library that the tools do not follow fails here.
"""

import importlib.util
import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def _load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tools", name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


V = _load("voice_engine")
SQ = _load("tone_square")
AP = _load("audio_preview")


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as f:
        return f.read()


def define(text, name):
    """The value of `#define name value`, with a u suffix and a shift allowed."""
    m = re.search(r"^#define\s+%s\s+\(?\s*([0-9a-fA-Fx]+)u?(?:\s*<<\s*([0-9]+))?\s*\)?\s*(?:/\*.*)?$"
                  % re.escape(name), text, re.M)
    if m is None:
        raise AssertionError("no #define %s" % name)
    v = int(m.group(1), 0)
    return v << int(m.group(2)) if m.group(2) else v


AUDIO_H = read("include", "robco", "audio.h")
AUDIO_C = read("src", "drv", "audio.c")


class ConstantsMatchTheC(unittest.TestCase):
    def test_voice_limits(self):
        self.assertEqual(V.N_MAX, define(AUDIO_H, "AUDIO_VOICE_N_MAX"))
        self.assertEqual(V.INC_MAX, define(AUDIO_H, "AUDIO_VOICE_INC_MAX"))
        self.assertEqual(V.AMP_MAX, define(AUDIO_H, "AUDIO_VOICE_AMP_MAX"))
        self.assertEqual(V.P2_MAX, define(AUDIO_H, "AUDIO_VOICE_P2_MAX"))
        self.assertEqual(V.SINE_LEN, define(AUDIO_H, "AUDIO_SINE_LEN"))

    def test_tone_defaults(self):
        self.assertEqual(SQ.TONE_PEAK, define(AUDIO_C, "ROBCO_AUDIO_TONE_PEAK"))
        self.assertEqual(SQ.TONE_RATE, define(AUDIO_C, "ROBCO_AUDIO_TONE_RATE"))
        self.assertIn("ROBCO_AUDIO_TONE_PEAK <= %d" % SQ.PEAK_MAX, AUDIO_C)

    def test_preview_bounds(self):
        self.assertEqual(AP.RATE_MIN, define(AUDIO_H, "AUDIO_PCM_RATE_MIN"))
        self.assertEqual(AP.RATE_MAX, define(AUDIO_H, "AUDIO_PCM_RATE_MAX"))
        self.assertEqual(AP.HZ_MIN, define(AUDIO_H, "AUDIO_HZ_MIN"))
        self.assertEqual(AP.HZ_MAX, define(AUDIO_H, "AUDIO_HZ_MAX"))
        self.assertEqual(AP.PWM_CLOCK, define(AUDIO_H, "AUDIO_PWM_CLOCK_TARGET"))
        self.assertEqual(AP.OUT_RATE * define(AUDIO_H, "AUDIO_PCM_PWM_RANGE"), AP.PWM_CLOCK)

    def test_ramp(self):
        self.assertEqual(AP.RAMP_LEN, define(AUDIO_C, "RAMP_LEN"))
        self.assertEqual(AP.RAMP_RATE, define(AUDIO_C, "RAMP_RATE"))


class Sine(unittest.TestCase):
    def test_crc(self):
        self.assertEqual(V.sine_crc(), 0x05067686)
        self.assertEqual(V.SINE_CRC, 0x05067686)
        self.assertEqual(len(V.SINE), 1024)

    def test_same_table_as_the_c(self):
        text = read("src", "lib", "audio_voice.c")
        m = re.search(r"audio_sine\[AUDIO_SINE_LEN\]\s*=\s*\{([^}]*)\}", text)
        self.assertIsNotNone(m)
        table = tuple(int(x) for x in m.group(1).replace("\n", " ").split(",") if x.strip())
        self.assertEqual(table, V.SINE)


class VoiceEngine(unittest.TestCase):
    def test_vector_of_the_host_test(self):
        # tests/host/test_audio_voice.c checks the same span.
        sp = (1000, 0, 0x10000000, 0, 20 << 16, 0, 0)
        self.assertEqual((V.span_at(sp, 4), V.span_at(sp, 5)), (5119, 4730))

    def test_one_rounding(self):
        # 100 on 1/256 of a step plus +1 step at volume 50 rounds once to 1.
        self.assertEqual(V.out_byte(100, 129, 50), 129)
        self.assertEqual(V.out_byte(-128, 128, 100), 127)
        self.assertEqual(V.out_byte(-127, 128, 100), 128)
        self.assertEqual(V.out_byte(128, 128, 100), 129)
        self.assertEqual(V.out_byte(30000, 255, 100), 255)
        self.assertEqual(V.out_byte(-30000, 0, 100), 0)

    def test_pack_is_28_bytes_a_span(self):
        spans = [(10, 1, 2, -3, 4 << 16, -5, 6), (20, 0, 7, 0, 0, 0, 0)]
        data = V.pack(spans)
        self.assertEqual(len(data), V.SPAN_BYTES * 2)
        self.assertEqual(V.unpack(data), spans)
        self.assertIsNone(V.check(spans[1:]))


class ToneSquare(unittest.TestCase):
    def test_half_period_floor(self):
        self.assertEqual(SQ.half_period(18750), 256)
        self.assertEqual(SQ.half_period(20000), 256)
        self.assertEqual(SQ.half_period(1000), 4800)
        self.assertEqual(SQ.half_period(0), 0)

    def test_amp(self):
        self.assertEqual([SQ.amp(127, v) for v in (100, 80, 20, 0, 150)], [127, 102, 25, 0, 127])


if __name__ == "__main__":
    unittest.main()

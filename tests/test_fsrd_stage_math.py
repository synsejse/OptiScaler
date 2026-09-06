"""CPU references for diagnostic stage switches, not GPU/image-quality tests."""
import itertools
import unittest

from test_fsrd_fidelity import fp16, fp32, fused_round_trip, material_guides, FP16_MAX, ZERO_RGB


def convert(raw, albedo, divide):
    signal = fp16(min(FP16_MAX, max(0, fp32(raw / albedo if divide else raw))))
    restored = fp32(signal * albedo) if divide else signal
    return signal, fp16(fp32(raw - restored))


def compose(signal, albedo, residual, multiply, add):
    color = fp32(signal * albedo) if multiply else signal
    color = fp32(color + (residual if add else 0))
    if not multiply:
        color = min(FP16_MAX, max(-FP16_MAX, color))
    return fp16(color)


class StageMath(unittest.TestCase):
    def test_default_matches_existing_identity_reference(self):
        for raw in (-65504., -.125, 0., fp16(.001), 1., 500., 65504.):
            for guide in (0., .01, .25, 1.):
                previous = fused_round_trip((raw,) * 3, (guide,) * 3, ZERO_RGB)
                albedo = previous["fused"][0]
                signal, residual = convert(raw, albedo, True)
                self.assertEqual(signal, previous["signal"][0])
                self.assertEqual(residual, previous["residual"][0])
                self.assertEqual(compose(signal, albedo, residual, True, True), previous["composed"][0])

    def test_no_albedo_identity_preserves_signed_source(self):
        for code in range(1, 1024):
            albedo = fp32(code / 1023.)
            for raw in (-65504., -.125, 0., fp16(.0001), .125, 100., 65504.):
                signal, residual = convert(raw, albedo, False)
                self.assertEqual(compose(signal, albedo, residual, False, True), raw)

    def test_each_switch_changes_only_its_stage(self):
        albedo = material_guides((.2,) * 3, ZERO_RGB)[2][0]
        raw = fp16(.7)
        for divide, multiply, add in itertools.product((False, True), repeat=3):
            signal, residual = convert(raw, albedo, divide)
            result = compose(signal, albedo, residual, multiply, add)
            expected = fp32(signal * albedo) if multiply else signal
            if add:
                expected += residual
            self.assertEqual(result, fp16(fp32(expected)))
        signal, residual = convert(raw, albedo, True)
        # Switching multiply off must NOT recalculate the residual to cancel it.
        self.assertNotAlmostEqual(compose(signal, albedo, residual, False, True), raw, places=1)
        self.assertEqual(compose(signal, albedo, residual, False, False), signal)

    def test_mismatched_lighting_and_residual_do_not_overflow_fp16(self):
        signal, residual = convert(FP16_MAX, fp32(1 / 1023.), True)
        self.assertGreater(signal + residual, FP16_MAX)
        self.assertEqual(compose(signal, 1 / 1023., residual, False, True), FP16_MAX)


if __name__ == "__main__":
    unittest.main()

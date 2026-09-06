"""CPU numerical reference for the linear-albedo fused bridge; no NumPy dependency.

These tests exercise float/texture rounding and semantic invariants, not GPU shader
execution or denoiser image quality. Source-level layout guards and live captures
cover those separately. Only the captured FP16-source contract is claimed here.
"""
import math
import random
import struct
import unittest


FP16_MAX = 65504.0
FP16_MIN_SUBNORMAL = 2.0 ** -24
ZERO_RGB = (0.0, 0.0, 0.0)


def fp32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def fp16(value):
    return struct.unpack("<e", struct.pack("<e", value))[0]


def finite_rgb(values):
    """Mirror the shader's whole-vector finite guard, not a material classifier."""
    values = tuple(fp32(value) for value in values)
    return values if all(math.isfinite(value) for value in values) else ZERO_RGB


def unorm10(value):
    # HLSL round uses nearest-even; storage is normalized integer, exposed as FP32.
    value = min(1.0, max(0.0, value))
    return fp32(round(fp32(value * 1023.0)) / 1023.0)


def material_guides(diffuse, specular):
    diffuse = tuple(unorm10(value) for value in finite_rgb(diffuse))
    specular = tuple(unorm10(value) for value in finite_rgb(specular))
    # The floor belongs solely to the shared denominator. Quantize it before use.
    fused = tuple(unorm10(max(fp32(1e-3), d, s)) for d, s in zip(diffuse, specular))
    return diffuse, specular, fused


def fused_round_trip(color, diffuse, specular, *, fused_arithmetic=False):
    """Identity-denoiser path, including FP16 signal/residual/composed storage.

    Exercise both separately rounded multiplies and an FMA-like single rounding;
    shader compiler contraction must not determine whether source color survives.
    Python doubles can represent the FP16 x normalized-FP32 product exactly.
    """
    raw = finite_rgb(color)
    diffuse, specular, fused = material_guides(diffuse, specular)
    signal, residual, composed = [], [], []
    for value, denominator in zip(raw, fused):
        demodulated = fp16(min(FP16_MAX, max(0.0, fp32(value / denominator))))
        product = demodulated * denominator
        preserved = fp16(fp32(value - (product if fused_arithmetic else fp32(product))))
        result = fp16(fp32((product if fused_arithmetic else fp32(product)) + preserved))
        signal.append(demodulated)
        residual.append(preserved)
        composed.append(result)
    return {"diffuse": diffuse, "specular": specular, "fused": fused,
            "signal": tuple(signal), "residual": tuple(residual), "composed": tuple(composed)}


def safe_roughness(value):
    return min(1.0, max(0.0, value)) if math.isfinite(value) else 0.0


def safe_hit_distance(value):
    return fp16(min(FP16_MAX, max(0.0, value))) if math.isfinite(value) else 0.0


def safe_motion_xy(values):
    return tuple(values) if all(math.isfinite(value) for value in values) else (0.0, 0.0)


def safe_depth_delta(value):
    return min(FP16_MAX, max(-FP16_MAX, value)) if math.isfinite(value) else 0.0


def valid_surface(position, normal, far_plane, *, source_depth=0.5):
    valid_depth = math.isfinite(source_depth)
    linear_depth = abs(position[2]) if valid_depth and math.isfinite(position[2]) else far_plane
    return (valid_depth and all(math.isfinite(value) for value in position)
            and abs(linear_depth - far_plane) > 1e-2
            and all(math.isfinite(value) for value in normal)
            and sum(value * value for value in normal) > 1e-12)


class FusedFidelity(unittest.TestCase):
    def test_zero_guides_remain_zero_but_denominator_is_nonzero(self):
        diffuse, specular, fused = material_guides(ZERO_RGB, ZERO_RGB)
        self.assertEqual(diffuse, ZERO_RGB)
        self.assertEqual(specular, ZERO_RGB)
        self.assertEqual(fused, (fp32(1.0 / 1023.0),) * 3)

    def test_single_material_component_does_not_invent_the_other(self):
        guide = (0.0, 0.25, 1.0)
        diffuse, specular, _ = material_guides(guide, ZERO_RGB)
        self.assertEqual(specular, ZERO_RGB)
        self.assertEqual(diffuse, tuple(map(unorm10, guide)))
        diffuse, specular, _ = material_guides(ZERO_RGB, guide)
        self.assertEqual(diffuse, ZERO_RGB)
        self.assertEqual(specular, tuple(map(unorm10, guide)))

    def test_white_material_guides_are_not_energy_corrected(self):
        white = (1.0, 1.0, 1.0)
        diffuse, specular, fused = material_guides(white, white)
        self.assertEqual(diffuse, white)
        self.assertEqual(specular, white)
        self.assertEqual(fused, white)

    def test_dark_material_has_no_per_guide_floor_or_eligibility_threshold(self):
        diffuse, specular, _ = material_guides((1.0 / 255.0, 0.0, 0.0), ZERO_RGB)
        self.assertLess(sum(diffuse), 1e-2)
        self.assertGreater(diffuse[0], 0.0)
        self.assertEqual(diffuse[1:], (0.0, 0.0))
        self.assertEqual(specular, ZERO_RGB)
        self.assertTrue(valid_surface((0.0, 0.0, 1.0), (0.0, 0.0, 1.0), 100.0))

    def test_denominator_matches_all_1024_storage_codes(self):
        for code in range(1024):
            with self.subTest(code=code):
                value = fp32(code / 1023.0)
                self.assertEqual(unorm10(value), value)
                _, _, fused = material_guides((value,) * 3, ZERO_RGB)
                self.assertEqual(fused, (fp32(max(1, code) / 1023.0),) * 3)

    def test_signed_fp16_extremes_and_small_values_survive(self):
        colors = [(FP16_MAX, -FP16_MAX, FP16_MIN_SUBNORMAL),
                  (fp16(1e-6), fp16(-1e-6), 0.0),
                  (1.0, -1.0, fp16(0.1))]
        guides = [ZERO_RGB, (1.0, 1.0, 1.0), (0.0, 0.01, 0.75)]
        for color in colors:
            for guide in guides:
                for fused_arithmetic in (False, True):
                    with self.subTest(color=color, guide=guide, fused=fused_arithmetic):
                        result = fused_round_trip(color, guide, ZERO_RGB,
                                                  fused_arithmetic=fused_arithmetic)
                        self.assertEqual(result["composed"], color)

    def test_saturated_demodulation_preserves_overflow_in_residual(self):
        color = (FP16_MAX,) * 3
        result = fused_round_trip(color, ZERO_RGB, ZERO_RGB)
        self.assertEqual(result["signal"], color)
        self.assertTrue(all(value > 0.0 for value in result["residual"]))
        self.assertEqual(result["composed"], color)

    def test_negative_source_is_residual_not_clipped_or_sent_to_rr(self):
        color = (-FP16_MAX, -1.0, -FP16_MIN_SUBNORMAL)
        result = fused_round_trip(color, (0.5,) * 3, ZERO_RGB)
        self.assertEqual(result["signal"], ZERO_RGB)
        self.assertEqual(result["residual"], color)
        self.assertEqual(result["composed"], color)

    def test_rounding_overshoot_can_have_negative_residual(self):
        cases = [fused_round_trip((1.0,) * 3, (code / 1023.0,) * 3, ZERO_RGB)
                 for code in range(1, 1024)]
        self.assertTrue(any(result["residual"][0] < 0.0 for result in cases))
        self.assertTrue(all(result["composed"] == (1.0,) * 3 for result in cases))

    def test_random_fp16_values_round_trip(self):
        rng = random.Random(0xF5_11)
        for index in range(1000):
            # Sample representable bit patterns, including subnormals and both signs;
            # uniform real-valued sampling would barely exercise small magnitudes.
            bits = [rng.randrange(0x7C00) | (rng.randrange(2) << 15) for _ in range(3)]
            color = tuple(struct.unpack("<e", struct.pack("<H", value))[0] for value in bits)
            diffuse = tuple(rng.randrange(256) / 255.0 for _ in range(3))
            specular = tuple(rng.randrange(256) / 255.0 for _ in range(3))
            for fused_arithmetic in (False, True):
                with self.subTest(index=index, fused=fused_arithmetic):
                    result = fused_round_trip(color, diffuse, specular,
                                              fused_arithmetic=fused_arithmetic)
                    self.assertEqual(result["composed"], color)
                    self.assertTrue(all(math.isfinite(v) for v in result["signal"] + result["residual"]))

    def test_nonfinite_source_and_guides_do_not_reach_rr(self):
        for invalid in (math.nan, math.inf, -math.inf):
            with self.subTest(invalid=invalid):
                result = fused_round_trip((1.0, invalid, 2.0), (0.2,) * 3, ZERO_RGB)
                self.assertEqual(result["signal"], ZERO_RGB)
                self.assertEqual(result["composed"], ZERO_RGB)
                diffuse, specular, fused = material_guides((invalid, 1.0, 1.0), (1.0, invalid, 1.0))
                self.assertEqual(diffuse, ZERO_RGB)
                self.assertEqual(specular, ZERO_RGB)
                self.assertTrue(all(math.isfinite(value) and value > 0.0 for value in fused))

    def test_nonfinite_normal_depth_roughness_and_hit_distance_guards(self):
        for invalid in (math.nan, math.inf, -math.inf):
            with self.subTest(invalid=invalid):
                self.assertFalse(valid_surface((0.0, 0.0, invalid), (0.0, 0.0, 1.0), 100.0))
                # Even a finite reconstructed/clamped position cannot legitimize invalid source depth.
                self.assertFalse(valid_surface((0.0, 0.0, 1.0), (0.0, 0.0, 1.0), 100.0,
                                               source_depth=invalid))
                self.assertFalse(valid_surface((0.0, 0.0, 1.0), (invalid, 0.0, 1.0), 100.0))
                self.assertEqual(safe_roughness(invalid), 0.0)
                self.assertEqual(safe_hit_distance(invalid), 0.0)
        self.assertFalse(valid_surface((0.0, 0.0, 1.0), ZERO_RGB, 100.0))
        self.assertFalse(valid_surface((0.0, 0.0, 100.0), (0.0, 0.0, 1.0), 100.0))
        self.assertEqual(safe_roughness(-0.1), 0.0)
        self.assertEqual(safe_roughness(1.1), 1.0)
        self.assertEqual(safe_hit_distance(-1.0), 0.0)
        self.assertEqual(safe_hit_distance(FP16_MAX), FP16_MAX)
        self.assertEqual(safe_hit_distance(FP16_MAX * 2.0), FP16_MAX)

    def test_motion_finite_guard_preserves_xy_without_clamping(self):
        for values in ((-0.125, 0.25), (-FP16_MAX, FP16_MAX), (1e6, -1e6)):
            self.assertEqual(safe_motion_xy(values), values)
        for invalid in (math.nan, math.inf, -math.inf):
            self.assertEqual(safe_motion_xy((invalid, 1.0)), (0.0, 0.0))
            self.assertEqual(safe_motion_xy((1.0, invalid)), (0.0, 0.0))

    def test_derived_depth_delta_is_bounded_for_fp16_storage(self):
        for value in (-FP16_MAX, -0.125, 0.0, 0.125, FP16_MAX):
            self.assertEqual(safe_depth_delta(value), value)
        self.assertEqual(safe_depth_delta(1e6), FP16_MAX)
        self.assertEqual(safe_depth_delta(-1e6), -FP16_MAX)
        for invalid in (math.nan, math.inf, -math.inf):
            self.assertEqual(safe_depth_delta(invalid), 0.0)


if __name__ == "__main__":
    unittest.main()

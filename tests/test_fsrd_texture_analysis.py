"""Numerical checks for the offline researcher; skipped in standard CI without NumPy."""
import unittest
from pathlib import Path

try:
    import numpy as np
except ImportError:
    raise unittest.SkipTest("Offline texture analysis requires NumPy")

from analyze_fsrd_textures import (analyze_pair, capture_identity, decode_octahedral, fraction, geometric_positions,
                                   hardware_depth_to_view_z, historical_inverse_projection,
                                   reproject_pixel_positions, sample_bilinear_scalar, shader_clamp_diagnostics, stats)


class TextureAnalysis(unittest.TestCase):
    def test_octahedral_axes(self):
        uv = np.array([[[.5, .5], [1., .5], [.5, 1.], [1., 1.]]])
        expected = np.array([[[0, 0, 1], [1, 0, 0], [0, 1, 0], [0, 0, -1]]])
        np.testing.assert_allclose(decode_octahedral(uv), expected)

    def test_manifest_matrix_memory_is_transposed_for_hlsl(self):
        inv_view = np.eye(4)
        inv_view[3, :3] = [10, 20, 30]
        previous_view = np.eye(4)
        previous_view[3, :3] = [-10, -20, -29]
        meta = {"inv_view": inv_view.tolist(), "previous_view": previous_view.tolist(),
                "inv_projection": np.eye(4).tolist(), "near": .01, "far": 100,
                "conversion_flags": 0}
        view, world, previous = geometric_positions(np.array([[.5]]), meta)
        np.testing.assert_allclose(view[0, 0], [0, 0, .5, 1])
        np.testing.assert_allclose(world[0, 0], [10, 20, 30.5, 1])
        np.testing.assert_allclose(previous[0, 0], [0, 0, 1.5, 1])

    def test_nonfinite_values_are_not_hidden(self):
        result = stats([1, 2, float("nan"), float("inf")])
        self.assertEqual(result["count"], 4)
        self.assertEqual(result["nonfinite"], 2)
        self.assertEqual(result["mean"], 1.5)

    def test_empty_fraction_is_not_reported_as_zero(self):
        self.assertIsNone(fraction([]))
        self.assertEqual(fraction([False, True]), .5)

    def test_pair_identity_includes_pid_and_complete_label(self):
        self.assertEqual(capture_identity(Path("current-with-hyphens-324-1000000-10")),
                         ("current-with-hyphens", "324", "1000000"))
        self.assertNotEqual(capture_identity(Path("current-324-1000000-10")),
                            capture_identity(Path("current-325-1000000-11")))

    def test_reversed_hardware_depth_conversion(self):
        # Infinite reversed perspective: device depth = near / positive view Z.
        inv = np.eye(4)
        inv[2:, 2:] = [[0, 50], [1, 0]]
        np.testing.assert_allclose(hardware_depth_to_view_z(np.array([.02, .002]), inv), [1, 10])
        inv[0, 2] = .1
        with self.assertRaises(ValueError):
            hardware_depth_to_view_z(.02, inv)

    def test_pure_fused_has_no_input_color_clamp(self):
        values = np.array([65500, 65501, 65504, 65505], dtype=np.float64)
        current = shader_clamp_diagnostics({"pipeline": "pure_fused"}, values, values)
        self.assertIsNone(current["input_color_shader_clamp_bounds"])
        self.assertIsNone(current["input_above_shader_color_clamp_fraction"])
        self.assertEqual(current["hit_distance_shader_clamp_max"], 65504)
        self.assertEqual(current["hit_distance_above_shader_clamp_fraction"], .25)
        legacy = shader_clamp_diagnostics({}, values, values)
        self.assertEqual(legacy["input_color_shader_clamp_bounds"], [0, 65500])
        self.assertEqual(legacy["input_above_shader_color_clamp_fraction"], .75)
        self.assertEqual(legacy["hit_distance_above_shader_clamp_fraction"], .75)

    @staticmethod
    def inverse_reversed_projection(near):
        # Column-major matrix memory used by the captured shader: depth = near / viewZ.
        inv = np.eye(4)
        inv[2:, 2:] = [[0, 1 / near], [1, 0]]
        return inv

    def test_historical_projection_is_not_current_projection(self):
        old_inverse = self.inverse_reversed_projection(2)
        current_inverse = self.inverse_reversed_projection(1)
        current = {"inv_projection": current_inverse.tolist(),
                   "previous_projection": np.linalg.inv(old_inverse).tolist()}
        previous = {"inv_projection": old_inverse.tolist()}
        np.testing.assert_allclose(historical_inverse_projection(current, previous), old_inverse)
        del current["previous_projection"]
        np.testing.assert_allclose(historical_inverse_projection(current, previous), old_inverse)
        # Even with zero camera movement, the same hardware depth maps to different view depths.
        self.assertEqual(hardware_depth_to_view_z(.25, old_inverse), 8)
        self.assertEqual(hardware_depth_to_view_z(.25, current_inverse), 4)

    def test_pair_keeps_camera_and_converted_object_motion_separate(self):
        current_inverse = self.inverse_reversed_projection(1)
        old_inverse = self.inverse_reversed_projection(2)
        identity = np.eye(4).tolist()
        current = {"inv_projection": current_inverse.tolist(),
                   "previous_projection": np.linalg.inv(old_inverse).tolist(),
                   "inv_view": identity, "previous_view": identity, "near": 1, "far": 1e5,
                   "conversion_flags": 37, "amd_jitter": [0, 0], "amd_motion_scale": [1, 1, 1]}
        previous = {"inv_projection": old_inverse.tolist(), "amd_jitter": [0, 0]}
        # Stationary camera, surface moves from previous depth5.25 to current depth5.
        # Previous projection near-plane also differs, so current coefficients are incorrect.
        current_hw, previous_hw = 1 / 5, 2 / 5.25
        tex = {"input_depth": np.array([[[current_hw, 0, 0, 1]]], dtype=np.float32),
               "converted_depth": np.array([[[5, 0, 0, 1]]], dtype=np.float32),
               "input_motion": np.array([[[0, 0, (previous_hw - current_hw) * 1000, 1]]], dtype=np.float32),
               "converted_motion": np.array([[[0, 0, .25, 0]]], dtype=np.float32)}
        old_tex = {"input_depth": np.array([[[previous_hw, 0, 0, 1]]], dtype=np.float32)}
        pair = analyze_pair(Path("current"), current, tex, Path("previous"), previous, old_tex)
        self.assertAlmostEqual(pair["camera_previous_depth_error_w1"]["mean"], .25, places=5)
        self.assertLess(pair["converted_previous_depth_error_w1"]["max"], 1e-5)
        self.assertLess(pair["encoded_engine_z_previous_depth_error_w1"]["max"], 1e-5)
        self.assertAlmostEqual(pair["converted_depth_delta_minus_camera"]["mean"], .25, places=5)

    def test_reprojection_adds_previous_minus_current_pixel_jitter(self):
        motion = np.zeros((2, 4, 4))
        current = {"amd_jitter": [.125, -.25], "amd_motion_scale": [1, 1, 1]}
        previous = {"amd_jitter": [-.125, .25]}
        x, y = reproject_pixel_positions(motion, current, previous)
        grid_y, grid_x = np.mgrid[:2, :4]
        np.testing.assert_allclose(x, grid_x - .5)
        np.testing.assert_allclose(y, grid_y - .5)

    def test_reprojection_filters_hardware_not_linear_depth(self):
        sampled, inside = sample_bilinear_scalar(np.array([[1., .5]]), np.array([[.5]]), np.array([[0.]]))
        self.assertTrue(inside[0, 0])
        np.testing.assert_allclose(sampled, [[.75]])
        linear = hardware_depth_to_view_z(sampled, self.inverse_reversed_projection(1))
        np.testing.assert_allclose(linear, [[4 / 3]])
        self.assertNotAlmostEqual(float(linear[0, 0]), (1 + 2) / 2)
        _, inside = sample_bilinear_scalar(np.array([[1., .5]]), np.array([[float("nan")]]), np.array([[0.]]))
        self.assertFalse(inside[0, 0])


if __name__ == "__main__":
    unittest.main()

"""Numerical checks for the offline researcher; skipped in standard CI without NumPy."""
import unittest
from pathlib import Path

try:
    import numpy as np
except ImportError:
    raise unittest.SkipTest("Offline texture analysis requires NumPy")

from analyze_fsrd_textures import (capture_identity, decode_octahedral, fraction, geometric_positions,
                                   hardware_depth_to_view_z, stats)


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


if __name__ == "__main__":
    unittest.main()

"""Numerical checks for the offline researcher; skipped in standard CI without NumPy."""
import unittest

try:
    import numpy as np
except ImportError:
    raise unittest.SkipTest("Offline texture analysis requires NumPy")

from analyze_fsrd_textures import decode_octahedral, geometric_positions, stats


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


if __name__ == "__main__":
    unittest.main()

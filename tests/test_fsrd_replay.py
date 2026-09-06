"""Native packing used by the independent captured-frame replay tool."""
import importlib.util
from pathlib import Path
import unittest

try:
    import numpy as np
except ImportError:
    np = None

if np is not None:
    spec = importlib.util.spec_from_file_location("prepare_replay", Path(__file__).resolve().parents[1] /
                                                "tools/fsrrr-replay/prepare.py")
    replay = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(replay)


@unittest.skipIf(np is None, "NumPy not installed")
class NativePacking(unittest.TestCase):
    def test_every_unorm_code(self):
        codes = np.arange(1024, dtype=np.float32)
        values = np.stack((codes / 1023, codes[::-1] / 1023, codes / 1023, (codes % 4) / 3), -1)[None]
        data = replay.encode(values, 24)
        restored = replay.decode(data, 24, 1024, 1)
        np.testing.assert_allclose(values, restored, atol=1e-7, rtol=0)
        self.assertEqual(data, replay.encode(restored, 24))

    def test_finite_half_bitpatterns(self):
        codes = np.arange(65536, dtype="<u2")
        values = codes.view("<f2")
        values = values[np.isfinite(values)].astype(np.float32).reshape(1, -1, 4)
        restored = replay.decode(replay.encode(values, 10), 10, values.shape[1], 1)
        np.testing.assert_array_equal(values, restored)
        np.testing.assert_array_equal(np.signbit(values), np.signbit(restored))

    def test_depth_keeps_full_float32(self):
        values = np.array([[[.02, 0, 0, 1], [16777.236, 0, 0, 1]]], np.float32)
        np.testing.assert_array_equal(values, replay.decode(replay.encode(values, 41), 41, 2, 1))

    def test_invalid_values_rejected(self):
        for fmt, value in [(10, 70000), (10, float("nan")), (24, -1), (24, 1.01), (41, float("inf"))]:
            with self.subTest(fmt=fmt, value=value), self.assertRaises(ValueError):
                replay.encode(np.full((1, 1, 4), value), fmt)

    def test_sqrt_encoding_quantization_bound(self):
        values = np.linspace(0, 1, 1024).reshape(1, 256, 4)
        transformed = values.copy()
        transformed[..., :3] = np.sqrt(transformed[..., :3])
        decoded = replay.decode(replay.encode(transformed, 24), 24, 256, 1)[..., :3] ** 2
        self.assertLessEqual(np.abs(decoded - values[..., :3]).max(), 1 / 1023)


if __name__ == "__main__":
    unittest.main()

"""Native packing used by the independent captured-frame replay tool."""
import importlib.util
import json
from pathlib import Path
import tempfile
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

    def test_prepare_keeps_inputs_and_allocation_sizes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = root / "capture"
            capture.mkdir()
            metadata = {
                "partial": False, "evaluation_succeeded": True, "pipeline": "pure_fused",
                "conversion_flags": 37, "render_size": [2, 1], "display_size": [4, 2],
                "frame": 42, "inv_view": np.eye(4).tolist(), "previous_view": np.eye(4).tolist(),
                "inv_projection": np.linalg.inv(np.array([[1, 0, 0, 0], [0, 2, 0, 0],
                                                          [0, 0, 0, 1], [0, 0, .02, 0]])).tolist(),
                "amd_motion_scale": [1, 1, 1], "amd_jitter": [.001, -.002], "near": .02,
                "far": 10000, "textures": [],
            }
            formats = dict(replay.FORMATS, denoised_radiance=10)
            originals = {}
            for name, fmt in formats.items():
                values = np.zeros((1, 2, 4), dtype=np.float32)
                values[..., :3] = 1 / 1023 if fmt == 24 else .125
                values[..., 3] = 1
                filename = name + ".f32"
                originals[filename] = values.astype("<f4").tobytes()
                (capture / filename).write_bytes(originals[filename])
                metadata["textures"].append({"name": name, "file": filename, "width": 2,
                                             "height": 1, "view_format": fmt,
                                             "resource_width": 4, "resource_height": 2})
            (capture / "manifest.json").write_text(json.dumps(metadata))
            linear = replay.prepare(capture, root / "linear", "linear", 16.67, "test adapter")
            sqrt = replay.prepare(capture, root / "sqrt", "sqrt", 16.67, "test adapter")
            self.assertEqual(linear["dispatch"]["flags"], 3)
            self.assertEqual(sqrt["dispatch"]["flags"], 1)
            self.assertEqual(linear["output_size"], [4, 2])
            self.assertEqual(linear["dispatch"]["render_size"], [2, 1])
            for entry in linear["textures"]:
                self.assertEqual(entry["resource_size"], [4, 2])
                if "albedo" not in entry["name"]:
                    self.assertEqual((root / "linear" / entry["file"]).read_bytes(),
                                     (root / "sqrt" / entry["file"]).read_bytes())
            for filename, data in originals.items():
                self.assertEqual((capture / filename).read_bytes(), data)
            with self.assertRaises(FileExistsError):
                replay.prepare(capture, root / "linear", "linear", 16.67, "test adapter")


if __name__ == "__main__":
    unittest.main()

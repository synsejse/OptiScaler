"""CPU-only tests for recorded AMD dispatch/settings replay."""
import copy
import importlib.util
from pathlib import Path
import unittest

try:
    import numpy as np
except ImportError:
    np = None

if np is not None:
    spec = importlib.util.spec_from_file_location("prepare_replay", Path(__file__).with_name("prepare.py"))
    replay = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(replay)


@unittest.skipIf(np is None, "NumPy not installed")
class RecordedDispatch(unittest.TestCase):
    def setUp(self):
        self.metadata = {"render_size": [1280, 720], "amd_dispatch": {
            "render_size": [1280, 720], "frame_index": 99, "flags": 2, "delta_time_ms": 27.5,
            "motion_scale": [1, 1, 1], "jitter": [.001, -.002],
            "camera_position_delta": [10, -20, 30], "camera_right": [1, 0, 0],
            "camera_up": [0, 1, 0], "camera_forward": [0, 0, 1],
            "camera_aspect_ratio": 16 / 9, "camera_near": .02,
            "camera_far": 16000, "camera_fov_vertical": 1.0,
        }, "amd_settings": {"1": 1, "2": 1, "3": 65504, "4": 50, "5": 0, "6": .01}}

    def test_native_inputs_used_without_matrix_reconstruction(self):
        original = copy.deepcopy(self.metadata)
        dispatch = replay.dispatch_from_metadata(self.metadata, "linear", None)
        self.assertEqual(dispatch["camera_delta"], [10, -20, 30])
        self.assertEqual(dispatch["delta_ms"], 27.5)
        self.assertEqual(dispatch["frame_index"], 99)
        self.assertEqual(dispatch["flags"], 3)  # RESET is intentionally added.
        self.assertEqual(dispatch["render_size"], [1280, 720])
        self.assertEqual(self.metadata, original)

    def test_explicit_duration_and_encoding_override(self):
        dispatch = replay.dispatch_from_metadata(self.metadata, "sqrt", .0167)
        self.assertEqual(dispatch["flags"], 1)
        self.assertEqual(dispatch["delta_ms"], .0167)

    def test_settings_are_complete_finite_copy(self):
        settings = replay.captured_settings(self.metadata)
        self.assertEqual(settings, self.metadata["amd_settings"])
        settings["1"] = 4
        self.assertEqual(self.metadata["amd_settings"]["1"], 1)
        self.assertIsNone(replay.captured_settings({}))

    def test_invalid_native_inputs_rejected(self):
        for key, value in [("render_size", [640, 360]), ("jitter", [0]),
                           ("camera_right", [float("nan"), 0, 0]),
                           ("camera_near", -1), ("camera_far", .01),
                           ("camera_fov_vertical", 4), ("delta_time_ms", 0),
                           ("frame_index", -1), ("frame_index", 1.5)]:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                altered = copy.deepcopy(self.metadata)
                altered["amd_dispatch"][key] = value
                replay.dispatch_from_metadata(altered, "linear", None)

    def test_bad_settings_rejected(self):
        for value in [float("inf"), 1e100, "1", True, None]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                altered = copy.deepcopy(self.metadata)
                altered["amd_settings"]["1"] = value
                replay.captured_settings(altered)
        with self.assertRaises(ValueError):
            replay.captured_settings({"amd_settings": {"1": 1}})


if __name__ == "__main__":
    unittest.main()

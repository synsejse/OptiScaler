"""Prepare a single reset-frame RR replay, without modifying the source capture.

Requires NumPy. Shader-value captures are repacked into their native DXGI formats.
The sqrt experiment changes only albedo representation, not the lighting signal.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests"))
from analyze_fsrd_textures import load_capture


FORMATS = {
    "converted_radiance": 10,
    "converted_depth": 41,
    "converted_motion": 10,
    "converted_normals": 24,
    "converted_diffuse_albedo": 24,
    "converted_specular_albedo": 24,
    "converted_fused_albedo": 24,
}


def encode(values, fmt):
    if not np.isfinite(values).all():
        raise ValueError("Nonfinite texture values")
    if fmt == 10:
        if np.max(np.abs(values)) > 65504:
            raise ValueError("FP16 overflow")
        return values.astype("<f2").tobytes()
    if fmt == 41:
        return values[..., 0].astype("<f4").tobytes()
    if fmt == 24:
        if np.any((values < 0) | (values > 1)):
            raise ValueError("UNORM value out of range")
        q = np.rint(values.astype(np.float64) * [1023, 1023, 1023, 3]).astype("<u4")
        return (q[..., 0] | (q[..., 1] << 10) | (q[..., 2] << 20) | (q[..., 3] << 30)).tobytes()
    raise ValueError(f"Unsupported DXGI format {fmt}")


def decode(data, fmt, width, height):
    if fmt == 10:
        return np.frombuffer(data, "<f2").reshape(height, width, 4).astype(np.float32)
    if fmt == 41:
        result = np.zeros((height, width, 4), np.float32)
        result[..., 0] = np.frombuffer(data, "<f4").reshape(height, width)
        result[..., 3] = 1
        return result
    if fmt == 24:
        q = np.frombuffer(data, "<u4").reshape(height, width)
        return np.stack([(q >> shift) & mask for shift, mask in
                         [(0, 1023), (10, 1023), (20, 1023), (30, 3)]], -1).astype(np.float32) / [1023, 1023, 1023, 3]
    raise ValueError(f"Unsupported DXGI format {fmt}")


def prepare(capture, destination, encoding, delta_ms, adapter):
    if encoding not in ("linear", "sqrt") or not np.isfinite(delta_ms) or delta_ms <= 0:
        raise ValueError("Invalid encoding or frame duration")
    metadata, textures = load_capture(capture)
    if metadata.get("partial", True) or not metadata.get("evaluation_succeeded"):
        raise ValueError("Capture must be complete and successfully evaluated")
    if metadata.get("pipeline") != "pure_fused" or not metadata["conversion_flags"] & 1:
        raise ValueError("Only the current linear-albedo pure-fused capture is supported")
    width, height = metadata["render_size"]
    if not (0 < width <= 8192 and 0 < height <= 8192):
        raise ValueError("Invalid render dimensions")
    entries = {entry["name"]: entry for entry in metadata["textures"]}
    # Validate every required input before creating any files.
    prepared = {}
    for name, fmt in FORMATS.items():
        entry = entries[name]
        values = textures[name]
        if values.shape != (height, width, 4) or entry["view_format"] != fmt:
            raise ValueError(f"Unexpected format or dimensions for {name}")
        packed = encode(values, fmt)
        restored = decode(packed, fmt, width, height)
        # UNORM shader decoding has float32 rounding, but should recover the same integer codes.
        channels = 1 if fmt == 41 else 4
        error = float(np.max(np.abs(restored[..., :channels] - values[..., :channels])))
        if error > (1e-7 if fmt == 24 else 0):
            raise ValueError(f"Cannot faithfully repack {name}: {error}")
        experiment_error = 0.0
        if encoding == "sqrt" and "albedo" in name:
            alternate = values.copy()
            alternate[..., :3] = np.sqrt(alternate[..., :3])
            packed = encode(alternate, fmt)
            restored = decode(packed, fmt, width, height)
            experiment_error = float(np.max(np.abs(restored[..., :3] ** 2 - values[..., :3])))
        prepared[name] = (fmt, packed, error, experiment_error)

    inv_view = np.asarray(metadata["inv_view"], dtype=np.float64)
    inv_projection = np.asarray(metadata["inv_projection"], dtype=np.float64)
    projection = np.linalg.inv(inv_projection)
    previous_position = np.linalg.inv(np.asarray(metadata["previous_view"], dtype=np.float64))[3, :3]
    basis = inv_view[:3, :3].copy()
    basis /= np.linalg.norm(basis, axis=1)[:, None]
    basis[2] *= -1 if projection[2, 3] < 0 else 1
    # These captures have an ordinary perspective projection. Reject oblique/asymmetric projections.
    allowed = np.zeros((4, 4), bool)
    allowed[0, 0] = allowed[1, 1] = allowed[2, 2] = allowed[2, 3] = allowed[3, 2] = True
    if np.any(np.abs(projection[~allowed]) > 1e-6):
        raise ValueError("Unsupported projection")
    dispatch = {
        "render_size": [width, height], "frame_index": metadata["frame"],
        "flags": 1 | (2 if encoding == "linear" else 0),
        "motion_scale": metadata["amd_motion_scale"], "jitter": metadata["amd_jitter"],
        "camera_right": basis[0].tolist(), "camera_up": basis[1].tolist(),
        "camera_forward": basis[2].tolist(),
        "camera_delta": (previous_position - inv_view[3, :3]).tolist(),
        "aspect": float(projection[1, 1] / projection[0, 0]),
        "fov": float(2 * np.arctan(1 / projection[1, 1])),
        "near": metadata["near"], "far": metadata["far"], "delta_ms": delta_ms,
    }
    job = {
        "schema": 1, "capture": str(capture.resolve()), "encoding": encoding, "adapter": adapter,
        "source_manifest_sha256": hashlib.sha256((capture / "manifest.json").read_bytes()).hexdigest(),
        "max_render_size": [max(width, metadata["display_size"][0]), max(height, metadata["display_size"][1])],
        "dispatch": dispatch,
        "limitations": ["One fresh-context RESET dispatch, not a temporal sequence.",
                        "Frame duration is supplied, not recorded in this capture.",
                        "Provider defaults are used; live tuning values were not captured.",
                        "Sqrt mode requantizes linear albedo to RGB10; radiance is unchanged.",
                        "Camera delta is reconstructed from captured float32 matrices."],
        "textures": [],
    }
    destination.mkdir(parents=True, exist_ok=False)
    for name, (fmt, packed, error, experiment_error) in prepared.items():
        filename = name + ".bin"
        (destination / filename).write_bytes(packed)
        job["textures"].append({"name": name, "file": filename, "format": fmt,
                                "bytes": len(packed), "sha256": hashlib.sha256(packed).hexdigest(),
                                "native_repack_max_error": error,
                                "sqrt_decoded_max_error": experiment_error})
    (destination / "job.json").write_text(json.dumps(job, indent=2, allow_nan=False) + "\n")
    return job


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("destination", type=Path, help="Must not already exist")
    parser.add_argument("--encoding", choices=("linear", "sqrt"), default="linear")
    parser.add_argument("--delta-ms", type=float, default=1000 / 60)
    parser.add_argument("--adapter", default="AMD Radeon RX 9060 XT")
    args = parser.parse_args()
    prepare(args.capture, args.destination, args.encoding, args.delta_ms, args.adapter)

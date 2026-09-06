"""Analyze opt-in FSR RR shader-value captures. Requires NumPy; never modifies captures."""
import argparse
import json
from pathlib import Path

import numpy as np


def stats(values):
    values = np.asarray(values, dtype=np.float64).ravel()
    finite = values[np.isfinite(values)]
    result = {"count": int(values.size), "nonfinite": int(values.size - finite.size)}
    if finite.size:
        result.update(zip(("min", "p50", "p95", "p99", "max"),
                          map(float, np.quantile(finite, (0, .5, .95, .99, 1)))))
        result["mean"] = float(finite.mean())
    return result


def normalize(v):
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-20)


def decode_octahedral(uv):
    xy = uv.astype(np.float64) * 2 - 1
    z = 1 - np.abs(xy).sum(axis=-1)
    t = np.maximum(-z, 0)
    xy += np.where(xy >= 0, -t[..., None], t[..., None])
    return normalize(np.dstack((xy, z)))


def geometric_positions(depth, metadata):
    """Mirror the current shader's matrix storage and reconstruction, not independent ground truth."""
    h, w = depth.shape
    y, x = np.mgrid[:h, :w]
    ndc = np.stack((2 * (x + .5) / w - 1, 1 - 2 * (y + .5) / h, depth, np.ones_like(depth)), -1)
    # Manifest holds column-major HLSL matrix memory as rows: row-vector multiplication uses it directly.
    view = ndc @ np.asarray(metadata["inv_projection"], dtype=np.float64)
    view /= view[..., 3:4]
    view[..., 2] = np.clip(np.abs(view[..., 2]), metadata["near"], metadata["far"]) * (
        -1 if metadata["conversion_flags"] & 16 else 1)
    world = view @ np.asarray(metadata["inv_view"], dtype=np.float64)
    previous = world @ np.asarray(metadata["previous_view"], dtype=np.float64)
    return view, world, previous


def load_capture(directory):
    metadata = json.loads((directory / "manifest.json").read_text())
    textures = {}
    for entry in metadata["textures"]:
        if "file" not in entry:
            continue
        path = directory / entry["file"]
        if path.stat().st_size != entry["width"] * entry["height"] * 16:
            raise ValueError(f"Incomplete texture: {path}")
        textures[entry["name"]] = np.memmap(path, dtype="<f4", mode="r",
                                          shape=(entry["height"], entry["width"], 4))
    return metadata, textures


def analyze(directory):
    meta, tex = load_capture(directory)
    raw = tex["input_color"][..., :3].astype(np.float64)
    diff = tex["input_diffuse_albedo"][..., :3].astype(np.float64)
    spec = tex["input_specular_albedo"][..., :3].astype(np.float64)
    depth = tex["converted_depth"][..., 0]
    valid = (np.abs(depth - meta["far"]) > .01) & ((diff + spec).sum(axis=-1) > .01)
    valid &= np.isfinite(raw).all(axis=-1) & np.isfinite(diff + spec).all(axis=-1)
    result = {"directory": str(directory), "frame": meta["frame"], "reset": meta["reset"],
              "floor_isolation": meta["floor_isolation"], "correlation_bias": meta["correlation_bias"],
              "valid_pixel_fraction": float(valid.mean()), "inventory": meta["textures"],
              "channel_stats": {name: [stats(value[..., c]) for c in range(4)] for name, value in tex.items()}}
    for name, original in (("diffuse", diff), ("specular", spec)):
        converted = tex[f"converted_{name}_albedo"][..., :3]
        difference = np.abs(converted - original)
        result[f"{name}_albedo_abs_error"] = stats(difference[valid])
        result[f"{name}_albedo_pixels_changed_over_1pct"] = float(np.any(difference[valid] > .01, axis=-1).mean())
        result[f"{name}_albedo_outside_0_1_fraction"] = float(np.any((original[valid] < 0) | (original[valid] > 1), axis=-1).mean())
    result["albedo_sum_over_1_fraction"] = float(np.any((diff + spec)[valid] > 1, axis=-1).mean())
    normals = tex["input_normal_roughness"][..., :3].astype(np.float64)
    result["normal_input_length"] = stats(np.linalg.norm(normals[valid], axis=-1))
    decoded = decode_octahedral(tex["converted_normals"][..., :2])
    normal_valid = valid & (np.linalg.norm(normals, axis=-1) > .1)
    angles = np.rad2deg(np.arccos(np.clip(np.sum(normalize(normals) * decoded, axis=-1), -1, 1)))
    result["normal_angular_error_degrees"] = stats(angles[normal_valid])
    roughness = tex["input_normal_roughness"][..., 3] if meta["packed_roughness"] else tex["input_roughness"][..., 0]
    result["roughness_abs_error"] = stats(np.abs(roughness - tex["converted_normals"][..., 2])[valid])
    motion = tex["input_motion"].astype(np.float64)
    result["motion_xy_abs_error"] = stats(np.abs(motion[..., :2] - tex["converted_motion"][..., :2]))
    hit = tex["input_specular_hit_distance"][..., 0].astype(np.float64)
    converted_hit = tex["converted_radiance"][..., 3]
    finite_hit = valid & np.isfinite(hit) & (hit >= 0)
    result["hit_distance_abs_error"] = stats(np.abs(hit - converted_hit)[finite_hit])
    result["hit_distance_relative_error"] = stats((np.abs(hit - converted_hit) / np.maximum(hit, 1e-5))[finite_hit])
    result["hit_distance_above_fp16_fraction"] = float((hit[finite_hit] > 65504).mean())

    lum = raw @ np.array((.2126, .7152, .0722))
    skip = tex["preserved_lighting"][..., :3].astype(np.float64)
    skip_lum = skip @ np.array((.2126, .7152, .0722))
    lit = valid & (lum > 1e-4)
    result["preserved_luminance_fraction"] = stats((skip_lum / np.maximum(lum, 1e-10))[lit])
    result["preserved_luminance_energy_fraction"] = float(skip_lum[lit].sum() / np.maximum(lum[lit].sum(), 1e-20))
    albedo = tex["converted_fused_albedo"][..., :3].astype(np.float64)
    roundtrip = tex["converted_radiance"][..., :3] * albedo + skip
    result["pre_denoiser_roundtrip_abs_error"] = stats(np.abs(raw - roundtrip)[valid])
    result["pre_denoiser_roundtrip_relative_luma_error"] = stats(
        (np.abs((roundtrip - raw) @ np.array((.2126, .7152, .0722))) / np.maximum(lum, 1e-10))[lit])
    denoised = tex["denoised_radiance"][..., :3] * albedo + skip
    composed = tex["composed_color"][..., :3].astype(np.float64)
    result["composition_abs_change_from_pure_denoised"] = stats(np.abs(composed - denoised)[valid])
    change = np.abs((composed - denoised) @ np.array((.2126, .7152, .0722))) / np.maximum(lum, 1e-10)
    result["composition_relative_luma_change"] = stats(change[lit])
    result["composition_pixels_over_10pct_luma_change"] = float((change[lit] > .1).mean())

    view, world, previous = geometric_positions(tex["input_depth"][..., 0], meta)
    expected_z = np.abs(previous[..., 2]) - np.abs(view[..., 2])
    result["depth_reconstruction_abs_error"] = stats(np.abs(np.abs(view[..., 2]) - depth)[valid])
    result["camera_depth_delta_abs_error"] = stats(np.abs(expected_z - tex["converted_motion"][..., 2])[valid])
    for channel in (2, 3):
        result[f"motion_{channel}_versus_camera_depth_delta_error"] = stats(np.abs(motion[..., channel] - expected_z)[valid])

    # Compare two normal-space hypotheses on locally smooth depth surfaces. Shading normals can differ
    # from geometric normals, so this is evidence, not proof of space or exact surface correspondence.
    xyz = world[..., :3]
    dx = xyz[1:-1, 2:] - xyz[1:-1, :-2]
    dy = xyz[2:, 1:-1] - xyz[:-2, 1:-1]
    geometric = normalize(np.cross(dx, dy))
    center = depth[1:-1, 1:-1]
    smooth = np.maximum(np.abs(depth[1:-1, 2:] - depth[1:-1, :-2]),
                        np.abs(depth[2:, 1:-1] - depth[:-2, 1:-1])) < .01 * np.maximum(center, .1)
    mask = normal_valid[1:-1, 1:-1] & smooth
    world_hypothesis = normalize(normals[1:-1, 1:-1])
    view_hypothesis = normalize(normals[1:-1, 1:-1] @ np.array(meta["inv_view"])[:3, :3])
    result["normal_world_hypothesis_abs_dot_geometry"] = stats(np.abs((world_hypothesis * geometric).sum(-1))[mask])
    result["normal_view_hypothesis_abs_dot_geometry"] = stats(np.abs((view_hypothesis * geometric).sum(-1))[mask])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_root", type=Path)
    args = parser.parse_args()
    reports = []
    for manifest in sorted(args.capture_root.glob("*/manifest.json")):
        reports.append(analyze(manifest.parent))
    print(json.dumps({"captures": reports}, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()

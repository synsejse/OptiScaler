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


def fraction(values):
    """An empty selection is unmeasured, not zero percent."""
    values = np.asarray(values)
    return float(values.mean()) if values.size else None


def capture_identity(directory):
    """Keep labels containing hyphens intact and do not pair different process/feature IDs."""
    fields = directory.name.rsplit("-", 3)
    if len(fields) != 4:
        raise ValueError(f"Invalid capture directory: {directory}")
    return tuple(fields[:3])


def hardware_depth_to_view_z(depth, inverse_projection):
    """Only the simple perspective projection observed in these Cyberpunk captures."""
    inv = np.asarray(inverse_projection)
    if not np.allclose(inv[:2, 2:], 0):
        raise ValueError("Depth reconstruction needs projected XY for this projection")
    return (depth * inv[2, 2] + inv[3, 2]) / (depth * inv[2, 3] + inv[3, 3])


def shader_clamp_diagnostics(metadata, raw, finite_hit):
    """Report the actual pipeline's clamps, not the retired 65500 color clamp."""
    pure = metadata.get("pipeline") == "pure_fused"
    hit_max = 65504 if pure else 65500
    return {
        "hit_distance_shader_clamp_max": hit_max,
        "hit_distance_above_shader_clamp_fraction": fraction(np.asarray(finite_hit) > hit_max),
        "input_color_shader_clamp_bounds": None if pure else [0, 65500],
        "input_above_shader_color_clamp_fraction": None if pure else fraction(np.asarray(raw) > 65500),
        "input_outside_fp16_range_fraction": fraction(np.abs(raw) > 65504),
    }


def historical_inverse_projection(current, previous):
    """Use the projection actually saved by the current dispatch for its history."""
    if "previous_projection" in current:
        return np.linalg.inv(np.asarray(current["previous_projection"], dtype=np.float64))
    # Legacy captures lacked historical projection metadata, but this consecutive pair has it.
    return np.asarray(previous["inv_projection"], dtype=np.float64)


def reproject_pixel_positions(motion, current, previous):
    h, w, _ = motion.shape
    y, x = np.mgrid[:h, :w]
    scale = np.asarray(current["amd_motion_scale"])
    # SDK 2.2 jitter metadata is NDC: undo (+2/width, -2/height) to obtain pixels.
    # NGX motion excludes jitter; the previous texture grid differs by previous-current jitter.
    current_jitter = np.asarray(current["amd_jitter"]) * np.array((w / 2, -h / 2))
    previous_jitter = np.asarray(previous["amd_jitter"]) * np.array((w / 2, -h / 2))
    jitter_delta = previous_jitter - current_jitter
    return (x + motion[..., 0] * scale[0] * w + jitter_delta[0],
            y + motion[..., 1] * scale[1] * h + jitter_delta[1])


def sample_bilinear_scalar(texture, x, y):
    """Sample hardware depth before linearizing; no extrapolation outside texel centers."""
    h, w = texture.shape
    inside = np.isfinite(x) & np.isfinite(y) & (x >= 0) & (x <= w - 1) & (y >= 0) & (y <= h - 1)
    safe_x = np.clip(np.where(np.isfinite(x), x, 0), 0, w - 1)
    safe_y = np.clip(np.where(np.isfinite(y), y, 0), 0, h - 1)
    x0, y0 = np.floor(safe_x).astype(np.int64), np.floor(safe_y).astype(np.int64)
    x1, y1 = np.minimum(x0 + 1, w - 1), np.minimum(y0 + 1, h - 1)
    tx, ty = safe_x - x0, safe_y - y0
    sample = ((texture[y0, x0] * (1 - tx) + texture[y0, x1] * tx) * (1 - ty)
              + (texture[y1, x0] * (1 - tx) + texture[y1, x1] * tx) * ty)
    return sample, inside


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
    if not meta["hw_depth"] or (meta["conversion_flags"] & ~(32 | 64)) != 5:
        raise ValueError(f"{directory}: analyzer currently supports the captured Cyberpunk hardware-depth, linear-albedo, packed-roughness convention only")
    raw = tex["input_color"][..., :3].astype(np.float64)
    diff = tex["input_diffuse_albedo"][..., :3].astype(np.float64)
    spec = tex["input_specular_albedo"][..., :3].astype(np.float64)
    depth = tex["converted_depth"][..., 0]
    valid = np.abs(depth - meta["far"]) > .01
    if meta.get("pipeline") == "pure_fused":
        normals = tex["input_normal_roughness"][..., :3]
        valid &= np.isfinite(normals).all(axis=-1) & ((normals * normals).sum(axis=-1) > 1e-12)
        valid &= np.isfinite(tex["input_depth"][..., 0])
    else:
        valid &= (diff + spec).sum(axis=-1) > .01
    valid &= np.isfinite(raw).all(axis=-1) & np.isfinite(diff + spec).all(axis=-1)
    result = {"directory": str(directory), "frame": meta["frame"], "reset": meta["reset"],
              "pipeline": meta.get("pipeline", "legacy_fused"),
              "motion_depth_encoding": meta.get("motion_depth_encoding", "camera_only"),
              "floor_isolation": meta["floor_isolation"], "correlation_bias": meta["correlation_bias"],
              "valid_pixel_fraction": float(valid.mean()), "inventory": meta["textures"],
              "channel_stats": {name: [stats(value[..., c]) for c in range(4)] for name, value in tex.items()}}
    for name, original in (("diffuse", diff), ("specular", spec)):
        converted = tex[f"converted_{name}_albedo"][..., :3]
        difference = np.abs(converted - original)
        result[f"{name}_albedo_abs_error"] = stats(difference[valid])
        result[f"{name}_albedo_pixels_changed_over_1pct"] = fraction(np.any(difference[valid] > .01, axis=-1))
        result[f"{name}_albedo_outside_0_1_fraction"] = fraction(np.any((original[valid] < 0) | (original[valid] > 1), axis=-1))
    result["albedo_sum_over_1_fraction"] = fraction(np.any((diff + spec)[valid] > 1, axis=-1))
    spec_changed = valid & np.any(np.abs(spec - tex["converted_specular_albedo"][..., :3]) > .01, axis=-1)
    result["specular_changed_pixels_both_albedos_white_fraction"] = fraction(
        (np.all(diff == 1, axis=-1) & np.all(spec == 1, axis=-1))[spec_changed])
    result["specular_changed_pixels_depth"] = stats(depth[spec_changed])
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
    result["hit_distance_above_fp16_fraction"] = fraction(hit[finite_hit] > 65504)
    result.update(shader_clamp_diagnostics(meta, raw, hit[finite_hit]))

    lum = raw @ np.array((.2126, .7152, .0722))
    result["input_negative_color_channel_fraction"] = fraction(raw < 0)
    if "input_bias" in tex:
        result["bias_nonzero_pixel_fraction"] = fraction(tex["input_bias"][..., 0] != 0)
    if "input_before_particles" in tex:
        before = tex["input_before_particles"][..., :3].astype(np.float64)
        result["before_particles_abs_difference"] = stats(np.abs(raw - before))
        result["before_particles_different_pixel_fraction"] = fraction(np.any(raw != before, axis=-1))
        result["before_particles_negative_rgb_residual_fraction"] = fraction(raw - before < 0)
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
    result["composition_pixels_over_10pct_luma_change"] = fraction(change[lit] > .1)

    view, world, previous = geometric_positions(tex["input_depth"][..., 0], meta)
    expected_z = np.abs(previous[..., 2]) - np.abs(view[..., 2])
    result["depth_reconstruction_abs_error"] = stats(np.abs(np.abs(view[..., 2]) - depth)[valid])
    result["camera_depth_delta_abs_error"] = stats(np.abs(expected_z - tex["converted_motion"][..., 2])[valid])
    for channel in (2, 3):
        result[f"motion_{channel}_versus_camera_depth_delta_error"] = stats(np.abs(motion[..., channel] - expected_z)[valid])
    result["motion_w_binary_fraction"] = fraction((motion[..., 3] == 0) | (motion[..., 3] == 1))
    if meta["conversion_flags"] & 64:
        result["reset_motion_depth_abs_error"] = stats(tex["converted_motion"][..., 2][valid])
    elif meta["conversion_flags"] & 32:
        # Mirror the verified producer conversion, including its guarded fallback. This checks
        # conversion arithmetic, not independent temporal image quality or correspondence.
        a, b, w = map(np.float32, meta["previous_depth_projection"])
        hw = tex["input_depth"][..., 0]
        z, writer = tex["input_motion"][..., 2], tex["input_motion"][..., 3]
        previous_hw = hw + z / np.float32(1000)
        denominator = w * previous_hw - a
        with np.errstate(divide="ignore", invalid="ignore", over="ignore"):
            previous_z = b / denominator
        decoded = (np.isfinite(z) & np.isfinite(previous_z) & (previous_z * w > 0) &
                   (previous_hw >= 0) & (previous_hw <= 1) & (hw >= 0) & (hw <= 1) &
                   ((writer == 1) | ((writer == 0) & (w * previous[..., 2] > .05))))
        engine_delta = np.abs(previous_z) - depth
        stored = np.clip(engine_delta, -65504, 65504).astype(np.float16).astype(np.float32)
        result["decoded_engine_motion_fraction"] = fraction(decoded[valid])
        result["decoded_motion_storage_abs_error"] = stats(
            np.abs(stored - tex["converted_motion"][..., 2])[valid & decoded])
    # The installed Cyberpunk producers now authenticate this representation; see FSR_RR_MOTION.md.
    # Comparing it with camera motion remains a diagnostic, not an object-motion ground truth.
    clip_previous = previous @ np.asarray(meta.get("previous_projection", np.linalg.inv(meta["inv_projection"])))
    hw_previous = clip_previous[..., 2] / clip_previous[..., 3]
    predicted_z = 1000 * (hw_previous - tex["input_depth"][..., 0])
    nearby = valid & (depth > .1) & (depth < 100)
    for w_value in (0, 1):
        mask = nearby & (motion[..., 3] == w_value)
        result[f"motion_z_1000x_hardware_camera_delta_abs_error_w{w_value}"] = stats(
            np.abs(motion[..., 2] - predicted_z)[mask])

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


def analyze_pair(directory, current, textures, previous_directory, previous, previous_textures):
    """Temporal depth comparison with independent camera and actual-converter baselines."""
    motion = textures["input_motion"].astype(np.float64)
    px, py = reproject_pixel_positions(motion, current, previous)
    previous_hw, inside = sample_bilinear_scalar(previous_textures["input_depth"][..., 0], px, py)
    previous_inverse = historical_inverse_projection(current, previous)
    with np.errstate(divide="ignore", invalid="ignore"):
        old_depth = np.abs(hardware_depth_to_view_z(previous_hw, previous_inverse))
    current_depth = textures["converted_depth"][..., 0].astype(np.float64)
    reconstructed_delta = old_depth - current_depth
    view, _, previous_view = geometric_positions(textures["input_depth"][..., 0], current)
    camera_delta = np.abs(previous_view[..., 2]) - np.abs(view[..., 2])
    converted_delta = textures["converted_motion"][..., 2].astype(np.float64)
    surface = (inside & np.isfinite(reconstructed_delta) & np.isfinite(camera_delta)
               & np.isfinite(converted_delta) & (current_depth > .1) & (current_depth < 100)
               & (old_depth > .1) & (old_depth < 100))
    # Preserve the historical report's modest-depth-change subset and label that restriction.
    # It is not an independent object correspondence or a production motion validity test.
    surface &= np.abs(reconstructed_delta) < .1 * np.maximum(current_depth, 1)
    pair = {
        "previous": str(previous_directory), "current": str(directory),
        "screen_motion_reprojected_depth_delta_minus_camera": stats((reconstructed_delta - camera_delta)[surface]),
        "screen_motion_reprojected_depth_delta_minus_converted": stats((reconstructed_delta - converted_delta)[surface]),
        "converted_depth_delta_minus_camera": stats((converted_delta - camera_delta)[surface]),
        "channel_z_minus_reprojected_depth_delta": stats((motion[..., 2] - reconstructed_delta)[surface]),
        "channel_w_minus_reprojected_depth_delta": stats((motion[..., 3] - reconstructed_delta)[surface]),
        "limitations": "Bilinear hardware-depth reprojection with pixel jitter correction and historical projection. "
                       "No independent object correspondence; disocclusions remain. Restricted to depth changes below "
                       "10% of max(currentDepth,1); capture stalls affect frame spacing. Not ground truth.",
    }
    encoded_previous_hw = textures["input_depth"][..., 0] + motion[..., 2] / 1000
    with np.errstate(divide="ignore", invalid="ignore"):
        encoded_previous_depth = np.abs(hardware_depth_to_view_z(encoded_previous_hw, previous_inverse))
    encoded_valid = (np.isfinite(encoded_previous_depth) & (encoded_previous_hw >= 0) & (encoded_previous_hw <= 1))
    for group in (0, 1):
        mask = surface & (motion[..., 3] == group)
        pair[f"camera_previous_depth_error_w{group}"] = stats(np.abs(current_depth + camera_delta - old_depth)[mask])
        pair[f"converted_previous_depth_error_w{group}"] = stats(np.abs(current_depth + converted_delta - old_depth)[mask])
        pair[f"encoded_engine_z_previous_depth_error_w{group}"] = stats(
            np.abs(encoded_previous_depth - old_depth)[mask & encoded_valid])
    return pair


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_root", type=Path)
    args = parser.parse_args()
    reports = []
    manifests = sorted(args.capture_root.glob("*/manifest.json"))
    for manifest in manifests:
        reports.append(analyze(manifest.parent))
    pairs = []
    loaded = [(path.parent, *load_capture(path.parent)) for path in manifests]
    for directory, current, textures in loaded:
        for previous_directory, previous, previous_textures in loaded:
            if (current["feature"] != previous["feature"] or current["frame"] != previous["frame"] + 1
                    or current["render_size"] != previous["render_size"] or current["reset"]
                    or capture_identity(directory) != capture_identity(previous_directory)):
                continue
            pairs.append(analyze_pair(directory, current, textures, previous_directory, previous, previous_textures))
    print(json.dumps({"captures": reports, "consecutive_pairs": pairs}, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()

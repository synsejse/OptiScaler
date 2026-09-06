"""Compare offline RR results against one captured frame, using a shared exposure.

Requires NumPy and Pillow. Outputs are diagnostics, not denoising quality scores.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests"))
from analyze_fsrd_textures import load_capture, stats


def compare(capture, results, destination):
    meta, tex = load_capture(capture)
    width, height = meta["render_size"]
    raw = tex["input_color"][..., :3].astype(np.float64)
    albedo = tex["converted_fused_albedo"][..., :3].astype(np.float64)
    residual = tex["preserved_lighting"][..., :3].astype(np.float64)
    source_hash = hashlib.sha256((capture / "manifest.json").read_bytes()).hexdigest()
    images = {"game-input": raw, "game-composed": tex["composed_color"][..., :3]}
    radiances = {}
    parameters = {}
    for result in results:
        info = json.loads((result / "result.json").read_text())
        if not info.get("completed") or info["source_manifest_sha256"] != source_hash:
            raise ValueError("Incomplete result or mismatched source capture")
        path = result / info["output"]
        if path.stat().st_size != width * height * 8:
            raise ValueError("Incomplete readback")
        signal = np.fromfile(path, "<f2").reshape(height, width, 4).astype(np.float64)
        if not np.isfinite(signal).all():
            raise ValueError("Nonfinite denoiser output")
        name = result.name
        if name in images:
            raise ValueError("Duplicate result name")
        radiances[name] = signal
        parameters[name] = info
        # Fixed original albedo/residual for both encodings: isolate denoiser differences.
        # Sqrt-guide quantization error is separately recorded by preparation.
        images[name] = (signal[..., :3] * albedo + residual).astype(np.float16).astype(np.float32)

    luma = np.array([.2126, .7152, .0722])
    factor = .5 / max(float(np.quantile(raw @ luma, .95)), 1e-8)

    def tone(value):
        value = np.maximum(value, 0) * factor
        value = value / (1 + value)
        value = np.where(value <= .0031308, value * 12.92, 1.055 * value ** (1 / 2.4) - .055)
        return np.uint8(np.clip(value, 0, 1) * 255 + .5)

    # Same predeclared panel rectangle as the earlier normal/identity/reset analysis.
    y, x = np.mgrid[:height, :width]
    panel = (x >= 330) & (x < 650) & (y >= 190) & (y < 420)
    panel &= (np.abs(tex["converted_depth"][..., 0] - meta["far"]) > .01)
    panel &= np.all(albedo > .002, axis=-1)
    pairs = panel[:, 1:] & panel[:, :-1]
    albedo_gradient = np.diff(np.log(np.maximum(albedo, 1e-6)), axis=1)[pairs].ravel()
    report = {"source_manifest_sha256": source_hash, "exposure": factor,
              "note": "Single-frame ROI structural diagnostics; noisy input is not ground truth.",
              "panel_pixels": int(panel.sum()), "panel_pairs": int(pairs.sum()),
              "parameters": parameters, "images": {}, "radiance_difference_from_game": {}}
    for name, image in images.items():
        gradient = np.diff(np.log(np.maximum(image, 1e-6)), axis=1)[pairs].ravel()
        report["images"][name] = {
            "panel_log_rgb_gradient_rms": float(np.sqrt(np.mean(gradient * gradient))),
            "panel_gradient_albedo_correlation": float(np.corrcoef(gradient, albedo_gradient)[0, 1]),
            "panel_luminance": stats((image @ luma)[panel]),
        }
    for name, signal in radiances.items():
        report["radiance_difference_from_game"][name] = stats(np.abs(signal - tex["denoised_radiance"]))
    destination.mkdir(parents=True, exist_ok=False)
    sheet = Image.new("RGB", (640 * len(images), 614), "#222222")
    draw = ImageDraw.Draw(sheet)
    for i, (name, pixels) in enumerate(images.items()):
        image = Image.fromarray(tone(pixels))
        image.save(destination / (name + ".png"))
        image.thumbnail((640, 360))
        draw.text((i * 640 + 4, 4), name, fill="white")
        sheet.paste(image, (i * 640, 24))
        crop = Image.fromarray(tone(pixels[190:420, 330:650]))
        sheet.paste(crop, (i * 640, 384))
    sheet.save(destination / "comparison.png")
    (destination / "comparison.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    print(json.dumps({"images": report["images"], "radiance_error": report["radiance_difference_from_game"]}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("destination", type=Path, help="Must not exist")
    parser.add_argument("results", type=Path, nargs="+")
    args = parser.parse_args()
    compare(args.capture, args.results, args.destination)

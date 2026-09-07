"""Readback integration guards; Windows CI/live capture exercise the actual GPU API."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "OptiScaler/upscalers/ffx/FSRDFogLayerCapture.cpp").read_text()


def body(name):
    start = SOURCE.index("{", SOURCE.index(name + "("))
    depth, end = 1, start + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


class EarlyGuideCapture(unittest.TestCase):
    def test_optional_companions_are_distinct_native_and_bounded(self):
        record = body("Record")
        for guard in ("guideCount && guideCount != 3",
                      "texture.resource.Get() == entry.source.resource.Get()",
                      "texture.resource.Get() == layers.boundCb12.resource.Get()",
                      "texture.resource.Get() == layers.earlyGuides[previous].resource.Get()",
                      "texture.subresource != 0", "texture.viewFormat != format", "desc.Format != format",
                      "desc.MipLevels != 1", "desc.DepthOrArraySize != 1",
                      "desc.Width != scene.Width", "desc.Height != scene.Height",
                      "fog capture including early guides exceeds the 256 MiB readback limit"):
            self.assertLess(record.index(guard), record.index("FSRDSubmission::Retain("))
        self.assertIn("i < 2 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT", record)
        self.assertIn("totalBytes += guides[i].bytes", record)
        self.assertIn("PrepareEntry(device, guides[i], false, i < 2)", record)
        self.assertLess(record.index("batch->earlyGuides = std::move(guides)"),
                        record.index("FSRDSubmission::Retain("))

    def test_albedo_format_is_not_admitted_into_scene_layer_path(self):
        prepare = body("PrepareEntry")
        albedo = prepare.split("else if (guideAlbedo)", 1)[1].split("else if (", 1)[0]
        self.assertIn("desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM", albedo)
        self.assertIn('entry.componentType = "unorm8"', albedo)
        self.assertIn("entry.pixelBytes = 4", albedo)
        self.assertIn('".rgba8unorm"', albedo)
        self.assertIn("PrepareEntry(device, entry);", body("Record"))
        self.assertIn('companion["schema"] = "optiscaler.fsr_rr.early_guide.v1"', SOURCE)
        self.assertIn('companion["value_transform"] = "none; native authored guide output bytes"', SOURCE)

    def test_fence_precedes_all_copies_and_completion_follows_all_files(self):
        record = body("Record")
        self.assertLess(record.index("FSRDSubmission::Retain("), record.index("if (batch->earlyGuides)\n            for (const auto& entry"))
        writer = body("WriteWhenComplete")
        self.assertLess(writer.index("FSRDSubmission::Complete("), writer.index("if (batch.earlyGuides)"))
        self.assertLess(writer.index("if (batch.earlyGuides)"), writer.index('metadata["complete"] = true'))
        for forbidden in ("SetPipelineState", "Dispatch(", "CreateShaderResourceView", "CreateUnorderedAccessView"):
            self.assertNotIn(forbidden, SOURCE)


if __name__ == "__main__":
    unittest.main()

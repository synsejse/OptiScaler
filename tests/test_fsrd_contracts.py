"""Source-level integration guards, supplementing (not replacing) live GPU validation."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "OptiScaler/shaders/fsrd_preprocess"


class RRContracts(unittest.TestCase):
    def test_both_modes_allocate_floor_scratch(self):
        source = (SHADERS / "FSRDPreprocessor_Dx12.cpp").read_text()
        allocation = source.split("void SetMaxRenderSize(", 1)[1].split("void DispatchPyramidSeed", 1)[0]
        common = allocation.split("if (m_isMode2)", 1)[0]
        for name in ("m_outputBuffer1", "m_outputBuffer2"):
            self.assertIn(f"{name} = CreateTex", common)

    def test_hit_distance_is_shared_by_both_modes(self):
        source = (SHADERS / "precompile/FSRDInputConv.hlsl").read_text()
        kernel = source.split("void CSMain", 1)[1]
        self.assertLess(kernel.index("const half hitDist"), kernel.index("if (IsSet(FLAGS_MODE_2_SIGNAL))"))
        self.assertIn("half4(demodColor, hitDist)", kernel)
        self.assertIn("half4(demodSpecular, hitDist)", kernel)
        self.assertNotIn("roughness < 0.2", kernel)

    def test_fused_composition_receives_raw_color(self):
        source = (SHADERS / "FSRDPreprocessor_Dx12.cpp").read_text()
        composition = source.split("void DispatchComposition", 1)[1].split("void Blit", 1)[0]
        self.assertEqual(composition.count(".InRawColor = desc.InRawColor"), 2)

    def test_all_denoiser_outputs_declare_uav(self):
        source = (SHADERS / "FSRDPreprocessor_Dx12.cpp").read_text()
        outputs = re.findall(r"\.output\s*=\s*ffxApiGetResourceDX12\([^,]+,\s*(\w+)\)", source)
        self.assertEqual(len(outputs), 3)
        self.assertTrue(all("FFX_API_RESOURCE_STATE_UNORDERED_ACCESS" in value for value in outputs))

    def test_partial_groups_reach_composition_barrier(self):
        source = (SHADERS / "precompile/FSRDOutputComp.hlsl").read_text()
        kernel = source.split("void CSMain", 1)[1]
        # The only pre-barrier return is inside the uniform raw-blit branch, which has no group barrier.
        self.assertNotIn("return;", kernel.split("if (IsSet(FLAGS_RAW_SOURCE_BLIT))", 1)[0])
        composition = kernel.split("const int2 smID = gtID.xy", 1)[1]
        self.assertLess(composition.index("PopulateSharedMemory"), composition.index("return;"))

    def test_packed_roughness_does_not_require_separate_resource(self):
        source = (ROOT / "OptiScaler/upscalers/ffx/FSRDFeature_Dx12.cpp").read_text()
        compact = re.sub(r"\s+", " ", source)
        self.assertIn("if (!_isRoughnessPacked && !TryGetLoggedResource", compact)
        self.assertNotIn("Defaulting to packed roughness", source)
        self.assertIn("_convDesc.Resources = {}", source)

    def test_sample_jitter_convention_is_preserved(self):
        source = (ROOT / "OptiScaler/upscalers/ffx/FSRDFeature_Dx12.cpp").read_text()
        self.assertIn("2.0f * (jitterX / (float) RenderWidth())", source)
        self.assertIn("-2.0f * (jitterY / (float) RenderHeight())", source)

    def test_recreated_feature_preserves_rr_layout(self):
        provider = (ROOT / "OptiScaler/upscalers/FeatureProvider_Dx12.cpp").read_text()
        self.assertIn("CopyRRCreateParameters(contextData->createParams)", provider)
        source = (ROOT / "OptiScaler/upscalers/ffx/FSRDFeature_Dx12.cpp").read_text()
        copy = source.split("void FSRDFeatureDx12::CopyRRCreateParameters", 1)[1].split("\n}", 1)[0]
        self.assertIn("NVSDK_NGX_Parameter_Use_HW_Depth", copy)
        self.assertIn("NVSDK_NGX_Parameter_DLSS_Roughness_Mode", copy)
        self.assertNotIn("static bool s_is", source)

    def test_albedo_storage_matches_demodulation(self):
        source = (SHADERS / "FSRDPreprocessor_Dx12.cpp").read_text()
        for name in ("FusedAlbedo", "SpecAlbedo", "DiffAlbedo"):
            self.assertIn(f"{name} = DXGI_FORMAT_R10G10B10A2_UNORM", source)
        shader = (SHADERS / "precompile/FSRDInputConv.hlsl").read_text()
        for name in ("specReflectance", "diffAlbedo"):
            quantize = f"round(saturate({name}.rgb) * 1023.0f) / 1023.0f"
            self.assertIn(quantize, shader)
            self.assertLess(shader.index(quantize), shader.index("half3 demodSpecular"))
        self.assertGreater(round(1e-3 * 1023) / 1023, 0)

    def test_msbuild_generates_all_four_kernels(self):
        tree = ET.parse(ROOT / "OptiScaler/OptiScaler.vcxproj")
        ns = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
        kernels = tree.findall(".//m:FSRDShader", ns)
        self.assertEqual(len(kernels), 4)
        for kernel in kernels:
            self.assertTrue((ROOT / "OptiScaler" / kernel.attrib["Include"].replace("\\", "/")).is_file())
        target = tree.find(".//m:Target[@Name='CompileFSRDShaders']", ns)
        self.assertEqual(target.attrib["BeforeTargets"], "ClCompile")
        self.assertIn("FSRDPreprocessCommon.hlsli", target.attrib["Inputs"])


if __name__ == "__main__":
    unittest.main()

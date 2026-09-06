"""Source-level integration guards, supplementing (not replacing) live GPU validation."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "OptiScaler/shaders/fsrd_preprocess"


class RRContracts(unittest.TestCase):
    def test_research_capture_waits_for_actual_submission(self):
        source = (ROOT / "OptiScaler/upscalers/ffx/FSRDResearchCapture.cpp").read_text()
        submitted = source.split("void Submitted(", 1)[1].split("void Poll()", 1)[0]
        self.assertIn("batch->submittedList", submitted)
        self.assertIn("queue->Signal", submitted)
        poll = source.split("void Poll()", 1)[1]
        self.assertLess(poll.index("GetCompletedValue"), poll.index("->Map("))
        self.assertIn("completed == UINT64_MAX", poll)
        self.assertNotIn("WaitForSingleObject", source)
        self.assertNotIn("Sleep(", source)
        hook = (ROOT / "OptiScaler/resource_tracking/ResTrack_dx12.cpp").read_text()
        for section in hook.split("FSRDResearch::Submitted(")[:-1]:
            self.assertRegex(section, r"o_ExecuteCommandLists\(This, NumCommandLists, ppCommandLists\);\s*$")

    def test_research_capture_is_bounded_and_preserves_channels(self):
        source = (ROOT / "OptiScaler/upscalers/ffx/FSRDResearchCapture.cpp").read_text()
        self.assertIn("registry.pending.size() >= 2", source)
        self.assertIn("registry.count >= 12", source)
        self.assertIn("batch->bytes + bytes > MaxBytes", source)
        shader = (SHADERS / "precompile/FSRDResearchCopy.hlsl").read_text()
        self.assertIn("RWStructuredBuffer<float4>", shader)
        self.assertIn("Input.Load(int3(id.xy, 0))", shader)
        self.assertNotIn("half", shader)
        self.assertNotIn("saturate", shader)

    def test_fused_path_still_allocates_both_floor_scratch_buffers(self):
        source = (SHADERS / "FSRDPreprocessor_Dx12.cpp").read_text()
        allocation = source.split("void SetMaxRenderSize(", 1)[1].split("void DispatchPyramidSeed", 1)[0]
        for name in ("m_outputBuffer1", "m_outputBuffer2"):
            self.assertIn(f"{name} = CreateTex", allocation)

    def test_fused_radiance_retains_hit_distance_and_material_guides(self):
        source = (SHADERS / "precompile/FSRDInputConv.hlsl").read_text()
        kernel = source.split("void CSMain", 1)[1]
        self.assertIn("OutRadiance[px] = half4(demodColor, hitDist)", kernel)
        self.assertIn("max(specReflectance.rgb, diffAlbedo.rgb)", kernel)
        self.assertIn("denosierColor / fusedAlbedo.rgb", kernel)
        for output in ("OutSpecAlbedo", "OutDiffAlbedo", "OutNormals", "OutMotion", "OutLinearDepth"):
            self.assertIn(f"{output}[px] =", kernel)
        self.assertNotIn("roughness < 0.2", kernel)

    def test_fused_composition_receives_raw_color(self):
        source = (SHADERS / "FSRDPreprocessor_Dx12.cpp").read_text()
        composition = source.split("void DispatchComposition", 1)[1].split("void Blit", 1)[0]
        self.assertEqual(composition.count(".InRawColor = desc.InRawColor"), 1)

    def test_all_denoiser_outputs_declare_uav(self):
        source = (SHADERS / "FSRDPreprocessor_Dx12.cpp").read_text()
        outputs = re.findall(r"\.output\s*=\s*ffxApiGetResourceDX12\([^,]+,\s*(\w+)\)", source)
        self.assertEqual(len(outputs), 1)
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
            self.assertLess(shader.index(quantize), shader.index("half3 demodColor"))
        self.assertGreater(round(1e-3 * 1023) / 1023, 0)

    def test_msbuild_generates_all_five_kernels(self):
        tree = ET.parse(ROOT / "OptiScaler/OptiScaler.vcxproj")
        ns = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
        kernels = tree.findall(".//m:FSRDShader", ns)
        self.assertEqual(len(kernels), 5)
        for kernel in kernels:
            self.assertTrue((ROOT / "OptiScaler" / kernel.attrib["Include"].replace("\\", "/")).is_file())
        target = tree.find(".//m:Target[@Name='CompileFSRDShaders']", ns)
        self.assertEqual(target.attrib["BeforeTargets"], "ClCompile")
        self.assertIn("FSRDPreprocessCommon.hlsli", target.attrib["Inputs"])

    def test_skipped_pixels_do_not_reuse_composed_color_as_motion(self):
        source = (SHADERS / "precompile/FSRDInputConv.hlsl").read_text()
        skipped = source.split("else // Skip", 1)[1]
        self.assertIn("OutMotion[px] = half4(InMotionVectors[px].rg, 0.0f, 0.0f)", skipped)

    def test_guessed_split_is_removed_not_just_hidden(self):
        feature = ROOT / "OptiScaler/upscalers/ffx/FSRDFeature_Dx12.cpp"
        paths = [*SHADERS.glob("*.h"), *SHADERS.glob("*.cpp"), *SHADERS.glob("precompile/*.hlsl"), feature]
        for path in paths:
            source = path.read_text()
            for symbol in ("Mode2Signal", "Mode2Inputs", "isMode2", "FLAGS_MODE_2_SIGNAL",
                           "FFX_DENOISER_MODE_2_SIGNALS", "ffxDispatchDescDenoiserInput2Signals",
                           "demodSpecular", "demodDiffuse", "specWeight", "InDenoisedSignal2"):
                self.assertNotIn(symbol, source, str(path))
        self.assertIn(".mode = FFX_DENOISER_MODE_1_SIGNAL", feature.read_text())

    def test_old_configuration_cannot_reactivate_split(self):
        config = (ROOT / "OptiScaler/Config.cpp").read_text()
        self.assertIn('ini.Delete("FSR-RR", "DenoiserMode")', config)
        self.assertNotIn('readInt("FSR-RR", "DenoiserMode")', config)
        self.assertNotRegex((ROOT / "OptiScaler.ini").read_text(), r"(?m)^DenoiserMode=")
        for name in ("Config.h", "State.h", "menu/menu_common.h", "menu/menu_common.cpp"):
            source = (ROOT / "OptiScaler" / name).read_text()
            self.assertNotIn("FfxDenoiserMode", source)
            self.assertNotIn("ffxDenoiserMode", source)

    def test_fused_composition_descriptor_layout_matches_shader(self):
        source = (SHADERS / "FSRDShaderData.h").read_text().split("namespace Composition", 1)[1]
        names = re.findall(r"ID3D12Resource\* (\w+);", source)
        shader = (SHADERS / "precompile/FSRDOutputComp.hlsl").read_text()
        textures = re.findall(r"Texture2D<[^>]+> (\w+) : register\(t(\d+)\)", shader)
        self.assertEqual(names, [name for name, _ in textures])
        self.assertEqual([int(slot) for _, slot in textures], list(range(5)))
        self.assertIn("SRV(t0, numDescriptors = 5)", shader)

    def test_conversion_resources_have_regular_ownership_and_explicit_uav_order(self):
        source = (SHADERS / "FSRDShaderData.h").read_text().split("namespace Conversion", 1)[1]
        output = source.split("struct Output", 1)[1].split("namespace Composition", 1)[0]
        self.assertNotIn("union", output.split("// Explicit UAV order", 1)[0])
        self.assertNotIn("~ComPtr", output)
        resources = re.findall(r"Resources\.(\w+)\.Get\(\)", output)
        self.assertEqual(resources, ["Radiance", "FusedAlbedo", "Motion", "Normals", "SpecAlbedo", "DiffAlbedo",
                                     "LinearDepth", "SkipSignal"])
        shader = (SHADERS / "precompile/FSRDInputConv.hlsl").read_text()
        textures = re.findall(r"RWTexture2D<[^>]+> Out(\w+) : register\(u(\d+)\)", shader)
        self.assertEqual(resources, [name for name, _ in textures])
        self.assertEqual([int(slot) for _, slot in textures], list(range(8)))


if __name__ == "__main__":
    unittest.main()

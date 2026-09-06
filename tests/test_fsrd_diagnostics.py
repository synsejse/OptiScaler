"""Source integration guards; policy behavior is also compiled/executed in Windows CI."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
FEATURE = (ROOT / "OptiScaler/upscalers/ffx/FSRDFeature_Dx12.cpp").read_text()
HEADER = (ROOT / "OptiScaler/upscalers/ffx/FSRDFeature_Dx12.h").read_text()
DIAGNOSTICS = (ROOT / "OptiScaler/upscalers/ffx/FSRDDiagnostics.h").read_text()
BASE = (ROOT / "OptiScaler/upscalers/IFeature.h").read_text()
COMPOSITOR = (ROOT / "OptiScaler/shaders/fsrd_preprocess/FSRDPreprocessor_Dx12.cpp").read_text()
MENU = (ROOT / "OptiScaler/menu/menu_common.cpp").read_text(encoding="utf-8-sig")


class Diagnostics(unittest.TestCase):
    def test_identity_reuses_the_normal_compositor_without_a_copy(self):
        self.assertIn("DispatchComposition(InCommandList, compDesc, diagnosticPlan.identity)", FEATURE)
        self.assertIn("identityDenoiser ? outResources.Radiance.Get() : m_outputBuffer1.Get()", COMPOSITOR)
        identity = FEATURE.split("if (diagnosticPlan.identity)", 1)[1].split("else", 1)[0]
        self.assertNotIn("DispatchDenoiser(", identity)
        self.assertNotIn("CopyTexture", identity)
        self.assertNotIn("CopyResource", identity)
        self.assertIn('"denoised_radiance", GetD3D12ResFromFFX(fusedSignal.radiance.input)', identity)

    def test_reset_requests_same_frame_capture_on_render_thread(self):
        evaluate = FEATURE.split("bool FSRDFeatureDx12::EvaluateInternal(", 1)[1]
        self.assertLess(evaluate.index("if (!PrepareDenoiserInput("), evaluate.index("const bool manualDenoiserReset"))
        self.assertLess(evaluate.index("FSRDResearch::Request(Handle()->Id)"), evaluate.index("FSRDResearch::WantsCapture"))
        request = evaluate.split("const bool manualDenoiserReset", 1)[1].split(";", 1)[0]
        self.assertIn("dbgMode == DebugModes::None && !identityRequested", request)
        self.assertIn("_diagnostics.resetDenoiserHistory.load() && FSRDResearch::Request(Handle()->Id)", request)
        self.assertIn("if (diagnosticPlan.resetDenoiser)\n        denoiserDesc.flags |= FFX_DENOISER_DISPATCH_RESET", evaluate)
        self.assertIn("if (isDenoiserReady && manualDenoiserReset)", evaluate)
        self.assertIn("_diagnostics.lastResetFrame.store(_frameCount)", evaluate)

    def test_separate_camera_and_denoiser_history(self):
        self.assertIn("params.reset |= _isInReset || _diagnosticUpscaleReset", FEATURE)
        self.assertEqual(FEATURE.count("_hasDenoiserHistory = diagnosticPlan.runDenoiser;"), 2)
        self.assertEqual(FEATURE.count("_identityWasActive = diagnosticPlan.identity;"), 1)
        output_debug = FEATURE.split("else // Debug visualization", 1)[1].split("bool FSRDFeatureDx12::Prepare", 1)[0]
        self.assertNotIn("_identityWasActive =", output_debug)
        self.assertNotIn("_isInReset |= manualDenoiserReset", FEATURE)
        self.assertNotIn("_hasCameraHistory = diagnosticPlan.runDenoiser", FEATURE)
        self.assertIn("const bool denoiserHadHistory = _hasDenoiserHistory;\n    _hasDenoiserHistory = false;", FEATURE)

    def test_manifest_distinguishes_identity_and_reset_from_real_amd_output(self):
        for field in ("identity_denoiser", "denoiser_executed", "denoiser_output_kind", "diagnostic_manual_reset",
                      "denoiser_reset", "denoiser_history_valid_on_entry", "bridge_upscaler_reset"):
            self.assertIn(f'{{"{field}"', FEATURE)
        self.assertIn('"identity_converted_input"', FEATURE)
        self.assertIn('"amd_filtered"', FEATURE)

    def test_controls_are_session_only_thread_safe_requests(self):
        for field in ("identityDenoiser", "resetDenoiserHistory"):
            self.assertIn(f"std::atomic<bool> {field} {{ false }}", DIAGNOSTICS)
        menu = MENU.split('ImGui::SeparatorText("Debug")', 1)[1].split("if (!state.ffxDenoiserDebugModes", 1)[0]
        self.assertIn('ImGui::Checkbox("Identity denoiser (diagnostic)"', menu)
        self.assertIn('ImGui::Button("Reset denoiser history + dump")', menu)
        self.assertIn("rr->RequestDenoiserReset();", menu)
        self.assertIn("!normalView || identity || rr->DenoiserResetPending() || dumpStatus.busy", menu)
        self.assertIn("currentFeature->GetFsrRRDiagnostics()", menu)
        self.assertIn("GetFsrRRDiagnostics() { return nullptr; }", BASE)
        self.assertIn("GetFsrRRDiagnostics() override { return &_diagnostics; }", HEADER)
        self.assertNotIn("static_cast<FSRDFeatureDx12*>", menu)
        self.assertNotIn("config->FfxDenoiserDebugMode =", menu)
        self.assertNotIn("_isInReset", menu)


if __name__ == "__main__":
    unittest.main()

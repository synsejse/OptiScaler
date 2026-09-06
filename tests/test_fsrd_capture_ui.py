"""Capture integration guards. Windows CI builds the implementation; GPU checks are separate."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "OptiScaler/upscalers/ffx/FSRDResearchCapture.cpp").read_text()
FEATURE = (ROOT / "OptiScaler/upscalers/ffx/FSRDFeature_Dx12.cpp").read_text()
MENU = (ROOT / "OptiScaler/menu/menu_common.cpp").read_text(encoding="utf-8-sig")


class CaptureUI(unittest.TestCase):
    def test_button_targets_selected_feature_without_modifying_configuration(self):
        menu = MENU.split('ImGui::SeparatorText("Debug")', 1)[1].split("if (!state.ffxDenoiserDebugModes", 1)[0]
        self.assertIn('ImGui::Button("Dump buffers")', menu)
        self.assertIn("FSRDResearch::Request(currentFeature->Handle()->Id)", menu)
        self.assertIn("dumpStatus.busy", menu)
        self.assertIn("CancelRequest()", menu)
        self.assertIn("SetClipboardText(dumpStatus.directory.c_str())", menu)
        self.assertNotIn("set_volatile_value", menu)
        self.assertNotIn("ResearchCapture =", menu)

    def test_gui_request_is_one_frame_and_independent_of_ini_and_legacy_limit(self):
        wants = SOURCE.split("bool WantsCapture(", 1)[1].split("static std::string", 1)[0]
        self.assertIn("||", wants)
        self.assertIn("== feature", wants)
        self.assertIn("FSRDResearch::WantsCapture(Handle()->Id)", FEATURE)
        begin = SOURCE.split("Capture Begin(", 1)[1].split("void Record(", 1)[0]
        gui = begin.split("if (fromGui)", 1)[1].split("else if", 1)[0]
        self.assertIn("registry.burst = 1", gui)
        self.assertIn("registry.burstFeature = feature", gui)
        self.assertIn("requestedFeature.store(NoFeature", gui)
        self.assertNotIn("registry.count", gui)
        self.assertIn("if (!fromGui)\n            ++registry.count", begin)
        self.assertIn("registry.pending.size() >= 2", begin)

    def test_queued_cancel_does_not_discard_gpu_owned_buffers(self):
        cancel = SOURCE.split("void CancelRequest()", 1)[1].split("Status GetStatus()", 1)[0]
        self.assertIn("requestedFeature.store(NoFeature", cancel)
        self.assertIn("registry.burst = 0", cancel)
        self.assertNotIn("pending.clear", cancel)
        request = SOURCE.split("bool Request(", 1)[1].split("void CancelRequest", 1)[0]
        self.assertIn("Busy(registry)", request)
        self.assertIn("std::lock_guard", request)

    def test_unique_utc_directory_never_overwrites_an_existing_dump(self):
        self.assertIn("GetSystemTime(&now)", SOURCE)
        self.assertIn("now.wMilliseconds, ++registry.sequence", SOURCE)
        self.assertIn("if (!std::filesystem::create_directory(batch->directory))", SOURCE)
        self.assertIn("refusing to overwrite", SOURCE)

    def test_fence_poll_does_not_depend_on_capture_setting(self):
        poll = SOURCE.split("void Poll()", 1)[1]
        self.assertNotIn("FfxDenoiserResearchCapture", poll)
        self.assertIn("if (!hasCaptures.load", poll)
        self.assertLess(poll.index("GetCompletedValue"), poll.index("->Map("))
        self.assertIn("FSRDResearch::Poll();", FEATURE)
        panel = MENU.split("// FSR Ray Regeneration", 1)[1]
        self.assertLess(panel.index("FSRDResearch::Poll();"), panel.index("if (currentBackend == Upscaler::FSRD)"))

    def test_manifest_reports_partial_and_is_published_only_after_successful_writes(self):
        poll = SOURCE.split("void Poll()", 1)[1]
        self.assertIn("if (!entry.ready)", poll)
        self.assertLess(poll.index("file.close()"), poll.index("const bool written"))
        self.assertIn('batch->metadata["partial"]', poll)
        self.assertIn('"manifest.json.tmp"', poll)
        self.assertLess(poll.index("if (!manifest)"), poll.index("std::filesystem::rename"))
        self.assertIn("Partial dump:", poll)

    def test_full_resolution_output_grows_scratch_and_restores_state(self):
        self.assertIn("batch->sources.push_back(batch->scratch)", SOURCE)
        output = SOURCE.split("void RecordOutput(", 1)[1].split("void Submitted", 1)[0]
        self.assertIn('Record(batch, "backend_output", texture, true)', output)
        self.assertIn("texture, state, readable", output)
        self.assertIn("texture, readable, state", output)
        self.assertLess(output.index("D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE"), output.index("FSRD::AddBarrier"))
        self.assertIn("cfg.OutputResourceBarrier.value()", FEATURE)
        self.assertIn('"output_stage"', FEATURE)

    def test_debug_mode_and_optional_layers_are_inventoried(self):
        for field in ("debug_mode", "denoiser_bypassed", "upscaler_bypassed", "display_size"):
            self.assertIn(f'{{"{field}"', FEATURE)
        for field in ("input_after_particles", "input_after_fog", "input_after_sss", "input_after_refraction",
                      "input_after_dof", "input_emissive", "input_alpha"):
            self.assertIn(f'{{"{field}"', FEATURE)
        self.assertIn('FSRDResearch::Record(research, "denoised_radiance", nullptr)', FEATURE)
        self.assertIn('FSRDResearch::Record(research, "composed_color", nullptr)', FEATURE)

    def test_failed_evaluations_are_partial_even_when_input_readbacks_succeed(self):
        self.assertIn("bool captureEvaluationSucceeded = false", FEATURE)
        self.assertIn("~CaptureCompletion() { FSRDResearch::Finish(capture, succeeded); }", FEATURE)
        finish = SOURCE.split("void Finish(", 1)[1].split("void Submitted", 1)[0]
        self.assertIn('batch->metadata["evaluation_succeeded"] = evaluationSucceeded', finish)
        self.assertIn("batch->partial |= !evaluationSucceeded", finish)


if __name__ == "__main__":
    unittest.main()

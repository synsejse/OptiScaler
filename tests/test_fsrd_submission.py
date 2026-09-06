"""Integration guards for the GPU-completion-based dispatch lifetime policy."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SubmissionLifetime(unittest.TestCase):
    def test_rr_rejects_incompatible_input_contract_before_recording(self):
        source = (ROOT / "OptiScaler/upscalers/ffx/FSRDFeature_Dx12.cpp").read_text()
        evaluate = source.split("bool FSRDFeatureDx12::EvaluateInternal", 1)[1].split(
            "bool FSRDFeatureDx12::PrepareDenoiserInput", 1)[0]
        self.assertLess(evaluate.index("FSRD::ValidateInputContract"), evaluate.index("UpdateSize(InParameters)"))
        self.assertLess(evaluate.index("UpdateSize(InParameters)"), evaluate.index("FSRD::IsSupportedMotionLayout"))
        self.assertLess(evaluate.index("FSRD::IsSupportedMotionLayout"), evaluate.index("PrepareDenoiserInput(InCommandList"))
        header = (ROOT / "OptiScaler/upscalers/ffx/FSRDInputValidation.h").read_text()
        for field in ("ColorResourceBarrier", "MVResourceBarrier", "DepthResourceBarrier"):
            self.assertIn(f"CheckState(config.{field}", header)
        self.assertIn("unrealAutoBarriers", header)
        self.assertIn("state == D3D12_RESOURCE_STATE_COMMON", header)
        self.assertIn("bits & ~readableStates", header)
        self.assertIn("!jittered && (lowResolution ||", header)
        self.assertGreaterEqual(header.count("static_assert("), 12)

    def test_all_consumed_ngx_subrect_origins_are_rejected(self):
        header = (ROOT / "OptiScaler/upscalers/ffx/FSRDInputValidation.h").read_text()
        for stem in ("DLSS_Input_Color_Subrect_Base", "DLSS_Input_Depth_Subrect_Base",
                     "DLSS_Input_MV_SubrectBase", "DLSS_Input_DiffuseAlbedo_Subrect_Base",
                     "DLSS_Input_SpecularAlbedo_Subrect_Base", "DLSS_Input_Normals_Subrect_Base",
                     "DLSS_Input_Roughness_Subrect_Base", "DLSSD_SpecularHitDistance_Subrect_Base",
                     "DLSS_Input_Translucency_SubrectBase", "DLSS_Input_Bias_Current_Color_SubrectBase",
                     "DLSS_Output_Subrect_Base"):
            for axis in "XY":
                self.assertIn(f"NVSDK_NGX_Parameter_{stem}_{axis}", header)
        self.assertIn("parameters.Get(key, &value) == NVSDK_NGX_Result_Success && value != 0", header)

    def test_sr_copy_restores_original_input_state(self):
        source = (ROOT / "OptiScaler/upscalers/ffx/FFXFeature_Dx12.cpp").read_text()
        self.assertIn("unsigned int reset = 0", source)
        self.assertIn("ID3D12Resource* paramColor = nullptr", source)
        self.assertIn("ID3D12Resource* const originalColor = paramColor", source)
        copied = source.split("InCommandList->CopyTextureRegion", 1)[1].split("paramColor = smallerColor[index]", 1)[0]
        self.assertRegex(copied, r"ResourceBarrier\(InCommandList, paramColor, D3D12_RESOURCE_STATE_COPY_SOURCE,\s*"
                                 r"D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE\)")
        self.assertIn("ResourceBarrier(InCommandList, originalColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE", source)

    def test_rr_version_does_not_overwrite_sr_provider_version(self):
        rr = (ROOT / "OptiScaler/upscalers/ffx/FSRDFeature_Dx12.cpp").read_text()
        self.assertIn("state.ffxDenoiserUpscalerVersion = FFXFeature::Version()", rr)
        self.assertIn("_denoiserVersion.parse_version(", rr)
        self.assertNotRegex(rr, r"(?m)^\s+parse_version\(")
        header = (ROOT / "OptiScaler/upscalers/ffx/FSRDFeature_Dx12.h").read_text()
        self.assertIn("Version() override { return _denoiserVersion; }", header)
        sr = (ROOT / "OptiScaler/upscalers/ffx/FFXFeature_Dx12.cpp").read_text()
        evaluate = sr.split("bool FFXFeatureDx12::EvaluateInternal", 1)[1].split("bool FFXFeatureDx12::InitFFX", 1)[0]
        self.assertEqual(evaluate.count("Version()"), 1)
        self.assertIn("const auto upscalerVersion = FFXFeature::Version()", evaluate)

    def test_optional_sr_resources_are_initialized(self):
        header = (ROOT / "OptiScaler/upscalers/ffx/FFXFeature_Dx12.h").read_text()
        self.assertIn("ID3D12Resource* smallerColor[2] = {}", header)

    def test_slots_are_not_recycled_by_frame_count(self):
        source = (ROOT / "OptiScaler/shaders/fsrd_preprocess/FSRDPreprocessor_Dx12.cpp").read_text()
        compute = source.split("struct ComputeState", 1)[1].split("// Private implementation", 1)[0]
        self.assertNotIn("m_cbCurrentFrameIndex", compute)
        self.assertNotIn("% backBufferCount", compute)
        self.assertIn("FSRDSubmission::Complete(item.ticket)", compute)
        self.assertIn("FSRDSubmission::Retain(m_pDev, list, slot->storage)", compute)
        self.assertLess(compute.index("AcquireStorage(cmdList)"), compute.index("memcpy(storage->mapped"))
        self.assertIn("m_slots.size() >= 64", compute)

    def test_storage_owns_all_gpu_references(self):
        source = (ROOT / "OptiScaler/shaders/fsrd_preprocess/FSRDPreprocessor_Dx12.cpp").read_text()
        for text in ("ComPtr<ID3D12RootSignature> root", "ComPtr<ID3D12PipelineState> pipeline",
                     "ComPtr<ID3D12Resource> constants", "FrameDescriptorHeap heap",
                     "std::vector<ComPtr<ID3D12Resource>> resources"):
            self.assertIn(text, source)
        self.assertEqual(source.count("storage->resources.emplace_back(resource)"), 2)

    def test_fences_follow_actual_submission_on_both_hook_paths(self):
        source = (ROOT / "OptiScaler/resource_tracking/ResTrack_dx12.cpp").read_text()
        self.assertEqual(source.count("FSRDSubmission::Submitted(This, fsrdSubmission)"), 2)
        for section in source.split("FSRDSubmission::Submitted(")[:-1]:
            self.assertRegex(section, r"FSRDSubmission::Preparing\(NumCommandLists, ppCommandLists\);\s*"
                                     r"o_ExecuteCommandLists\(This, NumCommandLists, ppCommandLists\);\s*"
                                     r"FSRDResearch::Submitted\(This, NumCommandLists, ppCommandLists\);\s*$")

    def test_submission_snapshot_cannot_pick_up_rerecorded_work(self):
        source = (ROOT / "OptiScaler/resource_tracking/FSRDSubmission.h").read_text()
        preparing = source.split("inline Submission Preparing", 1)[1].split("inline void Submitted", 1)[0]
        self.assertIn("ticket->submitted = true", preparing)
        self.assertIn("submission.push_back(ticket)", preparing)
        submitted = source.split("inline void Submitted", 1)[1]
        self.assertIn("for (const auto& ticket : submission)", submitted)
        self.assertNotIn("ticket->list.Get()", submitted)

    def test_retired_com_owners_are_released_outside_registry_lock(self):
        source = (ROOT / "OptiScaler/resource_tracking/FSRDSubmission.h").read_text()
        retain = source.split("inline std::shared_ptr<Ticket> Retain", 1)[1].split("using Submission", 1)[0]
        submitted = source.split("inline void Submitted", 1)[1]
        for section in (retain, submitted):
            self.assertLess(section.index("retired;"), section.index("std::lock_guard lock"))
            self.assertIn("CollectCompletedLocked(registry, retired)", section)

    def test_observer_is_bounded_and_never_waits_on_unsubmitted_work(self):
        source = (ROOT / "OptiScaler/resource_tracking/FSRDSubmission.h").read_text()
        self.assertIn("registry.pending.size() >= 64", source)
        self.assertIn("registry.queues.size() >= 16", source)
        self.assertIn("item.queue.Get() == queue", source)
        self.assertIn("queue->Signal(progress->fence.Get(), ++progress->value)", source)
        self.assertIn("ticket->signalFailed = FAILED(result)", source)
        for forbidden in ("WaitForSingleObject", "SetEventOnCompletion", "Sleep(", "sleep_for", "queue->Wait"):
            self.assertNotIn(forbidden, source)


if __name__ == "__main__":
    unittest.main()

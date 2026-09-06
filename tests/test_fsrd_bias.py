"""Source integration guards for the shared SR/RR bias dispatch lifetime fix.

The portable C++ submission-policy test exercises unfinished recordings and queue
completion; these guards verify Bias actually uses that policy and owns its GPU data.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "OptiScaler/shaders/bias/Bias_Dx12.cpp").read_text()
HEADER = (ROOT / "OptiScaler/shaders/bias/Bias_Dx12.h").read_text()


class BiasDispatchLifetime(unittest.TestCase):
    def test_descriptor_reuse_requires_actual_completion(self):
        self.assertIn("FSRDSubmission::Complete(item.ticket)", SOURCE)
        self.assertIn("FSRDSubmission::Retain(_device, commandList, slot->storage)", SOURCE)
        self.assertNotIn("BIAS_NUM_OF_HEAPS", SOURCE + HEADER)
        self.assertNotIn("_frameHeaps", SOURCE + HEADER)
        self.assertNotIn("_counter", SOURCE)

    def test_pool_exhaustion_fails_without_waiting_or_recycling(self):
        self.assertIn("_dispatchSlots.size() >= 64", SOURCE)
        self.assertIn("throw std::runtime_error(\"Bias dispatch storage limit reached", SOURCE)
        self.assertIn("catch (const std::exception& error)", SOURCE)
        for forbidden in ("WaitForSingleObject", "SetEventOnCompletion", "Sleep(", "sleep_for"):
            self.assertNotIn(forbidden, SOURCE)

    def test_storage_owns_every_recorded_gpu_object(self):
        storage = SOURCE.split("struct Bias_Dx12::DispatchStorage", 1)[1].split("};", 1)[0]
        for declaration in ("FrameDescriptorHeap heap", "ComPtr<ID3D12Resource> constants",
                            "ComPtr<ID3D12Resource> input", "ComPtr<ID3D12Resource> output",
                            "ComPtr<ID3D12RootSignature> root", "ComPtr<ID3D12PipelineState> pipeline"):
            self.assertIn(declaration, storage)
        self.assertIn("storage->root = _rootSignature", SOURCE)
        self.assertIn("storage->pipeline = _pipelineState", SOURCE)
        self.assertNotIn("ScopedGpuTime_Dx12", SOURCE)

    def test_retention_precedes_descriptor_and_constant_updates(self):
        dispatch = SOURCE.split("bool Bias_Dx12::Dispatch(", 1)[1].split("Bias_Dx12::Bias_Dx12(", 1)[0]
        acquired = dispatch.index("AcquireDispatchStorage(InCmdList)")
        for operation in ("storage->input = InResource", "storage->output = OutResource",
                          "CreateShaderResourceView", "CreateUnorderedAccessView", "CreateConstantsBuffer"):
            self.assertLess(acquired, dispatch.index(operation))
        self.assertLess(dispatch.index("storage->output = OutResource"), dispatch.index("InCmdList->Dispatch("))
        self.assertIn("CreateConstantsBuffer(_device, storage->constants.Get()", dispatch)
        self.assertNotIn("_constantBuffer", SOURCE)

    def test_reusing_output_does_not_reset_tracked_state(self):
        create = SOURCE.split("bool Bias_Dx12::CreateBufferResource(", 1)[1].split(
            "void Bias_Dx12::SetBufferState(", 1)[0]
        reuse = create.split("if (_buffer)", 1)[1].split("auto resourceFlags", 1)[0]
        for dimension in ("Width", "Height", "Format"):
            self.assertIn(f"bufferDesc.{dimension} == sourceDesc.{dimension}", reuse)
        self.assertIn("return true", reuse)
        self.assertNotIn("_bufferState =", reuse)
        self.assertLess(create.index("return true"), create.index("Shader_Dx12::CreateBufferResource"))
        self.assertLess(create.index("Shader_Dx12::CreateBufferResource"), create.index("_bufferState = InState"))

    def test_shader_semantics_and_constant_alignment_are_unchanged(self):
        self.assertIn("struct alignas(256) InternalConstants", HEADER)
        self.assertIn("constants.Bias = std::clamp(InBias, 0.0f, 0.9f)", SOURCE)
        self.assertIn("storage->heap.Initialize(_device, 1, 1, 1)", SOURCE)
        self.assertIn("SetupRootSignature(InDevice, 1, 1, 1)", SOURCE)
        self.assertIn("InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1)", SOURCE)


if __name__ == "__main__":
    unittest.main()

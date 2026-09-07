"""CPU/source guards for the optional native bound-CB12 probe; CI/live validate GPU behavior."""
from pathlib import Path
import re
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
PROBE = (ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkFogProbe.cpp").read_text()
COPY = (ROOT / "OptiScaler/upscalers/ffx/FSRDFogLayerCapture.cpp").read_text()
HEADER = (ROOT / "OptiScaler/upscalers/ffx/FSRDFogLayerCapture.h").read_text()


def body(source, name):
    begin = source.index("{", source.index(name + "("))
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]


class BoundCb12Probe(unittest.TestCase):
    def test_shader_reads_only_original_high_accesses_as_raw_uint(self):
        shader = PROBE.split('BoundCb12Shader[] = R"hlsl(', 1)[1].split(')hlsl"', 1)[0]
        self.assertIn("register(b12)", shader)
        self.assertIn("uint4 sharedWords[28]", shader)
        self.assertIn("uint4 PSMain", shader)
        self.assertIn("uint4 PSMain()", shader)
        self.assertIn("return sharedWords[BOUND_CB12_REGISTER]", shader)
        indices = [int(i) for i in re.search(r"BoundCb12Registers \{ ([\d, ]+) \}", PROBE).group(1).split(",")]
        self.assertEqual(indices, [21, 22, 23, 24, 27])
        for forbidden in ("asfloat", "RW", "Texture", "Sample", "Interlocked", "ddx", "ddy", "discard",
                          "SV_Position", "position", "if (", "uint pixel"):
            self.assertNotIn(forbidden, shader)
        # Native UINT storage preserves nonfinite-float bit patterns, subnormals,
        # signed zero, and arbitrary mixed integer constants without float casts.
        patterns = [0x80000000, 0x00000001, 0x7F800001, 0xFFFFFFFF,
                    0x7F800000, 0xFF800000, 0x3F800000, 0xDEADBEEF]
        registers = [[patterns[(r + c) % len(patterns)] for c in range(4)] for r in range(28)]
        expected = [v for i in indices for v in registers[i]]
        encoded = struct.pack("<20I", *expected)
        self.assertEqual(len(encoded), 80)
        self.assertEqual(list(struct.unpack("<20I", encoded)), expected)

    def test_fixed_draw_selection_survives_coarse_broadcast_and_scissor(self):
        # Each coarse invocation returns the SAME fixed register for its draw.
        # Scissor preserves only that draw's fine column, regardless of which
        # coarse location runs the shader (including outside that column).
        values = [(0x80000000 + i, 0x7F800001 + i, i, 0xDEADBEEF - i) for i in range(5)]
        for coarse_width in (1, 2, 4):
            for coarse_height in (1, 2, 4):
                for selected_location in ("first", "last"):
                    output = [None] * 5
                    for column, constant in enumerate(values):
                        coarse_x = column // coarse_width * coarse_width
                        location = coarse_x if selected_location == "first" else coarse_x + coarse_width - 1
                        self.assertGreaterEqual(location, coarse_x)
                        for x in range(coarse_x, coarse_x + coarse_width):
                            for y in range(coarse_height):
                                if column <= x < column + 1 and 0 <= y < 1:
                                    output[x] = constant
                    self.assertEqual(output, values)

    def test_high_only_optional_pso_gate_and_exact_existing_vertex_root(self):
        prepare = body(PROBE, "PrepareBoundCb12Pso")
        for guard in ("!captureEnabled.load()", "!tagged.authored",
                      "tagged.pixelSha256 != FogShaders[0].sha256", "compatible.SampleMask & 1",
                      "D3D12_FORMAT_SUPPORT1_RENDER_TARGET"):
            self.assertLess(prepare.index(guard), prepare.index("D3DCompile("))
        self.assertIn("auto clone = compatible", prepare)
        self.assertNotIn("clone.VS =", prepare)
        self.assertNotIn("clone.pRootSignature =", prepare)
        self.assertIn("DXGI_FORMAT_R32G32B32A32_UINT", prepare)
        self.assertIn('"PSMain", "ps_5_0"', prepare)
        original = body(PROBE, "PrepareAuthoredPso")
        self.assertLess(original.index("hash.Finish() != FogVertexSha256"),
                        original.index("PrepareBoundCb12Pso("))
        self.assertLess(original.index("BlendEnable = FALSE"), original.index("PrepareBoundCb12Pso("))
        self.assertLess(original.index("tagged.vertexBytes.data()"), original.index("PrepareBoundCb12Pso("))

    def test_optional_preparation_failure_does_not_disable_original_three_layers(self):
        pso = body(PROBE, "PrepareBoundCb12Pso")
        self.assertIn("catch (...)", pso)
        self.assertIn("for (auto& variant : tagged.boundCb12) variant = {}", pso)
        self.assertIn("std::array<BoundCb12Variant, BoundCb12Registers.size()> variants", pso)
        self.assertLess(pso.index("if (FAILED(created) || !variant.pso) return"),
                        pso.index("tagged.boundCb12 = std::move(variants)"))
        self.assertNotIn("tagged.authored.Reset()", pso)
        target = body(PROBE, "PrepareBoundCb12Target")
        self.assertIn("if (!HasBoundCb12Psos(plan))", target)
        self.assertIn("std::all_of(plan.boundCb12Psos.begin(), plan.boundCb12Psos.end()", PROBE)
        self.assertIn("plan.layers.boundCb12 = {}", target)
        self.assertNotIn("plan.layers = {}", target)
        self.assertNotIn("captureTrackingValid.store(false)", target)
        self.assertNotIn("CopyMain(", target)

    def test_optional_resources_are_bounded_and_retained_before_any_gpu_commands(self):
        target = body(PROBE, "PrepareBoundCb12Target")
        self.assertIn("bytes > remainingBytes", target)
        self.assertIn("DXGI_FORMAT_R32G32B32A32_UINT, 5, 1, 1, 1", target)
        prepare = body(PROBE, "PrepareCapture")
        self.assertLess(prepare.index("PrepareBoundCb12Target("), prepare.index("FSRDSubmission::Retain("))
        self.assertLess(prepare.index("FSRDSubmission::Retain("), prepare.index("CopyMain("))
        self.assertIn("MaxCaptureTextureBytes - (mainBytes + copyCount * copyBytes + authoredBytes)", prepare)
        self.assertIn("plan->drawState = state", prepare)

    def test_companion_draw_preserves_roots_heaps_and_restores_exact_graphics_arrays(self):
        draw = body(PROBE, "CaptureBoundCb12")
        self.assertLess(draw.index("!HasBoundCb12Psos(plan) || !plan.layers.boundCb12.resource"),
                        draw.index("ClearRenderTargetView("))
        for forbidden in ("SetGraphicsRoot", "SetComputeRoot", "SetDescriptorHeaps", "Dispatch(",
                          "PrepareDenoiserInput", "RestoreRoot(", "RSSetShadingRate", "SV_Position"):
            self.assertNotIn(forbidden, draw)
        self.assertIn("~RestoreConstantProbeState()", draw)
        self.assertIn("originalSetPso(list, plan.originalPso.Get())", draw)
        self.assertIn("&plan.frozenOriginalRtv", draw)
        self.assertIn("plan.drawState.viewportCount, plan.drawState.viewports.data()", draw)
        self.assertIn("plan.drawState.scissorCount, plan.drawState.scissors.data()", draw)
        self.assertIn("D3D12_VIEWPORT viewport { 0, 0, 5, 1, 0, 1 }", draw)
        self.assertIn("for (size_t i = 0; i < BoundCb12Registers.size(); ++i)", draw)
        self.assertIn("D3D12_RECT scissor { LONG(i), 0, LONG(i + 1), 1 }", draw)
        self.assertIn("originalSetPso(list, plan.boundCb12Psos[i].Get())", draw)
        self.assertEqual(draw.count("originalDraw(list, 3, 1, 0, 0)"), 1)
        self.assertLess(draw.index("originalDraw(list, 3, 1, 0, 0)"),
                        draw.index('plan.provenance["bound_cb12_probe"]["status"]'))
        finish = body(PROBE, "FinishCapture")
        self.assertLess(finish.index("originalDraw(list, 3, 1, 0, 0)"), finish.index("CaptureBoundCb12("))
        self.assertLess(finish.index("CaptureBoundCb12("), finish.index("FSRDFogLayerCapture::Record("))

    def test_readback_preserves_three_float_layers_and_validates_uint_companion_separately(self):
        self.assertIn("std::array<Entry, 3> entries", COPY)
        self.assertIn("std::optional<Entry> boundCb12", COPY)
        self.assertIn("bool boundCb12 = false", COPY)
        entry = body(COPY, "PrepareEntry")
        for guard in ("entry.source.viewFormat != DXGI_FORMAT_R32G32B32A32_UINT",
                      "desc.Format != DXGI_FORMAT_R32G32B32A32_UINT", "desc.Width != 5", "desc.Height != 1",
                      "desc.MipLevels != 1", "desc.DepthOrArraySize != 1", "entry.source.subresource != 0",
                      "desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D", "desc.SampleDesc.Count != 1"):
            self.assertIn(guard, entry)
        self.assertIn('entry.componentType = "uint32"', entry)
        self.assertIn('".rgba32u"', entry)
        record = body(COPY, "Record")
        self.assertIn("layers.before.viewFormat != layers.after.viewFormat", record)
        self.assertLess(record.index("layers must have matching subresource dimensions"),
                        record.index("if (layers.boundCb12.resource)"))
        self.assertIn("entry.source.resource.Get() == layers.boundCb12.resource.Get()", record)
        self.assertIn("PrepareEntry(device, *batch->boundCb12, true)", record)
        self.assertIn("totalBytes += batch->boundCb12->bytes", record)
        self.assertIn("fog capture including bound cb12 exceeds the 256 MiB readback limit", record)
        self.assertNotIn('metadata["layers"].push_back(std::move(companion))', record)

    def test_companion_uses_same_fence_and_atomic_capture_completion_policy(self):
        record = body(COPY, "Record")
        self.assertLess(record.index("FSRDSubmission::Retain("),
                        record.index("RecordCopy(list, *batch->boundCb12)"))
        worker = body(COPY, "WriteWhenComplete")
        self.assertLess(worker.index("FSRDSubmission::Complete("), worker.index("WriteEntry(batch, *batch.boundCb12)"))
        self.assertLess(worker.index("WriteEntry(batch, *batch.boundCb12)"), worker.index('metadata["complete"] = true'))

    def test_metadata_describes_native_words_source_and_authentication_without_frame_claim(self):
        for field in ("generated_ps_source_template", "generated_ps_template_sha256", "generated_ps_variants",
                      "register_index", "output_column", "scissor_rect", "source", "source_sha256", "bytecode_sha256",
                      "original_ps_sha256", "original_vs_sha256", "inherited_bindings", "graphics_state_restore"):
            self.assertIn('"' + field + '"', body(PROBE, "PrepareCapture"))
        self.assertIn("no engine-frame assertion", PROBE)
        self.assertIn("optiscaler.fsr_rr.bound_cb12_probe.v2", PROBE)
        self.assertIn('"private_draws_recorded"', PROBE)
        self.assertIn('"draw_count", BoundCb12Registers.size()', PROBE)
        self.assertIn('"#define BOUND_CB12_REGISTER {}\\n{}"', body(PROBE, "PrepareBoundCb12Pso"))
        for field in ("optiscaler.fsr_rr.bound_cb12_words.v1", 'companion["register_indices"] = { 21, 22, 23, 24, 27 }',
                      'companion["cb_register"] = 12', 'companion["register_space"] = 0',
                      'batch->metadata["companions"].push_back'):
            self.assertIn(field, COPY)
        self.assertIn("80 native uint32 bytes", HEADER)


if __name__ == "__main__":
    unittest.main()

"""Metadata-only fog probe integration guards; Windows CI/live testing are separate."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkFogProbe.cpp").read_text()


def function(name):
    start = SOURCE.index(name + "(")
    opening = SOURCE.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[opening:end]


class FogProbe(unittest.TestCase):
    def test_opt_in_precedes_initialization_and_hooks_need_authentication(self):
        init = function("Initialize")
        self.assertLess(init.index("if (!enabled)"), init.index("std::call_once"))
        self.assertLess(init.index("if (!Authenticate(entry))"), init.index("DetourAttach"))
        self.assertIn("if (!active.load() || !device)", function("HookDevice"))
        self.assertIn("if (!active.load() || !list)", function("HookCommandList"))

    def test_complete_executable_and_live_entry_authentication(self):
        authenticate = function("Authenticate")
        for evidence in ("Cyberpunk2077.exe", "version_t(3, 0, 80, 51928)", "version_t(2, 3, 1, 0)",
                         "hash.Finish() != ExeSha256", "ReadFile(", "IMAGE_FILE_MACHINE_AMD64",
                         "0x68af45ea", "0x04efc000", "prologue != FogPrologue", "MEM_IMAGE", "PAGE_GUARD"):
            self.assertIn(evidence, authenticate)
        self.assertIn("a7de82945c03e041fc7339fcf9066224d98db2f5d80fea50f7947bb350a60991", SOURCE)
        prologue = SOURCE.split("FogPrologue = {", 1)[1].split("};", 1)[0]
        self.assertEqual(bytes(int(x, 16) for x in re.findall(r"0x[0-9a-f]+", prologue)),
                         bytes.fromhex("48 8b c4 48 89 58 08 55 56 57 41 54 41 55 41 56 41 57 48 8d 6c 24 a0 48 81 ec 60 01 00 00"))

    def test_original_entry_points_called_exactly_once(self):
        for hook, original in (("HookFogNode", "originalFogNode"), ("HookSetPso", "originalSetPso"),
                               ("HookSetRtv", "originalSetRtv"), ("HookDraw", "originalDraw"),
                               ("HookDrawIndexed", "originalDrawIndexed"),
                               ("HookCreateGraphics", "originalCreateGraphics"),
                               ("HookCreateStream", "originalCreateStream")):
            self.assertEqual(function(hook).count(original + "("), 1)

    def test_thread_local_scope_restores_parent_and_does_not_escape(self):
        self.assertIn("thread_local Scope* scope = nullptr", SOURCE)
        hook = function("HookFogNode")
        self.assertLess(hook.index("scope = &current"), hook.index("originalFogNode(node, context)"))
        self.assertIn("~RestoreScope() { scope = previous; }", hook)
        self.assertIn("if (!scope || inMetadata)", function("LogDraw"))

    def test_metadata_only_and_no_resource_state_or_file_writes(self):
        for mutation in ("->Draw", "->Dispatch", "->Copy", "->ResourceBarrier", "->SetPipelineState",
                         "->OMSetRenderTargets", "WriteFile(", "CreateShaderResourceView", "CreateRenderTargetView"):
            self.assertNotIn(mutation, SOURCE)
        self.assertIn("originalSetRtv(list, count, rtvs, contiguous, dsv)", SOURCE)

    def test_never_claims_preexisting_pso_or_rtv_was_observed(self):
        draw = function("LogDraw")
        self.assertIn("s.psoList == list && s.pso", draw)
        self.assertIn("s.rtvList == list", draw)
        self.assertIn('"pso_observed_in_scope"', draw)
        self.assertIn('"rtv_observed_in_scope"', draw)
        self.assertIn('"unknown/not authenticated"', draw)
        self.assertIn("MaxDrawLogs = 32", SOURCE)
        self.assertIn("MaxPsoLogs = 16", SOURCE)

    def test_pso_authentication_uses_full_hash_and_fails_closed(self):
        pso = function("RecordPso")
        self.assertLess(pso.index("desc.PS.BytecodeLength != identity.bytes"), pso.index("hash.Add("))
        self.assertLess(pso.index("hash.Finish() != identity.sha256"), pso.index("data.tagged.push_back"))
        for digest in ("a7a57220b8f5c1abddd8205d626ece403df647152d9a7afe147d5c285bc6589a",
                       "79fc7accdf41dd02a41d101effc20786fb809c867bc31502abc7c51548130f78",
                       "3d2bfd8ac57ac8673accb3fa48757e1848481b0a3ef5ade3feba6153095ccf6b",
                       "0953f807c7a784c8012fe37eb46bd3ea40ecb17aa94baba36e10ad61d1206738"):
            self.assertIn(digest, SOURCE)
        self.assertIn("FAILED(D3DX12ParsePipelineStream(*desc, &parsed))", function("HookCreateStream"))
        self.assertIn("pipeline-library loads not authenticated", SOURCE)

    def test_blend_and_target_metadata_use_only_effective_descriptors(self):
        pso = function("RecordPso")
        self.assertIn("std::min(desc.NumRenderTargets", pso)
        self.assertIn("desc.BlendState.IndependentBlendEnable ? i : 0", pso)
        for field in ("src", "dst", "op", "src_alpha", "dst_alpha", "op_alpha", "write_mask",
                      "format", "root_signature", "sample_count"):
            self.assertIn('"' + field + '"', pso)

    def test_rearm_is_explicit_bounded_and_preserves_sequence(self):
        poll = function("PollRearm")
        self.assertIn("MaxRearms = 2", SOURCE)
        self.assertIn("now + 1000", poll)
        self.assertIn("arm.load() >= MaxRearms", poll)
        self.assertIn('Util::ExePath().parent_path() / L"FSRRR-fog-probe.request"', poll)
        self.assertIn("FILE_ATTRIBUTE_DIRECTORY", poll)
        self.assertLess(poll.index("if (!DeleteFileW(request.c_str()))"), poll.index("drawLogs.store(0)"))
        self.assertNotIn("scopes.store", poll)
        self.assertIn("not a frame association", poll)


if __name__ == "__main__":
    unittest.main()

"""Standalone guide readback contract and compiled typed request lifecycle.

GPU behavior is verified by Windows CI/live diagnostics, not by source checks.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "OptiScaler/upscalers/ffx/FSRDFogLayerCapture.cpp").read_text()
HEADER = (ROOT / "OptiScaler/upscalers/ffx/FSRDFogLayerCapture.h").read_text()


def body(name):
    start = SOURCE.index("{", SOURCE.index(name + "("))
    depth, end = 1, start + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


class StandaloneGuideCapture(unittest.TestCase):
    def test_native_distinct_contract_precedes_allocation_and_commands(self):
        record = body("RecordEarlyGuides")
        for guard in ("!keepAlive", "!texture.resource", "identities[i].Get() == identities[previous].Get()",
                      "texture.subresource != 0", "texture.viewFormat != format", "desc.Format != format",
                      "desc.MipLevels != 1", "desc.DepthOrArraySize != 1", "desc.SampleDesc.Count != 1",
                      "desc.SampleDesc.Quality != 0", "texture.state != GuideState",
                      "entry.footprint.Footprint.Width != first.Width",
                      "entry.footprint.Footprint.Height != first.Height", "entry.bytes > MaxBytes - totalBytes"):
            self.assertLess(record.index(guard), record.index("AllocateReadback(device, entry)"))
        self.assertIn("i < 2 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT", record)
        self.assertIn("PrepareEntry(device, entry, false, i < 2)", record)
        self.assertIn("D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE", record)
        self.assertIn("batch->keepAlive = keepAlive", record)
        self.assertIn("CheckSameDevice(device, list)", record)

    def test_standalone_manifest_has_only_native_guide_companions(self):
        record = body("RecordEarlyGuides")
        self.assertIn('"optiscaler.fsr_rr.early_guide_capture.v1"', record)
        self.assertIn('{ "render_extent", { extent.Width, extent.Height } }', record)
        self.assertIn('"FSRRR-early-guide-captures" / Timestamp("early-guides")', record)
        self.assertIn('companion["schema"] = "optiscaler.fsr_rr.early_guide.v1"', record)
        self.assertIn('companion["uav_register"] = i', record)
        self.assertIn('"early_diffuse_albedo", "early_specular_albedo", "early_normal_roughness"', record)
        self.assertIn('"source authenticity and frame association not verified', record.replace('caller_supplied; ', ''))
        for forbidden in ('"layers"', '"scene_before"', '"scene_after"', '"authored_fog"', 'layers.before', 'layers.after'):
            self.assertNotIn(forbidden, record)

    def test_retention_fence_writer_and_failure_are_separate_from_recorded(self):
        record = body("RecordEarlyGuides")
        self.assertLess(record.index("FSRDSubmission::Retain(device, list, batch)"), record.index("RecordCopy(list, entry)"))
        self.assertLess(record.index("RecordCopy(list, entry)"), record.index("recorded = true"))
        self.assertLess(record.index("recorded = true"), record.index("CreateThread("))
        self.assertIn('return recorded;', record)
        self.assertIn('bool claimed = false', record)
        self.assertIn('if (claimed)', record)
        self.assertIn('noexcept;', HEADER.split('bool RecordEarlyGuides', 1)[1].split('// Caller preconditions', 1)[0])
        writer = body("WriteWhenComplete")
        self.assertLess(writer.index("FSRDSubmission::Complete(args.ticket)"), writer.index("WriteEntry(batch, entry)"))
        self.assertLess(writer.index("WriteEntry(batch, entry)"), writer.index('metadata["complete"] = true'))
        self.assertIn('SubmissionFailed(args.ticket)', writer)
        self.assertIn('CompletionTimeoutMs', writer)
        self.assertNotIn('registry', body("RecordCopy"))
        for forbidden in ("SetPipelineState", "Dispatch(", "OMSetRenderTargets", "ExecuteCommandLists", "WaitForSingleObject"):
            self.assertNotIn(forbidden, record)

    def test_wrong_kind_record_cannot_consume_request_or_emit_commands(self):
        for name, kind in (("Record", "FogLayers"), ("RecordEarlyGuides", "EarlyGuides")):
            record = body(name)
            kind_guard = record.index(f"registry.kind.load(std::memory_order_relaxed) != RequestKind::{kind}")
            consume = record.index("registry.requested.exchange(false")
            self.assertLess(kind_guard, consume)
            self.assertIn("||", record[kind_guard:consume])
            self.assertLess(consume, record.index("FSRDSubmission::Retain("))

    def test_compiled_actual_typed_request_lifecycle(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to compile actual typed request functions")
        functions = '\n'.join(signature + body(name) for name, signature in (
            ("RequestKindCapture", "bool RequestKindCapture(RequestKind kind)"),
            ("CancelKindCapture", "void CancelKindCapture(RequestKind kind)"),
            ("Request", "bool Request()"), ("RequestEarlyGuides", "bool RequestEarlyGuides()"),
            ("CancelRequest", "void CancelRequest()"), ("CancelEarlyGuideRequest", "void CancelEarlyGuideRequest()"),
            ("WantsCapture", "bool WantsCapture()"), ("WantsEarlyGuideCapture", "bool WantsEarlyGuideCapture()"),
            ("GetStatus", "Status GetStatus()")))
        status = HEADER[HEADER.index('struct Status'):HEADER.index('// Initially one recorded attempt')]
        harness = r'''
#include <atomic>
#include <cassert>
#include <mutex>
#include <string>
''' + status + r'''
enum class RequestKind { FogLayers, EarlyGuides };
struct Registry {
    std::mutex mutex;
    std::atomic<bool> requested{false};
    std::atomic<RequestKind> kind{RequestKind::FogLayers};
    Status status;
};
Registry& GetRegistry() { static Registry r; return r; }
''' + functions + r'''
int main() {
    assert(!WantsCapture() && !WantsEarlyGuideCapture());
    assert(RequestEarlyGuides());
    assert(WantsEarlyGuideCapture() && !WantsCapture());
    assert(GetStatus().kind=="early_guides" && GetStatus().queued && GetStatus().busy);
    assert(!Request() && !RequestEarlyGuides());
    CancelRequest(); // wrong kind cannot cancel/consume it
    assert(WantsEarlyGuideCapture());
    CancelEarlyGuideRequest();
    assert(!WantsEarlyGuideCapture() && !GetStatus().attempted);
    assert(Request());
    assert(GetStatus().kind=="fog_layers" && WantsCapture() && !WantsEarlyGuideCapture());
    CancelEarlyGuideRequest();
    assert(WantsCapture());
    CancelRequest();
    assert(!WantsCapture());
    assert(RequestEarlyGuides());
    auto& r=GetRegistry();
    { std::lock_guard lock(r.mutex); r.requested.store(false); r.status.attempted=true; }
    CancelRequest(); CancelEarlyGuideRequest();
    assert(GetStatus().attempted && !Request() && !RequestEarlyGuides());
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-guide-request-") as directory:
            path = Path(directory)
            source = path / 'request.cpp'
            source.write_text(harness)
            executable = path / 'request'
            compiled = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-O2', str(source),
                                       '-o', str(executable)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()

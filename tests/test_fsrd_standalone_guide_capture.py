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

    def test_standalone_manifest_has_three_native_guides_without_fake_fog_layers(self):
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
            kind_slot = record.index(f"GetRegistry(RequestKind::{kind})")
            consume = record.index("registry.requested.exchange(false")
            self.assertLess(kind_slot, consume)
            self.assertNotIn("registry.kind", record)
            self.assertLess(consume, record.index("FSRDSubmission::Retain("))

    def test_worker_routes_to_immutable_origin_without_changing_completion_policy(self):
        writer = body("WriteWhenComplete")
        thread = body("WriterThread")
        for code in (writer, thread):
            self.assertIn("guidesOnly ? RequestKind::EarlyGuides : RequestKind::FogLayers", code)
            self.assertIn("FinishStatus(kind,", code)
            self.assertNotIn("GetStatus()", code)
            self.assertNotIn("GetEarlyGuideStatus()", code)
            self.assertNotIn("FinishStatus(false", code)
        self.assertIn("FSRDSubmission::Complete(args.ticket)", writer)
        self.assertIn("SubmissionFailed(args.ticket)", writer)
        self.assertIn("CompletionTimeoutMs", writer)
        self.assertIn("constexpr UINT64 MaxBytes = 256ull * 1024 * 1024;", SOURCE)
        self.assertIn("new std::array<Registry, 2>", body("GetRegistry"))

    def test_compiled_actual_two_slot_lifecycle_and_retirement(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to compile actual typed request functions")
        functions = '\n'.join(signature + body(name) for name, signature in (
            ("FinishStatus", "void FinishStatus(RequestKind kind, bool complete, const std::string& message, bool releaseStorage)"),
            ("RequestKindCapture", "bool RequestKindCapture(RequestKind kind)"),
            ("CancelKindCapture", "void CancelKindCapture(RequestKind kind)"),
            ("Request", "bool Request()"), ("RequestEarlyGuides", "bool RequestEarlyGuides()"),
            ("CancelRequest", "void CancelRequest()"), ("CancelEarlyGuideRequest", "void CancelEarlyGuideRequest()"),
            ("WantsCapture", "bool WantsCapture()"), ("WantsEarlyGuideCapture", "bool WantsEarlyGuideCapture()"),
            ("GetStatus", "Status GetStatus()"), ("GetEarlyGuideStatus", "Status GetEarlyGuideStatus()")))
        status = HEADER[HEADER.index('struct Status'):HEADER.index('// One recorded attempt')]
        registry = SOURCE[SOURCE.index('enum class RequestKind'):SOURCE.index('void Check(')]
        # Compile the actual admission prefixes of BOTH recording entry points:
        # the surrounding D3D12 allocation/commands are deliberately not mocked here.
        fog = body("Record")[1:body("Record").index("HMODULE module")]
        early = body("RecordEarlyGuides")
        early = early[early.index("auto& registry"):early.index("if (!device")]
        claims = 'bool ClaimFog() {' + fog + 'return true;}\n'
        claims += 'bool ClaimEarly() { bool claimed = false; ' + early + 'return claimed;}\n'
        harness = r'''
#include <array>
#include <atomic>
#include <barrier>
#include <cassert>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
''' + status + r'''
struct Batch {
    std::function<void()> onRelease;
    ~Batch() { if (onRelease) onRelease(); }
};
namespace FSRDSubmission {
struct Ticket {
    std::function<void()> onRelease;
    ~Ticket() { if (onRelease) onRelease(); }
};
}
''' + registry + functions + claims + r'''
int main() {
    auto& fog=GetRegistry(RequestKind::FogLayers);
    auto& early=GetRegistry(RequestKind::EarlyGuides);
    assert(&fog!=&early);
    assert(!WantsCapture() && !WantsEarlyGuideCapture());
    assert(!ClaimFog() && !ClaimEarly());
    assert(RequestEarlyGuides());
    assert(WantsEarlyGuideCapture() && !WantsCapture());
    assert(GetEarlyGuideStatus().kind=="early_guides" && GetEarlyGuideStatus().queued && GetEarlyGuideStatus().busy);
    assert(GetStatus().kind.empty()); // No implicit latest-kind selection.
    assert(!ClaimFog() && WantsEarlyGuideCapture());
    CancelRequest(); // wrong kind cannot cancel/consume it
    assert(WantsEarlyGuideCapture());
    assert(Request() && WantsCapture() && WantsEarlyGuideCapture());
    assert(!Request() && !RequestEarlyGuides());
    CancelEarlyGuideRequest();
    assert(!WantsEarlyGuideCapture() && !GetEarlyGuideStatus().attempted);
    assert(!ClaimEarly() && WantsCapture());
    assert(GetStatus().kind=="fog_layers" && WantsCapture() && !WantsEarlyGuideCapture());
    CancelRequest();
    assert(!WantsCapture());

    // Simultaneous independent requests admit exactly one pending request per slot.
    std::atomic<unsigned> fogAdmitted=0,earlyAdmitted=0;
    std::barrier start(17);
    {
        std::vector<std::jthread> threads;
        for(unsigned i=0;i<16;++i) threads.emplace_back([&,i]{
            start.arrive_and_wait();
            if(i%2) earlyAdmitted+=RequestEarlyGuides(); else fogAdmitted+=Request();
        });
        start.arrive_and_wait();
    }
    assert(fogAdmitted==1 && earlyAdmitted==1);
    assert(ClaimEarly() && !ClaimEarly() && WantsCapture());
    assert(GetEarlyGuideStatus().attempted && !GetStatus().attempted);
    CancelEarlyGuideRequest(); // A recorded kind cannot be canceled/rearmed.
    assert(!RequestEarlyGuides() && WantsCapture());
    assert(ClaimFog() && !ClaimFog());
    CancelRequest(); CancelEarlyGuideRequest();
    assert(GetStatus().attempted && GetEarlyGuideStatus().attempted);
    assert(!Request() && !RequestEarlyGuides());

    // Each worker owns its separate status and pending references. In particular,
    // completing Fog must not release Early's timed-out GPU storage (or vice versa).
    unsigned fogReleased=0,earlyReleased=0;
    auto attach=[&](Registry& slot,unsigned& released){
        auto checkUnlocked=[&slot,&released]{
            assert(slot.mutex.try_lock()); slot.mutex.unlock(); ++released;
        };
        slot.pending=std::make_shared<Batch>();slot.pending->onRelease=checkUnlocked;
        slot.ticket=std::make_shared<FSRDSubmission::Ticket>();slot.ticket->onRelease=checkUnlocked;
    };
    attach(fog,fogReleased);attach(early,earlyReleased);
    std::weak_ptr<Batch> fogBatch=fog.pending,earlyBatch=early.pending;
    std::weak_ptr<FSRDSubmission::Ticket> fogTicket=fog.ticket,earlyTicket=early.ticket;
    FinishStatus(RequestKind::EarlyGuides,false,"early timeout",false);
    assert(!GetEarlyGuideStatus().busy && !GetEarlyGuideStatus().complete);
    assert(GetStatus().busy && GetStatus().message=="Preparing native fog-layer readback.");
    assert(fogReleased==0 && earlyReleased==0);
    FinishStatus(RequestKind::FogLayers,true,"fog complete",true);
    assert(GetStatus().complete && GetStatus().message=="fog complete");
    assert(GetEarlyGuideStatus().message=="early timeout" && !GetEarlyGuideStatus().complete);
    assert(fogReleased==2 && earlyReleased==0 && fogBatch.expired() && fogTicket.expired());
    assert(!earlyBatch.expired() && !earlyTicket.expired());
    assert(!Request() && !RequestEarlyGuides()); // Completion/failure never resets attempts.
    // Successful retirement of the remaining slot is tested without pretending
    // timeout establishes GPU completion (this is a direct CPU policy exercise).
    FinishStatus(RequestKind::EarlyGuides,true,"early CPU retirement test",true);
    assert(earlyReleased==2 && earlyBatch.expired() && earlyTicket.expired());
    assert(GetStatus().message=="fog complete" && !RequestEarlyGuides());
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-guide-request-") as directory:
            path = Path(directory)
            source = path / 'request.cpp'
            source.write_text(harness)
            executable = path / 'request'
            for flags in (['-O0'], ['-O3', '-ffast-math']):
                compiled = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-pthread', *flags,
                                           str(source), '-o', str(executable)], capture_output=True, text=True)
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()

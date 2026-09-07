"""One-shot private-guide integration guards and compiled admission predicates."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkFogProbe.cpp"
SOURCE = CPP.read_text()


def function(name, full=False):
    marker = SOURCE.index(name + "(")
    start = SOURCE.rfind("\n", 0, marker) + 1
    opening = SOURCE.index("{", marker)
    depth, end = 1, opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start if full else opening:end]


class EarlyGuideIntegration(unittest.TestCase):
    def test_two_markers_one_attempt_and_bounded_deferral(self):
        poll = function("PollRearm")
        self.assertLess(poll.index('L"FSRRR-fog-capture.request"'), poll.index('L"FSRRR-early-guides.request"'))
        self.assertLess(poll.index("DeleteFileW(captureRequest.c_str())"), poll.index("earlyRequested.store(true)"))
        self.assertLess(poll.index("DeleteFileW(earlyRequest.c_str())"), poll.index("earlyRequested.store(true)"))
        self.assertIn("!earlyAttempted.load()", poll)
        self.assertIn("earlyAttempted.exchange(true)", function("PrepareAndRecordEarlyGuides"))
        capture = function("PrepareCapture")
        self.assertLess(capture.index("earlyRequested.load()"), capture.index("earlyAvailability = Json::parse"))
        self.assertLess(capture.index("earlyRequestedAt.load() < 10000"), capture.index("captureStarted.exchange(true)"))
        # CPU producer evidence itself is gated by the additional explicit request.
        initializer = function("HookGBufferInitializer")
        self.assertLess(initializer.index("earlyRequested.load() && !earlyAttempted.load() && !inMetadata"),
                        initializer.index("FSRDCyberpunkEarlyGuides::Describe("))

    def test_authentication_precedes_initializer_hook_and_original_calls_once(self):
        init = function("Initialize")
        self.assertLess(init.index("if (!Authenticate(entry))"), init.index("MatchLiveCode(image, code)"))
        self.assertLess(init.index("MatchLiveCode(image, code)"), init.index("HookGBufferInitializer"))
        for hook, original in (("HookGBufferInitializer", "originalGBufferInitializer"),
                               ("HookClearRtv", "originalClearRtv"), ("HookClearDsv", "originalClearDsv"),
                               ("HookCreateCompute", "originalCreateCompute")):
            self.assertEqual(function(hook).count(original + "("), 1)
        initializer = function("HookGBufferInitializer")
        self.assertLess(initializer.index("originalGBufferInitializer(context, h0, h1, h2, hs)"),
                        initializer.index("data.earlyProducer = candidate"))
        self.assertIn("producerScope = previous", initializer)
        self.assertIn("retired = std::move(data.earlyProducer)", initializer)
        self.assertIn("Final COM releases of the old producer occur outside", initializer)

    def test_actual_original_clears_and_owned_resources_are_required(self):
        observe = function("ObserveInitializerClear")
        for guard in ("rectangles || rects", "D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL",
                      "index >= 4", "descriptor != producer.clearDescriptors[index]",
                      "!found->second.known", "found->second.predicated", "found->second.queryCount",
                      "producer.generation != generation", "rtv->view.Texture2D.MipSlice",
                      "refs <= 0", "native != producer.textures[index].native", "clearDescriptor != descriptor",
                      "MaxInitializerBytes - producer.resourceBytes"):
            self.assertIn(guard, observe)
        self.assertLess(observe.index("refs <= 0"), observe.index("ComPtr<ID3D12Resource> resource"))
        self.assertIn("producer.resources[index] = std::move(resource)", observe)
        match = function("ProducerMatches")
        for guard in ("producer.clearMask != 15", "producer.clearCount != 4", "producer.generation != generation",
                      "producer.nativeList != uintptr_t(list)", "EarlyFrameSource(producer.metadata)",
                      "SameEarlyReservation(producer.metadata, current, i)"):
            self.assertIn(guard, match)
        self.assertIn('"last_material_writer", "not_proven"', function("PrepareAndRecordEarlyGuides"))

    def test_cpu_heap_ranges_bounded_and_exact_authored_descriptors_copied(self):
        heap = function("HookCreateHeap")
        for guard in ("D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV", "D3D12_DESCRIPTOR_HEAP_FLAG_NONE",
                      "MaxCpuSrvHeaps", "MaxCpuSrvSlots - data.cpuSrvSlots", "MaxCpuSrvBytes - data.cpuSrvBytes",
                      "earlyHeapTrackingValid.store(false)"):
            self.assertIn(guard, heap)
        optional = heap.split("if (SUCCEEDED(hr) && desc && desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV", 1)[0]
        self.assertNotIn("captureTrackingValid.store(false)", optional)
        prepare = function("PrepareAndRecordEarlyGuides")
        for guard in ("alternate_cpu_srv_handle", "ordinary_cpu_srv_handle", "raw_array_size",
                      "raw_dimension_mip_bits", "raw_flags_bits", "formatTag != 0x18 && formatTag != 0x19",
                      "DXGI_FORMAT_R24G8_TYPELESS", "DXGI_FORMAT_R32G8X24_TYPELESS",
                      "heap.generation", "ProducerMatches(*producer, fresh", "earlyHeapTrackingValid.load()"):
            self.assertIn(guard, prepare)
        self.assertNotIn("CreateShaderResourceView", prepare)
        self.assertNotIn("CreateUnorderedAccessView", prepare)
        self.assertLess(prepare.index("ProducerMatches(*producer, current"),
                        prepare.index("CyberpunkGuidePass::Prepare("))

    def test_exact_shader_matrix_and_branch_contract(self):
        shader = function("RecordGuideShader")
        self.assertIn("shader.BytecodeLength != GuideShaderBytes", shader)
        self.assertLess(shader.index("ReadExactMemory"), shader.index("hash.Finish() != GuideShaderSha256"))
        self.assertLess(shader.index("hash.Finish() != GuideShaderSha256"), shader.index("data.guideShader ="))
        prepare = function("PrepareAndRecordEarlyGuides")
        for evidence in ("view + 0x1c0", "view + 0x180", "inverseProjection != checkProjection",
                         "inverseView != checkView", "CyberpunkGuideMatrix::Generate", "TransparencyInput::PreTransparencySurface",
                         'settings.at("extra_specular_enabled").get<unsigned>() != 0', "CyberpunkGuideConstants::PackPass",
                         "CyberpunkGuideConstants::PackShared", 'evidence["cb12_words"]', 'evidence["cb6_words"]'):
            self.assertIn(evidence, prepare)

    def test_private_pass_before_original_draw_and_no_live_scene_output(self):
        capture = function("PrepareCapture")
        self.assertLess(capture.index("FSRDSubmission::Retain("), capture.index("PrepareAndRecordEarlyGuides(list, *plan)"))
        draw = function("HookDraw")
        self.assertLess(draw.index("PrepareCapture("), draw.index("originalDraw("))
        self.assertEqual(draw.count("originalDraw("), 1)
        prepare = function("PrepareAndRecordEarlyGuides")
        self.assertIn("producer->textures[i].native == uintptr_t(plan.main.Get())", prepare)
        self.assertIn("plan.layers.earlyGuides[i].resource = plan.earlyWork->Outputs()[i]", prepare)
        self.assertNotIn("plan.layers.before =", prepare)
        self.assertNotIn("plan.main =", prepare)
        self.assertNotIn("->CopyResource", prepare)
        self.assertNotIn("->ResourceBarrier", prepare) # Original input state requests belong to engine core.

    def test_fatal_outcome_cannot_become_allocation_failure_fallback(self):
        prepare = function("PrepareAndRecordEarlyGuides")
        recorded = prepare.split("CyberpunkEngineAccess::RecordPrivateCompute(", 1)[1]
        self.assertLess(recorded.index("Outcome::ScopeLostAfterPrivate"), recorded.index('evidence["engine_state_requests"]'))
        fatal = recorded.split("Outcome::ScopeLostAfterPrivate", 1)[1].split('evidence["engine_state_requests"]', 1)[0]
        self.assertLess(fatal.index("earlyFatalRecording.store(true)"), fatal.index("LOG_ERROR"))
        self.assertNotIn('evidence[', fatal)
        self.assertIn("active.load() && captureEnabled.load() && earlyRequested.load()", fatal)
        self.assertIn("image == authenticatedImage.load()", fatal)
        self.assertIn("image == uintptr_t(GetModuleHandleW(nullptr))", fatal)
        self.assertIn("TerminateProcess(GetCurrentProcess(), 0xf51d0001u)", fatal)
        self.assertEqual(prepare.count("TerminateProcess("), 1)
        self.assertEqual(SOURCE.count("TerminateProcess("), 2) # Separately authenticated lighting fatal path.
        draw = function("HookDraw")
        self.assertLess(draw.index("earlyFatalRecording.load()"), draw.index("originalDraw("))

    def test_refusal_has_actionable_observed_mismatch_metadata(self):
        prepare = function("PrepareAndRecordEarlyGuides")
        for field in ("initializer_observation", "fog_observation", "native_list", "recording_generation",
                      "cpu_frame_source", "view", "position", "clear_mask", "clear_count", "inputs", "failure"):
            self.assertIn('"' + field + '"', prepare)
        self.assertLess(prepare.index('evidence["initializer_observation"]'),
                        prepare.index("if (!producer || !ProducerMatches("))

    def test_compiled_exact_producer_admission_predicates(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to compile actual producer admission predicates")
        definitions = "\n".join(function(name, True) for name in
                                 ("EarlyInput", "EarlyFrameSource", "SameEarlyReservation", "ProducerMatches"))
        harness = r'''
#include <array>
#include <cstdint>
#include <stdexcept>
#include <cassert>
#include <json.hpp>
using Json=nlohmann::json;
struct ID3D12GraphicsCommandList {};
struct EarlyProducer
{
    Json metadata;
    bool valid=true;
    unsigned clearMask=15,clearCount=4;
    uintptr_t nativeList=0x1234;
    uint64_t generation=17;
    std::array<bool,4> resources={true,true,true,true};
};
''' + definitions + r'''
Json Metadata(uint32_t position)
{
    Json metadata={{"view",0x5678},{"view_dimensions",{1280,720}},{"inputs",Json::array()}};
    metadata["camera_provenance"]["frame_id_virtual_route"]["explicit_frame_id_source"]={
        {"status","CPU_value_present"},{"repeated_source_fields_equal",true},{"source_value",101}};
    constexpr uint32_t keys[]={0x63bcf380,0x64bcf513,0x65bcf6a6,0x61f178d4};
    for(unsigned i=0;i<4;++i)
    {
        Json interval={{"status","compiler_interval_observed"},{"repeated_metadata_equal",true},
            {"inclusive_contains_position",true},{"end_event_relation","before"},{"graph_phase",2},
            {"record_used_flag",1},{"record_handle",i+1},{"current_position",position},
            {"holder_first_use",10},{"holder_end_event_position",100},{"holder_reservation_end",110},
            {"graph_address",0x8000},{"holder_address",0x9000+i*0x58},{"resource_record_address",0xa000+i*0x48},
            {"record_kind",0},{"record_policy",1}};
        Json registry={{"status","borrowed_address_observed"},{"ref_status",6},{"ref_status_after",6},
            {"borrowed_native_address",0xb000+i*0x100}};
        metadata["inputs"].push_back({{"base_key",keys[i]},{"namespaced_key",keys[i]},
            {"handle",i+1},{"status","handle_present"},{"logical_interval",interval},{"texture_registry",registry}});
    }
    return metadata;
}
int main()
{
    const auto list=reinterpret_cast<ID3D12GraphicsCommandList*>(uintptr_t(0x1234));
    EarlyProducer producer;producer.metadata=Metadata(10);auto current=Metadata(50);
    assert(ProducerMatches(producer,current,list,17));
    auto reject=[&](auto alter) { auto p=producer;auto c=current;alter(p,c);assert(!ProducerMatches(p,c,list,17)); };
    reject([](auto& p,auto&){p.clearMask=7;});
    reject([](auto& p,auto&){p.clearCount=3;});
    reject([](auto& p,auto&){p.valid=false;});
    reject([](auto& p,auto&){p.nativeList=0x7777;});
    reject([](auto& p,auto&){++p.generation;});
    reject([](auto& p,auto&){p.resources[2]=false;});
    reject([](auto&,auto& c){c["view"]=0x7777;});
    reject([](auto&,auto& c){c["view_dimensions"][0]=1920;});
    reject([](auto&,auto& c){c["camera_provenance"]["frame_id_virtual_route"]["explicit_frame_id_source"]["source_value"]=102;});
    reject([](auto&,auto& c){c["camera_provenance"]["frame_id_virtual_route"]["explicit_frame_id_source"]["status"]="unavailable";});
    reject([](auto&,auto& c){c["inputs"][3]["texture_registry"]["borrowed_native_address"]=7;});
    reject([](auto&,auto& c){c["inputs"][0]["logical_interval"]["holder_address"]=7;});
    reject([](auto&,auto& c){c["inputs"][0]["logical_interval"]["resource_record_address"]=7;});
    reject([](auto&,auto& c){c["inputs"][0]["logical_interval"]["holder_end_event_position"]=50;});
    reject([](auto&,auto& c){c["inputs"][0]["logical_interval"]["current_position"]=9;});
    reject([](auto&,auto& c){c["inputs"][0]["logical_interval"]["record_used_flag"]=0;});
    reject([](auto&,auto& c){c["inputs"][0]["texture_registry"]["ref_status"]=0;});
    reject([](auto&,auto& c){c["inputs"][0]["namespaced_key"]=7;});
    reject([](auto& p,auto&){p.metadata["inputs"][0]["logical_interval"]["current_position"]=60;});
    assert(!ProducerMatches(producer,current,list,18));
    // Refcounts can legitimately rise due to engine retention; identity/range must remain exact.
    current["inputs"][0]["texture_registry"]["ref_status"]=8;
    current["inputs"][0]["texture_registry"]["ref_status_after"]=8;
    assert(ProducerMatches(producer,current,list,17));
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-guide-admission-") as name:
            directory = Path(name)
            source, binary = directory / "test.cpp", directory / "test"
            source.write_text(harness)
            compiled = subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", str(source),
                                       "-I", str(ROOT / "external/nlohmann"), "-o", str(binary)],
                                      capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()

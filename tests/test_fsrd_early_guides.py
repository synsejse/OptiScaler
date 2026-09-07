"""CPU-only early guide lookup guards and optional compiled synthetic-memory test."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkEarlyGuides.cpp"
SOURCE = CPP.read_text()


class EarlyGuides(unittest.TestCase):
    def test_observation_is_only_in_authenticated_capture_or_explicit_early_request(self):
        probe = (CPP.parent / "FSRDCyberpunkFogProbe.cpp").read_text()
        self.assertEqual(probe.count("FSRDCyberpunkEarlyGuides::Describe("), 11)
        depth = probe.split("void PrepareFogDepth(", 1)[1].split("void RecordFogDepth(", 1)[0]
        self.assertEqual(depth.count("FSRDCyberpunkEarlyGuides::Describe("), 2)
        self.assertIn("!fogDepthAuthenticated.load()", depth)
        self.assertIn("!scope->depthBindObserved", depth)
        initializer = probe.split("void __fastcall HookGBufferInitializer(", 1)[1].split("void ObserveInitializerClear(", 1)[0]
        self.assertLess(initializer.index("earlyRequested.load() && !earlyAttempted.load() && !inMetadata"),
                        initializer.index("FSRDCyberpunkEarlyGuides::Describe("))
        capture = probe.split("std::shared_ptr<CapturePlan> PrepareCapture(", 1)[1].split("void PublishFogEndpoint(", 1)[0]
        self.assertLess(capture.index("!FSRDFogLayerCapture::WantsCapture()"),
                        capture.index("FSRDCyberpunkEarlyGuides::Describe("))
        self.assertIn("Describe(s.context, uintptr_t(GetModuleHandleW(nullptr)))", capture)
        self.assertLess(capture.index('plan->provenance["endpoint_origin"]'),
                        capture.index('plan->provenance["early_guide_availability"]'))
        lighting = probe.split("std::shared_ptr<LightingCapturePlan> PrepareLightingCapture(", 1)[1].split("void FinishLightingCapture(", 1)[0]
        self.assertEqual(lighting.count("FSRDCyberpunkEarlyGuides::Describe("), 2)
        draw = probe.split("void WINAPI HookDraw(", 1)[1].split("void WINAPI HookDrawIndexed(", 1)[0]
        for gate in ("lightingRequested.load()", "WantsEarlyGuideCapture()", "MatchesFinalLightingDraw(",
                     "!lightingAttempted.exchange(true)"):
            self.assertLess(draw.index(gate), draw.index("PrepareLightingCapture("))
        ray = probe.split("std::shared_ptr<RayCopyBundle> PrepareRayCopy(", 1)[1].split("void FinishRayCopy(", 1)[0]
        self.assertEqual(ray.count("FSRDCyberpunkEarlyGuides::Describe("), 1)
        self.assertLess(ray.index("!lightingRequested.load()"), ray.index("FSRDCyberpunkEarlyGuides::Describe("))
        # Temporal selection needs this exact original-use camera before it can
        # choose the frame packet. Capture/request and native-scope gates still
        # precede the read; there is no previous/global camera fallback.
        for gate in ("privateResetArming.load(std::memory_order_acquire)",
                     "!SameRayCopyScope(*plan)", "TemporalRecordingRequested()"):
            self.assertLess(ray.index(gate), ray.index("FSRDCyberpunkEarlyGuides::Describe("))
        self.assertLess(ray.index("FSRDCyberpunkEarlyGuides::Describe("),
                        ray.index("SelectTemporalFrame(*window, metadata"))
        self.assertIn("if (window && !packet) return {};", ray)
        reset = probe.split("void RecordPrivateReset(", 1)[1].split("std::shared_ptr<CapturePlan> PrepareCapture(", 1)[0]
        self.assertEqual(reset.count("FSRDCyberpunkEarlyGuides::Describe("), 1)
        self.assertLess(reset.index("if (!packet) return"), reset.index("FSRDCyberpunkEarlyGuides::Describe("))
        self.assertLess(reset.index("FSRD::PrivateDenoise::Prepare("), reset.index("FSRDCyberpunkEarlyGuides::Describe("))
        self.assertLess(reset.index("FSRDCyberpunkEarlyGuides::Describe("), reset.index("EmbedConsumer("))
        self.assertIn("source.SameFrame(repeated)", reset)
        temporal = probe.split("PrivateResetPacket* ObserveTemporalFog(", 1)[1].split("void WINAPI HookDraw(", 1)[0]
        self.assertEqual(temporal.count("FSRDCyberpunkEarlyGuides::Describe("), 1)
        for gate in ("!window || privateResetArming.load", "!scope->fogHelper",
                     "CyberpunkFogDepth::DrawReturnRva", "!s.boundRtv.known",
                     "!IsFullRgbViewport(", "ResTrack_Dx12::PrepareSubmission("):
            self.assertLess(temporal.index(gate), temporal.index("FSRDCyberpunkEarlyGuides::Describe("))
        self.assertLess(temporal.index("FSRDCyberpunkEarlyGuides::Describe("),
                        temporal.index("ResetSource::ParseRawTemporal(metadata)"))

    def test_read_only_and_no_persistent_engine_state(self):
        self.assertIn("ReadProcessMemory(GetCurrentProcess()", SOURCE)
        for forbidden in ("WriteProcessMemory", "DetourAttach", "GetProcAddress", "CreateThread",
                          "ID3D12", "->Dispatch", "ResourceBarrier", "CreateFile", "WriteFile",
                          "std::mutex", "thread_local", "State::Instance", "Config::Instance"):
            self.assertNotIn(forbidden, SOURCE)
        self.assertIn('"retained_engine_pointers", false', SOURCE)
        self.assertIn('"snapshot_atomic", false', SOURCE)
        self.assertIn('"shared_cb12", "not_captured"', SOURCE)

    def test_exact_static_offsets_and_finite_bounds(self):
        for expected in ("0x51b848", "0x5e00 + 0x40", "0x20", "0x28", "0x34",
                         "uint64_t(i) * 24 + 8", "i ? i - 1 : 0", "boundary >= context.position",
                         "keys[1] == key && keys[2] == key", "context.nameSpace << 24",
                         "0x63bcf380", "0x64bcf513", "0x65bcf6a6", "0x61f178d4",
                         "0x1d70", "0x2e8", "0x17d0", "features.Test(0x35)",
                         "0xdebf0c27", "0x15eab19c", "0x268", "0x274"):
            self.assertIn(expected, SOURCE)
        for bound in ("MaxReadCalls = 4096", "MaxReadBytes = 128 * 1024", "MaxChain = 256",
                      "MaxVersions = 256", "MaxTableElements = 1024 * 1024", "MaxStride = 256"):
            self.assertIn(bound, SOURCE)
        self.assertIn("actual != sizeof(result)", SOURCE)
        self.assertIn("sizeof(T) - 1 > std::numeric_limits<uintptr_t>::max() - address", SOURCE)
        self.assertIn("if (!holder)", SOURCE)
        self.assertIn("if (!resourceRecord)", SOURCE)
        self.assertIn("if (!selected)", SOURCE)

    def test_known_settings_and_unknown_feature_are_distinct(self):
        for rva in ("0x38137f0", "0x3310670", "0x3813840"):
            self.assertIn(rva, SOURCE)
        self.assertIn('if (!extra.contains("feature_0x35"))', SOURCE)
        self.assertIn('"current view feature unavailable"', SOURCE)
        self.assertIn("std::isfinite(scale)", SOURCE)
        self.assertIn('"extra_specular_scale_bits"', SOURCE)

    def test_registry_mapping_remains_borrowed_cpu_metadata(self):
        for evidence in ("TextureRegistryRva = 0x3438a28", "TextureSlotCount = 0x8000",
                         "TextureSlotStride = 0xb0", "TextureRefOffset = 0x2f1d0",
                         "TextureNativeOffset = 0x2f1d8", "handle > TextureSlotCount", "refs <= 0",
                         '"native_address_dereferenced", false', '"lifetime", "not_established"',
                         '"resource_state", "not_observed"', "refs != refsAfter"):
            self.assertIn(evidence, SOURCE)
        for forbidden in ("AddRef(", "Release(", "GetDesc(", "reinterpret_cast<decltype"):
            self.assertNotIn(forbidden, SOURCE)

    def test_intervals_are_bounded_compiler_metadata_not_gpu_proof(self):
        for evidence in ("HolderArenaOffset = 0x514a18", "HolderStride = 0x58", "HolderCapacity = 320",
                         '"inclusive_compiler_reservation_only"', '"producer_completion", "not_established"',
                         '"alias_lifetime", "not_established"', '"holder_end_event_position"',
                         "ranges[0] <= context.position && context.position <= ranges[1]",
                         "ranges[0] > ranges[2] || ranges[2] > ranges[1]",
                         "(holder - arena) % HolderStride", "count > HolderCapacity",
                         "phase != 2 || !context.counter || used != 1"):
            self.assertIn(evidence, SOURCE)

    def test_descriptor_sources_are_raw_and_not_a_usable_srv_claim(self):
        for evidence in ('"ordinary_cpu_srv_handle"', '"alternate_cpu_srv_handle"',
                         '"raw_dimension_mip_bits"', '"raw_format_sample_bits"', '"raw_array_size"',
                         '"raw_flags_bits"', '"descriptor_handles_dereferenced", false',
                         '"native_view_format", "not_observed"', '"usable_srv", "not_established"',
                         '"engine_SRV_request_mask_not_current_state"',
                         '"t4_material_class_stencil_view"', "Address(nativeSlot, 0x30)",
                         "Address(nativeSlot, 0x48)", "Address(nativeSlot, 0x4e)"):
            self.assertIn(evidence, SOURCE)
        self.assertNotIn("t4_material_uint2", SOURCE)

    def test_observational_camera_and_exact_motion_precedence(self):
        for evidence in ('"streamline_producer_observed", false', '"frame_token", "not_observed"',
                         '"matrix_payload", "current_view_CPU_source_rows_only"',
                         '"effective_ngx_reset", "not_established"',
                         '"final_ngx_constants", "not_established"', "0x3e4, 0x80000000u",
                         "value == 0", "if (overrideHandle)", "SetHandle(result, overrideHandle)",
                         "features.Test(0x5a)", "features.Test(0x37)", "features.Test(0x46)"):
            self.assertIn(evidence, SOURCE)
        self.assertNotIn("overrideHandle <= INT32_MAX", SOURCE)
        self.assertIn("bit / 64", SOURCE)
        self.assertIn("bit & 63", SOURCE)

    def test_raw_camera_rows_are_sources_not_reconstructed_gpu_constants(self):
        for evidence in ('"optiscaler.fsr_rr.early_camera_sources.v2"',
                         '"bound_shared_cb12_match", "not_validated"',
                         '"camera_position_binding", "not_observed"',
                         '"jitter_free_projection", "not_captured_or_reconstructed"',
                         '"previous_camera", "not_observed"', '"source_uint32_rows"',
                         '"source_float_rows"', '"repeated_source_words_equal"',
                         '"fov_degrees"', '"producer_scale_bits", 0x37000000u',
                         '"source_int32_xyz"', '"view_pointer_unchanged"'):
            self.assertIn(evidence, SOURCE)
        for offset in ("0xc0", "0x180", "0x200", "0x1c0", "0x360"):
            self.assertIn(f"FloatRowsField<4, 4>(read, context.view, {offset})", SOURCE)
        for offset in ("0x2c0", "0x2e0", "0x2d0"):
            self.assertIn(f"FloatRowsField<1, 3>(read, context.view, {offset})", SOURCE)
        self.assertIn("FloatRowsField<1, 2>(read, context.view, 0xa0)", SOURCE)
        for forbidden in ("fov_radians", "XMMatrix", "XMVector", "std::tan", "std::atan"):
            self.assertNotIn(forbidden, SOURCE)

    def test_reset_byte_repeat_is_observational_not_effective_ngx_reset(self):
        history = SOURCE.split('Json history =', 1)[1].split('result["history"]', 1)[0]
        self.assertIn('"native_SL_reset_equals_zero"', history)
        self.assertIn('"repeated_source_fields_equal", nullptr', history)
        self.assertEqual(history.count('read.Read<uint8_t>(Address(context.view, 0xef0))'), 2)
        self.assertIn('history["producer_reset_candidate"] = value == 0', history)
        self.assertIn('"effective_ngx_reset", "not_established"', SOURCE)

    def test_frame_id_getter_route_is_metadata_without_virtual_call(self):
        route = SOURCE.split("Json FrameIdVirtualRoute(", 1)[1].split("Json CameraProvenance(", 1)[0]
        for evidence in ('"virtual_call_performed", false', '"returned_object", "not_observed"',
                         '"frame_id", "not_observed"', '"target_executable", "not_established"',
                         "Address(context.context, 0)", "Address(object, 0)", "Address(vtable, 0x20)",
                         "target < image || target > imageLast", '"target_rva"] = target - image'):
            self.assertIn(evidence, route)
        for forbidden in ("reinterpret_cast", "std::function", "GetProcAddress", "Address(object, 0x1a0)"):
            self.assertNotIn(forbidden, route)

    def test_explicit_frame_id_requires_exact_leaf_and_repeated_source(self):
        source = SOURCE.split("Json ExplicitFrameIdSource(", 1)[1].split("Json FrameIdVirtualRoute(", 1)[0]
        for evidence in ("GetterRva = 0x18ec810", "FrameIdObjectOffset = 0x1b0",
                         "0x48, 0x8d, 0x41, 0x10, 0xc3", "target != Address(image, GetterRva)",
                         "code != GetterBytes", "value == read.Read<uint32_t>(sourceAddress)",
                         '"CPU_source_for_later_explicit_Streamline_frame_ID"',
                         '"value_passed_to_streamline", "not_observed"', '"frame_token", "not_observed"'):
            self.assertIn(evidence, source)
        for forbidden in ("reinterpret_cast", "std::function", "GetProcAddress"):
            self.assertNotIn(forbidden, source)

    def test_project_contains_standalone_files(self):
        for project in ("OptiScaler.vcxproj", "OptiScaler.vcxproj.filters"):
            source = (ROOT / "OptiScaler" / project).read_text()
            for extension in ("h", "cpp"):
                self.assertEqual(source.count(f'Include="upscalers\\ffx\\FSRDCyberpunkEarlyGuides.{extension}"'), 1)

    def test_compiled_production_lookup_with_synthetic_memory(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to a host C++20 compiler for the actual production lookup test")
        # Compile the real source with only Win32 memory access mocked. All graph
        # walking, selection, bounds, settings and JSON code remain unchanged.
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <Windows.h>
static std::unordered_map<uintptr_t, unsigned char> memory;
static bool partial = false;
static uintptr_t mutateAfterRead = 0;
static uintptr_t eraseAfterRead = 0;
bool ReadProcessMemory(HANDLE, const void* source, void* destination, SIZE_T size, SIZE_T* actual)
{
    const auto address = reinterpret_cast<uintptr_t>(source);
    *actual = 0;
    for (SIZE_T i = 0; i < size; ++i)
    {
        const auto found = memory.find(address + i);
        if (found == memory.end()) return false;
        static_cast<unsigned char*>(destination)[i] = found->second;
        ++*actual;
    }
    if (partial && size) --*actual;
    if (mutateAfterRead == address)
    {
        memory[address] ^= 1;
        mutateAfterRead = 0;
    }
    if (eraseAfterRead == address)
    {
        memory.erase(address);
        eraseAfterRead = 0;
    }
    return true;
}
template<typename T> void put(uintptr_t address, T value)
{
    const auto data = reinterpret_cast<const unsigned char*>(&value);
    for (size_t i = 0; i < sizeof(T); ++i) memory[address + i] = data[i];
}
PRODUCTION_INCLUDE
using namespace FSRDCyberpunkEarlyGuides;
constexpr uintptr_t context = 0x10000, view = 0x20000, graph = 0x40000;
constexpr uintptr_t buckets = 0x800000, entries = 0x900000, image = 0x10000000;
constexpr uintptr_t registry = 0x20000000;
constexpr uintptr_t holders = graph + 0x514a18;
constexpr uint32_t keys[] = {0x63bcf380,0x64bcf513,0x65bcf6a6,0x61f178d4,0xdebf0c27,0x15eab19c};
void setup()
{
    memory.clear(); partial = false; mutateAfterRead = 0; eraseAfterRead = 0;
    put<uint8_t>(context + 0x30, 2); put<uint32_t>(context + 0x34, 3);
    put<uint8_t>(context + 0x38, 0); put<uintptr_t>(context + 0x18, view);
    put<uintptr_t>(context + 8, 0x30000); put<uintptr_t>(0x30000, graph);
    put<uint32_t>(graph + 0x40, 12);
    put<uint8_t>(graph + 2, 2); put<uint32_t>(graph + 0x514a10, 6);
    put<uint32_t>(view + 0x34, 1280); put<uint32_t>(view + 0x38, 720);
    put<uint64_t>(view + 0x17d0, 0);
    put<uint64_t>(view + 0x17d8, 0);
    put<uintptr_t>(view + 0x1d70, 0xc00000);
    put<uint32_t>(0xc00268, 0); put<uint32_t>(0xc00274, 0);
    put<float>(view + 0xb0, .1f); put<float>(view + 0xb4, 1000.f);
    put<float>(view + 0x90, 70.f); put<float>(view + 0x98, 1.777f);
    put<float>(view + 0x9c, 1.25f);
    put<std::array<float,2>>(view + 0xa0, {.03125f, -.0625f});
    put<std::array<int32_t,3>>(view + 0x70, {-131072, 262144, INT32_MIN});
    put<std::array<float,3>>(view + 0x2c0, {1.f, 2.f, 3.f});
    put<std::array<float,3>>(view + 0x2e0, {4.f, 5.f, 6.f});
    put<std::array<float,3>>(view + 0x2d0, {7.f, 8.f, 9.f});
    for (const auto offset : {0xc0u, 0x180u, 0x1c0u, 0x200u, 0x360u})
    {
        std::array<uint32_t,16> words;
        for (size_t i = 0; i < words.size(); ++i) words[i] = 0x3f000000u + offset * 16 + i;
        put(view + offset, words);
    }
    put<float>(view + 0x3e0, .25f); put<float>(view + 0x3e4, -.125f);
    put<uint32_t>(view + 0x3e8, 1280); put<uint32_t>(view + 0x3ec, 720);
    put<uint32_t>(view + 0x3f0, 17); put<uint8_t>(view + 0x3f4, 0x84);
    put<uint8_t>(view + 0xef0, 1);
    put<uintptr_t>(context, 0xe00000); put<uintptr_t>(0xe00000, 0xf00000);
    put<uintptr_t>(0xf00020, image + 0x123450); // Function target deliberately has no readable mock memory.
    put<TableHeader>(graph + 0x51b848, {buckets, 6, 13, entries, 0, 0x40});
    std::array<uint32_t,13> heads; heads.fill(UINT32_MAX);
    for (uint32_t i = 0; i < 6; ++i)
    {
        const auto key = keys[i] ^ (3u << 24), bucket = key % 13;
        put<std::array<uint32_t,3>>(entries + i * 0x40, {heads[bucket], key, key});
        heads[bucket] = i;
        put<uintptr_t>(entries + i * 0x40 + 0x20, holders + i * 0x58);
        put<std::array<uint64_t,3>>(holders + i * 0x58, {2, 30, 20});
        put<uintptr_t>(holders + i * 0x58 + 0x50, 0xb00000 + i * 0x100);
        put<std::array<uint64_t,2>>(0xb00000 + i * 0x100, {2, 30});
        put<uint8_t>(0xb00010 + i * 0x100, 1);
        put<uint32_t>(0xb00014 + i * 0x100, i + 100);
        put<uint8_t>(0xb0003c + i * 0x100, 0); put<uint8_t>(0xb00040 + i * 0x100, 1);
    }
    for (uint32_t i = 0; i < 13; ++i) put<uint32_t>(buckets + i * 4, heads[i]);
    put<int32_t>(image + NoVModeRva, -1);
    put<uintptr_t>(image + TextureRegistryRva, registry);
    for (uint32_t handle = 100; handle <= 105; ++handle)
    {
        put<int32_t>(registry + TextureRefOffset + (handle - 1) * TextureSlotStride, 1);
        // The native address is intentionally NOT readable in mock memory.
        put<uintptr_t>(registry + TextureNativeOffset + (handle - 1) * TextureSlotStride,
                       0x30000000 + handle * 0x1000);
        const auto nativeSlot = registry + TextureNativeOffset + (handle - 1) * TextureSlotStride;
        // Descriptor addresses are also deliberately not readable in mock memory.
        put<std::array<uintptr_t,2>>(nativeSlot + 0x30,
            {0x40000000 + handle * 0x1000, 0x50000000 + handle * 0x1000});
        put<uint32_t>(nativeSlot + 0x48, 0xe0);
        put<std::array<uint8_t,12>>(nativeSlot + 0x4e,
            {0x00,0x05,0xd0,0x02,0x01,0x00,0x10,0x19,0x05,0x00,0x00,0x00});
    }
}
Json describe() { return Json::parse(Describe(reinterpret_cast<void*>(context), image)); }
Json intervalOnly(uintptr_t selectedHolder = holders)
{
    Reader read;
    return LogicalInterval(read, ReadContext(read, context), selectedHolder, 0xb00000, 100);
}
void setupFrameId()
{
    setup();
    put<uintptr_t>(0xf00020, image + 0x18ec810);
    put<std::array<uint8_t,5>>(image + 0x18ec810, {0x48,0x8d,0x41,0x10,0xc3});
    put<uint32_t>(0xe001b0, 12345);
}
int main()
{
    setup(); auto result = describe();
    assert(!result.contains("read_failure"));
    for (int i = 0; i < 4; ++i) assert(result["inputs"][i]["handle"] == i + 100);
    for (int i = 0; i < 4; ++i)
    {
        const auto& mapping = result["inputs"][i]["texture_registry"];
        assert(mapping["status"] == "borrowed_address_observed");
        assert(mapping["borrowed_native_address"] == 0x30000000 + (i + 100) * 0x1000);
        assert(mapping["ref_status"] == 1 && mapping["ref_status_after"] == 1);
        assert(mapping["lifetime"] == "not_established");
        assert(mapping["gpu_initialized"] == "not_established");
        const auto& descriptors = mapping["descriptor_sources"];
        assert(descriptors["status"] == "cpu_descriptor_sources_observed");
        assert(descriptors["ordinary_cpu_srv_handle"] == 0x40000000 + (i + 100) * 0x1000);
        assert(descriptors["alternate_cpu_srv_handle"] == 0x50000000 + (i + 100) * 0x1000);
        assert(descriptors["requested_srv_state_mask"] == 0xe0);
        assert(descriptors["raw_array_size"] == 1 && descriptors["raw_dimension_mip_bits"] == 0x10);
        assert(descriptors["raw_format_sample_bits"] == 0x19 && descriptors["raw_flags_bits"] == 5);
        assert(descriptors["raw_compact_descriptor_bytes"].size() == 12);
        assert(descriptors["native_view_format"] == "not_observed");
        assert(descriptors["usable_srv"] == "not_established" && descriptors["resource_state"] == "not_observed");
        const auto& interval = result["inputs"][i]["logical_interval"];
        assert(interval["status"] == "compiler_interval_observed");
        assert(interval["holder_index"] == i && interval["holder_capacity"] == 320);
        assert(interval["inclusive_contains_position"] == true);
        assert(interval["holder_reservation_end"] == 30 && interval["holder_end_event_position"] == 20);
        assert(interval["end_event_relation"] == "before" && interval["repeated_metadata_equal"] == true);
        assert(interval["producer_completion"] == "not_established" && interval["alias_lifetime"] == "not_established");
    }
    assert(result["guide_settings"]["NoV_mode"] == -1);
    assert(result["guide_settings"]["extra_specular_enabled"] == 0);
    assert(result["guide_settings"]["extra_specular_scale"] == 0);
    assert(result["guide_settings"]["extra_specular_scale_bits"].get<uint32_t>() == 0);
    assert(result["graph_position"] == 11 && result["namespace"] == 3);
    assert(result["read_calls"].get<size_t>() < MaxReadCalls);
    assert(result["inputs"].size() == 8);
    // Closed reservation endpoints are inclusive, but the separately retained
    // end event can already precede the reservation end. Neither is GPU proof.
    for (const uint32_t position : {1, 2, 19, 20, 21, 30, 31})
    {
        put<uint32_t>(graph + 0x40, position + 1);
        const auto interval = intervalOnly();
        assert(interval["status"] == "compiler_interval_observed");
        assert(interval["inclusive_contains_position"] == (position >= 2 && position <= 30));
        assert(interval["end_event_relation"] == (position < 20 ? "before" : position == 20 ? "at" : "after"));
    }
    setup(); put<uint8_t>(context + 0x38, 3); put<uint32_t>(graph + 3*0x5e00 + 0x40, 12);
    put<std::array<uint64_t,3>>(holders, {0x30002, 0x3001e, 0x30014});
    auto interval = intervalOnly();
    assert(interval["current_position"] == 0x3000b && interval["inclusive_contains_position"] == true);
    // A physical record can be reused later; its last assigned range must not
    // replace the selected logical holder range in the coverage conclusion.
    setup(); put<std::array<uint64_t,2>>(0xb00000, {40, 50});
    interval = intervalOnly();
    assert(interval["record_range_begin"] == 40 && interval["inclusive_contains_position"] == true);
    for (const uint32_t count : {0, 321})
    {
        setup(); put<uint32_t>(graph + 0x514a10, count); interval = intervalOnly();
        assert(interval["status"] == "unavailable" && !interval.contains("inclusive_contains_position"));
    }
    setup();
    for (const uintptr_t holder : {holders - 1, holders + 1, holders + 6*0x58, holders + 320*0x58})
    {
        interval = intervalOnly(holder);
        assert(interval["reason"] == "holder outside current bounded graph arena");
        assert(!interval.contains("holder_first_use"));
    }
    for (const auto ranges : {std::array<uint64_t,3>{21,30,20}, {2,19,20},
                              {2,UINT64_MAX,20}, {2,0x01000000,20}})
    {
        setup(); put(holders, ranges); interval = intervalOnly();
        assert(interval["reason"] == "unsupported or unordered compiler interval");
        assert(!interval.contains("inclusive_contains_position"));
    }
    for (const auto address : {graph + 2, graph + 0x40, uintptr_t(0xb00010)})
    {
        setup(); put<uint8_t>(address, 0); interval = intervalOnly();
        assert(interval["reason"] == "executing operation and assigned record not established");
        assert(!interval.contains("inclusive_contains_position"));
    }
    for (const auto address : {holders, holders + 0x50, graph + 0x514a10, uintptr_t(0xb00000)})
    {
        setup(); mutateAfterRead = address; result = describe();
        const auto& changed = result["inputs"][0]["logical_interval"];
        assert(changed["repeated_metadata_equal"] == false);
        assert(!changed.contains("inclusive_contains_position"));
        assert(result["inputs"][0]["status"] == "handle_present"); // Still CPU handle metadata only.
    }
    setup(); memory.erase(holders + 23); interval = intervalOnly();
    assert(interval["status"] == "unavailable" && !interval.contains("inclusive_contains_position"));
    setup(); partial = true;
    Reader intervalRead;
    GraphContext intervalContext; intervalContext.context = context; intervalContext.graph = graph;
    interval = LogicalInterval(intervalRead, intervalContext, holders, 0xb00000, 100);
    assert(interval["status"] == "unavailable");
    partial = false; intervalRead.calls = MaxReadCalls;
    assert(LogicalInterval(intervalRead, intervalContext, holders, 0xb00000, 100)["reason"] ==
           "CPU metadata read budget exhausted");
    setup(); result = describe();
    assert(result["inputs"][5]["handle"] == 104 && result["inputs"][5]["streamline_tag"] == 0);
    assert(result["inputs"][6]["handle"] == 0 && result["inputs"][6]["status"] == "unavailable");
    assert(result["inputs"][7]["status"] == "not_enabled_by_current_view");
    assert(result["camera_provenance"]["jitter_y"]["producer_candidate"] == .125);
    assert(result["camera_provenance"]["history"]["producer_reset_candidate"] == false);
    assert(result["camera_provenance"]["history"]["source_byte"] == 1);
    assert(result["camera_provenance"]["history"]["repeated_source_fields_equal"] == true);
    assert(result["camera_provenance"]["history"]["semantics"] == "native_SL_reset_equals_zero");
    const auto camera = result["camera_provenance"];
    assert(camera["schema"] == "optiscaler.fsr_rr.early_camera_sources.v2");
    assert(camera["snapshot_atomic"] == false && camera["view_pointer_unchanged"] == true);
    assert(camera["bound_shared_cb12_match"] == "not_validated");
    assert(camera["jitter_free_projection"] == "not_captured_or_reconstructed");
    assert(camera["fov_degrees"]["producer_candidate"] == 70.f && !camera.contains("fov_radians"));
    assert(camera["projection_zoom"]["producer_candidate"] == 1.25f);
    assert(camera["authored_lens_offset"]["source_float_rows"][0][0] == .03125f);
    assert(camera["authored_lens_offset"]["source_float_rows"][0][1] == -.0625f);
    assert(camera["position"]["source_int32_xyz"][0] == -131072);
    assert(camera["position"]["source_uint32_words"][2] == 0x80000000u);
    assert(camera["position"]["producer_candidate"] == Json::array({-1.f, 2.f, -16384.f}));
    assert(camera["basis_right"]["source_float_rows"][0] == Json::array({1.f,2.f,3.f}));
    assert(camera["basis_up"]["source_float_rows"][0] == Json::array({4.f,5.f,6.f}));
    assert(camera["basis_forward"]["source_float_rows"][0] == Json::array({7.f,8.f,9.f}));
    assert(camera["native_jitter_width"]["source_value"] == 1280);
    assert(camera["native_jitter_height"]["source_value"] == 720);
    assert(camera["jitter_related_field"]["source_value"] == 17);
    assert(camera["projection_flags"]["source_value"] == 0x84);
    assert(camera["projection_flags"]["reverse_z_mask"] == 4);
    assert(camera["frame_id_virtual_route"]["status"] == "image_local_target_observed");
    assert(camera["frame_id_virtual_route"]["object_address"] == 0xe00000);
    assert(camera["frame_id_virtual_route"]["vtable_address"] == 0xf00000);
    assert(camera["frame_id_virtual_route"]["target_address"] == image + 0x123450);
    assert(camera["frame_id_virtual_route"]["target_rva"] == 0x123450);
    assert(camera["frame_id_virtual_route"]["virtual_call_performed"] == false);
    assert(camera["frame_id_virtual_route"]["returned_object"] == "not_observed");
    assert(camera["frame_id_virtual_route"]["explicit_frame_id_source"]["status"] == "unavailable");
    const auto& matrices = camera["matrices"];
    assert(matrices.size() == 5);
    for (const auto& matrix : matrices)
    {
        assert(matrix["status"] == "CPU_value_present" && matrix["all_finite"] == true);
        assert(matrix["repeated_source_words_equal"] == true && matrix["byte_size"] == 64);
        const auto offset = matrix["view_offset"].get<uint32_t>();
        assert(matrix["source_uint32_rows"].size() == 4);
        for (size_t row = 0; row < 4; ++row)
        {
            assert(matrix["source_uint32_rows"][row].size() == 4);
            for (size_t col = 0; col < 4; ++col)
                assert(matrix["source_uint32_rows"][row][col] == 0x3f000000u + offset*16 + row*4 + col);
        }
    }
    assert(matrices["native_view"]["view_offset"] == 0xc0);
    assert(matrices["inverse_native_view"]["view_offset"] == 0x180);
    assert(matrices["native_projection_jittered"]["view_offset"] == 0x200);
    assert(matrices["inverse_native_projection_jittered"]["view_offset"] == 0x1c0);
    assert(matrices["depth_converted_projection_jittered"]["view_offset"] == 0x360);
    put<uint8_t>(view + 0xef0, 0); put<uint32_t>(view + 0x3e4, 0);
    result = describe();
    assert(result["camera_provenance"]["history"]["producer_reset_candidate"] == true);
    assert(result["camera_provenance"]["history"]["repeated_source_fields_equal"] == true);
    assert(result["camera_provenance"]["jitter_y"]["producer_candidate_bits"] == 0x80000000u);
    put<uint32_t>(view + 0x90, 0x7fc01234u); result = describe();
    assert(result["camera_provenance"]["fov_degrees"]["source_bits"] == 0x7fc01234u);
    assert(result["camera_provenance"]["fov_degrees"]["producer_candidate"].is_null());
    memory.erase(view + 0xef0); memory.erase(view + 0xb0); result = describe();
    assert(!result.contains("read_failure"));
    assert(result["camera_provenance"]["history"]["status"] == "unavailable");
    assert(result["camera_provenance"]["history"]["repeated_source_fields_equal"].is_null());
    assert(result["camera_provenance"]["near_plane"]["status"] == "unavailable");
    setup(); mutateAfterRead = view + 0xef0; result = describe();
    assert(result["camera_provenance"]["history"]["status"] == "CPU_value_present");
    assert(result["camera_provenance"]["history"]["source_byte"] == 1);
    assert(result["camera_provenance"]["history"]["producer_reset_candidate"] == false);
    assert(result["camera_provenance"]["history"]["repeated_source_fields_equal"] == false);
    setup(); eraseAfterRead = view + 0xef0; result = describe();
    assert(result["camera_provenance"]["history"]["status"] == "unavailable");
    assert(result["camera_provenance"]["history"]["source_byte"] == 1);
    assert(result["camera_provenance"]["history"]["repeated_source_fields_equal"].is_null());
    setup(); put<uint8_t>(view + 0xef0, 255); result = describe();
    assert(result["camera_provenance"]["history"]["source_byte"] == 255);
    assert(result["camera_provenance"]["history"]["producer_reset_candidate"] == false);
    assert(result["camera_provenance"]["history"]["repeated_source_fields_equal"] == true);
    setup(); put<uint32_t>(view + 0x1c0, 0x7fc05678u);
    put<uint32_t>(view + 0x1c4, 0xff800000u); put<uint32_t>(view + 0x1c8, 0x80000000u);
    result = describe();
    const auto raw = result["camera_provenance"]["matrices"]["inverse_native_projection_jittered"];
    assert(raw["all_finite"] == false && raw["status"] == "CPU_value_present");
    assert(raw["source_uint32_rows"][0][0] == 0x7fc05678u);
    assert(raw["source_uint32_rows"][0][1] == 0xff800000u);
    assert(raw["source_uint32_rows"][0][2] == 0x80000000u);
    assert(raw["source_float_rows"][0][0].is_null() && raw["source_float_rows"][0][1].is_null());
    assert(raw["source_float_rows"][0][2] == 0.f); // Sign remains exact in uint32 rows.
    setup(); mutateAfterRead = view + 0x180; result = describe();
    assert(result["camera_provenance"]["matrices"]["inverse_native_view"]["repeated_source_words_equal"] == false);
    assert(result["camera_provenance"]["matrices"]["inverse_native_view"]["source_uint32_rows"][0][0] ==
           0x3f000000u + 0x180*16); // First raw snapshot is retained without repair.
    setup(); memory.erase(view + 0x200 + 63); result = describe();
    assert(!result.contains("read_failure"));
    assert(result["camera_provenance"]["matrices"]["native_projection_jittered"]["status"] == "unavailable");
    assert(!result["camera_provenance"]["matrices"]["native_projection_jittered"].contains("source_uint32_rows"));
    assert(result["camera_provenance"]["matrices"]["inverse_native_view"]["status"] == "CPU_value_present");
    setup(); memory.erase(view + 0x70); memory.erase(view + 0x3e8); result = describe();
    assert(result["camera_provenance"]["position"]["status"] == "unavailable");
    assert(result["camera_provenance"]["native_jitter_width"]["status"] == "unavailable");
    setup(); mutateAfterRead = context + 0x18; result = describe();
    assert(result["camera_provenance"]["view_pointer_unchanged"] == false);
    setup(); partial = true;
    Reader cameraRead;
    const auto partialRows = FloatRowsField<4,4>(cameraRead, view, 0xc0);
    assert(partialRows["status"] == "unavailable" && !partialRows.contains("source_uint32_rows"));
    partial = false;
    const auto overflowRows = FloatRowsField<4,4>(cameraRead, UINTPTR_MAX - 0xc0, 0xc0);
    assert(overflowRows["reason"] == "invalid read range");
    cameraRead.calls = MaxReadCalls;
    assert((FloatRowsField<4,4>(cameraRead, view, 0xc0)["status"] == "unavailable"));
    for (const auto invalidTarget : {uintptr_t(0), image - 1, image + ImageBytes, UINTPTR_MAX})
    {
        setup(); put<uintptr_t>(0xf00020, invalidTarget); result = describe();
        const auto& route = result["camera_provenance"]["frame_id_virtual_route"];
        assert(route["status"] == "unavailable" && !route.contains("target_address") && !route.contains("target_rva"));
        assert(result["camera_provenance"]["frame_token"] == "not_observed");
    }
    for (const auto validTarget : {image, image + ImageBytes - 1})
    {
        setup(); put<uintptr_t>(0xf00020, validTarget); result = describe();
        assert(result["camera_provenance"]["frame_id_virtual_route"]["target_rva"] == validTarget - image);
        assert(result["camera_provenance"]["frame_id_virtual_route"]["target_executable"] == "not_established");
    }
    for (const auto unavailableAddress : {context, uintptr_t(0xe00000), uintptr_t(0xf00020)})
    {
        setup(); memory.erase(unavailableAddress); result = describe();
        assert(!result.contains("read_failure"));
        assert(result["camera_provenance"]["frame_id_virtual_route"]["status"] == "unavailable");
    }
    setup(); put<uintptr_t>(context, 0); result = describe();
    assert(result["camera_provenance"]["frame_id_virtual_route"]["status"] == "unavailable");
    setup(); put<uintptr_t>(0xe00000, UINTPTR_MAX); result = describe();
    assert(result["camera_provenance"]["frame_id_virtual_route"]["reason"] == "null or overflowing address");
    for (const uint32_t value : {0u, 1u, 12345u, uint32_t(INT32_MAX), UINT32_MAX})
    {
        setupFrameId(); put<uint32_t>(0xe001b0, value); result = describe();
        const auto& frame = result["camera_provenance"]["frame_id_virtual_route"]["explicit_frame_id_source"];
        assert(frame["status"] == "CPU_value_present" && frame["source_value"] == value);
        assert(frame["source_object_offset"] == 0x1b0 && frame["source_address"] == 0xe001b0);
        assert(frame["repeated_source_fields_equal"] == true && frame["virtual_call_performed"] == false);
        assert(frame["value_passed_to_streamline"] == "not_observed" && frame["frame_token"] == "not_observed");
        assert(result["camera_provenance"]["streamline_producer_observed"] == false);
    }
    for (unsigned offset = 0; offset < 5; ++offset)
    {
        setupFrameId(); memory[image + 0x18ec810 + offset] ^= 1; result = describe();
        const auto& frame = result["camera_provenance"]["frame_id_virtual_route"]["explicit_frame_id_source"];
        assert(frame["reason"] == "frame-ID getter bytes differ from authenticated leaf");
        assert(!frame.contains("source_value"));
    }
    for (const auto address : {context, uintptr_t(0xe00000), uintptr_t(0xf00020),
                               uintptr_t(0xe001b0), image + 0x18ec810})
    {
        setupFrameId(); mutateAfterRead = address; result = describe();
        const auto& frame = result["camera_provenance"]["frame_id_virtual_route"]["explicit_frame_id_source"];
        assert(frame["repeated_source_fields_equal"] == false && !frame.contains("source_value"));
    }
    for (const auto address : {image + 0x18ec814, uintptr_t(0xe001b3)})
    {
        setupFrameId(); memory.erase(address); result = describe();
        const auto& frame = result["camera_provenance"]["frame_id_virtual_route"]["explicit_frame_id_source"];
        assert(frame["status"] == "unavailable" && !frame.contains("source_value"));
    }
    setupFrameId(); put<uintptr_t>(0xf00020, image + 0x18ec811); result = describe();
    assert(result["camera_provenance"]["frame_id_virtual_route"]["explicit_frame_id_source"]["reason"] ==
           "frame-ID getter route not authenticated for scalar read");
    Reader frameRead; GraphContext frameContext; frameContext.context = context;
    setupFrameId();
    assert(ExplicitFrameIdSource(frameRead, frameContext, image, UINTPTR_MAX - 0x100,
                                0xf00000, image + 0x18ec810)["reason"] == "null or overflowing address");
    partial = true;
    assert(ExplicitFrameIdSource(frameRead, frameContext, image, 0xe00000,
                                0xf00000, image + 0x18ec810)["status"] == "unavailable");
    partial = false; frameRead.calls = MaxReadCalls;
    assert(ExplicitFrameIdSource(frameRead, frameContext, image, 0xe00000,
                                0xf00000, image + 0x18ec810)["reason"] == "CPU metadata read budget exhausted");
    setup(); put<uint64_t>(view + 0x17d8, uint64_t(1) << (0x5a & 63));
    result = describe(); assert(result["inputs"][6]["handle"] == 105);
    put<uint64_t>(view + 0x17d0, uint64_t(1) << 0x37);
    put<uint32_t>(0xc00268, 901); result = describe();
    assert(result["inputs"][6]["handle"] == 901);
    assert(result["inputs"][6]["selected_source"] == "current_view_owner_0x268");
    put<uint32_t>(0xc00268, 0xffffffffu); result = describe();
    assert(result["inputs"][6]["handle"] == 0xffffffffu);
    assert(result["inputs"][6]["status"] == "unavailable"); // Must not use valid fallback.
    put<uint32_t>(0xc00268, 0); result = describe();
    assert(result["inputs"][6]["handle"] == 105);
    memory.erase(0xc00268); result = describe();
    assert(!result["inputs"][6].contains("handle")); // Unknown override is not zero.
    put<uint32_t>(0xc00268, 902); memory.erase(view + 0x17d8); result = describe();
    assert(result["inputs"][6]["handle"] == 902); // Nonzero override can prove final source.
    assert(!result["inputs"][6].contains("feature_0x5a"));
    put<uint32_t>(0xc00268, 0); result = describe();
    assert(!result["inputs"][6].contains("handle"));
    setup(); put<uint64_t>(view + 0x17d8, uint64_t(1) << (0x46 & 63));
    put<uint32_t>(0xc00274, 903); result = describe();
    assert(result["inputs"][7]["handle"] == 903 && result["inputs"][7]["streamline_tag"] == 42);
    put<uint32_t>(0xc00274, 0x80000001u); result = describe();
    assert(result["inputs"][7]["status"] == "unavailable");
    memory.erase(view + 0x17d8); result = describe();
    assert(result["inputs"][7]["status"] == "unavailable");
    assert(!result["inputs"][7].contains("feature_0x46"));
    setup();
    put<uint64_t>(view + 0x17d0, uint64_t(1) << 0x35);
    put<uintptr_t>(view + 0x1d70, 0xc00000); put<uint32_t>(0xc002e8, 900);
    put<uint8_t>(image + ExtraSpecularEnableRva, 1);
    put<float>(image + ExtraSpecularScaleRva, .75f);
    result = describe(); assert(result["inputs"][4]["handle"] == 900);
    assert(result["guide_settings"]["extra_specular_scale"] == .75);
    assert(result["guide_settings"]["extra_specular_scale_bits"].get<uint32_t>() == 0x3f400000u);
    setup(); put<uintptr_t>(entries + 0x20, 0);
    put<uintptr_t>(entries + 0x28, 0xd00000); put<uint32_t>(entries + 0x34, 3);
    for (unsigned i = 0; i < 3; ++i) put<uintptr_t>(0xd00000 + i * 24, holders + i * 0x58);
    put<uint64_t>(0xd00008, 0); put<uint64_t>(0xd00020, 10); put<uint64_t>(0xd00038, 99);
    result = describe(); assert(result["inputs"][0]["handle"] == 101);
    assert(result["inputs"][0]["selected_version_index"] == 1);
    put<uint32_t>(graph + 0x40, 11); result = describe();
    assert(result["inputs"][0]["handle"] == 100); // Equality selects PREVIOUS version.
    put<uint32_t>(graph + 0x40, 101); result = describe();
    assert(result["inputs"][0]["status"] == "unavailable");
    put<uint32_t>(entries + 0x34, 0); result = describe();
    assert(result["inputs"][0]["reason"] == "empty or unsupported version array");
    setup(); auto key = keys[0] ^ (3u << 24);
    put<uint32_t>(buckets + (key % 13) * 4, 0);
    put<std::array<uint32_t,3>>(entries, {0, 123, 123}); result = describe();
    assert(result["inputs"][0]["reason"] == "graph hash chain bound or cycle");
    setup(); put<uintptr_t>(entries + 0x20, 0); // Missing version metadata must be caught.
    result = describe(); assert(result["inputs"][0]["status"] == "unavailable");
    setup(); put<uintptr_t>(holders + 0x50, 0); result = describe();
    assert(result["inputs"][0]["reason"] == "selected resource record unavailable");
    setup(); put<uint8_t>(context + 0x30, 3); result = describe();
    assert(result["namespace"] == 0 && result["inputs"][0]["status"] == "unavailable");
    setup(); put<uint32_t>(graph + 0x40, 65537); result = describe();
    assert(result["read_failure"] == "graph version position exceeds supported range");
    setup(); partial = true; result = describe(); assert(result.contains("read_failure"));
    setup(); memory.erase(view + 0x17d0); result = describe();
    assert(result["guide_settings"]["extra_specular"] == "current view feature unavailable");
    assert(!result["guide_settings"].contains("extra_specular_enabled"));
    assert(!result["guide_settings"].contains("extra_specular_scale_bits"));
    setup(); const auto refAddress = registry + TextureRefOffset + 99 * TextureSlotStride;
    const auto nativeAddress = registry + TextureNativeOffset + 99 * TextureSlotStride;
    for (const uint8_t format : {0x18, 0x19, 0xff})
    {
        setup(); put<uint8_t>(nativeAddress + 0x55, format); result = describe();
        const auto& descriptors = result["inputs"][0]["texture_registry"]["descriptor_sources"];
        assert(descriptors["raw_format_sample_bits"] == format);
        assert(descriptors["native_view_format"] == "not_observed");
    }
    setup(); put<uint16_t>(nativeAddress + 0x52, 0xabcd);
    put<uint8_t>(nativeAddress + 0x54, 0xff); put<uint32_t>(nativeAddress + 0x56, 0x80000005u);
    put<std::array<uintptr_t,2>>(nativeAddress + 0x30, {0, UINTPTR_MAX}); result = describe();
    auto descriptors = result["inputs"][0]["texture_registry"]["descriptor_sources"];
    assert(descriptors["status"] == "cpu_descriptor_sources_observed");
    assert(descriptors["ordinary_cpu_srv_handle"] == 0 && descriptors["alternate_cpu_srv_handle"] == UINTPTR_MAX);
    assert(descriptors["raw_array_size"] == 0xabcd && descriptors["raw_dimension_mip_bits"] == 0xff);
    assert(descriptors["raw_flags_bits"] == 0x80000005u);
    assert(descriptors["usable_srv"] == "not_established");
    for (const auto offset : {0x30u, 0x48u, 0x4eu})
    {
        setup(); mutateAfterRead = nativeAddress + offset; result = describe();
        const auto& mapping = result["inputs"][0]["texture_registry"];
        assert(mapping["status"] == "borrowed_address_observed");
        assert(mapping["descriptor_sources"]["status"] == "unavailable");
        assert(mapping["descriptor_sources"]["repeated_source_fields_equal"] == false);
        assert(!mapping["descriptor_sources"].contains("ordinary_cpu_srv_handle"));
    }
    setup(); memory.erase(nativeAddress + 0x4e + 11); result = describe();
    assert(result["inputs"][0]["texture_registry"]["status"] == "borrowed_address_observed");
    assert(result["inputs"][0]["texture_registry"]["descriptor_sources"]["status"] == "unavailable");
    Reader descriptorRead;
    assert(DescriptorSources(descriptorRead, UINTPTR_MAX - 0x30)["status"] == "unavailable");
    descriptorRead.calls = MaxReadCalls;
    assert(DescriptorSources(descriptorRead, nativeAddress)["reason"] == "CPU metadata read budget exhausted");
    setup(); partial = true; descriptorRead.calls = 0;
    assert(DescriptorSources(descriptorRead, nativeAddress)["status"] == "unavailable");
    setup();
    for (const auto status : {0, -1, INT32_MIN})
    {
        put<int32_t>(refAddress, status); result = describe();
        const auto& mapping = result["inputs"][0]["texture_registry"];
        assert(mapping["status"] == "unavailable" && mapping["ref_status"] == status);
        assert(!mapping.contains("borrowed_native_address"));
    }
    setup(); memory.erase(refAddress); result = describe();
    assert(!result["inputs"][0]["texture_registry"].contains("ref_status"));
    setup(); memory.erase(nativeAddress); result = describe();
    assert(!result["inputs"][0]["texture_registry"].contains("borrowed_native_address"));
    setup(); put<uintptr_t>(nativeAddress, 0); result = describe();
    assert(result["inputs"][0]["texture_registry"]["reason"] == "native resource address unavailable");
    setup(); mutateAfterRead = refAddress; result = describe();
    assert(result["inputs"][0]["texture_registry"]["reason"] == "texture registry changed during metadata reads");
    setup(); mutateAfterRead = nativeAddress; result = describe();
    assert(result["inputs"][0]["texture_registry"]["status"] == "unavailable");
    setup(); mutateAfterRead = image + TextureRegistryRva; result = describe();
    assert(result["inputs"][0]["texture_registry"]["status"] == "unavailable");
    for (const auto handle : {0u, TextureSlotCount + 1, uint32_t(INT32_MAX), UINT32_MAX})
    {
        Reader bounded;
        const auto mapping = RegistryMapping(bounded, image, handle);
        assert(mapping["status"] == "unavailable" && bounded.calls == 0);
    }
    for (const auto handle : {1u, TextureSlotCount})
    {
        setup();
        put<int32_t>(registry + TextureRefOffset + (handle - 1) * TextureSlotStride, 1);
        put<uintptr_t>(registry + TextureNativeOffset + (handle - 1) * TextureSlotStride, 0x12345678);
        Reader bounded; const auto mapping = RegistryMapping(bounded, image, handle);
        assert(mapping["status"] == "borrowed_address_observed");
        assert(mapping["slot_index"] == handle - 1 && bounded.calls == 7);
        assert(mapping["descriptor_sources"]["status"] == "unavailable"); // Optional fields absent.
    }
    setup(); put<uintptr_t>(image + TextureRegistryRva, 0); result = describe();
    assert(result["inputs"][0]["texture_registry"]["status"] == "unavailable");
    setup(); put<uintptr_t>(image + TextureRegistryRva, UINTPTR_MAX); result = describe();
    assert(result["inputs"][0]["texture_registry"]["reason"] == "null or overflowing address");
    Reader read; read.calls = MaxReadCalls;
    try { read.Read<uint8_t>(context + 0x30); assert(false); } catch (const std::exception&) {}
    read.calls = 0; read.bytes = MaxReadBytes - 3;
    try { read.Read<uint32_t>(context + 0x30); assert(false); } catch (const std::exception&) {}
    try { Address(UINTPTR_MAX, 1); assert(false); } catch (const std::exception&) {}
    assert(Json::parse(Describe(nullptr, image)).contains("read_failure"));
}
'''.replace("PRODUCTION_INCLUDE", '#include "' + str(CPP) + '"')
        with tempfile.TemporaryDirectory(prefix="fsrd-early-guides-") as directory:
            directory = Path(directory)
            (directory / "pch.h").write_text("")
            (directory / "Windows.h").write_text(
                "#pragma once\n#include <cstddef>\nusing SIZE_T=std::size_t; using HANDLE=void*;\n"
                "inline HANDLE GetCurrentProcess(){return nullptr;}\n"
                "bool ReadProcessMemory(HANDLE,const void*,void*,SIZE_T,SIZE_T*);\n")
            test = directory / "test.cpp"
            test.write_text(harness)
            binary = directory / "test"
            subprocess.run([compiler, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(directory), "-I" + str(ROOT / "external/nlohmann"),
                            str(test), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()

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
    def test_observation_is_only_in_accepted_authenticated_capture(self):
        probe = (CPP.parent / "FSRDCyberpunkFogProbe.cpp").read_text()
        self.assertEqual(probe.count("FSRDCyberpunkEarlyGuides::Describe("), 1)
        capture = probe.split("std::shared_ptr<CapturePlan> PrepareCapture(", 1)[1].split("void PublishFogEndpoint(", 1)[0]
        self.assertLess(capture.index("!FSRDFogLayerCapture::WantsCapture()"),
                        capture.index("FSRDCyberpunkEarlyGuides::Describe("))
        self.assertIn("Describe(s.context, uintptr_t(GetModuleHandleW(nullptr)))", capture)
        self.assertLess(capture.index('plan->provenance["endpoint_origin"]'),
                        capture.index('plan->provenance["early_guide_availability"]'))

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

    def test_observational_camera_and_exact_motion_precedence(self):
        for evidence in ('"streamline_producer_observed", false', '"frame_token", "not_observed"',
                         '"matrix_payload", "not_captured"', '"effective_ngx_reset", "not_established"',
                         '"final_ngx_constants", "not_established"', "0x3e4, 0x80000000u",
                         "value == 0", "if (overrideHandle)", "SetHandle(result, overrideHandle)",
                         "features.Test(0x5a)", "features.Test(0x37)", "features.Test(0x46)"):
            self.assertIn(evidence, SOURCE)
        self.assertNotIn("overrideHandle <= INT32_MAX", SOURCE)
        self.assertIn("bit / 64", SOURCE)
        self.assertIn("bit & 63", SOURCE)

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
constexpr uint32_t keys[] = {0x63bcf380,0x64bcf513,0x65bcf6a6,0x61f178d4,0xdebf0c27,0x15eab19c};
void setup()
{
    memory.clear(); partial = false; mutateAfterRead = 0;
    put<uint8_t>(context + 0x30, 2); put<uint32_t>(context + 0x34, 3);
    put<uint8_t>(context + 0x38, 0); put<uintptr_t>(context + 0x18, view);
    put<uintptr_t>(context + 8, 0x30000); put<uintptr_t>(0x30000, graph);
    put<uint32_t>(graph + 0x40, 12);
    put<uint32_t>(view + 0x34, 1280); put<uint32_t>(view + 0x38, 720);
    put<uint64_t>(view + 0x17d0, 0);
    put<uint64_t>(view + 0x17d8, 0);
    put<uintptr_t>(view + 0x1d70, 0xc00000);
    put<uint32_t>(0xc00268, 0); put<uint32_t>(0xc00274, 0);
    put<float>(view + 0xb0, .1f); put<float>(view + 0xb4, 1000.f);
    put<float>(view + 0x90, 1.2f); put<float>(view + 0x98, 1.777f);
    put<float>(view + 0x3e0, .25f); put<float>(view + 0x3e4, -.125f);
    put<uint8_t>(view + 0xef0, 1);
    put<TableHeader>(graph + 0x51b848, {buckets, 6, 13, entries, 0, 0x40});
    std::array<uint32_t,13> heads; heads.fill(UINT32_MAX);
    for (uint32_t i = 0; i < 6; ++i)
    {
        const auto key = keys[i] ^ (3u << 24), bucket = key % 13;
        put<std::array<uint32_t,3>>(entries + i * 0x40, {heads[bucket], key, key});
        heads[bucket] = i;
        put<uintptr_t>(entries + i * 0x40 + 0x20, 0xa00000 + i * 0x100);
        put<uintptr_t>(0xa00050 + i * 0x100, 0xb00000 + i * 0x100);
        put<uint32_t>(0xb00014 + i * 0x100, i + 100);
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
    }
}
Json describe() { return Json::parse(Describe(reinterpret_cast<void*>(context), image)); }
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
    }
    assert(result["guide_settings"]["NoV_mode"] == -1);
    assert(result["guide_settings"]["extra_specular_enabled"] == 0);
    assert(result["graph_position"] == 11 && result["namespace"] == 3);
    assert(result["read_calls"].get<size_t>() < MaxReadCalls);
    assert(result["inputs"].size() == 8);
    assert(result["inputs"][5]["handle"] == 104 && result["inputs"][5]["streamline_tag"] == 0);
    assert(result["inputs"][6]["handle"] == 0 && result["inputs"][6]["status"] == "unavailable");
    assert(result["inputs"][7]["status"] == "not_enabled_by_current_view");
    assert(result["camera_provenance"]["jitter_y"]["producer_candidate"] == .125);
    assert(result["camera_provenance"]["history"]["producer_reset_candidate"] == false);
    put<uint8_t>(view + 0xef0, 0); put<uint32_t>(view + 0x3e4, 0);
    result = describe();
    assert(result["camera_provenance"]["history"]["producer_reset_candidate"] == true);
    assert(result["camera_provenance"]["jitter_y"]["producer_candidate_bits"] == 0x80000000u);
    put<uint32_t>(view + 0x90, 0x7fc01234u); result = describe();
    assert(result["camera_provenance"]["fov_radians"]["source_bits"] == 0x7fc01234u);
    assert(result["camera_provenance"]["fov_radians"]["producer_candidate"].is_null());
    memory.erase(view + 0xef0); memory.erase(view + 0xb0); result = describe();
    assert(!result.contains("read_failure"));
    assert(result["camera_provenance"]["history"]["status"] == "unavailable");
    assert(result["camera_provenance"]["near_plane"]["status"] == "unavailable");
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
    setup(); put<uintptr_t>(entries + 0x20, 0);
    put<uintptr_t>(entries + 0x28, 0xd00000); put<uint32_t>(entries + 0x34, 3);
    for (unsigned i = 0; i < 3; ++i) put<uintptr_t>(0xd00000 + i * 24, 0xa00000 + i * 0x100);
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
    setup(); put<uintptr_t>(0xa00050, 0); result = describe();
    assert(result["inputs"][0]["reason"] == "selected resource record unavailable");
    setup(); put<uint8_t>(context + 0x30, 3); result = describe();
    assert(result["namespace"] == 0 && result["inputs"][0]["status"] == "unavailable");
    setup(); put<uint32_t>(graph + 0x40, 65537); result = describe();
    assert(result["read_failure"] == "graph version position exceeds supported range");
    setup(); partial = true; result = describe(); assert(result.contains("read_failure"));
    setup(); memory.erase(view + 0x17d0); result = describe();
    assert(result["guide_settings"]["extra_specular"] == "current view feature unavailable");
    assert(!result["guide_settings"].contains("extra_specular_enabled"));
    setup(); const auto refAddress = registry + TextureRefOffset + 99 * TextureSlotStride;
    const auto nativeAddress = registry + TextureNativeOffset + 99 * TextureSlotStride;
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
        assert(mapping["slot_index"] == handle - 1 && bounded.calls == 6);
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

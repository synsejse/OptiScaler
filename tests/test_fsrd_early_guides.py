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
                         "0x1d70", "0x2e8", "0x17d0", ">> 0x35"):
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
constexpr uint32_t keys[] = {0x63bcf380,0x64bcf513,0x65bcf6a6,0x61f178d4};
void setup()
{
    memory.clear(); partial = false;
    put<uint8_t>(context + 0x30, 2); put<uint32_t>(context + 0x34, 3);
    put<uint8_t>(context + 0x38, 0); put<uintptr_t>(context + 0x18, view);
    put<uintptr_t>(context + 8, 0x30000); put<uintptr_t>(0x30000, graph);
    put<uint32_t>(graph + 0x40, 12);
    put<uint32_t>(view + 0x34, 1280); put<uint32_t>(view + 0x38, 720);
    put<uint64_t>(view + 0x17d0, 0);
    put<TableHeader>(graph + 0x51b848, {buckets, 4, 13, entries, 0, 0x40});
    std::array<uint32_t,13> heads; heads.fill(UINT32_MAX);
    for (uint32_t i = 0; i < 4; ++i)
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
}
Json describe() { return Json::parse(Describe(reinterpret_cast<void*>(context), image)); }
int main()
{
    setup(); auto result = describe();
    assert(!result.contains("read_failure"));
    for (int i = 0; i < 4; ++i) assert(result["inputs"][i]["handle"] == i + 100);
    assert(result["guide_settings"]["NoV_mode"] == -1);
    assert(result["guide_settings"]["extra_specular_enabled"] == 0);
    assert(result["graph_position"] == 11 && result["namespace"] == 3);
    assert(result["read_calls"].get<size_t>() < MaxReadCalls);
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

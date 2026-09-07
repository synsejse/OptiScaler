"""Exact lighting-route, current binding and readonly depth admission tests."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkFogProbe.cpp"
SOURCE = CPP.read_text()
EXE = Path("/home/synse/Games/Heroic/Games/Cyberpunk 2077/bin/x64/Cyberpunk2077.exe")


def function(name, full=False):
    marker = SOURCE.index(name + "(")
    start = SOURCE.rfind("\n", 0, marker) + 1
    opening = SOURCE.index("{", marker)
    depth, end = 1, opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start if full else opening:end]


class LightingGuides(unittest.TestCase):
    def test_full_body_manifest_matches_authenticated_executable(self):
        if not EXE.is_file():
            self.skipTest("Authenticated local executable unavailable")
        data = EXE.read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         "a7de82945c03e041fc7339fcf9066224d98db2f5d80fea50f7947bb350a60991")
        nt = struct.unpack_from("<I", data, 0x3c)[0]
        section_offset = nt + 24 + struct.unpack_from("<H", data, nt + 20)[0]
        sections = [struct.unpack_from("<IIII", data, section_offset + i * 40 + 8)
                    for i in range(struct.unpack_from("<H", data, nt + 6)[0])]
        manifest = SOURCE.split("LightingCode[] = {", 1)[1].split("};", 1)[0]
        ranges = re.findall(r'\{ (0x[0-9a-f]+), (0x[0-9a-f]+), "([0-9a-f]{64})" \}', manifest)
        self.assertEqual(len(ranges), 9)
        for start_text, size_text, expected in ranges:
            start, size = int(start_text, 16), int(size_text, 16)
            with self.subTest(rva=start_text):
                payload = None
                for _, va, raw_size, raw in sections:
                    if va <= start and start + size <= va + raw_size:
                        payload = data[raw + start - va:raw + start - va + size]
                self.assertIsNotNone(payload)
                self.assertEqual(hashlib.sha256(payload).hexdigest(), expected)

    def test_original_scopes_and_each_native_draw_run_once(self):
        node, helper, draw = function("HookLightingNode"), function("HookFullscreenHelper"), function("HookDraw")
        self.assertEqual(node.count("originalLightingNode("), 1)
        self.assertEqual(helper.count("originalFullscreenHelper("), 1)
        self.assertIn("lightingScope = previous", node)
        self.assertIn("current->finalHelper = previous", helper)
        self.assertIn("caller == authenticatedImage.load() + FinalLightingHelperReturnRva", helper)
        self.assertIn("uintptr_t(_ReturnAddress())", helper)
        self.assertIn("uintptr_t(_ReturnAddress())", draw)
        self.assertEqual(draw.count("originalDraw("), 1)
        self.assertLess(draw.index("PrepareLightingCapture("), draw.index("originalDraw("))
        self.assertLess(draw.index("originalDraw("), draw.index("FinishLightingCapture("))
        init = function("Initialize")
        self.assertLess(init.index("std::begin(LightingCode)"), init.index("HookLightingNode"))
        self.assertLess(init.index("std::begin(LightingCode)"), init.index("HookFullscreenHelper"))

    def test_separate_one_shot_request_and_bounded_no_route_timeout(self):
        poll = function("PollRearm")
        for guard in ('L"FSRRR-lighting-guides.request"', "RequestEarlyGuides()", "CancelEarlyGuideRequest()",
                      "GetTickCount64() - lightingRequestedAt.load() >= 10000", "lightingAttempted.store(true)"):
            self.assertIn(guard, poll)
        draw = function("HookDraw")
        for guard in ("lightingRequested.load()", "WantsEarlyGuideCapture()", "MatchesFinalLightingDraw(",
                      "!lightingAttempted.exchange(true)"):
            self.assertLess(draw.index(guard), draw.index("PrepareLightingCapture("))
        lighting = draw.split("std::shared_ptr<CapturePlan> plan;", 1)[0]
        self.assertNotIn("FSRDFogLayerCapture::CancelRequest()", lighting)
        self.assertNotIn("ProducerMatches", function("PrepareLightingCapture"))
        self.assertNotIn("earlyProducer", function("PrepareLightingCapture"))

    def test_owned_current_use_and_table_type_precede_resource_addref(self):
        prepare = function("PrepareLightingCapture")
        self.assertLess(prepare.index("ObserveLightingBindings("), prepare.index("reinterpret_cast<ID3D12Resource*>"))
        for guard in ("SameEarlyReservation(", "descriptor_sources", "MaxOwnedInputBytes - ownedBytes",
                      "target.known", "target.resource", "LightingDepthAlias(", "readonly_dsv"):
            self.assertIn(guard, prepare)
        bindings = function("ObserveLightingBindings")
        for guard in ("rangeIndex >= 64", "resourceMask", "samplerMask", "dirty70 | dirty78",
                      "reg - first >= count", "index >= 65536", "range[13] == 2", "range[14] >= 64",
                      "alternate_cpu_srv_handle", "ordinary_cpu_srv_handle"):
            self.assertIn(guard, bindings)
        self.assertIn("not an inferred engine allocation", bindings)

    def test_compute_only_preserves_two_mrts_and_readonly_dsv(self):
        finish = function("FinishLightingCapture")
        for forbidden in ("originalDraw(", "originalSetRtv(", "OMSetRenderTargets", "CopyMain", "CreateRenderTargetView"):
            self.assertNotIn(forbidden, finish)
        self.assertIn("input.preserveReadOnlyDepth = true", finish)
        self.assertLess(finish.index("FSRDSubmission::Retain("), finish.index("RecordPrivateCompute("))
        self.assertLess(finish.index("RecordPrivateCompute("), finish.index("RecordEarlyGuides("))
        self.assertIn("PrivateRecordedRestored", finish)
        host = SOURCE.split("struct LightingEngineHost", 1)[1].split("PrepareLightingGuideWork", 1)[0]
        for guard in ("found->second.generation != plan.drawState.generation", "found->second.predicated",
                      "found->second.renderPass", "found->second.queryCount", "LightingDepthAlias(plan, inputs[3])"):
            self.assertIn(guard, host)
        work = function("PrepareLightingGuideWork")
        self.assertIn("PreTransparencySurface", work)
        self.assertIn('settings.at("extra_specular_enabled").get<unsigned>() != 0', work)
        self.assertNotIn("CreateShaderResourceView", work)

    def test_post_private_fatal_outcome_is_not_a_catchable_refusal(self):
        finish = function("FinishLightingCapture")
        fatal = finish.split("Outcome::ScopeLostAfterPrivate", 1)[1].split('plan->provenance["engine_state_requests"]', 1)[0]
        self.assertLess(fatal.index("earlyFatalRecording.store(true)"), fatal.index("LOG_ERROR"))
        self.assertNotIn("provenance[", fatal)
        self.assertIn("lightingRequested.load()", fatal)
        self.assertIn("input.image == authenticatedImage.load()", fatal)
        self.assertIn("input.image == uintptr_t(GetModuleHandleW(nullptr))", fatal)
        self.assertIn("TerminateProcess(GetCurrentProcess(), 0xf51d0001u)", fatal)
        self.assertEqual(finish.count("TerminateProcess("), 1)

    def test_compiled_actual_binding_cache_readonly_alias_and_exact_route(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to compile actual lighting admission helpers")
        definitions = "\n".join(function(name, True) for name in
                                 ("ObserveLightingBindings", "LightingDepthAlias", "MatchesFinalLightingDraw"))
        harness = r'''
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <cassert>
#include <stdexcept>
#include <json.hpp>
using Json=nlohmann::json;using UINT=unsigned;
constexpr uintptr_t Image=0x140000000,Slots=0x1000,Tls=0x2000,Engine=0x3000,Cache=0x4000,
    Layout=0x5000,Descriptors=0x10000,List=0x20000,Pso=0x30000,Registry=0x40000;
constexpr uintptr_t NativeFullscreenDrawReturnRva=0x20ccac;
namespace FSRD::CyberpunkEngineAccess {
constexpr uintptr_t RegistryRva=0x3438a28;
struct TextureBorrow {uint32_t handle;uintptr_t native;}; }
struct Handle {uintptr_t ptr=0;};
struct LightingCapturePlan {Handle dsv;std::array<FSRD::CyberpunkEngineAccess::TextureBorrow,4> textures;};
struct FakeScope {bool hasDsv=true;Handle dsv;};
FakeScope scopeStorage;FakeScope* lightingScope=&scopeStorage;
std::atomic<uintptr_t> authenticatedImage{Image};
std::map<uintptr_t,std::vector<unsigned char>> memory;
template<class T> void Put(uintptr_t address,const T& value) {
    auto& bytes=memory[address];bytes.resize(sizeof(T));std::memcpy(bytes.data(),&value,sizeof(T)); }
template<class T> bool ReadEarly(uintptr_t address,T& value) {
    auto it=memory.find(address);if(it==memory.end()||it->second.size()!=sizeof(T))return false;
    std::memcpy(&value,it->second.data(),sizeof(T));return true; }
template<class T> bool ReadEarlyAt(uintptr_t base,uintptr_t offset,T& value) {
    return base&&offset<=UINTPTR_MAX-base&&ReadEarly(base+offset,value); }
uintptr_t __readgsqword(unsigned offset) {assert(offset==0x58);return Slots;}
const Json& EarlyInput(const Json& metadata,size_t index){return metadata.at("inputs").at(index);}
''' + definitions + r'''
Json Setup()
{
    memory.clear();lightingScope=&scopeStorage;scopeStorage.hasDsv=true;
    Put<uintptr_t>(Slots,Tls);Put<uint8_t>(Tls+0x14,1);Put<uintptr_t>(Tls+0x188,Engine);
    Put<uintptr_t>(Engine+0x30,List);Put<uintptr_t>(Engine+0x3d0,Pso);Put<uintptr_t>(Engine+0x60,Cache);
    Put<uintptr_t>(Cache+0x68,Layout);Put<uintptr_t>(Cache+0x28,Descriptors);
    Put<uint64_t>(Cache+0x70,0);Put<uint64_t>(Cache+0x78,0);
    Put<uint64_t>(Layout,0);Put<uint64_t>(Layout+8,3);
    Json metadata={{"inputs",Json::array()}};
    constexpr unsigned registers[]={1,2,3,14};
    for(unsigned i=0;i<4;++i)
    {
        Put<uint8_t>(Layout+0x5c3+2*registers[i],i==3?1:0);
        Put<uintptr_t>(Descriptors+i*8,0x50000+i*16);
        Json entry;entry["texture_registry"]["descriptor_sources"]={
            {"status","cpu_descriptor_sources_observed"},{"repeated_source_fields_equal",true},
            {i==3?"alternate_cpu_srv_handle":"ordinary_cpu_srv_handle",0x50000+i*16}};
        metadata["inputs"].push_back(entry);
    }
    for(unsigned i=0;i<2;++i)
    {
        std::array<uint8_t,16> range{};
        uint32_t base=i?3:0;uint16_t first=i?14:1,count=i?1:3;
        std::memcpy(range.data()+4,&base,4);std::memcpy(range.data()+8,&first,2);std::memcpy(range.data()+10,&count,2);
        range[13]=1;range[14]=17+i;Put(Layout+0x38+i*16,range);
    }
    return metadata;
}
int main()
{
    auto metadata=Setup();auto result=ObserveLightingBindings(List,Pso,metadata);
    assert(result["pixel_srvs"].size()==4&&result["pixel_srvs"][3]["native_root_parameter"]==18);
    auto reject=[](auto alter){auto m=Setup();alter(m);bool refused=false;
        try{ObserveLightingBindings(List,Pso,m);}catch(...){refused=true;}assert(refused);};
    reject([](auto&){Put<uint8_t>(Tls+0x14,0);});
    reject([](auto&){Put<uintptr_t>(Engine+0x30,List+1);});
    reject([](auto&){Put<uintptr_t>(Engine+0x3d0,Pso+1);});
    reject([](auto&){Put<uint8_t>(Layout+0x5c3+2,0xff);});
    reject([](auto&){Put<uint8_t>(Layout+0x5c3+2,64);});
    reject([](auto&){Put<uint64_t>(Cache+0x70,1);});
    reject([](auto&){Put<uint64_t>(Cache+0x78,2);});
    reject([](auto&){Put<uint64_t>(Layout+8,2);});
    reject([](auto&){Put<uint64_t>(Layout,1);});
    reject([](auto&){std::array<uint8_t,16> r;ReadEarly(Layout+0x38,r);r[13]=2;Put(Layout+0x38,r);});
    reject([](auto&){std::array<uint8_t,16> r;ReadEarly(Layout+0x38,r);r[14]=64;Put(Layout+0x38,r);});
    reject([](auto&){std::array<uint8_t,16> r;ReadEarly(Layout+0x38,r);r[10]=0;Put(Layout+0x38,r);});
    reject([](auto&){std::array<uint8_t,16> r;ReadEarly(Layout+0x38,r);r[8]=2;Put(Layout+0x38,r);});
    reject([](auto&){std::array<uint8_t,16> r;ReadEarly(Layout+0x38,r);uint32_t b=65536;std::memcpy(r.data()+4,&b,4);Put(Layout+0x38,r);});
    reject([](auto& m){m["inputs"][0]["texture_registry"]["descriptor_sources"]["ordinary_cpu_srv_handle"]=7;});
    reject([](auto& m){m["inputs"][3]["texture_registry"]["descriptor_sources"]["repeated_source_fields_equal"]=false;});
    const uintptr_t slot=Registry+0x2f1d8+3*0xb0;
    auto depthSetup=[&]{Setup();Put<uintptr_t>(Image+FSRD::CyberpunkEngineAccess::RegistryRva,Registry);
        Put<int32_t>(slot-8,6);Put<uintptr_t>(slot,0x60000);Put<uintptr_t>(slot+0x28,0x70000);
        Put<uint32_t>(slot+0x48,0xe0);std::array<uint8_t,12> raw{};raw[4]=1;raw[7]=0x19;raw[8]=5;Put(slot+0x4e,raw);
        LightingCapturePlan p;p.textures[3]={4,0x60000};p.dsv.ptr=0x70000;scopeStorage.dsv=p.dsv;return p;};
    auto plan=depthSetup();assert(LightingDepthAlias(plan,plan.textures[3]));
    auto rejectDepth=[&](auto alter){auto p=depthSetup();alter(p);assert(!LightingDepthAlias(p,p.textures[3]));};
    rejectDepth([](auto&){scopeStorage.hasDsv=false;});
    rejectDepth([](auto&){scopeStorage.dsv.ptr=7;});
    rejectDepth([&](auto&){Put<int32_t>(slot-8,0);});
    rejectDepth([&](auto&){Put<uintptr_t>(slot,7);});
    rejectDepth([&](auto&){Put<uintptr_t>(slot+0x28,7);});
    rejectDepth([&](auto&){Put<uint32_t>(slot+0x48,0xc0);});
    for(auto field:{4,5,6,7,8})rejectDepth([&](auto&){std::array<uint8_t,12> raw;ReadEarly(slot+0x4e,raw);
        raw[field]=field==4?2:field==5?1:field==6?1:field==7?0x59:0;Put(slot+0x4e,raw);});
    assert(MatchesFinalLightingDraw(true,true,Image+NativeFullscreenDrawReturnRva,Image,3,1,0,0));
    assert(!MatchesFinalLightingDraw(false,true,Image+NativeFullscreenDrawReturnRva,Image,3,1,0,0));
    assert(!MatchesFinalLightingDraw(true,false,Image+NativeFullscreenDrawReturnRva,Image,3,1,0,0));
    assert(!MatchesFinalLightingDraw(true,true,Image+NativeFullscreenDrawReturnRva+1,Image,3,1,0,0));
    assert(!MatchesFinalLightingDraw(true,true,Image+NativeFullscreenDrawReturnRva,Image,6,1,0,0));
    assert(!MatchesFinalLightingDraw(true,true,Image+NativeFullscreenDrawReturnRva,Image,3,2,0,0));
    assert(!MatchesFinalLightingDraw(true,true,Image+NativeFullscreenDrawReturnRva,Image,3,1,1,0));
    assert(!MatchesFinalLightingDraw(true,true,Image+NativeFullscreenDrawReturnRva,Image,3,1,0,1));
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-lighting-guides-") as name:
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

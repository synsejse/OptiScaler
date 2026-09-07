"""Actual VS/PS exposure binding-reader tests and narrow integration guards.

Synthetic memory exercises the production CPU reader, not a rewritten model.
It cannot establish real GPU descriptor contents, submission or engine lifetime.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkFogProbe.cpp").read_text()
ENGINE = (ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkEngineAccess.h").read_text()


def function(name, full=False):
    marker = SOURCE.index(name + "(")
    start = SOURCE.rfind("\n", 0, marker) + 1
    opening = SOURCE.index("{", marker)
    depth, end = 1, opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start if full else opening:end]


class LightingExposure(unittest.TestCase):
    def test_actual_authentication_and_use_precede_addref_and_copy(self):
        prepare = function("PrepareLightingExposure")
        addref = prepare.index("reinterpret_cast<ID3D12Resource*>(source.native)")
        for guard in ('plan.provenance.at("lighting_shader").at("matched").get<bool>()',
                      "FSRD::CyberpunkExposureSource::Code", "MatchLiveCode(",
                      "FSRD::CyberpunkExposureSource::Observe(", "ObserveExposureBindings(plan, source.descriptor)"):
            self.assertLess(prepare.index(guard), addref)
        copy = prepare.index("FSRD::CyberpunkExposurePass::Work::Prepare(")
        for guard in ("desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER", "desc.Width < source.byteCount",
                      "desc.Width > 1024 * 1024", "source.descriptor >= heap.start",
                      "(source.descriptor - heap.start) % heap.increment == 0",
                      "(source.descriptor - heap.start) / heap.increment < heap.count",
                      "if (plan.exposureView.heap)", "if (!SameExposureSource(plan))"):
            self.assertLess(prepare.index(guard), copy)
        self.assertIn("!work || !SameExposureSource(plan)", prepare[copy:])
        self.assertLess(prepare.index("!work || !SameExposureSource(plan)"),
                        prepare.index("plan.exposureWork = std::move(work)"))
        self.assertIn("plan.exposureWork.reset()", prepare)
        for forbidden in ("RequestBufferState(", "CopyBufferRegion(", "Record(list)", "SetDescriptorHeaps("):
            self.assertNotIn(forbidden, prepare)

    def test_repeated_source_and_both_stage_bindings_are_fail_closed(self):
        same = function("SameExposureSource")
        for guard in ("earlyHeapTrackingValid.load()", "plan.exposureView.resource", "plan.exposureView.heap",
                      "FSRD::CyberpunkExposureSource::Observe(", "fresh == plan.exposureSource",
                      "uintptr_t(plan.exposureView.resource.Get()) == fresh.native",
                      "plan.exposureView.descriptor.ptr == fresh.descriptor",
                      "ObserveExposureBindings(plan, fresh.descriptor) == plan.exposureBindings",
                      "catch (...) { return false; }"):
            self.assertIn(guard, same)
        host = SOURCE.split("struct LightingEngineHost", 1)[1].split("PrepareLightingGuideWork", 1)[0]
        for guard in ("plan.exposureWork", "buffer.handle == plan.exposureSource.handle",
                      "buffer.native == plan.exposureSource.native", "SameExposureSource(plan)"):
            self.assertIn(guard, host)
        self.assertIn("IsExposureBufferAdmitted", host)

    def test_optional_recording_uses_engine_state_owner_and_private_companion(self):
        finish = function("FinishLightingCapture")
        self.assertIn("if (plan->exposureWork) input.exposure =", finish)
        self.assertLess(finish.index("FSRDSubmission::Retain("), finish.index("RecordPrivateCompute("))
        self.assertIn("plan->work->Record(list) && (!plan->exposureWork || plan->exposureWork->Record(list))", finish)
        self.assertLess(finish.index("PrivateRecordedRestored"), finish.index("exposure.resource ="))
        self.assertIn("exposure.resource = plan->exposureWork->Output()", finish)
        self.assertIn("exposure.viewFormat = DXGI_FORMAT_R32G32B32A32_UINT", finish)
        self.assertIn("exposure.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |", finish)
        self.assertIn("plan->exposureWork ? &exposure : nullptr", finish)
        self.assertLess(finish.index("RecordPrivateCompute("), finish.index("RecordEarlyGuides("))
        self.assertIn("input.exposure.handle, InputReadState", ENGINE)
        self.assertIn("InputReadState = 0xc0", ENGINE)
        self.assertIn("RequestBufferStateRva = 0x1f51c4", ENGINE)
        for forbidden in ("CopyBufferRegion(", "OMSetRenderTargets", "originalDraw(", "CPU_exposure_getter("):
            self.assertNotIn(forbidden, finish)

    def test_metadata_keeps_raw_words_separate_from_normalization_and_frame_claims(self):
        prepare = function("PrepareLightingExposure")
        for field in ('{ "normalization", "none" }', '{ "CPU_exposure_getter_called", false }',
                      'evidence["source_byte_offsets"] = { 0, 4, 8, 12, 16, 20, 24 }',
                      'evidence["source_state"] = 0xc0', 'evidence["actual_bindings"] = plan.exposureBindings'):
            self.assertIn(field, prepare)
        self.assertNotIn('"same_frame", true', prepare)
        self.assertNotIn('"normalization", true', prepare)

    def test_compiled_actual_both_stage_cache_reader(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to compile actual exposure binding reader")
        harness = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>
#include <json.hpp>
using Json=nlohmann::json;
constexpr uintptr_t Slots=0x1000,Tls=0x2000,Engine=0x3000,Cache=0x4000,
    Layout=0x5000,Descriptors=0x10000,List=0x20000,Pso=0x30000,Expected=0x40000;
constexpr unsigned Ranges[2]={3,11},Roots[2]={17,27},Indices[2]={205,300};
struct PsoRef {void* Get()const{return reinterpret_cast<void*>(Pso);}};
struct LightingCapturePlan {uintptr_t list=List;PsoRef pso;};
std::map<uintptr_t,std::vector<unsigned char>> memory;
template<class T> void Put(uintptr_t address,const T& value) {
 auto& bytes=memory[address];bytes.resize(sizeof(T));std::memcpy(bytes.data(),&value,sizeof(T));
}
template<class T> bool ReadEarly(uintptr_t address,T& value) {
 const auto it=memory.find(address);
 if(it==memory.end()||it->second.size()!=sizeof(T))return false;
 std::memcpy(&value,it->second.data(),sizeof(T));return true;
}
template<class T> bool ReadEarlyAt(uintptr_t base,uintptr_t offset,T& value) {
 return base&&offset<=UINTPTR_MAX-base&&ReadEarly(base+offset,value);
}
uintptr_t __readgsqword(unsigned offset){assert(offset==0x58);return Slots;}
''' + function("ObserveExposureBindings", True) + r'''
uintptr_t MapAddress(unsigned stage){return Layout+0x4c3+2*(stage*128+37);}
uintptr_t RangeAddress(unsigned stage){return Layout+0x38+16*Ranges[stage];}
template<class Alter> void AlterRange(unsigned stage,Alter alter) {
 std::array<uint8_t,16> range{};assert(ReadEarly(RangeAddress(stage),range));
 alter(range);Put(RangeAddress(stage),range);
}
LightingCapturePlan Setup() {
 memory.clear();
 Put<uintptr_t>(Slots,Tls);Put<uint8_t>(Tls+0x14,1);Put<uintptr_t>(Tls+0x188,Engine);
 Put<uintptr_t>(Engine+0x30,List);Put<uintptr_t>(Engine+0x3d0,Pso);Put<uintptr_t>(Engine+0x60,Cache);
 Put<uintptr_t>(Cache+0x68,Layout);Put<uintptr_t>(Cache+0x28,Descriptors);
 Put<uint64_t>(Cache+0x70,0);Put<uint64_t>(Cache+0x78,0);
 Put<uint64_t>(Layout,0);Put<uint64_t>(Layout+8,(uint64_t(1)<<Ranges[0])|(uint64_t(1)<<Ranges[1]));
 for(unsigned stage=0;stage<2;++stage) {
  Put<uint8_t>(MapAddress(stage),Ranges[stage]);
  std::array<uint8_t,16> range{};
  const uint32_t base=stage?300:200;const uint16_t first=stage?37:32,count=stage?1:16;
  std::memcpy(range.data()+4,&base,4);std::memcpy(range.data()+8,&first,2);std::memcpy(range.data()+10,&count,2);
  range[13]=1;range[14]=Roots[stage];Put(RangeAddress(stage),range);
  Put<uintptr_t>(Descriptors+Indices[stage]*8,Expected);
 }
 return {};
}
int main() {
 auto plan=Setup();auto result=ObserveExposureBindings(plan,Expected);
 assert(result["stages"].size()==2);
 assert(result["tls"]==Tls&&result["engine"]==Engine&&result["descriptor_array"]==Descriptors);
 for(unsigned stage=0;stage<2;++stage) {
  const auto& s=result["stages"][stage];
  assert(s["stage"]==(stage?"pixel":"vertex")&&s["register"]==37);
  assert(s["range_index"]==Ranges[stage]&&s["descriptor_index"]==Indices[stage]);
  assert(s["native_root_parameter"]==Roots[stage]&&s["cpu_srv_handle"]==Expected);
 }
 auto reject=[](auto alter) {
  auto p=Setup();uintptr_t expected=Expected;alter(p,expected);bool refused=false;
  try{ObserveExposureBindings(p,expected);}catch(const std::exception&){refused=true;}
  assert(refused);
 };
 reject([](auto&,auto&){memory.erase(Slots);});
 reject([](auto&,auto&){Put<uintptr_t>(Slots,0);});
 reject([](auto&,auto&){Put<uint8_t>(Tls+0x14,0);});
 reject([](auto&,auto&){memory.erase(Tls+0x188);});
 reject([](auto&,auto&){Put<uintptr_t>(Engine+0x30,List+1);});
 reject([](auto& p,auto&){p.list=List+1;});
 reject([](auto&,auto&){Put<uintptr_t>(Engine+0x3d0,Pso+1);});
 reject([](auto&,auto&){Put<uintptr_t>(Engine+0x60,0);});
 reject([](auto&,auto&){Put<uintptr_t>(Cache+0x68,0);});
 reject([](auto&,auto&){Put<uintptr_t>(Cache+0x28,0);});
 reject([](auto&,auto& e){e=0;});
 for(unsigned stage=0;stage<2;++stage) {
  const uint64_t bit=uint64_t(1)<<Ranges[stage];
  reject([&](auto&,auto&){Put<uint8_t>(MapAddress(stage),64);});
  reject([&](auto&,auto&){Put<uint8_t>(MapAddress(stage),0xff);});
  reject([&](auto&,auto&){memory.erase(MapAddress(stage));});
  reject([&](auto&,auto&){memory.erase(RangeAddress(stage));});
  reject([&](auto&,auto&){Put<uint64_t>(Cache+0x70,bit);});
  reject([&](auto&,auto&){Put<uint64_t>(Cache+0x78,bit);});
  reject([&](auto&,auto&){uint64_t mask;assert(ReadEarly(Layout+8,mask));Put(Layout+8,mask&~bit);});
  reject([&](auto&,auto&){Put<uint64_t>(Layout,bit);});
  reject([&](auto&,auto&){AlterRange(stage,[](auto& r){r[13]=2;});});
  reject([&](auto&,auto&){AlterRange(stage,[](auto& r){r[14]=64;});});
  reject([&](auto&,auto&){AlterRange(stage,[](auto& r){uint16_t n=0;std::memcpy(r.data()+10,&n,2);});});
  reject([&](auto&,auto&){AlterRange(stage,[](auto& r){uint16_t f=38;std::memcpy(r.data()+8,&f,2);});});
  reject([&](auto&,auto&){AlterRange(stage,[](auto& r){uint16_t f=36,n=1;std::memcpy(r.data()+8,&f,2);std::memcpy(r.data()+10,&n,2);});});
  reject([&](auto&,auto&){AlterRange(stage,[](auto& r){uint32_t b=65536;std::memcpy(r.data()+4,&b,4);});});
  reject([&](auto&,auto&){AlterRange(stage,[](auto& r){uint32_t b=0xffffffff;std::memcpy(r.data()+4,&b,4);});});
  reject([&](auto&,auto&){memory.erase(Descriptors+Indices[stage]*8);});
  // One correct stage cannot hide the other stage's different/null descriptor.
  reject([&](auto&,auto&){Put<uintptr_t>(Descriptors+Indices[stage]*8,Expected+16);});
  reject([&](auto&,auto&){Put<uintptr_t>(Descriptors+Indices[stage]*8,0);});
 }
 // Bits belonging only to unrelated ranges do not falsely reject this request.
 plan=Setup();Put<uint64_t>(Cache+0x70,1);Put<uint64_t>(Cache+0x78,2);Put<uint64_t>(Layout,4);
 assert(ObserveExposureBindings(plan,Expected)["stages"].size()==2);
 // Conservative descriptor-index cap has the intended inclusive final valid slot.
 plan=Setup();AlterRange(1,[](auto& r){uint32_t b=65535;std::memcpy(r.data()+4,&b,4);});
 Put<uintptr_t>(Descriptors+65535*8,Expected);
 assert(ObserveExposureBindings(plan,Expected)["stages"][1]["descriptor_index"]==65535);
 // Optional original PS-only t8 uses the SAME authenticated SRV route, without
 // accepting an absent VS t8 or confusing t8 with t37's descriptor index.
 plan=Setup();Put<uint8_t>(Layout+0x4c3+2*(128+8),Ranges[1]);
 AlterRange(1,[](auto& r){uint16_t first=5,count=6;std::memcpy(r.data()+8,&first,2);std::memcpy(r.data()+10,&count,2);});
 Put<uintptr_t>(Descriptors+303*8,Expected);
 auto t8=ObserveExposureBindings(plan,Expected,8,1);
 assert(t8["stages"].size()==1&&t8["stages"][0]["stage"]=="pixel"&&t8["stages"][0]["register"]==8);
 assert(t8["stages"][0]["descriptor_index"]==303);
 Put<uintptr_t>(Descriptors+303*8,Expected+1);
 bool refused=false;try{ObserveExposureBindings(plan,Expected,8,1);}catch(const std::exception&){refused=true;}
 assert(refused);
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-lighting-exposure-") as directory:
            path = Path(directory)
            source, binary = path / "exposure.cpp", path / "exposure"
            source.write_text(harness)
            compiled = subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-O2", str(source),
                                       "-I", str(ROOT / "external/nlohmann"), "-o", str(binary)],
                                      capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()

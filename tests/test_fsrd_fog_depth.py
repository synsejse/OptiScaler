"""Compile the pure current-Fog t0 reader, including real EXE hash fixtures."""
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
HEADER = ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkFogDepth.h"


class FogDepth(unittest.TestCase):
    def test_bounded_current_pixel_t0_and_authored_plane(self):
        compiler = os.environ.get("CXX") or shutil.which("c++")
        if not compiler:
            self.skipTest("Set CXX to compile the actual portable depth reader")
        harness = r'''
#include <cassert>
#include <map>
#include <stdexcept>
#include <vector>
#include "FSRDCyberpunkFogDepth.h"
namespace D=FSRD::CyberpunkFogDepth;
constexpr uintptr_t Image=0x140000000,Context=0x100000,View=0x200000,Tls=0x300000,
 Engine=0x400000,List=0x500000,Pso=0x510000,Cache=0x600000,Layout=0x700000,
 Array=0x800000,Registry=0x900000,Slot=Registry+0x2f1d8,Native=0xa00000,Descriptor=0xb00000;
struct Host {
 std::map<uintptr_t,uint8_t> memory;
 std::vector<uintptr_t> reads;
 uintptr_t throwing=0;unsigned mutate=0;
 template<class T>void Put(uintptr_t address,const T& value){
  std::array<uint8_t,sizeof(T)> raw{};std::memcpy(raw.data(),&value,sizeof(T));
  for(size_t i=0;i<raw.size();++i)memory[address+i]=raw[i];
 }
 bool Read(uintptr_t address,void* output,size_t size){
  reads.push_back(address);if(address==throwing)throw std::runtime_error("read");
  for(size_t i=0;i<size;++i){auto p=memory.find(address+i);if(p==memory.end())return false;
   static_cast<uint8_t*>(output)[i]=p->second;}
  if(address==Array&&mutate){auto selected=mutate;mutate=0;
   if(selected==1)Put<int32_t>(Slot-8,2);
   if(selected==2)Put<uintptr_t>(Context+0x18,View+1);
   if(selected==3)Put<uint64_t>(Cache+0x70,1);
   if(selected==4)Put<uint64_t>(Cache+0x70,1ull<<63);
   if(selected==5)Put<uintptr_t>(Slot+0x68,Native+1);
   if(selected==6)Put<int32_t>(Slot-8,1);
   if(selected==7)Put<int32_t>(Slot-8,0);
   if(selected==8)Put<int32_t>(Slot-8,-1);
  }return true;
 }
};
D::Scope Scope(){return {9,12,Context,View,Tls,Engine,List,Pso};}
Host Setup(){
 Host h;h.Put<uintptr_t>(Context+0x18,View);h.Put<uint8_t>(Tls+0x14,1);
 h.Put<uintptr_t>(Tls+0x188,Engine);h.Put<uintptr_t>(Engine+0x30,List);
 h.Put<uintptr_t>(Engine+0x3d0,Pso);h.Put<uintptr_t>(Engine+0x60,Cache);
 h.Put<uintptr_t>(Cache+0x68,Layout);h.Put<uintptr_t>(Cache+0x28,Array);
 h.Put<uintptr_t>(Image+0x3438a28,Registry);h.Put<int32_t>(Slot-8,1);
 h.Put<uintptr_t>(Slot,Native);h.Put<uintptr_t>(Slot+0x30,Descriptor);
 h.Put<uintptr_t>(Slot+0x38,0);h.Put<uintptr_t>(Slot+0x68,Native);
 h.Put<uint32_t>(Slot+0x48,0xe0);
 h.Put(Slot+0x4e,std::array<uint8_t,12>{0,5,0xd0,2,1,0,0x10,0x1b,5,0,4,0x10});
 h.Put<uint8_t>(Layout+0x5c3,0);std::array<uint8_t,16> range{};
 range[10]=1;range[14]=17;h.Put(Layout+0x38,range);
 h.Put<uint64_t>(Layout,0);h.Put<uint64_t>(Layout+8,1);
 h.Put<uint64_t>(Cache+0x70,0);h.Put<uint64_t>(Cache+0x78,0);
 h.Put<uintptr_t>(Array,Descriptor);return h;
}
bool Observe(Host& h,D::Snapshot& out,D::Failure* failure=nullptr,D::ChangedSnapshots* changed=nullptr){
 return D::Observe(h,Image,Image+D::DrawReturnRva,Scope(),1,out,failure,changed);
}
int main(){
 static_assert(std::is_trivially_copyable_v<D::Snapshot>);
 static_assert(noexcept(D::Observe(std::declval<Host&>(),0,0,D::Scope{},0,std::declval<D::Snapshot&>())));
 auto h=Setup();D::Snapshot out;D::Failure why;
 assert(Observe(h,out,&why)&&why==D::Failure::None);
 assert(out.scope==Scope()&&out.native==Native&&out.descriptor==Descriptor&&out.handle==1);
 assert(out.registry==Registry&&out.slot==Slot&&out.residencyUnderlying==Native);
 assert(out.cache==Cache&&out.layout==Layout&&out.descriptorArray==Array);
 assert(out.rangeIndex==0&&out.rootParameter==17&&out.mapAddress==Layout+0x5c3);
 assert(out.requestedSrvState==0xe0&&out.srvFormat==41&&out.srvDimension==4&&out.planeSlice==0);
 assert(out.mostDetailedMip==0&&out.srvMipLevels==UINT32_MAX&&out.componentMapping==0x1688);
 assert(out.width==1280&&out.height==720&&out.compact[7]==0x1b);
 const D::Snapshot empty;
 auto reject=[&](auto change,D::Failure expected){auto t=Setup();change(t);auto got=out;
  assert(!Observe(t,got,&why)&&got==empty&&why==expected);};
 for(auto address:{Context+0x18,Tls+0x188,Engine+0x30,Engine+0x3d0,Engine+0x60,
                   Cache+0x68,Cache+0x28,Image+0x3438a28})
  reject([&](Host& t){t.Put<uintptr_t>(address,0);},D::Failure::CurrentScope);
 reject([](Host& t){t.Put<uint8_t>(Tls+0x14,0);},D::Failure::CurrentScope);
 for(int32_t refs:{0,-1})reject([&](Host& t){t.Put<int32_t>(Slot-8,refs);},D::Failure::TextureSource);
 for(auto address:{Slot,Slot+0x30})reject([&](Host& t){t.Put<uintptr_t>(address,0);},D::Failure::TextureSource);
 reject([](Host& t){t.Put<uintptr_t>(Image+0x3438a28,UINTPTR_MAX-8);},D::Failure::TextureSource);
 for(auto tag:{0x18,0x19,0x11,0x5b,0x9b})
  reject([&](Host& t){t.Put<uint8_t>(Slot+0x55,uint8_t(tag));},D::Failure::UnsupportedView);
 for(auto dimensions:{0,0x11,0x20})
  reject([&](Host& t){t.Put<uint8_t>(Slot+0x54,uint8_t(dimensions));},D::Failure::UnsupportedView);
 for(auto offset:{0u,2u,4u})
  reject([&](Host& t){t.Put<uint16_t>(Slot + 0x4e + offset,0);},D::Failure::UnsupportedView);
 reject([](Host& t){t.Put<uint16_t>(Slot+0x52,2);},D::Failure::UnsupportedView);
 for(auto flags:{0,1,4,0x45})
  reject([&](Host& t){t.Put<uint8_t>(Slot+0x56,uint8_t(flags));},D::Failure::UnsupportedView);
 reject([](Host& t){t.Put<uintptr_t>(Slot+0x38,Descriptor+1);},D::Failure::UnsupportedView);
 for(uint32_t state:{0u,0x40u,0xc0u,0x8e0u})
  reject([&](Host& t){t.Put<uint32_t>(Slot+0x48,state);},D::Failure::UnsupportedView);
 for(auto index:{64,255})reject([&](Host& t){t.Put<uint8_t>(Layout+0x5c3,uint8_t(index));},D::Failure::BindingRange);
 reject([](Host& t){t.Put<uintptr_t>(Cache+0x68,UINTPTR_MAX-8);},D::Failure::BindingRange);
 reject([](Host& t){t.Put<uint64_t>(Layout+8,0);},D::Failure::BindingRange);
 reject([](Host& t){t.Put<uint64_t>(Layout,1);},D::Failure::BindingRange);
 for(auto address:{Cache+0x70,Cache+0x78})
  reject([&](Host& t){t.Put<uint64_t>(address,1);},D::Failure::BindingRange);
 reject([](Host& t){t.Put<uint8_t>(Layout+0x38+13,2);},D::Failure::BindingRange);
 reject([](Host& t){t.Put<uint8_t>(Layout+0x38+14,64);},D::Failure::BindingRange);
 reject([](Host& t){t.Put<uint16_t>(Layout+0x38+8,1);},D::Failure::BindingRange);
 reject([](Host& t){t.Put<uint16_t>(Layout+0x38+10,0);},D::Failure::BindingRange);
 for(uint32_t index:{65536u,UINT32_MAX})
  reject([&](Host& t){t.Put<uint32_t>(Layout+0x38+4,index);},D::Failure::BindingRange);
 reject([](Host& t){t.Put<uintptr_t>(Array,0);},D::Failure::BindingRange);
 reject([](Host& t){t.Put<uintptr_t>(Array,Descriptor+1);},D::Failure::DescriptorMismatch);
 reject([](Host& t){t.throwing=Array;},D::Failure::ReadException);
 // Positive count changes preserve identity and retain both actual sample values.
 for(unsigned mutation:{1u,6u}) {
  auto t=Setup();if(mutation==6)t.Put<int32_t>(Slot-8,2);t.mutate=mutation;
  D::ChangedSnapshots changed;D::Snapshot got;
  assert(Observe(t,got,&why,&changed)&&why==D::Failure::None&&changed.available);
  assert(changed.first.refs==(mutation==1?1:2)&&changed.second.refs==(mutation==1?2:1));
  assert(got==changed.second&&D::SameBindingIdentity(changed.first,changed.second));
  const auto preservedFirst=changed.first,preservedSecond=changed.second;
  assert(D::SameBindingIdentity(changed.first,changed.second));
  assert(changed.first==preservedFirst&&changed.second==preservedSecond);
  t=Setup();t.mutate=7;
  assert(!Observe(t,got,&why,&changed)&&why==D::Failure::TextureSource&&got==empty&&!changed.available);
  t=Setup();t.mutate=8;assert(!Observe(t,got,&why,&changed)&&!changed.available&&got==empty);
 }
 // Only the numeric positive count is excluded. No identity/format/range field is.
 h=Setup();assert(Observe(h,out));
 auto changedIdentity=[&](auto alter){auto b=out;alter(b);assert(!D::SameBindingIdentity(out,b));};
 #define DIFFER(field) changedIdentity([](auto& s){++s.field;})
 DIFFER(image);DIFFER(registry);DIFFER(slot);DIFFER(native);DIFFER(descriptor);DIFFER(alternateDescriptor);
 DIFFER(residencyUnderlying);DIFFER(cache);DIFFER(layout);DIFFER(descriptorArray);DIFFER(mapAddress);
 DIFFER(handle);DIFFER(requestedSrvState);DIFFER(descriptorIndex);DIFFER(rangeIndex);DIFFER(rootParameter);
 DIFFER(width);DIFFER(height);DIFFER(srvFormat);DIFFER(srvDimension);DIFFER(planeSlice);DIFFER(mostDetailedMip);
 DIFFER(srvMipLevels);DIFFER(componentMapping);DIFFER(scope.serial);DIFFER(scope.recordingGeneration);
 DIFFER(scope.graphContext);DIFFER(scope.view);DIFFER(scope.tls);DIFFER(scope.engine);DIFFER(scope.list);DIFFER(scope.pso);
 #undef DIFFER
 for(size_t i=0;i<out.compact.size();++i)changedIdentity([&](auto& s){s.compact[i]^=1;});
 for(size_t i=0;i<out.range.size();++i)changedIdentity([&](auto& s){s.range[i]^=1;});
 for(int32_t count:{0,-1,INT32_MIN}) {
  auto bad=out;bad.refs=count;assert(!D::SameBindingIdentity(bad,out)&&!D::SameBindingIdentity(out,bad));
 }
 reject([](Host& t){t.mutate=2;},D::Failure::CurrentScope);
 reject([](Host& t){t.mutate=3;},D::Failure::BindingRange);
 reject([](Host& t){t.mutate=5;},D::Failure::Changed);
 h=Setup();h.mutate=4;assert(Observe(h,out)); // Unrelated range dirtiness isn't selected correspondence.
 for(auto handle:{0u,32769u,UINT32_MAX}){
  h=Setup();assert(!D::Observe(h,Image,Image+D::DrawReturnRva,Scope(),handle,out,&why));
  assert(out==empty&&why==D::Failure::TextureSource);
 }
 for(auto image:{uintptr_t(0),UINTPTR_MAX-8}){
  h=Setup();assert(!D::Observe(h,image,Image+D::DrawReturnRva,Scope(),1,out,&why));
  assert(out==empty&&why==D::Failure::Caller&&h.reads.empty());
 }
 h=Setup();assert(!D::Observe(h,Image,Image+D::DrawReturnRva+1,Scope(),1,out)&&h.reads.empty());
 for(unsigned i=0;i<8;++i){auto s=Scope();
  if(i==0)s.serial=0;if(i==1)s.recordingGeneration=0;if(i==2)s.graphContext=0;if(i==3)s.view=0;
  if(i==4)s.tls=0;if(i==5)s.engine=0;if(i==6)s.list=0;if(i==7)s.pso=0;
  h=Setup();assert(!D::Observe(h,Image,Image+D::DrawReturnRva,s,1,out)&&out==empty&&h.reads.empty());
 }
 assert(D::IsDepthBind(Image,Image+D::BindReturnRva,0,1,1));
 assert(!D::IsDepthBind(Image,Image+D::BindReturnRva,0,1,2));
 assert(!D::IsDepthBind(Image,Image+D::BindReturnRva,0,2,1));
 assert(!D::IsDepthBind(Image,Image+D::BindReturnRva,1,1,1));
 assert(!D::IsDepthBind(Image,Image+D::BindReturnRva+1,0,1,1));
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-fog-depth-") as directory:
            source = Path(directory) / "test.cpp"
            source.write_text(harness)
            for flags in (("-O0",), ("-O3", "-ffast-math")):
                binary = Path(directory) / ("test" + flags[0])
                subprocess.run([compiler, "-std=c++20", *flags, "-Wall", "-Wextra", "-Werror",
                                "-Wno-misleading-indentation", "-I", str(HEADER.parent),
                                str(source), "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True)

    def test_exact_installed_code_and_fog_binder_call(self):
        exe = Path("/home/synse/Games/Heroic/Games/Cyberpunk 2077/bin/x64/Cyberpunk2077.exe")
        if not exe.exists():
            self.skipTest("Authenticated local game fixture unavailable")
        data = exe.read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         "a7de82945c03e041fc7339fcf9066224d98db2f5d80fea50f7947bb350a60991")
        ranges = re.findall(r'\{ (0x[0-9a-f]+), (0x[0-9a-f]+), "([0-9a-f]{64})" \}', HEADER.read_text())
        self.assertEqual(len(ranges), 7)
        for start, count, digest in ranges:
            rva, size = int(start, 16), int(count, 16)
            self.assertEqual(hashlib.sha256(data[rva-0xc00:rva-0xc00+size]).hexdigest(), digest)
        for caller, target in ((0x61fbf5, 0x1f3a6c), (0x61fc4f, 0x20c954)):
            self.assertEqual(data[caller-5-0xc00], 0xe8)
            self.assertEqual(caller + struct.unpack_from("<i", data, caller-4-0xc00)[0], target)
        # Exact leaf instructions executed for tag1b: subtract19, subtract2,
        # branch to return41. This format is depth R, not alternate stencil XG.
        self.assertEqual(data[0x20cf9b-0xc00:0x20cfa9-0xc00].hex(), "83ea190f84ad00000083ea027488")
        self.assertEqual(data[0x20cf31-0xc00:0x20cf37-0xc00].hex(), "b829000000c3")

    def test_metadata_only_and_existing_graph_selection_reused(self):
        source = HEADER.read_text()
        for forbidden in ("AddRef(", "QueryInterface(", "GetDesc(", "RequestState(",
                          "ResourceBarrier(", "CreateShaderResourceView(", "GetCurrentList("):
            self.assertNotIn(forbidden, source)
        self.assertIn("EarlyGuides::Describe(GraphKey)", source)
        self.assertIn("requestedSrvState=e0 is not an observed current native state", source)
        self.assertIn("Diagnostic cap, NOT engine array capacity", source)
        self.assertIn("using CyberpunkEngineAccess::Detail::Read;", source)


if __name__ == "__main__":
    unittest.main()

"""Compile the actual bounded ray cache observer; no engine or GPU calls."""
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
HEADER = ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkRayBindings.h"


class RayBindings(unittest.TestCase):
    def test_actual_header_binding_maps_receipt_scope_and_bounded_refusals(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to compile the actual portable ray observer")
        harness = r'''
#include <cassert>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>
#include "FSRDCyberpunkRayBindings.h"
namespace R=FSRD::CyberpunkRayBindings;
namespace C=FSRD::CyberpunkRayConstants;
constexpr uintptr_t Image=0x140000000, Context=0x100000, View=0x200000,
    Tls=0x300000, Engine=0x400000, List=0x500000, List4=0x510000,
    Cache=0x600000, Layout=0x700000, Array=0x800000, Registry=0x900000;
constexpr uintptr_t B6=0xa00000, Motion=0xb00000, Radiance=0xc00000, Hit=0xd00000;
constexpr uintptr_t Slot(unsigned h) { return Registry+0x2f1d8+uintptr_t(h-1)*0xb0; }
constexpr uintptr_t Extra(unsigned h) { return 0xe00000+uintptr_t(h)*0x100; }
constexpr uintptr_t UavArray(unsigned h) { return 0xf00000+uintptr_t(h)*0x100; }
constexpr uintptr_t Maps[]={0x497,0x6cb,0x90f,0x91f};
constexpr unsigned Regs[]={6,4,0,8};
constexpr uintptr_t Descriptors[]={B6,Motion,Radiance,Hit};
struct Host
{
    std::map<uintptr_t,uint8_t> memory;
    std::vector<uintptr_t> reads;
    uintptr_t throwing=0, mutationTrigger=0;
    unsigned mutation=0;
    template<class T> void Put(uintptr_t address,const T& value)
    {
        std::array<uint8_t,sizeof(T)> data{};std::memcpy(data.data(),&value,sizeof(value));
        for(size_t i=0;i<data.size();++i)memory[address+i]=data[i];
    }
    bool Read(uintptr_t address,void* output,size_t bytes)
    {
        reads.push_back(address);
        if(address==throwing)throw std::runtime_error("unavailable memory");
        for(size_t i=0;i<bytes;++i)
        {
            auto found=memory.find(address+i);if(found==memory.end())return false;
            static_cast<uint8_t*>(output)[i]=found->second;
        }
        if(address==mutationTrigger&&mutation)
        {
            auto selected=mutation;mutation=0;
            if(selected==1)Put<int32_t>(Slot(1)-8,2);
            if(selected==2)Put<uintptr_t>(Context+0x18,View+1);
            if(selected==3)Put<uint64_t>(Cache+0x70,1);
            if(selected==4)Put<uint64_t>(Cache+0x70,1ull<<63);
            if(selected==5)Put<uintptr_t>(Engine+0x90,B6+1);
            if(selected==6)Put<uintptr_t>(Slot(2)+0x40,Extra(2)+1);
        }
        return true;
    }
    bool WasRead(uintptr_t address)const
    { for(auto value:reads)if(value==address)return true;return false; }
};
R::Scope Scope()
{ return {9,12,Context,View,Tls,Engine,List,0}; }
R::TextureHandles Handles() { return {1,2,3}; }
R::Receipt Receipt()
{
    R::Receipt r;r.scope=Scope();r.callerRva=C::UploadReturnRvas[0];
    r.cache=Cache;r.descriptor=B6;r.phase=C::Phase::Uploaded;
    r.words[1212/4]=0;r.words[3168/4]=1;return r;
}
void Range(Host& h,unsigned i,uint32_t base,uint16_t first,uint16_t count)
{
    std::array<uint8_t,16> range{};
    std::memcpy(range.data()+4,&base,4);std::memcpy(range.data()+8,&first,2);
    std::memcpy(range.data()+10,&count,2);range[13]=2;range[14]=uint8_t(12+i);
    h.Put(Layout+0x38+16*i,range);
}
void Texture(Host& h,unsigned handle,uintptr_t descriptor)
{
    h.Put<int32_t>(Slot(handle)-8,1);h.Put<uintptr_t>(Slot(handle),0x1200000+handle*0x100);
    h.Put<uint32_t>(Slot(handle)+0x48,0xc0);
    h.Put(Slot(handle)+0x4e,std::array<uint8_t,12>{0,5,0xd0,2,1,0,0x10,0x11,0xb,0,0x10,0x16});
    h.Put<uintptr_t>(Slot(handle)+0x30,descriptor+0x1000);
    h.Put<uintptr_t>(Slot(handle)+0x40,Extra(handle));
    h.Put<uintptr_t>(Extra(handle)+0x28,UavArray(handle));
    h.Put<uintptr_t>(UavArray(handle),descriptor);
}
Host Setup()
{
    Host h;h.Put<uint8_t>(Tls+0x14,1);h.Put<uintptr_t>(Tls+0x188,Engine);
    h.Put<uintptr_t>(Engine+0x30,List);h.Put<uintptr_t>(Engine+0x40,List4);
    h.Put<uintptr_t>(Engine+0x60,Cache);h.Put<uintptr_t>(Engine+0x90,B6);
    h.Put<uintptr_t>(Context+0x18,View);h.Put<uintptr_t>(Cache+0x68,Layout);
    h.Put<uintptr_t>(Cache+0x28,Array);h.Put<uintptr_t>(Image+0x3438a28,Registry);
    h.Put<uint64_t>(Cache+0x70,0);h.Put<uint64_t>(Cache+0x78,0);
    h.Put<uint64_t>(Layout,0);h.Put<uint64_t>(Layout+8,15);
    for(unsigned i=0;i<4;++i)
    { h.Put<uint8_t>(Layout+Maps[i],uint8_t(i));Range(h,i,i,Regs[i],1);h.Put<uintptr_t>(Array+8*i,Descriptors[i]); }
    Texture(h,1,Motion);h.Put<uintptr_t>(Slot(1)+0x30,Motion);
    Texture(h,2,Radiance);Texture(h,3,Hit);return h;
}
bool Observe(Host& h,R::Snapshot& out,R::Failure* reason=nullptr)
{ return R::Observe(h,Image,Image+R::DispatchReturnRva,List4,Scope(),Receipt(),Handles(),out,reason); }
int main()
{
    static_assert(std::is_trivially_copyable_v<R::Snapshot>);
    static_assert(noexcept(Observe(std::declval<Host&>(),std::declval<R::Snapshot&>()))==false);
    static_assert(noexcept(R::Observe(std::declval<Host&>(),0,0,0,R::Scope{},R::Receipt{},R::TextureHandles{},std::declval<R::Snapshot&>())));
    auto h=Setup();R::Snapshot out;R::Failure reason;
    assert(Observe(h,out,&reason)&&reason==R::Failure::None);
    assert(out.scope==Scope()&&out.list4==List4&&List4!=List&&out.cache==Cache&&out.layout==Layout);
    assert(out.callerRva==R::DispatchReturnRva&&out.descriptorArray==Array&&out.registry==Registry);
    assert(out.b6.descriptor==B6&&out.b6.shaderRegister==6&&out.b6.mapAddress==Layout+0x497);
    for(unsigned i=0;i<3;++i)
    {
        assert(out.textures[i].handle==i+1&&out.textures[i].descriptor==Descriptors[i+1]);
        assert(out.textures[i].binding.descriptor==Descriptors[i+1]);
        assert(out.textures[i].binding.rootParameter==13+i&&out.textures[i].binding.rangeIndex==i+1);
        assert(out.textures[i].binding.mapAddress==Layout+Maps[i+1]);
    }
    assert(!out.textures[0].extra&&!out.textures[0].uavArray);
    assert(out.textures[1].extra==Extra(2)&&out.textures[1].uavArray==UavArray(2));
    assert(h.WasRead(UavArray(2))&&!h.WasRead(Slot(2)+0x30));
    R::Snapshot repeated;assert(Observe(h,repeated)&&out==repeated);
    const R::Snapshot empty;
    auto reject=[&](auto change,R::Failure expected)
    {
        auto t=Setup();change(t);auto result=out;R::Failure why=R::Failure::None;
        assert(!Observe(t,result,&why)&&result==empty&&why==expected);
    };
    for(auto address:{Tls+0x188,Engine+0x30,Engine+0x40,Engine+0x60,Engine+0x90,Context+0x18,Cache+0x68,Cache+0x28,Image+0x3438a28})
        reject([&](Host& t){t.Put<uintptr_t>(address,0);},R::Failure::CurrentScope);
    reject([](Host& t){t.Put<uint8_t>(Tls+0x14,0);},R::Failure::CurrentScope);
    reject([](Host& t){t.Put<uintptr_t>(Cache+0x68,UINTPTR_MAX-8);},R::Failure::BindingRange);
    for(unsigned i=0;i<4;++i)
    {
        reject([&](Host& t){t.Put<uint8_t>(Layout+Maps[i],0xff);},R::Failure::BindingRange);
        reject([&](Host& t){t.Put<uint8_t>(Layout+Maps[i],64);},R::Failure::BindingRange);
        reject([&](Host& t){t.Put<uint64_t>(Layout+8,15^(1ull<<i));},R::Failure::BindingRange);
        reject([&](Host& t){t.Put<uint64_t>(Layout,1ull<<i);},R::Failure::BindingRange);
        reject([&](Host& t){t.Put<uint64_t>(Cache+0x70,1ull<<i);},R::Failure::BindingRange);
        reject([&](Host& t){t.Put<uint64_t>(Cache+0x78,1ull<<i);},R::Failure::BindingRange);
        reject([&](Host& t){t.Put<uint8_t>(Layout+0x38+16*i+13,1);},R::Failure::BindingRange);
        reject([&](Host& t){t.Put<uint8_t>(Layout+0x38+16*i+14,64);},R::Failure::BindingRange);
        reject([&](Host& t){Range(t,i,i,Regs[i]+1,1);},R::Failure::BindingRange);
        reject([&](Host& t){Range(t,i,i,Regs[i],0);},R::Failure::BindingRange);
        reject([&](Host& t){Range(t,i,65536,Regs[i],1);},R::Failure::BindingRange);
        reject([&](Host& t){Range(t,i,UINT32_MAX,Regs[i],1);},R::Failure::BindingRange);
        reject([&](Host& t){t.Put<uintptr_t>(Array+8*i,Descriptors[i]+1);},R::Failure::DescriptorMismatch);
        reject([&](Host& t){t.Put<uintptr_t>(Array+8*i,0);},R::Failure::BindingRange);
    }
    for(unsigned handle=1;handle<=3;++handle)
    {
        reject([&](Host& t){t.Put<int32_t>(Slot(handle)-8,0);},R::Failure::TextureSource);
        reject([&](Host& t){t.Put<int32_t>(Slot(handle)-8,-1);},R::Failure::TextureSource);
        reject([&](Host& t){t.Put<uintptr_t>(Slot(handle),0);},R::Failure::TextureSource);
    }
    for(unsigned handle:{2u,3u})
    {
        reject([&](Host& t){t.Put<uint8_t>(Slot(handle)+0x54,0);},R::Failure::TextureSource);
        reject([&](Host& t){t.Put<uint8_t>(Slot(handle)+0x54,0x20);},R::Failure::TextureSource);
        reject([&](Host& t){t.Put<uintptr_t>(Slot(handle)+0x40,0);},R::Failure::TextureSource);
        reject([&](Host& t){t.Put<uintptr_t>(Extra(handle)+0x28,0);},R::Failure::TextureSource);
        reject([&](Host& t){t.Put<uintptr_t>(UavArray(handle),0);},R::Failure::TextureSource);
        reject([&](Host& t){t.Put<uintptr_t>(Slot(handle)+0x40,UINTPTR_MAX-16);},R::Failure::TextureSource);
    }
    reject([](Host& t){t.Put<uintptr_t>(Image+0x3438a28,UINTPTR_MAX-16);},R::Failure::TextureSource);
    reject([](Host& t){t.Put<uintptr_t>(Cache+0x28,UINTPTR_MAX-4);},R::Failure::BindingRange);
    reject([](Host& t){t.throwing=Array;},R::Failure::ReadException);
    reject([](Host& t){t.memory.erase(UavArray(3));},R::Failure::TextureSource);
    for(unsigned mutation:{1u,2u,3u,5u,6u})
        reject([&](Host& t){t.mutationTrigger=Array+24;t.mutation=mutation;},
            mutation==1?R::Failure::Changed:mutation==2||mutation==5?R::Failure::CurrentScope:
            mutation==3?R::Failure::BindingRange:R::Failure::TextureSource);
    h=Setup();h.mutationTrigger=Array+24;h.mutation=4;
    assert(Observe(h,repeated)&&repeated==out); // Unrelated dirty range is not a global race guard.
    h=Setup();Range(h,0,65535,6,1);h.Put<uintptr_t>(Array+8*65535,B6);
    assert(Observe(h,repeated)&&repeated.b6.descriptorIndex==65535);
    h=Setup();Range(h,1,10,2,3);h.Put<uintptr_t>(Array+8*12,Motion);
    assert(Observe(h,repeated)&&repeated.textures[0].binding.descriptorIndex==12);
    for(auto caller:{uintptr_t(0),Image+R::DispatchReturnRva+1,Image+0x293412b})
    { auto t=Setup();assert(!R::Observe(t,Image,caller,List4,Scope(),Receipt(),Handles(),repeated,&reason)&&reason==R::Failure::Caller&&repeated==empty); }
    h=Setup();assert(!R::Observe(h,0,R::DispatchReturnRva,List4,Scope(),Receipt(),Handles(),repeated));
    assert(!R::Observe(h,UINTPTR_MAX,Image+R::DispatchReturnRva,List4,Scope(),Receipt(),Handles(),repeated));
    for(unsigned change=0;change<10;++change)
    {
        auto r=Receipt();auto s=Scope();auto t=Setup();
        if(change==0)r.phase=C::Phase::Pending;
        if(change==1)r.phase=C::Phase::Invalid;
        if(change==2)r.source=0x123;
        if(change==3)r.scope.serial++;
        if(change==4)r.scope.recordingGeneration++;
        if(change==5)r.callerRva++;
        if(change==6)r.cache=0;
        if(change==7)r.descriptor=0;
        if(change==8){s.serial=0;r.scope=s;}
        if(change==9){s.recordingGeneration=0;r.scope=s;}
        assert(!R::Observe(t,Image,Image+R::DispatchReturnRva,List4,s,r,Handles(),repeated,&reason));
        assert(repeated==empty&&(change>=8?reason==R::Failure::CurrentScope:reason==R::Failure::UploadReceipt));
        if(change<8)assert(t.reads.empty());
    }
    for(uint32_t handle:{0u,32769u,UINT32_MAX})
    { auto t=Setup();auto selected=Handles();selected.hit=handle;assert(!R::Observe(t,Image,Image+R::DispatchReturnRva,List4,Scope(),Receipt(),selected,repeated,&reason)&&reason==R::Failure::TextureSource); }
    h=Setup();auto receipt=Receipt();receipt.callerRva=C::UploadReturnRvas[1];
    assert(R::Observe(h,Image,Image+R::DispatchReturnRva,List4,Scope(),receipt,Handles(),repeated));
    assert(R::FailureName(R::Failure::None)=="observed");
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-ray-bindings-") as directory:
            source = Path(directory) / "test.cpp"
            source.write_text(harness)
            for optimization in ("-O0", "-O3"):
                binary = Path(directory) / optimization[1:]
                result = subprocess.run([compiler, "-std=c++20", optimization, "-Wall", "-Wextra", "-Werror",
                                         "-I", str(HEADER.parent), str(source), "-o", str(binary)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([str(binary)], check=True)

    def test_code_manifest_matches_exact_installed_executable(self):
        executable = Path('/home/synse/Games/Heroic/Games/Cyberpunk 2077/bin/x64/Cyberpunk2077.exe')
        if not executable.is_file():
            self.skipTest('Exact installed executable fixture unavailable')
        data = executable.read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(), 'a7de82945c03e041fc7339fcf9066224d98db2f5d80fea50f7947bb350a60991')
        pe = struct.unpack_from('<I', data, 0x3c)[0]
        count, optional = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
        sections = [struct.unpack_from('<IIII', data, pe + 24 + optional + i * 40 + 8) for i in range(count)]
        entries = re.findall(r'\{ (0x[0-9a-f]+), (0x[0-9a-f]+), "([0-9a-f]{64})" \}', HEADER.read_text())
        self.assertEqual([int(entry[0], 16) for entry in entries], [0x2934058, 0x2a406e4, 0x1f22e4, 0x1f3a6c, 0x153f94])
        for rva, size, digest in entries:
            rva, size = int(rva, 16), int(size, 16)
            virtual, start, _, raw = next(section for section in sections if section[1] <= rva and rva + size <= section[1] + section[0])
            offset = raw + rva - start
            self.assertEqual(hashlib.sha256(data[offset:offset + size]).hexdigest(), digest)

    def test_observer_does_not_make_ownership_or_gpu_readiness_claims(self):
        source = HEADER.read_text()
        for unavailable in ('No COM ownership', 'graph alias lease', 'immutable GPU CBV contents', 'producer',
                            'List and List4 interface', 'diagnostic read bound'):
            self.assertIn(unavailable, source)
        for forbidden in ('AddRef(', 'Release(', 'ResourceBarrier(', 'DispatchRays(', 'GetDesc(', 'reinterpret_cast'):
            self.assertNotIn(forbidden, source)


if __name__ == '__main__':
    unittest.main()

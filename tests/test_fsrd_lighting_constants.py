"""Actual original lighting CB6 CPU receipt and descriptor correspondence."""
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
HEADER = ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkLightingConstants.h"
EXE = Path("/home/synse/Games/Heroic/Games/Cyberpunk 2077/bin/x64/Cyberpunk2077.exe")


class LightingConstants(unittest.TestCase):
    def test_exact_code_manifest_and_upload_caller(self):
        if not EXE.is_file():
            self.skipTest("Authenticated local executable unavailable")
        data = EXE.read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         "a7de82945c03e041fc7339fcf9066224d98db2f5d80fea50f7947bb350a60991")
        nt = struct.unpack_from("<I", data, 0x3c)[0]
        optional = nt + 24
        section_offset = optional + struct.unpack_from("<H", data, nt + 20)[0]
        sections = [struct.unpack_from("<IIII", data, section_offset + i * 40 + 8)
                    for i in range(struct.unpack_from("<H", data, nt + 6)[0])]

        def read(rva, size):
            for _, va, raw_size, raw_pointer in sections:
                if va <= rva and rva + size <= va + raw_size:
                    return data[raw_pointer + rva - va:raw_pointer + rva - va + size]
            self.fail(f"Unmapped range {rva:x}+{size:x}")

        pdata_rva, pdata_size = struct.unpack_from("<II", data, optional + 112 + 3 * 8)
        functions = {start: end for start, end, _ in struct.iter_unpack("<III", read(pdata_rva, pdata_size))}
        ranges = re.findall(r'\{ (0x[0-9a-f]+), (0x[0-9a-f]+), "([0-9a-f]{64})" \}', HEADER.read_text())
        self.assertEqual([int(start, 16) for start, _, _ in ranges],
                         [0xad2254, 0x1f0114, 0x1f3978, 0x1f405c, 0x1ee438, 0x1f22e4])
        for start_text, size_text, expected in ranges:
            start, size = int(start_text, 16), int(size_text, 16)
            self.assertEqual(hashlib.sha256(read(start, size)).hexdigest(), expected)
            self.assertEqual(start + size, functions.get(start))
        self.assertEqual(read(0x155453, 9), bytes.fromhex("48 8d 55 20 b9 f0 00 00 00"))
        self.assertEqual(0x1554a1 + struct.unpack("<i", read(0x15549d, 4))[0], 0xad2254)
        self.assertEqual(read(0xad228b, 6), bytes.fromhex("41 b9 06 00 00 00"))
        self.assertEqual(read(0x1f01a0, 6), bytes.fromhex("ff 90 88 00 00 00"))

    def test_actual_receipt_scope_lifetime_binding_and_refusals(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to compile the actual portable receipt helper")
        harness = r'''
#include <cassert>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>
#include "FSRDCyberpunkLightingConstants.h"
namespace C=FSRD::CyberpunkLightingConstants;
constexpr uintptr_t Image=0x140000000, Context=0x100000, View=0x200000,
    Tls=0x300000, Engine=0x400000, List=0x500000, Cache=0x600000,
    Layout=0x700000, Descriptors=0x800000, Source=0x900000,
    Descriptor=0xa00000, Pso=0xb00000;
constexpr C::Scope Scope{1,2,Context,View,Tls,Engine,List};
struct Host
{
    std::map<uintptr_t,std::vector<unsigned char>> memory;
    uintptr_t throwing=0;
    template<class T> void Put(uintptr_t address,T value)
    { auto& bytes=memory[address];bytes.resize(sizeof(T));std::memcpy(bytes.data(),&value,sizeof(T)); }
    bool Read(uintptr_t address,void* output,size_t bytes)
    {
        if(address==throwing)throw std::runtime_error("bounded read failure");
        auto it=memory.find(address);
        if(it==memory.end()||it->second.size()!=bytes)return false;
        std::memcpy(output,it->second.data(),bytes);return true;
    }
};
std::array<uint8_t,16> Range(uint32_t base=3,uint16_t first=5,uint16_t count=2)
{
    std::array<uint8_t,16> r{};
    std::memcpy(r.data()+4,&base,4);std::memcpy(r.data()+8,&first,2);std::memcpy(r.data()+10,&count,2);
    r[13]=1;r[14]=12;return r;
}
Host Setup()
{
    Host h;h.Put<uint8_t>(Tls+0x14,1);h.Put<uintptr_t>(Tls+0x188,Engine);
    h.Put<uintptr_t>(Engine+0x30,List);h.Put<uintptr_t>(Engine+0x60,Cache);
    h.Put<uintptr_t>(Engine+0xa0,Descriptor);h.Put<uintptr_t>(Engine+0x3d0,Pso);
    h.Put<uintptr_t>(Context+0x18,View);h.Put<uintptr_t>(Cache+0x68,Layout);
    h.Put<uintptr_t>(Cache+0x28,Descriptors);h.Put<uint8_t>(Layout+0x47b,7);
    h.Put(Layout+0x38+7*16,Range());h.Put<uint64_t>(Layout+8,1ull<<7);h.Put<uint64_t>(Layout,0);
    h.Put<uint64_t>(Cache+0x70,0);h.Put<uint64_t>(Cache+0x78,0);h.Put<uintptr_t>(Descriptors+4*8,Descriptor);
    std::array<uint32_t,60> words{};for(uint32_t i=0;i<60;++i)words[i]=0x10000000+i;
    // Preserve arbitrary bit patterns, including NaNs: this is not float math.
    words[36]=0x7fc01234;words[37]=0xffffffff;words[41]=1;words[54]=0;
    h.Put(Source,words);return h;
}
C::Receipt Uploaded(Host& h,bool witness=false)
{
    C::Receipt r;assert(C::Begin(h,Image,Image+C::UploadReturnRva,Scope,240,Source,r));
    assert(r.phase==C::Phase::Pending&&r.words[36]==0x7fc01234);
    if(witness)C::DescriptorWrite(r,Scope,Image+C::NativeCbvReturnRva,Descriptor,0xc00000,256);
    assert(C::Complete(h,Scope,r));assert(!r.source);return r;
}
int main()
{
    static_assert(std::is_trivially_copyable_v<C::Receipt>);
    static_assert(noexcept(C::Begin(std::declval<Host&>(),0,0,Scope,240,0,std::declval<C::Receipt&>())));
    auto h=Setup();auto r=Uploaded(h);C::Binding b;
    assert(C::ObserveBound(h,Scope,Pso,r,b));assert(b.rangeIndex==7&&b.nativeRootParameter==12);
    assert(b.descriptorIndex==4&&b.descriptor==Descriptor&&!C::HasNativeDescriptorWitness(r));
    assert(r.words[37]==0xffffffff&&r.words[41]==1&&r.words[54]==0);
    auto other=Scope;++other.serial;assert(!C::ObserveBound(h,other,Pso,r,b));assert(b==C::Binding{});
    other=Scope;++other.recordingGeneration;assert(!C::ObserveBound(h,other,Pso,r,b));
    auto rejectBegin=[&](uintptr_t caller,uint32_t bytes,uintptr_t source,C::Scope scope=Scope)
    { auto t=Uploaded(h);assert(!C::Begin(h,Image,caller,scope,bytes,source,t));assert(t.phase==C::Phase::Empty); };
    rejectBegin(Image+C::UploadReturnRva+1,240,Source);rejectBegin(Image+C::UploadReturnRva,256,Source);
    rejectBegin(Image+C::UploadReturnRva,240,~uintptr_t(0)-32);other=Scope;other.serial=0;
    rejectBegin(Image+C::UploadReturnRva,240,Source,other);
    auto rejected=[&](auto change)
    { auto hh=Setup();auto rr=Uploaded(hh);change(hh);C::Binding bb;b.rangeIndex=63;
      assert(!C::ObserveBound(hh,Scope,Pso,rr,bb));assert(bb==C::Binding{}); };
    rejected([](Host& t){t.Put<uintptr_t>(Engine+0x30,List+1);});
    rejected([](Host& t){t.Put<uintptr_t>(Context+0x18,View+1);});
    rejected([](Host& t){t.Put<uintptr_t>(Engine+0x3d0,Pso+1);});
    rejected([](Host& t){t.Put<uintptr_t>(Engine+0xa0,Descriptor+1);});
    rejected([](Host& t){t.Put<uintptr_t>(Descriptors+4*8,Descriptor+1);});
    rejected([](Host& t){t.Put<uint8_t>(Layout+0x47b,64);});
    rejected([](Host& t){t.Put<uint64_t>(Layout+8,0);});
    rejected([](Host& t){t.Put<uint64_t>(Layout,1ull<<7);});
    rejected([](Host& t){t.Put<uint64_t>(Cache+0x70,1ull<<7);});
    rejected([](Host& t){t.Put<uint64_t>(Cache+0x78,1ull<<7);});
    rejected([](Host& t){auto x=Range();x[13]=2;t.Put(Layout+0x38+7*16,x);});
    rejected([](Host& t){auto x=Range();x[14]=64;t.Put(Layout+0x38+7*16,x);});
    rejected([](Host& t){t.Put(Layout+0x38+7*16,Range(65536));});
    rejected([](Host& t){t.Put(Layout+0x38+7*16,Range(0,7,1));});
    rejected([](Host& t){t.Put(Layout+0x38+7*16,Range(0,5,1));});
    rejected([](Host& t){t.throwing=Layout+0x47b;});
    assert(C::Begin(h,Image,Image+C::UploadReturnRva,Scope,240,Source,r));
    std::array<uint32_t,60> changed{};h.Put(Source,changed);
    assert(!C::Complete(h,Scope,r)&&r.phase==C::Phase::Invalid&&!r.source);
    h=Setup();r=Uploaded(h,true);assert(C::HasNativeDescriptorWitness(r));
    C::DescriptorWrite(r,Scope,Image+C::NativeCbvReturnRva,Descriptor+1,0xd00000,256);
    assert(C::HasNativeDescriptorWitness(r));
    C::DescriptorWrite(r,Scope,Image+C::NativeCbvReturnRva,Descriptor,0xd00000,256);
    assert(r.phase==C::Phase::Invalid&&!C::HasNativeDescriptorWitness(r));
    assert(C::Begin(h,Image,Image+C::UploadReturnRva,Scope,240,Source,r));
    C::DescriptorWrite(r,other,Image+C::NativeCbvReturnRva,Descriptor,0xc00000,256);
    assert(r.phase==C::Phase::Invalid);
    assert(C::Begin(h,Image,Image+C::UploadReturnRva,Scope,240,Source,r));
    C::DescriptorWrite(r,Scope,Image+C::NativeCbvReturnRva,Descriptor,0xc00001,256);
    assert(r.phase==C::Phase::Invalid);
    r=Uploaded(h);C::Invalidate(r);assert(!C::ObserveBound(h,Scope,Pso,r,b));
    h.throwing=Source;assert(!C::Begin(h,Image,Image+C::UploadReturnRva,Scope,240,Source,r));
    assert(r.phase==C::Phase::Empty);
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-lighting-constants-") as directory:
            source = Path(directory) / "test.cpp"
            source.write_text(harness)
            for optimization in ("-O0", "-O3"):
                output = Path(directory) / optimization[1:]
                result = subprocess.run([compiler, "-std=c++20", optimization, "-Wall", "-Wextra", "-Werror",
                                         "-I", str(HEADER.parent), str(source), "-o", str(output)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    unittest.main()

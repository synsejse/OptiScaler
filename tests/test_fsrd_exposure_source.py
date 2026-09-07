"""CPU-only exact selector/registry reader, using the production portable header."""
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
HEADER = ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkExposureSource.h"
EXE = Path("/home/synse/Games/Heroic/Games/Cyberpunk 2077/bin/x64/Cyberpunk2077.exe")


class ExposureSource(unittest.TestCase):
    def test_manifest_matches_authenticated_complete_bodies_and_leaf(self):
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
                         [0x775268, 0x7752b0, 0x7752f0, 0x1f5438, 0x2213bc])
        relocation_rva, relocation_size = struct.unpack_from("<II", data, optional + 112 + 5 * 8)
        relocations = []
        offset = 0
        while offset < relocation_size:
            page, size = struct.unpack("<II", read(relocation_rva + offset, 8))
            for (entry,) in struct.iter_unpack("<H", read(relocation_rva + offset + 8, size - 8)):
                if entry >> 12:
                    relocations.append(page + (entry & 4095))
            offset += size
        for start_text, size_text, expected in ranges:
            start, size = int(start_text, 16), int(size_text, 16)
            with self.subTest(rva=start_text):
                self.assertEqual(hashlib.sha256(read(start, size)).hexdigest(), expected)
                self.assertEqual(start + size, functions.get(start))
                self.assertFalse(any(start <= address < start + size for address in relocations))
        self.assertEqual(read(0x18ec810, 5), bytes.fromhex("48 8d 41 10 c3"))
        self.assertFalse(any(0x18ec810 <= address < 0x18ec815 for address in relocations))
        self.assertEqual(struct.unpack_from("<I", data, nt + 8)[0], 0x68af45ea)
        self.assertEqual(struct.unpack_from("<I", data, optional + 56)[0], 0x04efc000)

    def test_actual_header_selector_refusals_and_complete_snapshot(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to compile the actual no-call exposure selector")
        harness = r'''
#include <cassert>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>
#include "FSRDCyberpunkExposureSource.h"
namespace E=FSRD::CyberpunkExposureSource;
constexpr uintptr_t Image=0x140000000, Context=0x100000, Object=0x200000,
                    Vtable=0x300000, ModeOwner=0x400000, View=0x500000,
                    Owner=0x600000, Registry=0x700000;
constexpr uintptr_t Slot(uint32_t handle) { return Registry+0x5c0af0+(handle-1)*0xb0; }
struct Host
{
    std::map<uintptr_t,std::vector<unsigned char>> memory;
    std::vector<uintptr_t> reads;
    uintptr_t throwing=0;
    template<class T> void Put(uintptr_t address,T value)
    { auto& bytes=memory[address];bytes.resize(sizeof(T));std::memcpy(bytes.data(),&value,sizeof(T)); }
    bool Read(uintptr_t address,void* output,size_t bytes)
    {
        reads.push_back(address);
        if(address==throwing)throw std::runtime_error("bounded read failed");
        const auto it=memory.find(address);
        if(it==memory.end()||it->second.size()!=bytes)return false;
        std::memcpy(output,it->second.data(),bytes);return true;
    }
    bool WasRead(uintptr_t address)const
    { for(auto read:reads)if(read==address)return true;return false; }
};
std::array<uint8_t,8> Compact(uint32_t bytes=28,uint16_t stride=28,uint8_t kind=0x38,uint8_t raw=0xa5)
{
    return {uint8_t(bytes),uint8_t(bytes>>8),uint8_t(bytes>>16),uint8_t(bytes>>24),
            uint8_t(stride),uint8_t(stride>>8),kind,raw};
}
void Buffer(Host& h,uint32_t handle)
{
    h.Put<int32_t>(Slot(handle),2);h.Put(Slot(handle)+8,Compact());
    h.Put<uintptr_t>(Slot(handle)+0x18,0x800000+handle*0x1000);
    h.Put<uintptr_t>(Slot(handle)+0x50,0x900000+handle*0x1000);
    h.Put<uintptr_t>(Slot(handle)+0x70,0);
}
Host Setup()
{
    Host h;
    h.Put<uintptr_t>(Context,Object);h.Put<uintptr_t>(Object,Vtable);
    h.Put<uintptr_t>(Vtable+0x20,Image+E::GetterRva);h.Put(Image+E::GetterRva,E::GetterBytes);
    h.Put<uintptr_t>(Object+0xfb0,ModeOwner);h.Put<uint32_t>(ModeOwner+0x10,1);
    h.Put<uintptr_t>(Context+0x18,View);h.Put<uintptr_t>(Context+0x20,0xa00000);
    h.Put<uintptr_t>(View+0x1d80,Owner);h.Put<uint32_t>(Owner+0x18,10);
    h.Put<uint32_t>(Image+E::GlobalHandleRva,11);h.Put<uintptr_t>(Image+E::RegistryRva,Registry);
    Buffer(h,10);Buffer(h,11);return h;
}
int main()
{
    static_assert(std::is_trivially_copyable_v<E::Snapshot>);
    static_assert(noexcept(E::Observe(std::declval<Host&>(),Image,Context,std::declval<E::Snapshot&>())));
    static_assert(std::size(E::Code)==5);
    const E::Snapshot empty;
    auto rejected=[&](auto change)
    {
        auto h=Setup();change(h);E::Snapshot out;out.handle=123;
        assert(!E::Observe(h,Image,Context,out)&&out==empty);
    };
    {
        auto h=Setup();E::Snapshot out,repeat;
        assert(E::Observe(h,Image,Context,out)&&E::Observe(h,Image,Context,repeat)&&out==repeat);
        assert(out.route==E::Route::ViewOwner&&out.handle==10&&out.selectedHandleAddress==Owner+0x18);
        assert(out.image==Image&&out.graphContext==Context&&out.object==Object&&out.vtable==Vtable);
        assert(out.getterTarget==Image+E::GetterRva&&out.getterBytes==E::GetterBytes&&out.modeOwner==ModeOwner);
        assert(out.mode==1&&out.view==View&&out.contextSecondary==0xa00000&&out.exposureOwner==Owner);
        assert(out.registry==Registry&&out.slot==Slot(10)&&out.native==0x80a000&&out.descriptor==0x90a000);
        assert(out.refCount==2&&out.compact==Compact()&&out.byteCount==28&&out.stride==28);
        assert(out.memoryKind==3&&out.viewKind==8&&!out.externalSynchronization);
        assert(!h.WasRead(Image+E::GlobalHandleRva));
        h.Put(Slot(10)+8,Compact(28,28,0x38,0xa6));
        assert(E::Observe(h,Image,Context,repeat)&&out!=repeat); // Preserve even uninterpreted byte7.
        h.Put(Slot(10)+8,Compact());h.Put<int32_t>(Slot(10),3);
        assert(E::Observe(h,Image,Context,repeat)&&out!=repeat); // Retention/reuse proof is caller-owned.
    }
    for(unsigned route=0;route<5;++route)
    {
        auto h=Setup();E::Snapshot out;E::Route expected=E::Route::None;
        if(route==0){h.Put<uintptr_t>(Context,0);expected=E::Route::GlobalNoObject;}
        if(route==1){h.Put<uintptr_t>(Object+0xfb0,0);expected=E::Route::GlobalNoModeOwner;}
        if(route==2||route==3){h.Put<uint32_t>(ModeOwner+0x10,route);expected=E::Route::GlobalModeDisabled;}
        if(route==4){h.Put<uintptr_t>(View+0x1d80,0);expected=E::Route::GlobalNoExposureOwner;}
        assert(E::Observe(h,Image,Context,out)&&out.route==expected&&out.handle==11);
        assert(out.selectedHandleAddress==Image+E::GlobalHandleRva&&!h.WasRead(Owner+0x18));
        if(route<4)assert(!h.WasRead(Context+0x18)&&!h.WasRead(Context+0x20));
        if(route==0)assert(!h.WasRead(Vtable+0x20)&&!h.WasRead(Image+E::GetterRva));
    }
    for(uint32_t mode:{0u,1u,4u,0xffffffffu})
    {
        auto h=Setup();h.Put<uint32_t>(ModeOwner+0x10,mode);E::Snapshot out;
        assert(E::Observe(h,Image,Context,out)&&out.route==E::Route::ViewOwner&&out.mode==mode);
    }
    for(uint32_t handle:{0u,0x8001u,0xffffffffu})
    {
        auto h=Setup();h.Put<uint32_t>(Owner+0x18,handle);E::Snapshot out;
        assert(!E::Observe(h,Image,Context,out)&&out==empty);
        assert(!h.WasRead(Image+E::GlobalHandleRva)); // Never "repair" a selected invalid owner handle.
    }
    for(uint32_t handle:{1u,0x8000u})
    {
        auto h=Setup();h.Put<uint32_t>(Owner+0x18,handle);Buffer(h,handle);E::Snapshot out;
        assert(E::Observe(h,Image,Context,out)&&out.handle==handle&&out.slot==Slot(handle));
    }
    for(uintptr_t address:{Context,Object,Vtable+0x20,Image+E::GetterRva,Object+0xfb0,ModeOwner+0x10,
                          Context+0x18,Context+0x20,View+0x1d80,Owner+0x18,Image+E::RegistryRva,
                          Slot(10),Slot(10)+8,Slot(10)+0x18,Slot(10)+0x50,Slot(10)+0x70})
    {
        rejected([&](Host& h){h.memory.erase(address);});
        rejected([&](Host& h){h.throwing=address;});
    }
    rejected([](Host& h){h.Put<uintptr_t>(Object,0);});
    rejected([](Host& h){h.Put<uintptr_t>(Vtable+0x20,Image+E::GetterRva+1);});
    rejected([](Host& h){auto bytes=E::GetterBytes;bytes[3]=0x18;h.Put(Image+E::GetterRva,bytes);});
    rejected([](Host& h){h.Put<uintptr_t>(Context+0x18,0);});
    rejected([](Host& h){h.Put<uintptr_t>(Context+0x20,0);});
    rejected([](Host& h){h.Put<uintptr_t>(Image+E::RegistryRva,0);});
    rejected([](Host& h){h.Put<uintptr_t>(Image+E::RegistryRva,~uintptr_t(0)-16);});
    rejected([](Host& h){h.Put<uintptr_t>(Context,~uintptr_t(0)-16);h.Put<uintptr_t>(~uintptr_t(0)-16,Vtable);});
    rejected([](Host& h){h.Put<uintptr_t>(View+0x1d80,~uintptr_t(0)-16);});
    for(int32_t refs:{0,-1})rejected([&](Host& h){h.Put<int32_t>(Slot(10),refs);});
    rejected([](Host& h){h.Put<uintptr_t>(Slot(10)+0x18,0);});
    rejected([](Host& h){h.Put<uintptr_t>(Slot(10)+0x50,0);});
    rejected([](Host& h){h.Put<uintptr_t>(Slot(10)+0x70,1);});
    for(unsigned kind=0;kind<16;++kind)for(unsigned view=0;view<16;++view)
    {
        auto h=Setup();h.Put(Slot(10)+8,Compact(28,28,uint8_t((kind<<4)|view)));E::Snapshot out;
        const bool admitted=(kind==0||kind==3||kind==6)&&(view==8||view==9||view==10||view==14);
        assert(E::Observe(h,Image,Context,out)==admitted);
        if(admitted)assert(out.memoryKind==kind&&out.viewKind==view);
        else assert(out==empty);
    }
    for(uint32_t size:{0u,1u,27u,E::MaxBufferBytes+1,0xffffffffu})
        rejected([&](Host& h){h.Put(Slot(10)+8,Compact(size));});
    for(uint16_t stride:{uint16_t(0),uint16_t(4),uint16_t(27),uint16_t(29),uint16_t(65535)})
        rejected([&](Host& h){h.Put(Slot(10)+8,Compact(28,stride));});
    for(uint32_t size:{28u,29u,56u,E::MaxBufferBytes})
    {
        auto h=Setup();h.Put(Slot(10)+8,Compact(size));E::Snapshot out;
        assert(E::Observe(h,Image,Context,out)&&out.byteCount==size); // Factory truncates element count.
    }
    for(uintptr_t image:{uintptr_t(0),~uintptr_t(0),~uintptr_t(0)-E::ImageBytes+2})
    {
        auto h=Setup();E::Snapshot out;out.handle=123;
        assert(!E::Observe(h,image,Context,out)&&out==empty&&h.reads.empty());
    }
    {auto h=Setup();E::Snapshot out;assert(!E::Observe(h,Image,0,out)&&out==empty&&h.reads.empty());}
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-exposure-source-") as temporary:
            folder = Path(temporary)
            source, output = folder / "harness.cpp", folder / "test"
            source.write_text(harness)
            subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O2",
                            "-I", str(HEADER.parent), str(source), "-o", str(output)], check=True, timeout=60)
            subprocess.run([str(output)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()

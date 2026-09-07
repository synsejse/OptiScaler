"""Compile the actual bounded final-lighting owner t8 reader, without game calls."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkLightingSource.h"


class LightingSource(unittest.TestCase):
    def test_actual_header_routes_bounds_compact_and_refusals(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to compile the actual portable lighting source")
        harness = r'''
#include <cassert>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>
#include "FSRDCyberpunkLightingSource.h"
namespace S=FSRD::CyberpunkLightingSource;
constexpr uintptr_t Image=0x140000000, Context=0x100000, View=0x200000,
    Owner=0x300000, Registry=0x400000, Native=0x500000, Descriptor=0x600000;
constexpr uintptr_t Slot(uint32_t handle=12) { return Registry+0x2f1d8+uintptr_t(handle-1)*0xb0; }
struct Host
{
    std::map<uintptr_t,std::vector<uint8_t>> memory;
    std::vector<uintptr_t> reads;
    uintptr_t throwing=0;
    template<class T> void Put(uintptr_t address,T value)
    { auto& bytes=memory[address];bytes.resize(sizeof(T));std::memcpy(bytes.data(),&value,sizeof(T)); }
    bool Read(uintptr_t address,void* output,size_t bytes)
    {
        reads.push_back(address);
        if(address==throwing)throw std::runtime_error("bounded memory unavailable");
        const auto found=memory.find(address);
        if(found==memory.end()||found->second.size()!=bytes)return false;
        std::memcpy(output,found->second.data(),bytes);return true;
    }
    bool WasRead(uintptr_t address)const
    { for(auto read:reads)if(read==address)return true;return false; }
};
std::array<uint8_t,12> Compact()
{
    // Raw dimensions are diagnostic here; native GetDesc/extent validation is host-owned.
    return {0x00,0x0a,0xa0,0x05,1,0,0,0x11,1,0,0,0};
}
void Texture(Host& h,uint32_t handle)
{
    h.Put<int32_t>(Slot(handle)-8,1);h.Put<uintptr_t>(Slot(handle),Native);
    h.Put<uintptr_t>(Slot(handle)+0x30,Descriptor);h.Put<uint32_t>(Slot(handle)+0x48,0xc0);
    h.Put(Slot(handle)+0x4e,Compact());h.Put<uintptr_t>(Slot(handle)+0x68,0);
}
Host Setup(uint32_t handle=12)
{
    Host h;h.Put<uint8_t>(Context+0x30,2);h.Put<uintptr_t>(Context+0x18,View);
    h.Put<uint64_t>(View+0x17d0,1ull<<51);h.Put<uintptr_t>(View+0x1d70,Owner);
    h.Put<uint32_t>(Owner+0x26c,handle);h.Put<uintptr_t>(Image+0x3438a28,Registry);
    Texture(h,handle);return h;
}
int main()
{
    static_assert(std::is_trivially_copyable_v<S::Snapshot>);
    static_assert(noexcept(S::Observe(std::declval<Host&>(),0,0,std::declval<S::Snapshot&>())));
    const S::Snapshot empty;
    auto h=Setup();S::Snapshot out;
    assert(S::Observe(h,Image,Context,out));
    assert(out.context==Context&&out.view==View&&out.owner==Owner&&out.registry==Registry);
    assert(out.handle==12&&out.slot==Slot()&&out.native==Native&&out.descriptor==Descriptor);
    assert(out.refs==1&&out.requestedState==0xc0&&out.compact==Compact()&&!out.externalSync);
    assert(h.WasRead(Owner+0x26c)&&!h.WasRead(Owner+0x270));
    S::Snapshot repeated;assert(S::Observe(h,Image,Context,repeated)&&out==repeated);
    h.Put<int32_t>(Slot()-8,2);assert(S::Observe(h,Image,Context,repeated)&&out!=repeated);
    auto reject=[&](auto change)
    {
        auto t=Setup();change(t);S::Snapshot result=out;
        assert(!S::Observe(t,Image,Context,result)&&result==empty);
    };
    reject([](Host& t){t.Put<uint8_t>(Context+0x30,0);});
    reject([](Host& t){t.Put<uintptr_t>(Context+0x18,0);});
    reject([](Host& t){t.Put<uint64_t>(View+0x17d0,0);});
    reject([](Host& t){t.Put<uint64_t>(View+0x17d0,(1ull<<51)|(1ull<<55));});
    reject([](Host& t){t.Put<uintptr_t>(View+0x1d70,0);});
    reject([](Host& t){t.Put<uint32_t>(Owner+0x26c,0);});
    reject([](Host& t){t.Put<uint32_t>(Owner+0x26c,0x8001);});
    reject([](Host& t){t.Put<uint32_t>(Owner+0x26c,0xffffffff);});
    reject([](Host& t){t.Put<uintptr_t>(Image+0x3438a28,0);});
    reject([](Host& t){t.Put<uintptr_t>(Image+0x3438a28,UINTPTR_MAX-16);});
    reject([](Host& t){t.Put<int32_t>(Slot()-8,0);});
    reject([](Host& t){t.Put<int32_t>(Slot()-8,-1);});
    reject([](Host& t){t.Put<uintptr_t>(Slot(),0);});
    reject([](Host& t){t.Put<uintptr_t>(Slot()+0x30,0);});
    reject([](Host& t){t.Put<uint32_t>(Slot()+0x48,0x80);});
    reject([](Host& t){t.Put<uint32_t>(Slot()+0x48,0x8c0);});
    reject([](Host& t){t.Put<uintptr_t>(Slot()+0x68,1);});
    reject([](Host& t){auto c=Compact();c[4]=0;t.Put(Slot()+0x4e,c);});
    reject([](Host& t){auto c=Compact();c[4]=2;t.Put(Slot()+0x4e,c);});
    reject([](Host& t){auto c=Compact();c[5]=1;t.Put(Slot()+0x4e,c);});
    reject([](Host& t){auto c=Compact();c[6]=1;t.Put(Slot()+0x4e,c);});
    reject([](Host& t){auto c=Compact();c[7]=0x51;t.Put(Slot()+0x4e,c);});
    reject([](Host& t){auto c=Compact();c[7]=0x10;t.Put(Slot()+0x4e,c);});
    reject([](Host& t){auto c=Compact();c[8]=0;t.Put(Slot()+0x4e,c);});
    reject([](Host& t){auto c=Compact();c[8]|=4;t.Put(Slot()+0x4e,c);});
    reject([](Host& t){auto c=Compact();c[8]|=0x40;t.Put(Slot()+0x4e,c);});
    reject([](Host& t){t.memory.erase(Slot()+0x30);});
    reject([](Host& t){t.throwing=Slot()+0x4e;});
    for(uint32_t handle:{1u,0x8000u})
    { auto t=Setup(handle);assert(S::Observe(t,Image,Context,repeated)&&repeated.handle==handle&&repeated.slot==Slot(handle)); }
    h=Setup();out=repeated;assert(!S::Observe(h,0,Context,out)&&out==empty);
    out=repeated;assert(!S::Observe(h,UINTPTR_MAX,Context,out)&&out==empty);
    out=repeated;assert(!S::Observe(h,Image,0,out)&&out==empty);
    out=repeated;assert(!S::Observe(h,Image,UINTPTR_MAX-16,out)&&out==empty);
    // Unrelated view bits and preserved compact metadata do not invent another selector.
    h=Setup();h.Put<uint64_t>(View+0x17d0,(1ull<<51)|1|(1ull<<63));
    assert(S::Observe(h,Image,Context,out));
    assert(S::IsFinalBind(Image,Image+0x155cc0,5,6,1));
    assert(S::IsFinalBind(Image,Image+0x155d24,5,5,1));
    assert(!S::IsFinalBind(Image,Image+0x155cc0,5,5,1));
    assert(!S::IsFinalBind(Image,Image+0x155d24,5,6,1));
    assert(!S::IsFinalBind(Image,Image+0x155cc0,4,6,1));
    assert(!S::IsFinalBind(Image,Image+0x155cc0,5,6,0));
    assert(!S::IsFinalBind(Image,Image+0x155cc0,5,6,2));
    assert(!S::IsFinalBind(Image,Image+0x155cc1,5,6,1));
    assert(!S::IsFinalBind(0,0x155cc0,5,6,1));
    assert(!S::IsFinalBind(UINTPTR_MAX,0x155cbf,5,6,1));
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-lighting-source-") as directory:
            source = Path(directory) / "test.cpp"
            source.write_text(harness)
            for optimization in ("-O0", "-O3"):
                output = Path(directory) / optimization[1:]
                result = subprocess.run([compiler, "-std=c++20", optimization, "-Wall", "-Wextra", "-Werror",
                                         "-I", str(HEADER.parent), str(source), "-o", str(output)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([str(output)], check=True)

    def test_host_bound_use_and_cpu_constants_proof_labels(self):
        source = (HEADER.parent / "FSRDCyberpunkFogProbe.cpp").read_text()
        binder = source.split("void __fastcall HookBindTextures(", 1)[1].split("void WINAPI HookSetPso", 1)[0]
        self.assertEqual(binder.count("originalBindTextures(first, count, handles, stage)"), 1)
        self.assertIn("stage == 1 && first <= 8 && uint64_t(first) + count > 8", binder)
        self.assertLess(binder.index("current->t8BindObserved = false"), binder.index("originalBindTextures("))
        self.assertIn("current->t8BindCalls == 1", binder)
        self.assertIn("IsFinalBind(", binder)
        self.assertIn("selected == repeated", binder)
        prepare = source.split("void PrepareLightingT8(", 1)[1].split("bool SameExposureSource", 1)[0]
        self.assertIn('"RayTracing_All_NRD"', prepare)
        self.assertIn("source.handle != lightingScope->t8Handle", prepare)
        self.assertLess(prepare.index("ObserveExposureBindings(plan, source.descriptor, 8, 1)"),
                        prepare.index("reinterpret_cast<ID3D12Resource*>"))
        self.assertIn("SameLightingT8Source(plan)", prepare)
        constants = source.split("void DescribeLightingConstants(", 1)[1].split("Json ObserveExposureBindings", 1)[0]
        self.assertIn('{ "gpu_payload_proven", false }', constants)
        self.assertIn("receipt.words[36]", constants)
        self.assertIn("receipt.words[37]", constants)
        self.assertIn("receipt.words[41]", constants)
        self.assertIn("receipt.words[54]", constants)


if __name__ == "__main__":
    unittest.main()

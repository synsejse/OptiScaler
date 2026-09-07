"""Exact original Fog table-restoration postcondition and native ABI adapter."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'OptiScaler/upscalers/ffx/FSRDCyberpunkFogProbe.cpp'
HEADER = SOURCE.with_name('FSRDCyberpunkFogDenoiseAccess.h')


def member(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


class FogTableRestore(unittest.TestCase):
    def test_exact_abi_authentication_and_final_postcondition(self):
        source = SOURCE.read_text().split('struct FogDepthEngineHost', 1)[1]
        adapter = member(source, 'void FlushGraphicsTables(')
        self.assertIn('void(__fastcall*)(void*, void*, bool)', adapter)
        self.assertIn('reinterpret_cast<void*>(cache), reinterpret_cast<void*>(engine), false', adapter)
        check = member(source, 'bool OriginalFogDepthTableRestored(')
        for required in ('SameFogDepthSource(plan)', 'd.rangeIndex < 64', 'range == d.range',
                         'rangeIndex == d.rangeIndex', '0x70, dirty70', '0x78, dirty78'):
            self.assertIn(required, check)
        for forbidden in ('Write', 'ResourceBarrier', 'SetGraphicsRootDescriptorTable', 'SetDescriptorHeaps'):
            self.assertNotIn(forbidden, check)
        header = HEADER.read_text()
        self.assertIn('FlushGraphicsTablesRva = 0x1f22e4', header)
        self.assertIn('{ FlushGraphicsTablesRva, 0x4dc, "f4a5782e0cead125409e02ce0ff209aa5468564dc286cd1403798a1bb18f8bfc" }', header)
        self.assertIn('{ 0x1e3d3ae, 0xd3, "d3e50b16ded932a6705c42e857498792ca5f92b31694cc70699ed9049b160c35" }', header)
        record = header.split('Result RecordPrivateCompute', 1)[1]
        self.assertLess(record.index('host.FlushGraphicsTables('), record.index('host.OriginalFogDepthTableRestored('))
        self.assertLess(record.index('host.OriginalFogDepthTableRestored('), record.index('result.bindingsRestored = true'))

    def test_installed_authenticated_native_hot_and_cold_body_fixture(self):
        executable = Path('/home/synse/Games/Heroic/Games/Cyberpunk 2077/bin/x64/Cyberpunk2077.exe')
        if not executable.is_file():
            self.skipTest('Optional installed SHA-authenticated Cyberpunk executable fixture unavailable')
        with executable.open('rb') as stream:
            # Exact current executable .text raw offset is RVA - 0xc00.
            for rva, size, digest in (
                (0x1f22e4, 0x4dc, 'f4a5782e0cead125409e02ce0ff209aa5468564dc286cd1403798a1bb18f8bfc'),
                (0x1e3d3ae, 0xd3, 'd3e50b16ded932a6705c42e857498792ca5f92b31694cc70699ed9049b160c35'),
            ):
                stream.seek(rva - 0xc00)
                self.assertEqual(hashlib.sha256(stream.read(size)).hexdigest(), digest)

    def test_actual_host_members_O0_O3_fast(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX to compile actual host methods')
        source = SOURCE.read_text().split('struct FogDepthEngineHost', 1)[1]
        methods = member(source, 'void FlushGraphicsTables(') + '\n' + member(source, 'bool OriginalFogDepthTableRestored(')
        harness = r'''
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>
#ifndef __fastcall
#define __fastcall
#endif
constexpr uintptr_t Cache=0x1000, Engine=0x2000, Layout=0x3000;
struct Snapshot {
 uintptr_t cache=Cache,layout=Layout;
 struct { uintptr_t engine=Engine; } scope;
 uint8_t rangeIndex=7;
 std::array<uint8_t,16> range{};
};
struct Plan { Snapshot depthSnapshot; };
bool sameSource=true;
bool SameFogDepthSource(const Plan&)noexcept{return sameSource;}
std::map<uintptr_t,std::vector<unsigned char>> memory;
uintptr_t failedRead=0;
unsigned reads=0,flushes=0;
template<class T>void Put(uintptr_t address,const T& value){
 auto& bytes=memory[address];bytes.resize(sizeof(value));std::memcpy(bytes.data(),&value,sizeof(value));
}
template<class T>bool ReadEarlyAt(uintptr_t base,uintptr_t offset,T& value)noexcept{
 ++reads;const uintptr_t address=base+offset;
 auto found=memory.find(address);
 if(address==failedRead||found==memory.end()||found->second.size()!=sizeof(value))return false;
 std::memcpy(&value,found->second.data(),sizeof(value));return true;
}
void NativeFlush(void* cache,void* engine,bool compute)noexcept{
 assert(uintptr_t(cache)==Cache&&uintptr_t(engine)==Engine&&!compute);++flushes;
}
struct Host { Plan plan;
METHODS
};
void Setup(Host& host){
 host=Host{};memory.clear();sameSource=true;failedRead=0;reads=0;
 auto& d=host.plan.depthSnapshot;d.range[13]=1;d.range[14]=3;
 Put(Layout+0x5c3,d.rangeIndex);Put(Layout+0x38+uintptr_t(d.rangeIndex)*16,d.range);
 Put(Cache+0x70,uint64_t(0));Put(Cache+0x78,uint64_t(0));
}
int main(){
 Host host;Setup(host);
 host.FlushGraphicsTables(uintptr_t(&NativeFlush),Cache,Engine);assert(flushes==1);
 assert(host.OriginalFogDepthTableRestored(Cache,Engine));
 // Each dirty mask independently prevents raw original-draw resumption.
 for(uintptr_t offset:{0x70,0x78}){
  Setup(host);Put(Cache+offset,uint64_t(1)<<7);assert(!host.OriginalFogDepthTableRestored(Cache,Engine));
  // Neighbor range dirtiness is not an invented all-stage rejection.
  Put(Cache+offset,uint64_t(1)<<8);assert(host.OriginalFogDepthTableRestored(Cache,Engine));
 }
 for(unsigned bad=0;bad<7;++bad){
  Setup(host);auto& d=host.plan.depthSnapshot;uintptr_t cache=Cache,engine=Engine;
  switch(bad){
   case 0:++cache;break;case 1:++engine;break;
   case 2:d.rangeIndex=64;break;
   case 3:sameSource=false;break;
   case 4:Put(Layout+0x5c3,uint8_t(8));break;
   case 5:{auto changed=d.range;++changed[14];Put(Layout+0x38+uintptr_t(d.rangeIndex)*16,changed);break;}
   case 6:++d.layout;break;
  }
  assert(!host.OriginalFogDepthTableRestored(cache,engine));
  if(bad<4)assert(reads==0);
 }
 for(uintptr_t address:{Layout+0x5c3,Layout+0x38+7*16,Cache+0x70,Cache+0x78}){
  Setup(host);failedRead=address;assert(!host.OriginalFogDepthTableRestored(Cache,Engine));
  failedRead=0;memory.erase(address);assert(!host.OriginalFogDepthTableRestored(Cache,Engine));
 }
 // The highest valid range uses a defined 64-bit shift and its saved map.
 Setup(host);auto& d=host.plan.depthSnapshot;d.rangeIndex=63;
 Put(Layout+0x5c3,d.rangeIndex);Put(Layout+0x38+63*16,d.range);
 assert(host.OriginalFogDepthTableRestored(Cache,Engine));
 Put(Cache+0x78,uint64_t(1)<<63);assert(!host.OriginalFogDepthTableRestored(Cache,Engine));
}
'''.replace('METHODS', methods)
        with tempfile.TemporaryDirectory(prefix='fsrd-fog-table-restore-') as temporary:
            test = Path(temporary) / 'test.cpp'
            test.write_text(harness)
            for flags in (['-O0'], ['-O3', '-ffast-math']):
                binary = Path(temporary) / 'test'
                subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags,
                                str(test), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()

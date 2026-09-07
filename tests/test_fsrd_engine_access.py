"""Compile the isolated scoped engine protocol; mocks never call the game/GPU."""
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
HEADER = ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkEngineAccess.h"
EXE = Path("/home/synse/Games/Heroic/Games/Cyberpunk 2077/bin/x64/Cyberpunk2077.exe")


class EngineAccess(unittest.TestCase):
    def test_manifest_matches_full_authenticated_bodies_when_fixture_available(self):
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
        leaf_ends = {0x1f719c: 0x1f71c9, 0x911bdc: 0x911c2e}
        ranges = re.findall(r'\{ (0x[0-9a-f]+), (0x[0-9a-f]+), "([0-9a-f]{64})" \}', HEADER.read_text())
        self.assertEqual(len(ranges), 11)
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
                self.assertEqual(start + size, leaf_ends.get(start, functions.get(start)))
                self.assertFalse(any(start <= address < start + size for address in relocations))

    def test_compiled_scope_checks_engine_order_and_restoration(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to compile the actual isolated engine protocol")
        harness = r'''
#include <cassert>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "FSRDCyberpunkEngineAccess.h"
namespace E=FSRD::CyberpunkEngineAccess;
constexpr uintptr_t Image=0x140000000, Tls=0x100000, Context=0x200000,
                    Registry=0x300000, List=0x400000, Pso=0x500000, Cache=0x600000;
struct Host
{
    std::map<uintptr_t,std::vector<unsigned char>> memory;
    std::vector<std::string> calls;
    std::vector<uint32_t> states;
    uint32_t thread=123;
    unsigned codeCount=0, codeFailure=99, getters=0;
    bool imageOk=true, scopeOk=true, direct=true, tlsOk=true;
    uintptr_t tls=Tls, currentList=List, actualPso=Pso;
    bool loseOnFlush=false;
    bool readonlyDepth=false;
    unsigned readonlyChecks=0;
    template<class T> void Put(uintptr_t address,T value)
    { auto& bytes=memory[address]; bytes.resize(sizeof(T)); std::memcpy(bytes.data(),&value,sizeof(T)); }
    bool Read(uintptr_t address,void* output,size_t bytes) noexcept
    {
        auto it=memory.find(address);
        if(it==memory.end()||it->second.size()!=bytes) return false;
        std::memcpy(output,it->second.data(),bytes); return true;
    }
    bool ExactImageAuthenticated(uintptr_t image,uintptr_t size,uint32_t stamp,std::string_view sha) noexcept
    { assert(image==Image&&size==E::ImageBytes&&stamp==E::ImageTimestamp&&sha==E::ExeSha256);return imageOk; }
    bool LiveCodeMatches(uintptr_t image,const E::CodeRange& code) noexcept
    {
        assert(image==Image&&code.rva==E::Code[codeCount].rva&&code.sha256.size()==64);
        return codeCount++!=codeFailure;
    }
    bool IsAdmittedFogScope(uint64_t scope,uintptr_t list,uintptr_t pso,
                           const std::array<E::TextureBorrow,4>&) noexcept
    { return scopeOk&&scope==42&&list==List&&pso==Pso; }
    bool IsReadOnlyDepthAliasAdmitted(const E::TextureBorrow& texture) noexcept
    { ++readonlyChecks;return readonlyDepth&&texture.handle==4&&texture.native==0x703000; }
    uint32_t ThreadId() noexcept { return thread; }
    bool ReadTlsSlotZero(uintptr_t& result) noexcept { result=tls;return tlsOk; }
    bool ListIsDirect(uintptr_t list) noexcept { assert(list==List);return direct; }
    uintptr_t CurrentNativeList(uintptr_t address) noexcept
    { assert(address==Image+E::GetCurrentListRva);++getters;return currentList; }
    void RequestState(uintptr_t address,uintptr_t context,uint32_t handle,uint32_t state,uint32_t subresource) noexcept
    {
        assert(address==Image+E::RequestStateRva&&context==Context&&subresource==0xffffffff);
        assert(state==((readonlyDepth&&handle==4)?0xe0u:0xc0u));states.push_back(state);
        calls.push_back("read"+std::to_string(handle));
    }
    void Flush(uintptr_t address,uintptr_t context) noexcept
    { assert(address==Image+E::FlushRva&&context==Context);calls.push_back("flush");if(loseOnFlush)scopeOk=false; }
    void Reenter(uintptr_t address,uintptr_t list) noexcept
    { assert(address==Image+E::ReenterRva&&list==List);calls.push_back("reentry"); }
    void RestorePso(uintptr_t list,uintptr_t pso) noexcept
    { assert(list==List&&pso==Pso);actualPso=pso;calls.push_back("pso"); }
};
E::Input Setup(Host& h)
{
    h.Put<uint8_t>(Tls+0x14,1);h.Put<uintptr_t>(Tls+0x188,Context);
    h.Put<uintptr_t>(Context+0x30,List);h.Put<uintptr_t>(Context+0x60,Cache);
    h.Put<uint32_t>(Context+0x68,3);h.Put<uintptr_t>(Context+0x3d0,Pso);
    h.Put<uintptr_t>(Image+E::RegistryRva,Registry);
    E::Input in;in.image=Image;in.list=List;in.originalPso=Pso;in.originalFogScope=42;
    for(unsigned i=0;i<4;++i)
    {
        in.textures[i]={i+1,uintptr_t(0x700000+i*0x1000)};
        const auto slot=Registry+0x2f1d8+i*0xb0;
        h.Put<int32_t>(slot-8,6);h.Put<uintptr_t>(slot,in.textures[i].native);
        h.Put<uint8_t>(slot+0x56,0);h.Put<uintptr_t>(slot+0x68,0);
    }
    return in;
}
int main()
{
    auto rejected=[](auto alter)
    {
        Host h;auto in=Setup(h);alter(h,in);unsigned callbacks=0;
        const auto result=E::RecordPrivateCompute(h,in,[&]{++callbacks;return true;});
        assert(result.outcome==E::Outcome::Refused&&!result.requestsIssued&&!result.callbackEntered);
        assert(!callbacks&&h.calls.empty());
    };
    for(unsigned i=0;i<11;++i)rejected([i](Host& h,E::Input&){h.codeFailure=i;});
    rejected([](Host& h,E::Input&){h.imageOk=false;});
    rejected([](Host& h,E::Input&){h.scopeOk=false;});
    rejected([](Host& h,E::Input&){h.Put<uint8_t>(Tls+0x14,0);});
    rejected([](Host& h,E::Input&){h.tlsOk=false;});
    rejected([](Host& h,E::Input&){h.Put<uintptr_t>(Context+0x3d0,Pso+1);});
    rejected([](Host& h,E::Input&){h.currentList=List+1;});
    rejected([](Host& h,E::Input&){h.direct=false;});
    rejected([](Host& h,E::Input&){h.Put<int32_t>(Registry+0x2f1d0,0);});
    rejected([](Host& h,E::Input&){h.Put<int32_t>(Registry+0x2f1d0,-1);});
    rejected([](Host& h,E::Input&){h.Put<uintptr_t>(Registry+0x2f1d8,1);});
    rejected([](Host& h,E::Input&){h.Put<uint8_t>(Registry+0x2f1d8+0x56,0x40);});
    rejected([](Host& h,E::Input&){h.Put<uintptr_t>(Registry+0x2f1d8+0x68,1);});
    rejected([](Host& h,E::Input&){h.Put<uint32_t>(Context+0x68,4);h.Put<uint8_t>(Registry+0x1a8e988,1);});
    rejected([](Host&,E::Input& in){in.textures[0].handle=0;});
    rejected([](Host&,E::Input& in){in.textures[0].handle=0x8001;});
    rejected([](Host&,E::Input& in){in.image=~uintptr_t(0);});
    rejected([](Host&,E::Input& in){in.preserveReadOnlyDepth=true;});
    rejected([](Host& h,E::Input& in){in.preserveReadOnlyDepth=true;h.readonlyDepth=true;in.textures[3].handle=3;});
    { Host h;auto in=Setup(h);h.Put<uint8_t>(Tls+0x14,0);
      E::RecordPrivateCompute(h,in,[]{return true;});assert(!h.getters); }
    for(unsigned mode=0;mode<3;++mode)
    {
        Host h;auto in=Setup(h);
        const auto result=E::RecordPrivateCompute(h,in,[&]() -> bool {
            h.calls.push_back("private");h.actualPso=123;
            if(mode==2)throw std::runtime_error("partial recording failed");
            return mode==0;
        });
        assert(result.outcome==(mode==0?E::Outcome::PrivateRecordedRestored:E::Outcome::PrivateFailedRestored));
        assert(result.requestsIssued==4&&result.callbackEntered&&result.bindingsRestored);
        assert(h.actualPso==Pso&&h.getters==1&&h.codeCount==11);
        assert((h.calls==std::vector<std::string>{"read1","read2","read3","read4","flush","private","reentry","pso"}));
        assert((h.states==std::vector<uint32_t>{0xc0,0xc0,0xc0,0xc0}));
    }
    { Host h;auto in=Setup(h);h.loseOnFlush=true;
      const auto result=E::RecordPrivateCompute(h,in,[]{assert(false);return true;});
      assert(result.outcome==E::Outcome::ScopeLostBeforePrivate&&result.requestsIssued==4&&!result.callbackEntered); }
    { Host h;auto in=Setup(h);
      const auto result=E::RecordPrivateCompute(h,in,[&]{h.thread=999;return true;});
      assert(result.outcome==E::Outcome::ScopeLostAfterPrivate&&!result.bindingsRestored);
      assert(h.calls.back()=="flush"); } // Never reenter a different TLS context.
    { Host h;auto in=Setup(h);h.Put<uint32_t>(Context+0x68,4);h.Put<uint8_t>(Registry+0x1a8e988,0);
      const auto result=E::RecordPrivateCompute(h,in,[]{return true;});
      assert(result.outcome==E::Outcome::PrivateRecordedRestored); }
    { Host h;auto in=Setup(h);in.preserveReadOnlyDepth=true;h.readonlyDepth=true;
      const auto result=E::RecordPrivateCompute(h,in,[]{return true;});
      assert(result.outcome==E::Outcome::PrivateRecordedRestored&&h.readonlyChecks>=4);
      assert((h.states==std::vector<uint32_t>{0xc0,0xc0,0xc0,0xe0})); }
    { Host h;auto in=Setup(h);in.preserveReadOnlyDepth=true;h.readonlyDepth=true;
      const auto result=E::RecordPrivateCompute(h,in,[&]{h.readonlyDepth=false;return true;});
      assert(result.outcome==E::Outcome::ScopeLostAfterPrivate&&!result.bindingsRestored); }
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-engine-access-") as name:
            directory = Path(name)
            source, binary = directory / "test.cpp", directory / "test"
            source.write_text(harness)
            process = subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", str(source),
                                      "-I", str(HEADER.parent), "-o", str(binary)],
                                     capture_output=True, text=True)
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()

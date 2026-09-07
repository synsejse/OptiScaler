"""Bounded original ray CPU upload receipt; does not establish GPU payload or readiness."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkRayConstants.h"


class RayConstants(unittest.TestCase):
    def test_host_original_calls_authentication_and_bounded_publication(self):
        text = (HEADER.parent / "FSRDCyberpunkFogProbe.cpp").read_text()
        node = text.split("void __fastcall HookRayNode(", 1)[1].split(
            "FSRD::CyberpunkRayConstants::Scope CurrentRayConstantScope()", 1)[0]
        self.assertEqual(node.count("originalRayNode(node, context);"), 1)
        self.assertIn("Invalidate(parent->receipt)", node)
        self.assertIn("~Restore() { rayScope = previous; }", node)
        # Optional missing-copy-endpoint diagnostics are after the original,
        # never a wrapper that could swallow or replay the game callback.
        self.assertLess(node.index("originalRayNode(node, context);"), node.index("Metadata("))
        self.assertNotIn("catch", node)
        upload = text.split("void __fastcall HookUploadRayConstants(", 1)[1].split(
            "FSRD::CyberpunkLightingConstants::Scope CurrentLightingConstantScope()", 1)[0]
        self.assertEqual(upload.count("originalUploadRayConstants(bytes, source);"), 1)
        positions = [upload.index(value) for value in (
            "CyberpunkRayConstants::Begin(", "originalUploadRayConstants(bytes, source);",
            "CyberpunkRayConstants::Complete(", "data.rayConstants.push_back(current->receipt)")]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("});\n    }\n    originalUploadRayConstants(bytes, source);", upload)
        self.assertIn("lightingRequested.load() && !lightingAttempted.load()", upload)
        self.assertIn("if (data.rayConstants.size() == 4)", upload)
        self.assertIn("data.rayConstants.erase(data.rayConstants.begin())", upload)
        self.assertIn("current->receipt.phase == FSRD::CyberpunkRayConstants::Phase::Pending", upload)
        self.assertIn("Invalidate(current->receipt)", upload)

        initialization = text.split("void Initialize(bool enabled)", 1)[1].split("void HookDevice(", 1)[0]
        authenticate = initialization.index("std::begin(FSRD::CyberpunkRayConstants::Code)")
        assign = initialization.index("image + FSRD::CyberpunkRayConstants::UploadRva")
        transaction = initialization.index("DetourTransactionBegin()")
        attach = initialization.index("DetourAttach(reinterpret_cast<PVOID*>(&originalUploadRayConstants)")
        self.assertLess(authenticate, assign)
        self.assertLess(assign, transaction)
        self.assertLess(transaction, attach)
        failure = initialization.split("if (error != NO_ERROR)", 1)[1]
        self.assertIn("originalRayNode = nullptr", failure)
        self.assertIn("originalUploadRayConstants = nullptr", failure)

        # Live EngineAccess hashes are checked after hooks are installed. Neither
        # hooked body may overlap those ranges, especially the adjacent cache getter.
        engine = (HEADER.parent / "FSRDCyberpunkEngineAccess.h").read_text()
        ranges = [(int(rva, 16), int(size, 16)) for rva, size in re.findall(
            r'\{ (0x[0-9a-f]+), (0x[0-9a-f]+), "[0-9a-f]{64}" \}', engine)]
        self.assertIn((0x1ee438, 0x37), ranges)
        for hook_start, hook_bytes in ((0xc6a3b4, 0x1a37), (0x1ee3cc, 0x6a)):
            for start, size in ranges:
                self.assertFalse(hook_start < start + size and start < hook_start + hook_bytes)
        for path in (ROOT / "OptiScaler/OptiScaler.vcxproj", ROOT / "OptiScaler/OptiScaler.vcxproj.filters"):
            self.assertEqual(path.read_text().count(
                'ClInclude Include="upscalers\\ffx\\FSRDCyberpunkRayConstants.h"'), 1)

    def test_host_frame_source_is_bounded_cpu_metadata_not_gpu_authority(self):
        text = (HEADER.parent / "FSRDCyberpunkFogProbe.cpp").read_text()
        scope = text.split("FSRD::CyberpunkRayConstants::Scope CurrentRayConstantScope()", 1)[1].split(
            "void __fastcall HookUploadRayConstants(", 1)[0]
        for value in ("0x48, 0x8d, 0x41, 0x10, 0xc3", "authenticatedImage.load() + 0x18ec810",
                      "ReadEarlyAt(vtable, 0x20, getter)", "ReadEarly(getter, bytes)",
                      "ReadEarlyAt(object, 0x1b0, result.frameSource)",
                      "ReadEarlyAt(object, 0x1b0, repeatedFrame)",
                      "repeatedFrame != result.frameSource", "found->second.generation"):
            self.assertIn(value, scope)
        self.assertNotRegex(scope, r"\bgetter\s*\(")
        self.assertNotIn("slGetNewFrameToken", scope)
        self.assertNotIn("originalUpload", scope)
        candidates = text.split('plan->provenance["ray_cpu_upload_candidates"]', 1)[1].split(
            'plan->provenance["current_inputs"]', 1)[0]
        for value in ('"gpu_payload_proven", false', '"dispatch_binding_proven", false',
                      '"same_frame_pairing", "not_asserted"', '"frame_source_cpu", observed.frameSource',
                      '"words", receipt.words'):
            self.assertIn(value, candidates)
        self.assertNotIn("receipt.source", candidates)

    def test_metadata_only_and_exact_manifest(self):
        text = HEADER.read_text()
        for forbidden in ("d3d12.h", "AddRef(", "GetDesc(", "ResourceBarrier", "reinterpret_cast",
                          "GetGPUVirtualAddress", "DescriptorWrite", "0xa0, descriptor"):
            self.assertNotIn(forbidden, text)
        for expected in ("0xc6bb70", "0xc6bbd6", "PayloadBytes = 0xcd0", "0x90, descriptor",
                         "EncodingByteOffset = 1212", "WriteHitByteOffset = 3168", "0x32f7798",
                         "CPU upload receipt only", "source = 0"):
            self.assertIn(expected, text)

    def test_compiled_original_payload_and_refusals(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX for actual C++20 receipt tests")
        harness = r'''
#include <cassert>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
#include "FSRDCyberpunkRayConstants.h"
using namespace FSRD::CyberpunkRayConstants;
struct Memory {
    std::vector<unsigned char> bytes=std::vector<unsigned char>(65536);
    unsigned reads=0; uintptr_t fail=0; bool throws=false;
    template<class T> void Put(uintptr_t address,const T& value) {
        assert(address+sizeof(value)<=bytes.size());
        std::memcpy(bytes.data()+address,&value,sizeof(value));
    }
    bool Read(uintptr_t address,void* output,size_t size) {
        ++reads; if(throws) throw std::runtime_error("read");
        if(address==fail || address>=bytes.size() || size>bytes.size()-address) return false;
        std::memcpy(output,bytes.data()+address,size); return true;
    }
};
constexpr uintptr_t image=0x140000000, source=0x4000;
Scope ScopeValue() { return {7,9,0x1000,0x2000,0x2200,0x3000,0x3800,0}; }
Memory Setup(const Scope& s) {
    Memory m; m.Put(s.tls+0x14,uint8_t(1)); m.Put(s.tls+0x188,s.engine);
    m.Put(s.engine+0x30,s.list); m.Put(s.graphContext+0x18,s.view);
    m.Put(s.engine+0x60,uintptr_t(0x3900)); m.Put(s.engine+0x90,uintptr_t(0xabcdef));
    // Pixel descriptor is deliberately different: helper must observe compute+90.
    m.Put(s.engine+0xa0,uintptr_t(0x123456));
    std::array<uint32_t,PayloadBytes/4> words{};
    for(unsigned i=0;i<words.size();++i) words[i]=0x80000000u+i;
    words[EncodingByteOffset/4]=0; words[WriteHitByteOffset/4]=1;
    m.Put(source,words); return m;
}
int main() {
    const Scope s=ScopeValue();
    for(auto caller: UploadReturnRvas) {
        auto m=Setup(s); Receipt r;
        assert(Begin(m,image,image+caller,s,PayloadBytes,source,r));
        assert(r.phase==Phase::Pending && r.source==source && r.scope==s);
        assert(r.descriptor==0xabcdef && r.callerRva==caller);
        assert(r.words[EncodingByteOffset/4]==0 && r.words[WriteHitByteOffset/4]==1);
        assert(r.words[819]==0x80000333u && m.reads==7);
        assert(Complete(m,s,r) && r.phase==Phase::Uploaded && !r.source && m.reads==14);
        assert(!Complete(m,s,r) && r.phase==Phase::Invalid && !r.source);
    }
    {
        auto m=Setup(s); Receipt r;
        m.Put(source+EncodingByteOffset,uint32_t(0x12345678));
        m.Put(source+WriteHitByteOffset,uint32_t(0xffffffff));
        assert(Begin(m,image,image+UploadReturnRvas[0],s,PayloadBytes,source,r));
        assert(Complete(m,s,r)); // Preserve raw words, do not infer a boolean or absolute hit.
        assert(r.words[EncodingByteOffset/4]==0x12345678 && r.words[WriteHitByteOffset/4]==0xffffffff);
    }
    for(unsigned change=0;change<9;++change) {
        auto m=Setup(s); Receipt r;
        assert(Begin(m,image,image+UploadReturnRvas[0],s,PayloadBytes,source,r));
        Scope after=s;
        switch(change) {
        case 0: ++after.serial; break; case 1: ++after.recordingGeneration; break;
        case 2: ++after.graphContext; break; case 3: ++after.view; break;
        case 4: ++after.tls; break; case 5: ++after.engine; break;
        case 6: ++after.list; break; case 7: ++after.frameSource; break;
        case 8: m.Put(source+100,uint32_t(0));
        }
        assert(!Complete(m,after,r) && r.phase==Phase::Invalid && !r.source);
    }
    for(unsigned change=0;change<6;++change) {
        auto m=Setup(s); Receipt r;
        assert(Begin(m,image,image+UploadReturnRvas[0],s,PayloadBytes,source,r));
        switch(change) {
        case 0: m.Put(s.tls+0x14,uint8_t(0)); break;
        case 1: m.Put(s.tls+0x188,uintptr_t(1)); break;
        case 2: m.Put(s.engine+0x30,uintptr_t(1)); break;
        case 3: m.Put(s.graphContext+0x18,uintptr_t(1)); break;
        case 4: m.Put(s.engine+0x60,uintptr_t(1)); break;
        case 5: m.Put(s.engine+0x90,uintptr_t(1));
        }
        assert(!Complete(m,s,r) && r.phase==Phase::Invalid && !r.source);
    }
    for(unsigned bad=0;bad<7;++bad) {
        auto m=Setup(s); Receipt r; uintptr_t base=image,caller=image+UploadReturnRvas[0],ptr=source;
        uint32_t count=PayloadBytes;
        switch(bad) {
        case 0: base=0; break; case 1: caller=image-1; break; case 2: ++caller; break;
        case 3: --count; break; case 4: ptr=0; break;
        case 5: ptr=std::numeric_limits<uintptr_t>::max()-4; break;
        case 6: base=std::numeric_limits<uintptr_t>::max()-4; caller=2;
        }
        assert(!Begin(m,base,caller,s,count,ptr,r) && r.phase==Phase::Empty && !r.source);
    }
    for(unsigned field=0;field<8;++field) {
        auto m=Setup(s); Receipt r; Scope bad=s;
        switch(field) {
        case 0: bad.serial=0; break; case 1: bad.recordingGeneration=0; break;
        case 2: bad.graphContext=0; break; case 3: bad.view=0; break;
        case 4: bad.tls=0; break; case 5: bad.engine=0; break; case 6: bad.list=0; break;
        case 7: bad.tls=std::numeric_limits<uintptr_t>::max()-4;
        }
        assert(!Begin(m,image,image+UploadReturnRvas[0],bad,PayloadBytes,source,r));
    }
    {
        auto m=Setup(s); Receipt r; m.throws=true;
        assert(!Begin(m,image,image+UploadReturnRvas[0],s,PayloadBytes,source,r));
        m.throws=false; assert(Begin(m,image,image+UploadReturnRvas[0],s,PayloadBytes,source,r));
        m.throws=true; assert(!Complete(m,s,r) && r.phase==Phase::Invalid && !r.source);
    }
    for(auto address:{s.tls+0x14,s.tls+0x188,s.engine+0x30,s.graphContext+0x18,
                      s.engine+0x60,s.engine+0x90,source}) {
        auto m=Setup(s); Receipt r; m.fail=address;
        assert(!Begin(m,image,image+UploadReturnRvas[0],s,PayloadBytes,source,r));
    }
    {
        auto m=Setup(s); Receipt r;
        assert(Begin(m,image,image+UploadReturnRvas[0],s,PayloadBytes,source,r));
        Invalidate(r); assert(!Complete(m,s,r) && !r.source);
        assert(Begin(m,image,image+UploadReturnRvas[0],s,PayloadBytes,source,r));
        assert(!Begin(m,image,image+1,s,PayloadBytes,source,r) && !r.source);
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-ray-constants-") as name:
            directory = Path(name)
            source = directory / "test.cpp"
            source.write_text(harness)
            for optimization in ("-O0", "-O3"):
                with self.subTest(optimization=optimization):
                    output = directory / optimization[1:]
                    compiled = subprocess.run([compiler, "-std=c++20", optimization, "-Wall", "-Wextra",
                                               "-Werror", "-pedantic", str(source), "-I", str(HEADER.parent),
                                               "-o", str(output)], capture_output=True, text=True)
                    self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                    subprocess.run([str(output)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()

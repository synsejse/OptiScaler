"""Exact lighting shader identities and bounded, owned runtime PSO association."""
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
HEADER = ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkLightingShaders.h"
SOURCE = HEADER.read_text()
CACHE = Path("/home/synse/Games/Heroic/Games/Cyberpunk 2077/engine/staticshader_final.cache")


class LightingShaders(unittest.TestCase):
    def test_identity_only_no_engine_calls_or_shader_payloads(self):
        for forbidden in ("CreateGraphicsPipelineState(", "CreatePipelineState(", "Dispatch(",
                          "ResourceBarrier(", "GetProcAddress(", "LoadLibrary", "ReadProcessMemory(",
                          "std::vector", "pso->GetCachedBlob", "Release("):
            self.assertNotIn(forbidden, SOURCE)
        self.assertIn("bool Record(", SOURCE)
        self.assertIn("HashExact&& hashExact) noexcept", SOURCE)
        self.assertIn("entry.identity.Swap(identity)", SOURCE)
        self.assertIn('"exposure_binding", "not_observed"', SOURCE)

    def test_table_matches_exact_local_cache_and_selector_records(self):
        if not CACHE.is_file():
            self.skipTest("Authenticated installed shader cache unavailable")
        data = CACHE.read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         "bff160947aba8df144200247adc39b44c26322360628d875f7c7218ad26c59ff")
        identities = re.findall(r'\{ (0x[0-9a-f]+), "([A-Za-z_]+)", ([0-9]+), "([0-9a-f]{64})" \}', SOURCE)
        self.assertEqual(len(identities), 8)

        def payload(guid):
            key, start, found = struct.pack("<Q", guid), 0, []
            while (start := data.find(key, start)) != -1:
                if data[start + 12:start + 16] == b"DXBC":
                    size = struct.unpack_from("<I", data, start + 8)[0]
                    blob = data[start + 12:start + 12 + size]
                    self.assertEqual(size, struct.unpack_from("<I", blob, 24)[0])
                    found.append(blob)
                start += 8
            self.assertEqual(len(found), 1)
            return found[0]

        for selector, name, size, sha in identities:
            with self.subTest(selector=selector, variant=name):
                key = struct.pack("<I", int(selector, 16))
                self.assertEqual(data.count(key), 1)
                record = data.index(key)
                vs_guid, ps_guid = struct.unpack_from("<QQ", data, record + 4)
                vs, ps = payload(vs_guid), payload(ps_guid)
                self.assertEqual(len(vs), 2361)
                self.assertEqual(hashlib.sha256(vs).hexdigest(),
                                 "174ce05e0a97ce65f80358b2a01bbadea4c314fb386cfac6064940a870f91a5a")
                self.assertEqual(len(ps), int(size))
                self.assertEqual(hashlib.sha256(ps).hexdigest(), sha)

    def test_compiled_registry_refusal_ownership_capacity_and_exception_safety(self):
        compiler = os.environ.get("CXX") or shutil.which("c++")
        if not compiler:
            self.skipTest("Set CXX to compile production registry")
        d3d = r'''
#pragma once
#include <cstddef>
using HRESULT=long;
#define FAILED(hr) ((hr)<0)
#define IID_PPV_ARGS(pp) 0,reinterpret_cast<void**>(pp)
struct IUnknown {
 virtual HRESULT QueryInterface(int,void**)=0;
 virtual unsigned AddRef()=0;
 virtual unsigned Release()=0;
 virtual ~IUnknown()=default;
};
struct ID3D12PipelineState: IUnknown {};
struct D3D12_SHADER_BYTECODE {const void* pShaderBytecode=nullptr;size_t BytecodeLength=0;};
'''
        wrl = r'''
#pragma once
#include <utility>
namespace Microsoft::WRL {
template<class T> class ComPtr {
 T* p=nullptr;
public:
 ComPtr()=default;
 ComPtr(const ComPtr&)=delete;
 ComPtr& operator=(const ComPtr&)=delete;
 ~ComPtr(){if(p)p->Release();}
 T* Get()const{return p;}
 explicit operator bool()const{return p!=nullptr;}
 T** operator&(){return &p;}
 void Swap(ComPtr& b){std::swap(p,b.p);}
}; }
'''
        harness = r'''
#include <cassert>
#include <functional>
#include <stdexcept>
#include <string>
#include "FSRDCyberpunkLightingShaders.h"
using namespace FSRD::CyberpunkLightingShaders;
struct Identity: IUnknown {
 unsigned refs=1;std::function<void()> onRelease;
 HRESULT QueryInterface(int,void** out)override{*out=this;AddRef();return 0;}
 unsigned AddRef()override{return ++refs;}
 unsigned Release()override{assert(refs>1);if(onRelease)onRelease();return --refs;}
};
struct Pso: ID3D12PipelineState {
 Identity id;Identity* canonical=&id;bool qiFail=false;
 HRESULT QueryInterface(int,void** out)override {
  *out=nullptr;if(qiFail)return -1;*out=canonical;canonical->AddRef();return 0;}
 unsigned AddRef()override{return canonical->AddRef();}
 unsigned Release()override{return canonical->Release();}
};
char vertexToken,pixelToken;
D3D12_SHADER_BYTECODE vs{&vertexToken,VertexBytes};
D3D12_SHADER_BYTECODE ps(size_t i){return {&pixelToken,PixelShaders[i].bytes};}
struct Hash {
 size_t i=0;unsigned calls=0;bool badVertex=false,badPixel=false,throws=false;
 std::string operator()(const void* data,size_t size) {
  ++calls;if(throws)throw std::runtime_error("read/hash failed");
  if(data==&vertexToken){assert(size==VertexBytes);return badVertex?"":std::string(VertexSha256);}
  assert(data==&pixelToken&&size==PixelShaders[i].bytes);
  return badPixel?std::string(64,'0'):std::string(PixelShaders[i].sha256);
 }
};
int main()
{
 Pso p;Hash h;
 {
  Registry r;
  assert(r.Describe(nullptr,0)["status"]=="pso_identity_unavailable");
  assert(r.Describe(&p,PixelShaders[0].selector)["status"]=="pso_not_observed");
  assert(p.id.refs==1);
  auto invalid=vs;invalid.BytecodeLength=0;
  assert(!r.Record(&p,invalid,ps(0),CreationPath::Graphics,h)&&h.calls==0);
  invalid=vs;invalid.pShaderBytecode=nullptr;
  assert(!r.Record(&p,invalid,ps(0),CreationPath::Graphics,h)&&h.calls==0);
  auto badPs=ps(0);badPs.BytecodeLength=SIZE_MAX;
  assert(!r.Record(&p,vs,badPs,CreationPath::Graphics,h)&&h.calls==0);
  badPs=ps(0);badPs.pShaderBytecode=nullptr;
  assert(!r.Record(&p,vs,badPs,CreationPath::Graphics,h)&&h.calls==0);
  assert(!r.Record(nullptr,vs,ps(0),CreationPath::Graphics,h)&&h.calls==0);
  assert(!r.Record(&p,vs,ps(0),CreationPath(99),h)&&h.calls==0);
  h.badVertex=true;assert(!r.Record(&p,vs,ps(0),CreationPath::Graphics,h)&&h.calls==1);
  h.badVertex=false;h.badPixel=true;assert(!r.Record(&p,vs,ps(0),CreationPath::Graphics,h));
  h.badPixel=false;h.throws=true;assert(!r.Record(&p,vs,ps(0),CreationPath::Graphics,h));
  h.throws=false;p.qiFail=true;assert(!r.Record(&p,vs,ps(0),CreationPath::Graphics,h));
  assert(r.Describe(&p,0)["status"]=="pso_identity_unavailable");p.qiFail=false;
  assert(p.id.refs==1);
  assert(r.Record(&p,vs,ps(0),CreationPath::Graphics,h));assert(p.id.refs==2);
  auto d=r.Describe(&p,PixelShaders[0].selector);
  assert(d["matched"]==true&&d["ps_sha256"]==PixelShaders[0].sha256&&d["vs_bytes"]==VertexBytes);
  assert(d["creation_path"]=="CreateGraphicsPipelineState"&&d["exposure_binding"]=="not_observed");
  assert(p.id.refs==2);
  assert(r.Describe(&p,0)["matched"]==false);
  assert(r.Describe(&p,PixelShaders[1].selector)["status"]=="selector_mismatch");
  // Another PSO interface pointer with same COM identity must resolve identically.
  Pso alias;alias.canonical=&p.id;
  assert(r.Describe(&alias,PixelShaders[0].selector)["matched"]==true);
  // Release re-enters Describe. This times out if any release occurs under lock.
  bool reentrant=false;unsigned releases=0;
  p.id.onRelease=[&]{++releases;if(!reentrant){reentrant=true;
    assert(r.Describe(&p,PixelShaders[0].selector)["matched"]==true);reentrant=false;}};
  assert(r.Record(&alias,vs,ps(0),CreationPath::Stream,h));
  assert(releases>0&&p.id.refs==2);p.id.onRelease={};
  // Conflicting authenticated bytecodes on one retained identity are not admissible.
  h.i=1;assert(!r.Record(&p,vs,ps(1),CreationPath::Graphics,h));
  assert(r.Describe(&p,PixelShaders[0].selector)["status"]=="conflicting_creation_observations");
  assert(r.Describe(&p,PixelShaders[1].selector)["matched"]==false);
 }
 assert(p.id.refs==1);
 std::array<Pso,Registry::MaxPsos+1> many;
 {
  Registry r;
  for(size_t i=0;i<many.size();++i){h.i=i%PixelShaders.size();
   bool accepted=r.Record(&many[i],vs,ps(h.i),CreationPath::Stream,h);
   assert(accepted==(i<Registry::MaxPsos));
   if(accepted){auto d=r.Describe(&many[i],PixelShaders[h.i].selector);
    assert(d["matched"]==true&&d["variant"]==PixelShaders[h.i].name&&d["creation_path"]=="CreatePipelineState");}}
  assert(many.back().id.refs==1);
  assert(r.Describe(&many.back(),PixelShaders[h.i].selector)["pso_capacity_reached"]==true);
  assert(r.Describe(&many.back(),PixelShaders[h.i].selector)["matched"]==false);
 }
 for(auto& q:many)assert(q.id.refs==1);
 {
  Registry r;h={};h.badVertex=true;
  for(size_t i=0;i<Registry::MaxCandidates;++i)assert(!r.Record(&p,vs,ps(0),CreationPath::Graphics,h));
  assert(h.calls==Registry::MaxCandidates);h.badVertex=false;
  assert(!r.Record(&p,vs,ps(0),CreationPath::Graphics,h)&&h.calls==Registry::MaxCandidates);
  assert(r.Describe(&p,0)["candidate_budget_exhausted"]==true&&p.id.refs==1);
 }
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-lighting-shaders-") as temp:
            target = Path(temp)
            (target / "wrl").mkdir()
            (target / "d3d12.h").write_text(d3d)
            (target / "wrl/client.h").write_text(wrl)
            (target / "test.cpp").write_text(harness)
            command = [compiler, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                       "-I", str(target), "-I", str(HEADER.parent),
                       "-I", str(ROOT / "external"), str(target / "test.cpp"), "-o", str(target / "test")]
            # Match the existing standalone test include location.
            json_header = next(ROOT.glob("external/**/json.hpp"))
            command[command.index(str(ROOT / "external"))] = str(json_header.parent)
            subprocess.run(command, check=True, capture_output=True, text=True)
            subprocess.run([str(target / "test")], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()

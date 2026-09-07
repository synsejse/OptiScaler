"""Original ray binding/DispatchRays host observation is bounded CPU metadata only."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkFogProbe.cpp").read_text()


def function(name):
    start = SOURCE.index(name + "(")
    opening = SOURCE.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[SOURCE.rfind("\n", 0, start) + 1:end]


class RayBindingsHost(unittest.TestCase):
    def test_original_calls_and_pre_detour_authentication(self):
        for hook, call in (("HookBindTextures", "originalBindTextures(first, count, handles, stage);"),
                           ("HookBindUavs", "originalBindUavs(first, count, handles);"),
                           ("HookDispatchRays", "originalDispatchRays(list, description);")):
            body = function(hook)
            self.assertEqual(body.count(call), 1)
            self.assertIn("RayBindingsArmed()", body)
            self.assertNotIn("catch", body.split(call)[1])
        self.assertIn("void(__fastcall*)(uint32_t, uint32_t, const uint32_t*)", SOURCE)
        self.assertIn("decltype(&ID3D12GraphicsCommandList4::DispatchRays)", SOURCE)
        init = function("Initialize")
        self.assertLess(init.index("std::begin(FSRD::CyberpunkRayConstants::Code)"),
                        init.index("std::begin(FSRD::CyberpunkRayBindings::Code)"))
        self.assertLess(init.index("std::begin(FSRD::CyberpunkRayBindings::Code)"),
                        init.index("DetourTransactionBegin()"))
        self.assertIn("if (originalBindTextures && std::all_of", init)
        self.assertIn("image + 0x153f94", init)
        self.assertIn("DetourAttach(reinterpret_cast<PVOID*>(&originalBindUavs), HookBindUavs)", init)
        failure = init.split("if (error != NO_ERROR)", 1)[1]
        self.assertIn("originalBindUavs = nullptr", failure)
        self.assertIn("rayBindingsAuthenticated.store(false)", failure)

    def test_optional_list4_discovery_is_not_lost_after_base_hooks(self):
        body = function("HookCommandList")
        installed = body.split("if (originalDraw)", 1)[1].split("auto** table", 1)[0]
        self.assertLess(installed.index("HookOptionalList4Metadata(list)"), installed.index("return;"))
        self.assertIn("if (error == NO_ERROR) HookOptionalList4Metadata(list);", body)
        optional = function("HookOptionalList4Metadata")
        for exact in ("list->QueryInterface(IID_PPV_ARGS(&list4))", "table4[76]", "table4[68]", "table4[69]",
                      "rayBindingsAuthenticated.load() && !originalDispatchRays", "if (needRays)",
                      "DetourAttach(reinterpret_cast<PVOID*>(&originalDispatchRays), HookDispatchRays)"):
            self.assertIn(exact, optional)
        self.assertNotIn("originalDraw", optional)
        # The parent D3D hooks call the probe before their own cached-hook return,
        # passing the unwrapped native list where available.
        hooks = (ROOT / "OptiScaler/hooks/D3D12_Hooks.cpp").read_text()
        late = hooks.split("void D3D12Hooks::HookToCommandListLate(", 1)[1]
        self.assertLess(late.index("FSRDCyberpunkFogProbe::HookCommandList(probeList)"),
                        late.index("if (s_SetComputeRootSignature.o_lateHook"))

    def test_no_gpu_actions_or_readiness_claims_and_bounded_candidates(self):
        body = function("HookDispatchRays")
        self.assertIn("current->dispatches < MaxRayDispatches", body)
        self.assertIn("data.rayDispatches.size() == MaxRayDispatches", body)
        self.assertIn("data.rayDispatches.erase(data.rayDispatches.begin())", body)
        self.assertIn("static_assert(sizeof(desc) == 104)", body)
        self.assertLess(body.index("CyberpunkRayBindings::Observe("), body.index("originalDispatchRays("))
        for forbidden in ("AddRef(", "GetDesc(", "ResourceBarrier(", "CopyTextureRegion(",
                          "RequestTextureState(", "SetPipelineState(", "GetGPUVirtualAddress("):
            self.assertNotIn(forbidden, body)
        for exact in ('"resource_readiness_proven", false', '"gpu_payload_proven", false',
                      '"same_frame_pairing", "not_asserted"', '"resource_ownership", "not_acquired"',
                      '"selected_state_object", "not_observed"', '"gpu_cbv_bytes_immutable", false',
                      "found->second.generation != observed.recordingGeneration", "found->second.predicated",
                      "found->second.renderPass", "found->second.queryCount",
                      "CurrentRayConstantScope() != observed"):
            self.assertIn(exact, body)
        node = function("HookRayNode")
        self.assertIn("InvalidateRayBinding(binding)", node)
        self.assertNotIn("Metadata(", node)
        self.assertIn('plan->provenance["ray_dispatch_candidates"] = data.rayDispatches;', SOURCE)

    def test_compiled_actual_host_receipts_and_refusal_dispatch(self):
        compiler = os.environ.get("CXX") or shutil.which("c++")
        if not compiler:
            self.skipTest("Set CXX to compile the actual host receipt bodies")
        structs = SOURCE.split("struct RayBindReceipt\n", 1)[1].split("bool ReadExactMemory(", 1)[0]
        helpers = "\n".join(function(name) for name in ("BeginRayBind", "CompleteRayBind"))
        hooks = "\n".join(function(name) for name in (
            "HookRayNode", "HookBindTextures", "HookBindUavs", "DescribeRayBinding", "HookDispatchRays"))
        harness = r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <json.hpp>
#include "FSRDCyberpunkRayBindings.h"
#define __fastcall
#define WINAPI
using Json=nlohmann::json;
thread_local bool inMetadata=false;
std::atomic<bool> rayBindingsAuthenticated{true},captureTrackingValid{true},lightingRequested{true},lightingAttempted{false};
std::atomic<uintptr_t> authenticatedImage{0x140000000};
std::atomic<uint64_t> scopes{10};
uintptr_t fakeCaller=0;
void* _ReturnAddress(){return reinterpret_cast<void*>(fakeCaller);}
struct LightingScope {
 bool t8BindObserved=false; unsigned t8BindCalls=0; uint32_t t8Handle=0,t8BindCount=0;uintptr_t t8BindCaller=0;
};
LightingScope* lightingScope=nullptr;
namespace FSRD::CyberpunkLightingSource {
bool IsFinalBind(uintptr_t,uintptr_t,uint32_t,uint32_t,uint8_t){return false;}
}
struct ID3D12GraphicsCommandList {};
struct ID3D12GraphicsCommandList4 {};
struct AddressRange {uint64_t StartAddress=0,SizeInBytes=0;};
struct AddressTable {uint64_t StartAddress=0,SizeInBytes=0,StrideInBytes=0;};
struct D3D12_DISPATCH_RAYS_DESC {
 AddressRange RayGenerationShaderRecord;AddressTable MissShaderTable,HitGroupTable,CallableShaderTable;
 uint32_t Width=1280,Height=720,Depth=1;
};
uint32_t handle=17; D3D12_DISPATCH_RAYS_DESC description;
unsigned reads=0,scopeReads=0,textureCalls=0,uavCalls=0,dispatchCalls=0,nodeCalls=0;
bool throwRead=false,failRead=false,throwScope=false;
unsigned mutateOriginal=0;
bool ReadExactMemory(uintptr_t p,void* out,size_t n) {
 ++reads;if(throwRead)throw std::runtime_error("read");if(failRead)return false;
 if((p==uintptr_t(&handle)&&n==sizeof(handle))||(p==uintptr_t(&description)&&n==sizeof(description)))
 {std::memcpy(out,reinterpret_cast<const void*>(p),n);return true;}return false;
}
template<class T>bool ReadEarly(uintptr_t p,T& out){return ReadExactMemory(p,&out,sizeof(out));}
FSRD::CyberpunkRayConstants::Scope observed{11,12,0x1000,0x2000,0x3000,0x4000,0x5000,141545};
FSRD::CyberpunkRayConstants::Scope CurrentRayConstantScope(){++scopeReads;if(throwScope)throw std::runtime_error("scope");return observed;}
struct ListState{bool known=true,predicated=false,renderPass=false;unsigned queryCount=0;uint64_t generation=12;};
struct Identity{void* value;void* Get()const{return value;}};
Identity ListIdentity(ID3D12GraphicsCommandList* list){return {list};}
struct Registry{std::mutex mutex;std::map<void*,ListState> lists;std::vector<Json> rayDispatches;};
Registry registry;Registry& Data(){return registry;}
bool originalBeginRenderPass=true,originalEndRenderPass=true;
void originalDispatchRays(ID3D12GraphicsCommandList4*,const D3D12_DISPATCH_RAYS_DESC*){++dispatchCalls;}
void originalRayNode(void*,void*){++nodeCalls;}
void originalBindTextures(uint32_t,uint32_t,const uint32_t*,uint8_t);
void originalBindUavs(uint32_t,uint32_t,const uint32_t*);
'''
        harness += "struct RayBindReceipt\n" + structs + "\n" + function("Metadata") + "\n" + helpers + "\n" + hooks
        harness += r'''
void Mutate() {
 if(mutateOriginal==1)++handle;
 if(mutateOriginal==2)++observed.recordingGeneration;
 if(mutateOriginal==3)HookRayNode(nullptr,reinterpret_cast<void*>(0x6000));
 if(mutateOriginal==4){mutateOriginal=0;HookBindTextures(4,1,&handle,2);}
 if(mutateOriginal==5)throw std::runtime_error("original");
}
void originalBindTextures(uint32_t,uint32_t,const uint32_t*,uint8_t){++textureCalls;Mutate();}
void originalBindUavs(uint32_t,uint32_t,const uint32_t*){++uavCalls;Mutate();}
void Setup(RayScope& s){
 s={};s.serial=11;s.context=reinterpret_cast<void*>(0x1000);rayScope=&s;
 observed={11,12,0x1000,0x2000,0x3000,0x4000,0x5000,141545};
 reads=scopeReads=textureCalls=uavCalls=dispatchCalls=nodeCalls=0;handle=17;
 failRead=throwRead=throwScope=false;mutateOriginal=0;
 lightingRequested=true;lightingAttempted=false;rayBindingsAuthenticated=true;captureTrackingValid=true;
 registry.rayDispatches.clear();registry.lists[reinterpret_cast<void*>(0x5000)]={};
 fakeCaller=authenticatedImage+RayBindingReturnRvas[0];
}
int main(){
 RayScope s;Setup(s);HookBindTextures(4,1,&handle,2);
 assert(textureCalls==1&&s.bindings[0].valid&&s.bindings[0].handle==17&&s.bindings[0].scope==observed);
 for(size_t i=1;i<3;++i){fakeCaller=authenticatedImage+RayBindingReturnRvas[i];HookBindUavs(RayBindingRegisters[i],1,&handle);}
 assert(uavCalls==2&&s.bindings[1].valid&&s.bindings[2].valid);
 for(unsigned mutation=1;mutation<=5;++mutation){
  Setup(s);mutateOriginal=mutation;bool threw=false;
  try{HookBindTextures(4,1,&handle,2);}catch(const std::runtime_error&){threw=true;}
  assert(textureCalls==(mutation==4?2u:1u));assert(!s.bindings[0].valid);assert(threw==(mutation==5));
  assert(rayScope==&s&&!inMetadata);
 }
 for(unsigned failure=0;failure<3;++failure){
  Setup(s);throwRead=failure==0;failRead=failure==1;throwScope=failure==2;
  HookBindTextures(4,1,&handle,2);assert(textureCalls==1&&!s.bindings[0].valid&&!inMetadata);
 }
 for(unsigned off=0;off<5;++off){
  Setup(s);
  if(off==0)lightingRequested=false;
  if(off==1)lightingAttempted=true;
  if(off==2)rayBindingsAuthenticated=false;
  if(off==3)captureTrackingValid=false;
  if(off==4)rayScope=nullptr;
  HookBindTextures(4,1,&handle,2);HookBindUavs(8,1,&handle);
  HookDispatchRays(reinterpret_cast<ID3D12GraphicsCommandList4*>(0x5100),&description);
  assert(textureCalls==1&&uavCalls==1&&dispatchCalls==1&&!reads&&!scopeReads&&registry.rayDispatches.empty());
 }
 // An earlier unrelated covering bind cannot establish a receipt; the exact call
 // may establish it once. ANY later covering write invalidates it irreversibly.
 Setup(s);fakeCaller=authenticatedImage+1;HookBindTextures(0,8,&handle,2);assert(!s.bindings[0].valid);
 fakeCaller=authenticatedImage+RayBindingReturnRvas[0];HookBindTextures(4,1,&handle,2);assert(s.bindings[0].valid);
 fakeCaller=authenticatedImage+1;HookBindTextures(4,1,nullptr,2);assert(!s.bindings[0].valid);
 fakeCaller=authenticatedImage+RayBindingReturnRvas[0];HookBindTextures(4,1,&handle,2);assert(!s.bindings[0].valid);
 Setup(s);s.bindings[0].writes=UINT32_MAX;HookBindTextures(4,1,&handle,2);assert(!s.bindings[0].valid);
 // Exceptions while observing the first of two overwritten UAV slots must not
 // leave the second stale. Each slot has a separate nonthrowing metadata guard.
 Setup(s);s.bindings[1].valid=s.bindings[2].valid=true;throwScope=true;
 fakeCaller=authenticatedImage+1;HookBindUavs(0,9,&handle);
 assert(!s.bindings[1].valid&&!s.bindings[2].valid&&uavCalls==1);
 // Compile and execute the actual API hook/JSON paths. Missing bind/cache proof
 // refuses but still forwards exactly once, preserving separate no-readiness data.
 Setup(s);fakeCaller=authenticatedImage+FSRD::CyberpunkRayBindings::DispatchReturnRva;
 for(unsigned i=0;i<12;++i)HookDispatchRays(reinterpret_cast<ID3D12GraphicsCommandList4*>(0x5100),&description);
 assert(dispatchCalls==12&&registry.rayDispatches.size()==8&&s.dispatches==8);
 assert(registry.rayDispatches[0]["status"]=="refused");
 assert(registry.rayDispatches[0]["resource_readiness_proven"]==false);
 for(unsigned failure=0;failure<3;++failure){
  Setup(s);throwRead=failure==0;failRead=failure==1;throwScope=failure==2;
  HookDispatchRays(reinterpret_cast<ID3D12GraphicsCommandList4*>(0x5100),&description);
  assert(dispatchCalls==1&&registry.rayDispatches.size()==1&&registry.rayDispatches[0]["status"]=="refused");
 }
 Setup(s);for(auto& b:s.bindings){b.valid=true;b.scope=observed;b.handle=17;}
 s.receipt.scope=observed;s.receipt.phase=FSRD::CyberpunkRayConstants::Phase::Uploaded;
 fakeCaller=authenticatedImage+FSRD::CyberpunkRayBindings::DispatchReturnRva;
 HookDispatchRays(reinterpret_cast<ID3D12GraphicsCommandList4*>(0x5100),&description);
 assert(dispatchCalls==1&&registry.rayDispatches.size()==1&&registry.rayDispatches[0]["status"]=="refused");
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-ray-host-") as temporary:
            source = Path(temporary) / "test.cpp"
            source.write_text(harness)
            for optimization in ("-O0", "-O3"):
                binary = Path(temporary) / ("test" + optimization)
                subprocess.run([compiler, "-std=c++20", optimization, "-Wall", "-Wextra", "-Werror",
                                "-I", str(ROOT / "OptiScaler/upscalers/ffx"),
                                "-I", str(ROOT / "external/nlohmann"), str(source), "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()

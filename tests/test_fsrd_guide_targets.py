"""Compile the actual guide target/factory/preparation/recording bodies with mocks.

Only runtime shader authentication is stubbed at its explicit trust boundary; its
unchanged byte/hash checks have separate source-policy coverage. These tests do not
execute the shader or establish real GPU readiness/frame/submission ordering.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'OptiScaler/upscalers/ffx'
SOURCE = (BASE / 'FSRDCyberpunkGuidePass.cpp').read_text()


class GuideTargets(unittest.TestCase):
    def test_actual_target_prepare_record_bodies(self):
        compiler = os.environ.get('CXX') or shutil.which('c++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('Set CXX for the actual guide target mock compilation')
        windows = r'''
#pragma once
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using UINT=unsigned;using UINT64=uint64_t;using SIZE_T=size_t;using HRESULT=int;
using D3D12_RESOURCE_STATES=unsigned;
#define SUCCEEDED(x) ((x)>=0)
#define IID_PPV_ARGS(x) (x)
#define LOG_WARN(...) ((void)0)
enum {D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT=256,
 DXGI_FORMAT_R8G8B8A8_UNORM=28,DXGI_FORMAT_R16G16B16A16_FLOAT=10,
 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE=0x40,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE=0x80,
 D3D12_RESOURCE_STATE_UNORDERED_ACCESS=8,D3D12_RESOURCE_STATE_GENERIC_READ=0xabc,
 D3D12_RESOURCE_DIMENSION_TEXTURE2D=3,D3D12_RESOURCE_DIMENSION_BUFFER=1,
 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS=4,D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE=8,
 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV=0,D3D12_DESCRIPTOR_HEAP_FLAG_NONE=0,
 D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE=1,D3D12_HEAP_TYPE_DEFAULT=1,D3D12_HEAP_TYPE_UPLOAD=2,
 D3D12_HEAP_FLAG_NONE=0,D3D12_DESCRIPTOR_RANGE_TYPE_SRV=0,D3D12_DESCRIPTOR_RANGE_TYPE_UAV=1,
 D3D12_ROOT_PARAMETER_TYPE_CBV=2,D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE=3,
 D3D_ROOT_SIGNATURE_VERSION_1=1,D3D12_SRV_DIMENSION_TEXTURE2D=2,D3D12_SRV_DIMENSION_BUFFER=1,
 D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING=5768,D3D12_UAV_DIMENSION_TEXTURE2D=2,
 D3D12_TEXTURE_LAYOUT_ROW_MAJOR=1,D3D12_COMMAND_LIST_TYPE_DIRECT=0,
 D3D12_RESOURCE_BARRIER_TYPE_TRANSITION=0};
constexpr UINT D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES=~UINT(0);
struct D3D12_CPU_DESCRIPTOR_HANDLE{size_t ptr=0;};
struct D3D12_GPU_DESCRIPTOR_HANDLE{uint64_t ptr=0;};
struct D3D12_RESOURCE_DESC{int Dimension=0;UINT64 Alignment=0,Width=0;UINT Height=0,DepthOrArraySize=0,MipLevels=0;
 int Format=0;struct{UINT Count=0,Quality=0;}SampleDesc;UINT Layout=0,Flags=0;};
struct D3D12_RESOURCE_ALLOCATION_INFO{UINT64 SizeInBytes;};
struct D3D12_HEAP_PROPERTIES{int Type=0,CPUPageProperty=0,MemoryPoolPreference=0;UINT CreationNodeMask=0,VisibleNodeMask=0;};
struct D3D12_DESCRIPTOR_HEAP_DESC{int Type=0;UINT NumDescriptors=0;int Flags=0;};
struct D3D12_DESCRIPTOR_RANGE{int RangeType;UINT NumDescriptors,BaseShaderRegister,RegisterSpace,OffsetInDescriptorsFromTableStart;};
struct D3D12_ROOT_PARAMETER{int ParameterType=0;struct{UINT ShaderRegister=0,RegisterSpace=0;}Descriptor;
 struct{UINT NumDescriptorRanges=0;D3D12_DESCRIPTOR_RANGE* pDescriptorRanges=nullptr;}DescriptorTable;};
struct D3D12_ROOT_SIGNATURE_DESC{UINT NumParameters=0;D3D12_ROOT_PARAMETER* pParameters=nullptr;};
struct ID3D12RootSignature;struct ID3D12PipelineState;struct ID3D12DescriptorHeap;struct ID3D12Resource;
struct D3D12_COMPUTE_PIPELINE_STATE_DESC{ID3D12RootSignature* pRootSignature=nullptr;
 struct{const void* pShaderBytecode=nullptr;size_t BytecodeLength=0;}CS;};
struct D3D12_SHADER_RESOURCE_VIEW_DESC{int Format=0,ViewDimension=0;UINT Shader4ComponentMapping=0;
 struct{UINT MipLevels=0;}Texture2D;struct{UINT NumElements=0,StructureByteStride=0;}Buffer;};
struct D3D12_UNORDERED_ACCESS_VIEW_DESC{int Format=0,ViewDimension=0;};
struct D3D12_RANGE{size_t Begin,End;};
struct D3D12_RESOURCE_BARRIER{int Type=0;struct{ID3D12Resource* pResource=nullptr;UINT Subresource=0;
 UINT StateBefore=0,StateAfter=0;}Transition;};
struct IUnknown{std::atomic<unsigned> refs=0;IUnknown* canonical=nullptr;bool identityFail=false;
 virtual ~IUnknown()=default;void AddRef(){++refs;}void Release(){assert(refs);if(!--refs)delete this;}
 HRESULT QueryInterface(IUnknown** out){if(identityFail)return -1;*out=canonical?canonical:this;(*out)->AddRef();return 0;}};
namespace Fake{inline std::atomic<unsigned> resources=0;inline std::vector<std::string> events;
 inline bool authenticate=true,retainFail=false,barrierThrow=false;inline unsigned dispatches=0;}
struct ID3D12Device:IUnknown{UINT creations=0,failCreateAt=0,descriptorCopies=0,uavs=0;
 UINT64 forcedAllocation=0;bool pipelineFail=false,mapFail=false;size_t nextHeap=4096;
 UINT GetDescriptorHandleIncrementSize(int){return 32;}
 D3D12_RESOURCE_ALLOCATION_INFO GetResourceAllocationInfo(UINT a,UINT n,const D3D12_RESOURCE_DESC* d){
 assert(a==0&&n==1);return{forcedAllocation?forcedAllocation:((d->Width*d->Height*(d->Format==10?8:4)+65535)&~UINT64(65535))};}
 HRESULT CreateCommittedResource(const D3D12_HEAP_PROPERTIES*,int,const D3D12_RESOURCE_DESC*,UINT,const void*,ID3D12Resource**);
 HRESULT CreateRootSignature(UINT,const void*,size_t,ID3D12RootSignature**);
 HRESULT CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC*,ID3D12PipelineState**);
 HRESULT CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC*,ID3D12DescriptorHeap**);
 void CopyDescriptorsSimple(UINT n,D3D12_CPU_DESCRIPTOR_HANDLE dst,D3D12_CPU_DESCRIPTOR_HANDLE src,int type){
 assert(n==1&&dst.ptr&&src.ptr&&type==0);++descriptorCopies;}
 void CreateShaderResourceView(ID3D12Resource* p,const D3D12_SHADER_RESOURCE_VIEW_DESC* d,D3D12_CPU_DESCRIPTOR_HANDLE h){
 assert(!p&&h.ptr&&d->Shader4ComponentMapping==5768);if(d->ViewDimension==1)assert(d->Buffer.StructureByteStride==28);}
 void CreateUnorderedAccessView(ID3D12Resource*,ID3D12Resource*,const D3D12_UNORDERED_ACCESS_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
};
struct ID3D12DeviceChild:IUnknown{ID3D12Device* device;bool deviceFail=false;
 explicit ID3D12DeviceChild(ID3D12Device* d):device(d){device->AddRef();}~ID3D12DeviceChild(){device->Release();}
 HRESULT GetDevice(ID3D12Device** p){if(deviceFail)return -1;*p=device;device->AddRef();return 0;}};
struct ID3D12Resource:ID3D12DeviceChild{D3D12_RESOURCE_DESC desc;UINT state=0;bool privateOutput=false;
 std::vector<std::byte> memory;explicit ID3D12Resource(ID3D12Device* d):ID3D12DeviceChild(d){++Fake::resources;}
 ~ID3D12Resource(){--Fake::resources;}D3D12_RESOURCE_DESC GetDesc(){return desc;}
 HRESULT Map(UINT i,const D3D12_RANGE* r,void** p){assert(i==0&&r->Begin==0&&r->End==0);
 if(device->mapFail)return -1;*p=memory.data();return 0;}void Unmap(UINT,const void*){}
 UINT64 GetGPUVirtualAddress(){return 0x100000;}};
struct ID3D12RootSignature:ID3D12DeviceChild{using ID3D12DeviceChild::ID3D12DeviceChild;};
struct ID3D12PipelineState:ID3D12DeviceChild{using ID3D12DeviceChild::ID3D12DeviceChild;};
struct ID3D12DescriptorHeap:ID3D12DeviceChild{D3D12_DESCRIPTOR_HEAP_DESC desc;size_t base=0;
 using ID3D12DeviceChild::ID3D12DeviceChild;D3D12_DESCRIPTOR_HEAP_DESC GetDesc(){return desc;}
 D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandleForHeapStart(){return{base};}
 D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandleForHeapStart(){return{base};}};
struct ID3DBlob:IUnknown{void* GetBufferPointer(){return this;}size_t GetBufferSize(){return 32;}};
inline HRESULT D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* d,int version,ID3DBlob** p,ID3DBlob**){
 assert(d->NumParameters==4&&version==1);*p=new ID3DBlob;(*p)->AddRef();return 0;}
inline HRESULT ID3D12Device::CreateRootSignature(UINT,const void*,size_t,ID3D12RootSignature** p){*p=new ID3D12RootSignature(this);(*p)->AddRef();return 0;}
inline HRESULT ID3D12Device::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* d,ID3D12PipelineState** p){
 assert(d->CS.BytecodeLength==5824&&d->pRootSignature);if(pipelineFail)return -1;*p=new ID3D12PipelineState(this);(*p)->AddRef();return 0;}
inline HRESULT ID3D12Device::CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* d,ID3D12DescriptorHeap** p){
 *p=new ID3D12DescriptorHeap(this);(*p)->AddRef();(*p)->desc=*d;(*p)->base=nextHeap;nextHeap+=4096;return 0;}
inline HRESULT ID3D12Device::CreateCommittedResource(const D3D12_HEAP_PROPERTIES* h,int flags,const D3D12_RESOURCE_DESC* d,
 UINT state,const void* clear,ID3D12Resource** p){++creations;
 assert(flags==0&&!clear&&h->CreationNodeMask==1&&h->VisibleNodeMask==1);
 assert(d->DepthOrArraySize==1&&d->MipLevels==1&&d->SampleDesc.Count==1&&d->SampleDesc.Quality==0);
 if(h->Type==1)assert(d->Dimension==3&&d->Flags==4&&state==8&&(d->Format==28||d->Format==10));
 else assert(h->Type==2&&d->Dimension==1&&d->Width==2048&&state==0xabc);
 if(creations==failCreateAt)return -1;*p=new ID3D12Resource(this);(*p)->AddRef();(*p)->desc=*d;
 (*p)->state=state;(*p)->privateOutput=h->Type==1;(*p)->memory.resize(h->Type==2?2048:16);return 0;}
inline void ID3D12Device::CreateUnorderedAccessView(ID3D12Resource* r,ID3D12Resource* counter,
 const D3D12_UNORDERED_ACCESS_VIEW_DESC* d,D3D12_CPU_DESCRIPTOR_HANDLE h){
 assert(r&&r->privateOutput&&!counter&&d->Format==r->desc.Format&&h.ptr);++uavs;}
struct ID3D12GraphicsCommandList:ID3D12DeviceChild{int type=0;using ID3D12DeviceChild::ID3D12DeviceChild;
 int GetType(){return type;}void SetPipelineState(ID3D12PipelineState*){Fake::events.push_back("pso");}
 void SetComputeRootSignature(ID3D12RootSignature*){Fake::events.push_back("root");}
 void SetDescriptorHeaps(UINT n,ID3D12DescriptorHeap** h){assert(n==1&&h[0]);Fake::events.push_back("heap");}
 void SetComputeRootConstantBufferView(UINT,UINT64){Fake::events.push_back("cb");}
 void SetComputeRootDescriptorTable(UINT,D3D12_GPU_DESCRIPTOR_HANDLE){Fake::events.push_back("table");}
 void Dispatch(UINT x,UINT y,UINT z){assert(x==1&&y==1&&z==1);++Fake::dispatches;Fake::events.push_back("dispatch");}
 void ResourceBarrier(UINT n,const D3D12_RESOURCE_BARRIER* b){Fake::events.push_back("barriers");assert(n==3);
 if(Fake::barrierThrow)throw 1;for(UINT i=0;i<n;++i){const auto& t=b[i].Transition;
 assert(t.pResource->privateOutput&&t.Subresource==~UINT(0)&&t.StateBefore==8&&t.StateAfter==0xc0&&t.pResource->state==8);
 t.pResource->state=t.StateAfter;}}
};
namespace Microsoft::WRL{template<class T>class ComPtr{T* p=nullptr;public:
 ComPtr()=default;ComPtr(T* q):p(q){if(p)p->AddRef();}ComPtr(const ComPtr& q):ComPtr(q.p){}
 ComPtr(ComPtr&& q):p(q.p){q.p=nullptr;}~ComPtr(){if(p)p->Release();}
 T* Get()const{return p;}T* operator->()const{return p;}explicit operator bool()const{return p!=nullptr;}
 ComPtr& operator=(T* q){if(q)q->AddRef();if(p)p->Release();p=q;return *this;}
 ComPtr& operator=(const ComPtr& q){return *this=q.p;}ComPtr& operator=(ComPtr&& q){
 if(this!=std::addressof(q)){if(p)p->Release();p=q.p;q.p=nullptr;}return *this;}
 T** operator&(){assert(!p);return &p;}};}
struct ScopedSkipHeapCapture{};
namespace FSRDSubmission{inline std::shared_ptr<void> retained;
 inline std::shared_ptr<void> Retain(ID3D12Device*,ID3D12GraphicsCommandList*,const std::shared_ptr<void>& owner){
 Fake::events.push_back("retain");if(Fake::retainFail)return{};retained=owner;return owner;}}
'''
        # Production code is extracted unchanged. Only Authenticate is substituted;
        # all new ownership/claim/factory/preparation/record paths remain executable.
        constants = SOURCE[SOURCE.index('using Microsoft::WRL::ComPtr;'):SOURCE.index('// Load CNG')]
        readers = SOURCE[SOURCE.index('ComPtr<IUnknown> Identity'):SOURCE.index('struct Targets::Impl')]
        implementation = ('#include "FSRDCyberpunkGuidePass.h"\n#include <string_view>\n'
                          'namespace FSRD::CyberpunkGuidePass { namespace {\n' + constants +
                          'bool Authenticate(std::span<const std::byte>) { return Fake::authenticate; }\n' +
                          readers + SOURCE[SOURCE.index('struct Targets::Impl'):])
        harness = r'''
#include "implementation.h"
using Microsoft::WRL::ComPtr;using namespace FSRD::CyberpunkGuidePass;
namespace Constants=FSRD::CyberpunkGuideConstants;
struct Fixture{ComPtr<ID3D12Device> device=new ID3D12Device;
 ComPtr<ID3D12GraphicsCommandList> list=new ID3D12GraphicsCommandList(device.Get());
 std::array<SourceView,4> sources;Constants::PassConstants pass{};Constants::SharedConstants shared{};
 std::array<std::byte,5824> shader{};
 Fixture(){assert(!FSRDSubmission::retained&&Fake::resources==0);Fake::events.clear();Fake::dispatches=0;
 Fake::authenticate=true;Fake::retainFail=Fake::barrierThrow=false;
 for(auto& s:sources){s.resource=new ID3D12Resource(device.Get());auto& d=s.resource->desc;
 d.Dimension=3;d.Width=32;d.Height=16;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;
 s.heap=new ID3D12DescriptorHeap(device.Get());s.heap->base=2048;s.heap->desc.NumDescriptors=8;s.descriptor.ptr=2048;}
 Constants::PassSources p;p.width=16;p.height=8;p.transparency=Constants::TransparencyInput::PreTransparencySurface;
 assert(Constants::PackPass(p,pass));}
 ~Fixture(){FSRDSubmission::retained.reset();}
 auto into(const std::shared_ptr<Targets>& t){return PrepareInto(device.Get(),16,8,sources,pass,shared,shader,t);}
 auto prepare(){return Prepare(device.Get(),16,8,sources,pass,shared,shader);}
};
int main(){
 {Fixture f;const char* error="old";auto targets=AllocateTargets(f.device.Get(),16,8,&error);
  assert(targets&&std::string(error).empty()&&f.device->creations==3);auto consumer=targets->Outputs();
  for(UINT i=0;i<3;++i){assert(consumer[i]->desc.Format==(i<2?28:10)&&consumer[i]->state==8);
   assert(consumer[i]->desc.Width==16&&consumer[i]->desc.Height==8);}
  auto work=f.into(targets);assert(work&&f.device->creations==4&&f.device->descriptorCopies==4&&f.device->uavs==3);
  for(UINT i=0;i<3;++i)assert(work->Outputs()[i].Get()==consumer[i].Get());
  assert(!f.into(targets)&&work->Record(f.list.Get())&&Fake::dispatches==1&&!work->Record(f.list.Get()));
  assert(Fake::events.front()=="retain"&&Fake::events.back()=="barriers");
  for(auto& out:consumer)assert(out->state==0xc0);
  work.reset();targets.reset();f.sources={};consumer={};assert(Fake::resources==8);
  FSRDSubmission::retained.reset();assert(Fake::resources==0);
 }
 for(UINT bad=0;bad<9;++bad){Fixture f;auto targets=AllocateTargets(f.device.Get(),16,8);
  if(bad==0)f.sources[0].resource=nullptr;if(bad==1)f.pass[4]=1;if(bad==2)f.pass[2]^=1;
  if(bad==3)Fake::authenticate=false;if(bad==4)f.sources[0].resource=targets->Outputs()[0];
  if(bad==5)f.sources[0].descriptor.ptr=2049;if(bad==6)f.device->pipelineFail=true;
  if(bad==7)f.device->mapFail=true;
  if(bad==8){ComPtr<ID3D12Device> other=new ID3D12Device;
   assert(!PrepareInto(other.Get(),16,8,f.sources,f.pass,f.shared,f.shader,targets));}
  else assert(!f.into(targets));
  Fake::authenticate=true;f.device->pipelineFail=f.device->mapFail=false;
  assert(!f.into(targets)&&Fake::events.empty());for(auto& out:targets->Outputs())assert(out->state==8);
 }
 {Fixture f;auto targets=AllocateTargets(f.device.Get(),16,8);
  assert(!PrepareInto(f.device.Get(),8,8,f.sources,f.pass,f.shared,f.shader,targets)&&!f.into(targets));}
 {Fixture f;assert(!f.into({})&&f.device->creations==0);f.sources[0].resource=nullptr;
  assert(!f.prepare()&&f.device->creations==0);}
 {Fixture f;auto work=f.prepare();assert(work&&f.device->creations==4&&work->Record(f.list.Get()));}
 {Fixture f;auto targets=AllocateTargets(f.device.Get(),16,8);std::shared_ptr<Work> a,b;
  std::thread first([&]{a=f.into(targets);});std::thread second([&]{b=f.into(targets);});first.join();second.join();
  assert(bool(a)!=bool(b)&&f.device->creations==4&&Fake::events.empty());}
 for(UINT bad=0;bad<4;++bad){Fixture f;const char* error="";
  if(bad==0)assert(!AllocateTargets(nullptr,16,8,&error));
  if(bad==1)assert(!AllocateTargets(f.device.Get(),0,8,&error));
  if(bad==2)assert(!AllocateTargets(f.device.Get(),8193,8,&error));
  if(bad==3){f.device->forcedAllocation=(256ull*1024*1024)/3+1;assert(!AllocateTargets(f.device.Get(),16,8,&error));}
  assert(*error&&f.device->creations==0&&Fake::resources==4);}
 {Fixture f;f.device->failCreateAt=3;assert(!AllocateTargets(f.device.Get(),16,8)&&Fake::resources==4);}
 for(bool barrier:{false,true}){Fixture f;auto targets=AllocateTargets(f.device.Get(),16,8);auto work=f.into(targets);
  Fake::retainFail=!barrier;Fake::barrierThrow=barrier;
  assert(!work->Record(f.list.Get())&&!work->Record(f.list.Get()));
  assert(Fake::dispatches==(barrier?1u:0u));work.reset();targets.reset();f.sources={};
  assert(Fake::resources==(barrier?8u:0u));FSRDSubmission::retained.reset();assert(Fake::resources==0);}
 assert(Fake::resources==0);
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-guide-targets-') as temporary:
            temp = Path(temporary)
            for name, contents in {'d3d12.h': windows, 'wrl/client.h': '#include "d3d12.h"\n',
                                   'implementation.h': implementation, 'test.cpp': harness}.items():
                target = temp / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(contents)
            for flags in (['-O0'], ['-O3', '-ffast-math']):
                result = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-pthread', *flags,
                                         '-I', str(temp), '-I', str(BASE), str(temp / 'test.cpp'),
                                         '-o', str(temp / 'test')], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(temp / 'test')], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()

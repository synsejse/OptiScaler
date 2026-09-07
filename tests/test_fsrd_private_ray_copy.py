"""Compile the actual copy Work against D3D12/submission recording mocks.

Checks native-copy admission, bounds, ordering and acyclic failure-path ownership;
this does not execute a GPU, validate native engine state or establish a frame join.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'OptiScaler/upscalers/ffx'
SOURCE = (BASE / 'FSRDPrivateRayCopy.cpp').read_text()
HEADER = (BASE / 'FSRDPrivateRayCopy.h').read_text()


class PrivateRayCopy(unittest.TestCase):
    def test_isolated_copy_only_native_contract(self):
        for forbidden in ('Config::', 'State::', 'NVSDK_', 'Dispatch(', 'SetPipelineState',
                          'SetComputeRoot', 'SetGraphicsRoot', 'SetDescriptorHeaps', 'OMSet',
                          'ExecuteCommandLists', 'WaitFor', 'CreateShaderResourceView',
                          'CreateUnorderedAccessView', 'CreateComputePipelineState',
                          'shared_from_this', 'D3D12_RESOURCE_STATE_UNORDERED_ACCESS'):
            self.assertNotIn(forbidden, SOURCE)
        self.assertIn('DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT', SOURCE)
        self.assertIn('formatInfo.PlaneCount == 1', SOURCE)
        self.assertIn('source.MipLevels == 1', SOURCE)
        self.assertIn('source.Format', SOURCE)
        self.assertIn('Every channel/bit is copied', HEADER)
        self.assertIn('NOT GPU completion or a frame association', HEADER)
        self.assertIn('Any subsequent required', HEADER)
        self.assertIn('restoration belongs to the caller', HEADER)

    def test_retention_precedes_commands_and_barriers_only_private(self):
        record = SOURCE.split('bool Work::Record', 1)[1]
        self.assertLess(record.index('FSRDSubmission::Retain'), record.index('CopyTextureRegion'))
        self.assertLess(record.index('Require(bool(retained)'), record.index('CopyTextureRegion'))
        self.assertIn('barrier.Transition.pResource = lease.outputs[i].Get()', record)
        self.assertIn('barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST', record)
        self.assertNotIn('barrier.Transition.pResource = lease.sources', record)
        lease = SOURCE.split('struct Lease\n', 1)[1].split('} // namespace', 1)[0]
        self.assertNotIn('Ticket', lease)
        self.assertNotIn('Work', lease)
        self.assertIn('Textures sources', lease)
        self.assertIn('Textures outputs', lease)
        prepare = SOURCE.split('std::shared_ptr<Work> Work::PrepareImpl', 1)[1].split('bool Work::Record', 1)[0]
        self.assertLess(prepare.index('Charge(device, output'), prepare.index('targets = AllocateTargets'))
        factory = SOURCE.split('std::shared_ptr<Targets> AllocateTargets', 1)[1].split('struct Work::Impl', 1)[0]
        self.assertLess(factory.index('Charge(device, output'), factory.index('CreateCommittedResource'))
        self.assertIn('std::shared_ptr<Targets> targets', lease)

    def test_preallocated_targets_are_opaque_fixed_and_single_claim(self):
        target = HEADER.split('class Targets\n', 1)[1].split('class Work\n', 1)[0]
        self.assertIn('Targets(const Targets&) = delete', target)
        self.assertIn('private:', target)
        self.assertGreater(target.index('explicit Targets('), target.index('private:'))
        self.assertIn('const Textures& Outputs() const noexcept', target)
        self.assertNotIn('Textures& outputs', HEADER)
        self.assertIn('std::atomic<bool> claimed = false', SOURCE)
        self.assertEqual(SOURCE.count('targets->_impl->claimed.exchange(true)'), 2)
        self.assertIn('lease.outputs = targets->_impl->outputs', SOURCE)
        self.assertIn('ray-copy source aliases private target', SOURCE)
        self.assertIn('actual successful production and GPU ordering', HEADER)

    def test_actual_cpp_admission_copy_bits_failure_and_lifetime(self):
        compiler = os.environ.get('CXX') or shutil.which('c++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('Set CXX for the actual ray-copy Work mock compilation')
        windows = r'''
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using UINT=unsigned; using UINT64=uint64_t; using HRESULT=int;
using DXGI_FORMAT=int; using D3D12_RESOURCE_STATES=unsigned;
#define SUCCEEDED(x) ((x)>=0)
#define IID_PPV_ARGS(x) (x)
enum { DXGI_FORMAT_R16G16B16A16_FLOAT=10, DXGI_FORMAT_R32_FLOAT=41,
 D3D12_RESOURCE_DIMENSION_TEXTURE2D=3, D3D12_TEXTURE_LAYOUT_UNKNOWN=0,
 D3D12_RESOURCE_FLAG_NONE=0, D3D12_HEAP_TYPE_DEFAULT=1, D3D12_HEAP_FLAG_NONE=0,
 D3D12_COMMAND_LIST_TYPE_DIRECT=0, D3D12_COMMAND_LIST_TYPE_COMPUTE=2,
 D3D12_FEATURE_FORMAT_INFO=5, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX=0,
 D3D12_RESOURCE_BARRIER_TYPE_TRANSITION=0,
 D3D12_RESOURCE_STATE_COPY_DEST=0x400, D3D12_RESOURCE_STATE_COPY_SOURCE=0x800,
 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE=0x40,
 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE=0x80 };
struct D3D12_RESOURCE_DESC { int Dimension=0; uint64_t Alignment=0,Width=0; UINT Height=0;
 UINT DepthOrArraySize=0,MipLevels=0; int Format=0;
 struct {UINT Count=0,Quality=0;} SampleDesc; UINT Layout=0,Flags=0; };
struct D3D12_RESOURCE_ALLOCATION_INFO {uint64_t SizeInBytes;};
struct D3D12_FEATURE_DATA_FORMAT_INFO {DXGI_FORMAT Format{}; unsigned char PlaneCount{};};
struct D3D12_HEAP_PROPERTIES {int Type{},CPUPageProperty{},MemoryPoolPreference{};
 UINT CreationNodeMask{},VisibleNodeMask{};};
struct IUnknown {std::atomic<unsigned> refs=0; IUnknown* canonical=nullptr; bool identityFail=false;
 virtual ~IUnknown()=default;
 void AddRef(){++refs;} void Release(){assert(refs); if(!--refs)delete this;}
 HRESULT QueryInterface(IUnknown** p){if(identityFail)return -1;
 *p=canonical?canonical:this;(*p)->AddRef();return 0;}};
struct ID3D12Resource;
namespace Fake {
inline std::vector<std::string> events;
inline unsigned liveResources=0,copyCalls=0,barrierCalls=0;
inline bool retainFail=false,retainThrow=false,barrierThrow=false;
inline unsigned copyThrowAt=0;
}
struct ID3D12Device: IUnknown {
 unsigned planes=1,creationCount=0,failCreateAt=0; bool featureFail=false;
 uint64_t forcedAllocation=0;
 D3D12_RESOURCE_ALLOCATION_INFO GetResourceAllocationInfo(UINT mask,UINT count,const D3D12_RESOURCE_DESC* d){
  assert(mask==0 && count==1);if(forcedAllocation)return {forcedAllocation};
  uint64_t bytes=d->Width*d->Height*(d->Format==10?8:4);
  return {(bytes+65535)&~uint64_t(65535)};}
 HRESULT CheckFeatureSupport(int feature,void* p,UINT bytes){
  assert(feature==D3D12_FEATURE_FORMAT_INFO && bytes==sizeof(D3D12_FEATURE_DATA_FORMAT_INFO));
  if(featureFail)return -1;static_cast<D3D12_FEATURE_DATA_FORMAT_INFO*>(p)->PlaneCount=planes;return 0;}
 HRESULT CreateCommittedResource(const D3D12_HEAP_PROPERTIES*,int,const D3D12_RESOURCE_DESC*,
   D3D12_RESOURCE_STATES,const void*,ID3D12Resource**);
};
struct ID3D12DeviceChild: IUnknown {ID3D12Device* device; bool deviceFail=false;
 explicit ID3D12DeviceChild(ID3D12Device* d):device(d){device->AddRef();}
 ~ID3D12DeviceChild(){device->Release();}
 HRESULT GetDevice(ID3D12Device** p){if(deviceFail)return -1;*p=device;device->AddRef();return 0;}};
struct ID3D12Resource: ID3D12DeviceChild {
 D3D12_RESOURCE_DESC desc; std::vector<uint32_t> words; unsigned state=0x8c0; bool privateOutput=false;
 explicit ID3D12Resource(ID3D12Device* d):ID3D12DeviceChild(d){++Fake::liveResources;}
 ~ID3D12Resource(){--Fake::liveResources;}
 D3D12_RESOURCE_DESC GetDesc(){return desc;}
};
inline HRESULT ID3D12Device::CreateCommittedResource(const D3D12_HEAP_PROPERTIES* h,int flags,
 const D3D12_RESOURCE_DESC* d,D3D12_RESOURCE_STATES state,const void* clear,ID3D12Resource** out){
 ++creationCount;assert(h->Type==1 && h->CreationNodeMask==1 && h->VisibleNodeMask==1);
 assert(flags==0 && clear==nullptr && state==0x400 && d->Flags==0 && d->Alignment==0);
 assert(d->Dimension==3 && d->DepthOrArraySize==1 && d->MipLevels==1 && d->SampleDesc.Count==1 && d->SampleDesc.Quality==0);
 if(creationCount==failCreateAt)return -1;
 auto* r=new ID3D12Resource(this);r->desc=*d;r->state=state;r->privateOutput=true;
 r->words.resize(d->Width*d->Height*(d->Format==10?2:1),0xdeadbeef);r->AddRef();*out=r;return 0;}
struct D3D12_BOX {UINT left,top,front,right,bottom,back;};
struct D3D12_TEXTURE_COPY_LOCATION {ID3D12Resource* pResource{};int Type{};UINT SubresourceIndex{};};
struct D3D12_RESOURCE_BARRIER {int Type=0,Flags=0;
 struct {ID3D12Resource* pResource{};UINT Subresource{};unsigned StateBefore{},StateAfter{};} Transition;};
struct ID3D12GraphicsCommandList: ID3D12DeviceChild {int type=0;
 explicit ID3D12GraphicsCommandList(ID3D12Device* d):ID3D12DeviceChild(d){}
 int GetType(){return type;}
 void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst,UINT x,UINT y,UINT z,
                        const D3D12_TEXTURE_COPY_LOCATION* src,const D3D12_BOX* box){
  ++Fake::copyCalls;Fake::events.push_back("copy");
  assert(dst->pResource->privateOutput && !src->pResource->privateOutput);
  assert(dst->pResource->state==0x400 && (src->pResource->state&0x800));
  assert(src->SubresourceIndex==0 && dst->SubresourceIndex==0 && src->Type==0 && dst->Type==0);
  assert(x==0 && y==0 && z==0 && box && box->left==0 && box->top==0 && box->front==0 && box->back==1);
  assert(src->pResource->desc.Format==dst->pResource->desc.Format);
  assert(dst->pResource->desc.Width==box->right && dst->pResource->desc.Height==box->bottom);
  if(Fake::copyCalls==Fake::copyThrowAt)throw 1;
  unsigned stride=src->pResource->desc.Format==10?2:1;
  for(UINT row=0;row<box->bottom;++row)
   std::copy_n(src->pResource->words.begin()+row*src->pResource->desc.Width*stride,
    box->right*stride,dst->pResource->words.begin()+row*box->right*stride);
 }
 void ResourceBarrier(UINT count,const D3D12_RESOURCE_BARRIER* b){
  ++Fake::barrierCalls;Fake::events.push_back("barriers");assert(count==2);
  if(Fake::barrierThrow)throw 2;
  for(UINT i=0;i<count;++i){auto& t=b[i].Transition;
   assert(b[i].Type==0 && b[i].Flags==0 && t.pResource->privateOutput && t.Subresource==0);
   assert(t.StateBefore==0x400 && t.StateAfter==0xc0 && t.pResource->state==t.StateBefore);
   t.pResource->state=t.StateAfter;}
 }
};
namespace Microsoft::WRL {
template<class T> class ComPtr {T* p=nullptr; public:
 ComPtr()=default; ComPtr(T* q):p(q){if(p)p->AddRef();}
 ComPtr(const ComPtr& q):ComPtr(q.p){} ComPtr(ComPtr&& q):p(q.p){q.p=nullptr;}
 ~ComPtr(){if(p)p->Release();} T* Get()const{return p;} T* operator->()const{return p;}
 explicit operator bool()const{return p!=nullptr;}
 ComPtr& operator=(T* q){if(q)q->AddRef();if(p)p->Release();p=q;return *this;}
 ComPtr& operator=(const ComPtr& q){return *this=q.p;}
 ComPtr& operator=(ComPtr&& q){if(this!=std::addressof(q)){if(p)p->Release();p=q.p;q.p=nullptr;}return *this;}
 T** operator&(){assert(!p);return &p;}
};}
struct ScopedSkipHeapCapture {};
'''
        submission = r'''
#pragma once
#include "d3d12.h"
namespace FSRDSubmission {
struct Ticket {std::vector<std::shared_ptr<void>> owners;};
inline std::shared_ptr<Ticket> pending;
inline std::shared_ptr<Ticket> Retain(ID3D12Device*,ID3D12GraphicsCommandList*,const std::shared_ptr<void>& owner){
 Fake::events.push_back("retain");if(Fake::retainThrow)throw 3;if(Fake::retainFail)return {};
 if(!pending)pending=std::make_shared<Ticket>();pending->owners.push_back(owner);return pending;}
}
'''
        harness = r'''
#include "FSRDPrivateRayCopy.cpp"
using Microsoft::WRL::ComPtr;
using namespace FSRD::PrivateRayCopy;
struct Fixture {
 ComPtr<ID3D12Device> device=new ID3D12Device;
 ComPtr<ID3D12GraphicsCommandList> list=new ID3D12GraphicsCommandList(device.Get());
 Textures sources;
 Fixture(){
  assert(!FSRDSubmission::pending && Fake::liveResources==0);
  Fake::events.clear();Fake::copyCalls=Fake::barrierCalls=0;
  Fake::retainFail=Fake::retainThrow=Fake::barrierThrow=false;Fake::copyThrowAt=0;
  for(unsigned i=0;i<2;++i){sources[i]=new ID3D12Resource(device.Get());auto& r=*sources[i].Get();
   r.desc.Dimension=3;r.desc.Width=32;r.desc.Height=16;r.desc.DepthOrArraySize=r.desc.MipLevels=1;
   r.desc.Format=i?41:10;r.desc.SampleDesc.Count=1;r.words.resize(32*16*(i?1:2));
   const uint32_t bits[]={0x80000000,0x7fc00001,0x7f800000,0xff800000,0x00000001,0x7c007e01,0xfc008000};
   for(unsigned j=0;j<r.words.size();++j)r.words[j]=bits[j%7]^(j<<3);
  }
 }
 ~Fixture(){FSRDSubmission::pending.reset();}
 std::shared_ptr<Work> prepare(const char** error=nullptr){return Prepare(device.Get(),16,8,sources,error);}
};
void invalidDescription(unsigned which){
 Fixture f;auto& d=f.sources[0]->desc;
 switch(which){case 0:d.Dimension=1;break;case 1:d.Width=15;break;case 2:d.Height=7;break;
 case 3:d.DepthOrArraySize=2;break;case 4:d.MipLevels=2;break;case 5:d.SampleDesc.Count=2;break;
 case 6:d.SampleDesc.Quality=1;break;case 7:d.Format=41;break;case 8:d.Format=9;break;}
 const char* error=nullptr;assert(!f.prepare(&error) && error && *error);
 assert(f.device->creationCount==0 && Fake::events.empty());
}
int main(){
 // Allocate first, expose stable references to an independent future consumer,
 // then bind exactly one source producer without allocating/replacing targets.
 {Fixture f;const char* error="old";auto targets=AllocateTargets(f.device.Get(),16,8,&error);
  assert(targets && std::string(error).empty() && f.device->creationCount==2);
  auto consumer=targets->Outputs();
  for(unsigned i=0;i<2;++i){assert(consumer[i]->state==0x400);
   assert(consumer[i]->desc.Width==16 && consumer[i]->desc.Height==8);}
  auto w=PrepareInto(f.device.Get(),16,8,f.sources,targets,&error);
  assert(w && std::string(error).empty() && f.device->creationCount==2 && !w->Recorded());
  assert(w->Outputs()[0].Get()==consumer[0].Get() && w->Outputs()[1].Get()==consumer[1].Get());
  assert(!PrepareInto(f.device.Get(),16,8,f.sources,targets,&error) && *error);
  assert(w->Record(f.list.Get()) && consumer[0]->state==0xc0 && consumer[1]->state==0xc0);
  w.reset();targets.reset();f.sources={};consumer={};assert(Fake::liveResources==4);
  FSRDSubmission::pending.reset();assert(Fake::liveResources==0);
 }
 {Fixture f;const char* error="";
  assert(!PrepareInto(f.device.Get(),16,8,f.sources,{},&error) && *error);
  assert(f.device->creationCount==0);
 }
 // Every attempted PrepareInto consumes the claim even if source/device/extent
 // admission fails. No output state or command changes occur on refusal.
 for(unsigned bad=0;bad<5;++bad){Fixture f;auto targets=AllocateTargets(f.device.Get(),16,8);
  auto sources=f.sources;ComPtr<ID3D12Device> other=new ID3D12Device;
  if(bad==0)sources[0]=nullptr;
  if(bad==1)sources[0]->desc.Format=41;
  if(bad==2)sources[0]=targets->Outputs()[0];
  const char* error="";
  assert(!PrepareInto(bad==3?other.Get():f.device.Get(),bad==4?8:16,8,sources,targets,&error));
  assert(*error && !PrepareInto(f.device.Get(),16,8,f.sources,targets));
  assert(Fake::events.empty() && targets->Outputs()[0]->state==0x400 && f.device->creationCount==2);
 }
 {Fixture f;auto targets=AllocateTargets(f.device.Get(),16,8);
  std::shared_ptr<Work> first,second;
  std::thread a([&]{first=PrepareInto(f.device.Get(),16,8,f.sources,targets);});
  std::thread b([&]{second=PrepareInto(f.device.Get(),16,8,f.sources,targets);});a.join();b.join();
  assert(bool(first)!=bool(second) && f.device->creationCount==2 && Fake::events.empty());
 }
 {Fixture f;const char* error="";
  assert(!AllocateTargets(nullptr,16,8,&error) && *error);
  assert(!AllocateTargets(f.device.Get(),0,8,&error) && *error);
  assert(!AllocateTargets(f.device.Get(),8193,8,&error) && *error);
  f.device->forcedAllocation=uint64_t(-1);
  assert(!AllocateTargets(f.device.Get(),16,8,&error) && *error && f.device->creationCount==0);
 }
 for(unsigned i=0;i<9;++i)invalidDescription(i);
 {Fixture f;assert(!Prepare(nullptr,16,8,f.sources));assert(!Prepare(f.device.Get(),0,8,f.sources));
  assert(!Prepare(f.device.Get(),16,0,f.sources));assert(!Prepare(f.device.Get(),8193,8,f.sources));}
 {Fixture f;f.sources[1]=nullptr;assert(!f.prepare());}
 {Fixture f;ComPtr<ID3D12Device> other=new ID3D12Device;
  f.sources[1]=new ID3D12Resource(other.Get());assert(!f.prepare());}
 {Fixture f;f.sources[0]->identityFail=true;assert(!f.prepare());}
 {Fixture f;f.sources[0]->deviceFail=true;assert(!f.prepare());}
 {Fixture f;f.sources[1]->canonical=f.sources[0].Get();assert(!f.prepare());
  assert(f.device->creationCount==0);}
 for(unsigned planes:{0u,2u}){Fixture f;f.device->planes=planes;assert(!f.prepare());}
 {Fixture f;f.device->featureFail=true;assert(!f.prepare());}
 {Fixture f;f.device->forcedAllocation=UINT64(-1);assert(!f.prepare());}
 {Fixture f;f.device->forcedAllocation=(256ull*1024*1024)/4+1;
  assert(!f.prepare() && f.device->creationCount==0);}
 {Fixture f;f.device->forcedAllocation=(256ull*1024*1024)/4;
  assert(f.prepare() && f.device->creationCount==2);}
 {Fixture f;f.device->failCreateAt=2;assert(!f.prepare());
  assert(Fake::liveResources==2 && !FSRDSubmission::pending);}
 {Fixture f;auto w=f.prepare();assert(w && !w->Recorded() && w->Error().empty());
  assert(w->Outputs()[0]->desc.Format==10 && w->Outputs()[1]->desc.Format==41);
  assert(w->Record(f.list.Get()) && w->Recorded());
  assert((Fake::events==std::vector<std::string>{"retain","copy","copy","barriers"}));
  for(unsigned i=0;i<2;++i){auto* out=w->Outputs()[i].Get();auto* in=f.sources[i].Get();
   assert(out->state==0xc0 && in->state==0x8c0);unsigned stride=i?1:2;
   for(unsigned y=0;y<8;++y)for(unsigned x=0;x<16*stride;++x)
    assert(out->words[y*16*stride+x]==in->words[y*32*stride+x]);
  }
  assert(!w->Record(f.list.Get()) && Fake::copyCalls==2);
  // Neither Work nor the original caller keeps ownership after this point.
  f.sources={};w.reset();assert(Fake::liveResources==4);
  assert(FSRDSubmission::pending && FSRDSubmission::pending->owners.size()==1);
  FSRDSubmission::pending.reset();assert(Fake::liveResources==0);
 }
 for(unsigned badList=0;badList<4;++badList){Fixture f;auto w=f.prepare();
  ComPtr<ID3D12Device> other=new ID3D12Device;
  ComPtr<ID3D12GraphicsCommandList> foreign=new ID3D12GraphicsCommandList(other.Get());
  if(badList==0)assert(!w->Record(nullptr));
  if(badList==1){f.list->type=2;assert(!w->Record(f.list.Get()));}
  if(badList==2)assert(!w->Record(foreign.Get()));
  if(badList==3){f.list->deviceFail=true;assert(!w->Record(f.list.Get()));}
  assert(!w->Recorded() && !w->Error().empty() && Fake::events.empty());
  assert(!w->Record(f.list.Get()));
 }
 for(bool throws:{false,true}){Fixture f;auto w=f.prepare();
  Fake::retainFail=!throws;Fake::retainThrow=throws;
  assert(!w->Record(f.list.Get()) && !w->Recorded() && !w->Error().empty());
  assert(Fake::copyCalls==0 && Fake::barrierCalls==0 && !FSRDSubmission::pending);
  w.reset();assert(Fake::liveResources==2);
 }
 for(unsigned failAt:{1u,2u,3u}){Fixture f;auto w=f.prepare();
  Fake::copyThrowAt=failAt<3?failAt:0;Fake::barrierThrow=failAt==3;
  assert(!w->Record(f.list.Get()) && !w->Recorded() && !w->Error().empty());
  assert(!w->Record(f.list.Get()) && FSRDSubmission::pending);
  assert(f.sources[0]->state==0x8c0 && f.sources[1]->state==0x8c0);
  f.sources={};w.reset();assert(Fake::liveResources==4);
  FSRDSubmission::pending.reset();assert(Fake::liveResources==0);
 }
 assert(Fake::liveResources==0);
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-private-ray-copy-') as temporary:
            temp = Path(temporary)
            files = {'d3d12.h': windows, 'pch.h': '#include "d3d12.h"\n',
                     'wrl/client.h': '#include "d3d12.h"\n',
                     'resource_tracking/FSRDSubmission.h': submission, 'test.cpp': harness}
            for name, contents in files.items():
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

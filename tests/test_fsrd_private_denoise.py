"""Actual private Work/Core compiled against recording D3D/provider/converter mocks.

Exercises CPU admission, explicit-provider/settings forwarding, one-shot recording and
acyclic fence ownership. It does not execute AMD, shaders or validate the Windows ABI.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'OptiScaler/upscalers/ffx'
SOURCE = (BASE / 'FSRDPrivateDenoise.cpp').read_text()
HEADER = (BASE / 'FSRDPrivateDenoise.h').read_text()


class PrivateDenoise(unittest.TestCase):
    def test_isolated_explicit_reset_and_no_scene_or_fallback_path(self):
        for forbidden in ('State::', 'Config::', 'NVSDK_', 'changeBackend', 'Request(',
                          'ExecuteCommandLists', 'WaitFor', 'OMSetRenderTargets', 'CopyTextureRegion',
                          'shared_from_this', 'DispatchComposition(list, composition, true)'):
            self.assertNotIn(forbidden, SOURCE)
        self.assertIn('DenoiserCore<FfxApiProxy> denoiser', SOURCE)
        self.assertIn('Converter::ConvFlags::ResetMotionHistory', SOURCE)
        self.assertIn('data->parameters.dispatch.flags |= FFX_DENOISER_DISPATCH_RESET', SOURCE)
        self.assertIn('lease.denoiser.Configure(parameters.settings)', SOURCE)
        self.assertIn('versionId = parameters.providerId', SOURCE)
        self.assertIn('DispatchComposition(list, composition, false)', SOURCE)
        self.assertIn('.Flags = 0', SOURCE)
        self.assertIn('NOT GPU completion', HEADER)
        self.assertIn('does not make that proxy globally thread-safe', HEADER)

    def test_lease_is_retained_before_commands_and_never_owns_converter_or_ticket(self):
        lease = SOURCE.split('struct Lease\n', 1)[1].split('} // namespace', 1)[0]
        self.assertNotIn('Converter', lease)
        self.assertNotIn('Ticket', lease)
        self.assertNotIn('Work', lease)
        for required in ('ComPtr<ID3D12Device> device', 'ComPtr<ID3D12Resource>', 'Textures textures', 'ContextLease'):
            self.assertIn(required, lease)
        context = SOURCE.split('struct ContextLease\n', 1)[1].split('\n};', 1)[0]
        self.assertIn('DenoiserCore<FfxApiProxy> denoiser', context)
        for forbidden in ('Converter', 'Ticket', 'Work', 'Session'):
            self.assertNotIn(forbidden, context)
        record = SOURCE.split('bool Work::Record', 1)[1]
        self.assertLess(record.index('FSRDSubmission::Retain'), record.index('DispatchConversion'))
        self.assertLess(record.index('Require(bool(retained)'), record.index('DispatchConversion'))
        self.assertLess(record.index('SetDenoiserOutputsWritable(list, false)'), record.index('Require(result'))
        self.assertIn('if (writable)', record)
        self.assertIn('No generic keepAlive', HEADER)

    def test_nonfinite_validation_survives_project_fast_math(self):
        self.assertIn('std::bit_cast<uint32_t>(value) & 0x7f800000u', SOURCE)
        self.assertNotIn('std::isfinite', SOURCE)

    def test_compiled_actual_work_lifecycle_and_failures(self):
        self._compile_work()

    def test_session_contract_is_explicit_and_separate_from_one_shot(self):
        prepare = SOURCE.split('std::shared_ptr<Work> Prepare(', 1)[1].split('Session::Session(', 1)[0]
        frame = SOURCE.split('std::shared_ptr<Work> PrepareFrame(', 1)[1].split('bool Work::Record(', 1)[0]
        self.assertIn('dispatch.flags |= FFX_DENOISER_DISPATCH_RESET', prepare)
        self.assertNotIn('|= FFX_DENOISER_DISPATCH_RESET', frame)
        self.assertNotIn('CreateContext(', frame)
        self.assertNotIn('.Configure(', frame)
        self.assertIn('Work::PrepareConverter(*data, "FSRD Private Temporal Frame")', frame)
        self.assertLess(frame.index('budget.Acquire'), frame.index('Work::PrepareConverter'))
        for required in ('HOST ATTESTATION', 'Same Execute-array order alone is NOT',
                         'previous frame not acknowledged', 'frameIndex != UINT32_MAX',
                         'sessionOrdinal > session.acknowledged', 'if (reserved) session->Stop()',
                         'if (data.session) data.session->Stop()'):
            self.assertIn(required, SOURCE + HEADER)

    def test_compiled_persistent_session_order_lifetime_and_refusals(self):
        self._compile_work(temporal=True)

    def test_compiled_visual_pass_one_context_18000_frames_no_periodic_reset(self):
        self._compile_work(temporal=True, visual=True)

    def _compile_work(self, temporal=False, visual=False):
        compiler = os.environ.get('CXX') or shutil.which('c++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('Set CXX for actual private Work mock compilation')
        formats = sorted(set(re.findall(r'DXGI_FORMAT_[A-Z0-9_]+', SOURCE)))
        api = r'''
#pragma once
#include <cstdint>
using ffxContext=void*;
using ffxApiMessage=void (*)(uint32_t,const wchar_t*);
enum ffxReturnCode_t {FFX_API_RETURN_OK,FFX_API_RETURN_ERROR_PARAMETER,FFX_API_RETURN_ERROR_RUNTIME_ERROR};
struct ffxHeader {uint64_t type{}; ffxHeader* pNext{};};
using ffxCreateContextDescHeader=ffxHeader; using ffxConfigureDescHeader=ffxHeader;
using ffxQueryDescHeader=ffxHeader; using ffxDispatchDescHeader=ffxHeader;
struct ffxAllocationCallbacks {};
#define FFX_API_EFFECT_MASK 0xffff0000u
#define FFX_API_BACKEND_MASK 0xff000000u
#define FFX_API_QUERY_DESC_TYPE_GET_VERSIONS 100
#define FFX_API_DESC_TYPE_OVERRIDE_VERSION 101
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 102
struct ffxQueryDescGetVersions {ffxHeader header; uint64_t createDescType{}; void* device{};
 uint64_t* outputCount{}; uint64_t* versionIds{}; const char** versionNames{};};
struct ffxOverrideVersion {ffxHeader header; uint64_t versionId{};};
struct ffxCreateBackendDX12Desc {ffxHeader header; void* device{};};
'''
        types = r'''
#pragma once
#include <cstdint>
struct FfxApiDimensions2D {uint32_t width{},height{};};
struct FfxApiFloatCoords2D {float x{},y{};};
struct FfxApiResource {void* resource{}; uint32_t state{};};
struct FfxApiEffectMemoryUsage {};
'''
        windows = r'''
#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
using UINT=unsigned; using UINT64=uint64_t; using HRESULT=int;
#define SUCCEEDED(x) ((x)>=0)
#define IID_PPV_ARGS(x) (x)
using DXGI_FORMAT=int;
FORMAT_DECLS
enum {D3D12_RESOURCE_DIMENSION_TEXTURE2D=3,D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE=8,
      D3D12_COMMAND_LIST_TYPE_DIRECT=0,D3D12_COMMAND_LIST_TYPE_COMPUTE=2};
struct D3D12_RESOURCE_DESC {int Dimension=3; uint64_t Width=1280; UINT Height=720;
 UINT DepthOrArraySize=1,MipLevels=1; int Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
 struct {UINT Count=1,Quality=0;} SampleDesc; UINT Flags=0;};
struct D3D12_RESOURCE_ALLOCATION_INFO {uint64_t SizeInBytes{};};
struct IUnknown {unsigned refs=0; virtual ~IUnknown()=default;
 void AddRef(){++refs;} void Release(){assert(refs); if(!--refs)delete this;}
 HRESULT QueryInterface(IUnknown** p){*p=this;AddRef();return 0;}};
struct ID3D12Device: IUnknown {uint64_t allocation=1280*720*8;
 D3D12_RESOURCE_ALLOCATION_INFO GetResourceAllocationInfo(UINT,UINT,const D3D12_RESOURCE_DESC*){return {allocation};}};
struct ID3D12DeviceChild: IUnknown {ID3D12Device* device;
 explicit ID3D12DeviceChild(ID3D12Device* d):device(d){device->AddRef();}
 ~ID3D12DeviceChild(){device->Release();}
 HRESULT GetDevice(ID3D12Device** p){*p=device;device->AddRef();return 0;}};
struct ID3D12Resource: ID3D12DeviceChild {D3D12_RESOURCE_DESC desc;
 explicit ID3D12Resource(ID3D12Device* d):ID3D12DeviceChild(d){}
 D3D12_RESOURCE_DESC GetDesc(){return desc;}};
struct ID3D12GraphicsCommandList: ID3D12DeviceChild {int type=0;
 explicit ID3D12GraphicsCommandList(ID3D12Device* d):ID3D12DeviceChild(d){}
 int GetType(){return type;}};
namespace Microsoft::WRL {
template<class T> class ComPtr {T* p=nullptr; public:
 ComPtr()=default; ComPtr(T* q):p(q){if(p)p->AddRef();}
 ComPtr(const ComPtr& q):ComPtr(q.p){} ComPtr(ComPtr&& q):p(q.p){q.p=nullptr;}
 ~ComPtr(){if(p)p->Release();} T* Get()const{return p;} T* operator->()const{return p;}
 explicit operator bool()const{return p!=nullptr;} bool operator!=(std::nullptr_t)const{return p!=nullptr;}
 ComPtr& operator=(T* q){if(q)q->AddRef();if(p)p->Release();p=q;return *this;}
 ComPtr& operator=(const ComPtr& q){return *this=q.p;}
 ComPtr& operator=(ComPtr&& q){if(this!=std::addressof(q)){if(p)p->Release();p=q.p;q.p=nullptr;}return *this;}
 T** operator&(){assert(!p);return &p;}
};}
namespace DirectX {struct XMFLOAT4 {float x{},y{},z{},w{};};struct XMFLOAT4X4{float m[4][4]{};};}
struct ScopedSkipSpoofingGlobal{}; struct ScopedSkipHeapCapture{};
namespace Fake {
inline std::vector<std::string> events;
inline bool retainFail=false,convertFail=false,composeFail=false,dispatchThrow=false;
inline bool createFail=false,defaultFail=false,configureFail=false,changedCount=false;
inline unsigned contexts=0,destroys=0,dispatches=0;
inline unsigned creates=0,defaultQueries=0,configurationCalls=0,lastConversionFlags=0;
inline bool temporal=false,allocationFail=false;
inline void (*duringDispatch)()=nullptr;
inline void (*duringAllocation)()=nullptr;
inline std::vector<void*> dispatchedContexts;
inline uint64_t provider=42,queriedProvider=0; inline float settings[6]{1,1,65504,50,0,.01f};
inline int dispatchResult=0;
}
'''.replace('FORMAT_DECLS', '\n'.join(f'constexpr int {name}={i + 1};' for i, name in enumerate(formats)))
        submission = r'''
#pragma once
#include "d3d12.h"
namespace FSRDSubmission {
struct Ticket {std::vector<std::shared_ptr<void>> owners;};
inline std::shared_ptr<Ticket> pending;
inline std::shared_ptr<Ticket> Retain(ID3D12Device*,ID3D12GraphicsCommandList*,const std::shared_ptr<void>& owner){
 Fake::events.push_back("retain");if(Fake::retainFail)return{};
 if(!pending)pending=std::make_shared<Ticket>();pending->owners.push_back(owner);return pending;}
}
'''
        converter = r'''
#pragma once
#include "d3d12.h"
#include "fsr-rr/ffx_denoiser.h"
#include "resource_tracking/FSRDSubmission.h"
class FSRDPreprocessor_Dx12 {public:
 enum class ConvFlags:uint32_t {NonGammaAlbedo=1,IsDepthLinear=2,IsRoughnessPacked=4,
  IsRightHanded=16,CyberpunkDepthMotion=32,ResetMotionHistory=64};
 union InputResources {struct{ID3D12Resource *InColor,*InDepth,*InMotionVectors,*InNormals,*InRoughness,
  *InSpecHitDist,*InDiffAlbedo,*InSpecAlbedo;};ID3D12Resource* AsArray[8];};
 struct ConversionDesc {InputResources Resources;DirectX::XMFLOAT4X4 InvViewMatrix,InvProjMatrix,PrevViewMatrix;
  DirectX::XMFLOAT4 PreviousDepthProjection,RenderSize;float NearPlane,FarPlane;uint32_t Flags;};
 struct CompositionDesc {DirectX::XMFLOAT4 DstTexSize;uint32_t Flags;};
 using Ptr=Microsoft::WRL::ComPtr<ID3D12Resource>;
 Microsoft::WRL::ComPtr<ID3D12Device> device;
 std::array<Ptr,10> textures;
 std::shared_ptr<FSRDSubmission::Ticket> converterSlotTicket; // Real converter owns tickets.
 FSRDPreprocessor_Dx12(std::string_view,ID3D12Device* d):device(d){}
 bool IsInit(){return true;}
 bool SetMaxRenderSize(UINT,UINT){if(Fake::duringAllocation)Fake::duringAllocation();
  if(Fake::allocationFail)return false;for(auto& t:textures)t=new ID3D12Resource(device.Get());return true;}
 void GetSignal(ffxDispatchDescDenoiserInput1Signal& s,ffxDispatchDescDenoiser& d)const{
  s={};s.header.type=FFX_API_DISPATCH_DESC_INPUT_1_SIGNAL_TYPE_DENOISER;
  s.radiance.input.resource=textures[0].Get();s.radiance.output.resource=textures[1].Get();
  s.fusedAlbedo.resource=textures[2].Get();d.header={FFX_API_DISPATCH_DESC_TYPE_DENOISER,&s.header};
  d.linearDepth.resource=textures[4].Get();d.motionVectors.resource=textures[5].Get();
  d.normals.resource=textures[6].Get();d.diffuseAlbedo.resource=textures[7].Get();d.specularAlbedo.resource=textures[8].Get();}
 ID3D12Resource* GetPreservedLighting()const{return textures[3].Get();}
 ID3D12Resource* GetCompositionOutput()const{return textures[9].Get();}
 bool DispatchConversion(ID3D12GraphicsCommandList* l,const ConversionDesc& c){
  assert(FSRDSubmission::pending && (Fake::temporal || (c.Flags&64)));Fake::lastConversionFlags=c.Flags;
  // A Storage owner, not this converter, is retained by the real converter.
  auto storage=std::make_shared<std::array<Ptr,10>>(textures);
  converterSlotTicket=FSRDSubmission::Retain(device.Get(),l,storage);
  Fake::events.push_back("convert");return !Fake::convertFail;}
 void SetDenoiserOutputsWritable(ID3D12GraphicsCommandList*,bool b){Fake::events.push_back(b?"uav":"srv");}
 bool DispatchComposition(ID3D12GraphicsCommandList*,const CompositionDesc& c,bool identity){
  assert(!identity && c.Flags==0 && c.DstTexSize.x==1280);Fake::events.push_back("compose");return !Fake::composeFail;}
};
'''
        proxy = r'''
#pragma once
#include "d3d12.h"
#include "fsr-rr/ffx_denoiser.h"
struct FfxApiProxy {
 static inline ffxDispatchDescDenoiser lastDispatch{};
 static ffxReturnCode_t D3D12_CreateContext(ffxContext* c,ffxHeader* h,const ffxAllocationCallbacks*){
  auto* d=reinterpret_cast<ffxCreateContextDescDenoiser*>(h);
  assert(d->mode==FFX_DENOISER_MODE_1_SIGNAL && d->flags==0 && d->maxRenderSize.width==(Fake::temporal?1280u:2560u));
  ++Fake::creates;
  auto* b=reinterpret_cast<ffxCreateBackendDX12Desc*>(h->pNext);
  auto* v=reinterpret_cast<ffxOverrideVersion*>(b->header.pNext);Fake::queriedProvider=v->versionId;
  if(Fake::createFail)return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
  *c=new int(9);++Fake::contexts;return FFX_API_RETURN_OK;}
 static ffxReturnCode_t D3D12_DestroyContext(ffxContext* c,const ffxAllocationCallbacks*){
  assert(Fake::contexts);delete static_cast<int*>(*c);*c=nullptr;--Fake::contexts;++Fake::destroys;return FFX_API_RETURN_OK;}
 static ffxReturnCode_t D3D12_Query(ffxContext*,ffxHeader* h){
  if(h->type==FFX_API_QUERY_DESC_TYPE_GET_VERSIONS){auto* d=reinterpret_cast<ffxQueryDescGetVersions*>(h);
   if(!d->versionIds){*d->outputCount=1;}else{d->versionIds[0]=Fake::provider;d->versionNames[0]="test RR 1.1";
    if(Fake::changedCount)*d->outputCount=0;}return FFX_API_RETURN_OK;}
  auto* d=reinterpret_cast<ffxQueryDescDenoiserGetDefaultKeyValue*>(h);
  ++Fake::defaultQueries;
  if(Fake::defaultFail)return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
  *static_cast<float*>(d->data)=Fake::settings[d->key-1];return FFX_API_RETURN_OK;}
 static ffxReturnCode_t D3D12_Configure(ffxContext*,const ffxHeader* h){
  ++Fake::configurationCalls;
  if(Fake::configureFail)return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
  auto* d=reinterpret_cast<const ffxConfigureDescDenoiserKeyValue*>(h);
  Fake::settings[d->key-1]=*static_cast<const float*>(d->data);return FFX_API_RETURN_OK;}
 static ffxReturnCode_t D3D12_Dispatch(ffxContext* context,const ffxHeader* h){
  auto* d=reinterpret_cast<const ffxDispatchDescDenoiser*>(h);lastDispatch=*d;
  assert(d->header.pNext && d->header.pNext->type==FFX_API_DISPATCH_DESC_INPUT_1_SIGNAL_TYPE_DENOISER);
  if(!Fake::temporal)assert(d->flags==3 && d->frameIndex==731 && d->deltaTime==17.25f && d->jitterOffsets.x==.001f);
  auto* s=reinterpret_cast<const ffxDispatchDescDenoiserInput1Signal*>(h->pNext);
  assert(s->radiance.input.resource && s->radiance.output.resource && s->radiance.input.resource!=s->radiance.output.resource);
  Fake::events.push_back("denoise");++Fake::dispatches;
  Fake::dispatchedContexts.push_back(*context);if(Fake::duringDispatch)Fake::duringDispatch();
  if(Fake::dispatchThrow)throw 3;return static_cast<ffxReturnCode_t>(Fake::dispatchResult);}
};
'''
        harness = r'''
#include <limits>
#include "FSRDPrivateDenoise.cpp"
using Microsoft::WRL::ComPtr;
using namespace FSRD::PrivateDenoise;
int main(){
 ComPtr<ID3D12Device> device=new ID3D12Device;
 ComPtr<ID3D12GraphicsCommandList> list=new ID3D12GraphicsCommandList(device.Get());
 std::array<ComPtr<ID3D12Resource>,8> inputs;
 Parameters p{};p.providerId=42;p.maxRenderSize={2560,1440};
 p.settings={1,1,65504,50,0,.01f};
 p.conversion.RenderSize={1280,720,1.f/1280,1.f/720};p.conversion.NearPlane=.1f;p.conversion.FarPlane=1000;
 p.conversion.Flags=1|4|32;
 const int formats[]={DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32G8X24_TYPELESS,
  DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16_FLOAT,
  DXGI_FORMAT_R16_FLOAT,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM};
 for(unsigned i=0;i<inputs.size();++i){inputs[i]=new ID3D12Resource(device.Get());
  inputs[i]->desc.Format=formats[i];p.conversion.Resources.AsArray[i]=inputs[i].Get();}
 p.dispatch.flags=2;p.dispatch.renderSize={1280,720};p.dispatch.deltaTime=17.25f;p.dispatch.frameIndex=731;
 p.dispatch.cameraRight={1,0,0};p.dispatch.cameraUp={0,1,0};p.dispatch.cameraForward={0,0,1};
 p.dispatch.cameraNear=.1f;p.dispatch.cameraFar=1000;p.dispatch.cameraAspectRatio=16.f/9;p.dispatch.cameraFovAngleVertical=1.2f;
 p.dispatch.jitterOffsets={.001f,-.002f};p.dispatch.motionVectorScale={1,1,1};
 auto rejected=[&](const Parameters& q){const char* error=nullptr;auto w=Prepare(device.Get(),q,&error);
  assert(!w && error && *error && !Fake::contexts && !FSRDSubmission::pending);};
 {auto q=p;q.providerId=43;rejected(q);}
 {auto q=p;q.dispatch.header.pNext=reinterpret_cast<ffxHeader*>(uintptr_t(1));rejected(q);}
 {auto q=p;q.dispatch.commandList=list.Get();rejected(q);}
 {auto q=p;q.conversion.Flags|=128;rejected(q);}
 {auto q=p;q.conversion.Flags|=2;rejected(q);}
 {auto q=p;q.dispatch.flags=0;rejected(q);}
 {auto q=p;q.dispatch.deltaTime=0;rejected(q);}
 for(uint32_t bits:{0x7f800000u,0xff800000u,0x7fc00000u,0x7f800001u}){
  auto q=p;q.dispatch.deltaTime=std::bit_cast<float>(bits);rejected(q);
  q=p;q.settings.gaussianKernelRelaxation=std::bit_cast<float>(bits);rejected(q);
  q=p;q.dispatch.cameraRight.x=std::bit_cast<float>(bits);rejected(q);
 }
 {auto q=p;q.dispatch.cameraRight={};rejected(q);}
 {auto q=p;q.conversion.RenderSize.z=.5f;rejected(q);}
 {auto q=p;q.conversion.InvViewMatrix.m[0][0]=std::numeric_limits<float>::quiet_NaN();rejected(q);}
 {auto q=p;q.conversion.Resources.InSpecHitDist=nullptr;rejected(q);}
 {auto q=p;q.maxRenderSize={8192,8192};rejected(q);}
 {inputs[5]->desc.Format=DXGI_FORMAT_R16_TYPELESS;rejected(p);inputs[5]->desc.Format=formats[5];}
 {device->allocation=256ull*1024*1024;rejected(p);device->allocation=1280*720*8;}
 Fake::changedCount=true;rejected(p);Fake::changedCount=false;
 Fake::createFail=true;rejected(p);Fake::createFail=false;
 Fake::defaultFail=true;rejected(p);Fake::defaultFail=false;
 {auto q=p;q.settings.stabilityBias=1.5f;Fake::configureFail=true;rejected(q);Fake::configureFail=false;}
 {
  auto q=p;q.settings.stabilityBias=1.5f;auto w=Prepare(device.Get(),q);assert(w && Fake::contexts==1);
  assert(Fake::queriedProvider==42 && w->ProviderName()=="test RR 1.1");
  assert(w->EffectiveParameters().settings.stabilityBias==1.5f && w->EffectiveParameters().dispatch.flags==3);
  assert(w->EffectiveParameters().conversion.Resources.InRoughness==nullptr);
  Fake::events.clear();assert(w->Record(list.Get()) && w->Recorded() && w->Error().empty());
  assert((Fake::events==std::vector<std::string>{"retain","retain","convert","uav","denoise","srv","compose"}));
  assert(w->Outputs().composed && w->Outputs().composed.Get()!=inputs[0].Get());
  auto eventCount=Fake::events.size();assert(!w->Record(list.Get()) && Fake::events.size()==eventCount);
  assert(FfxApiProxy::lastDispatch.linearDepth.resource!=inputs[1].Get());
  auto destroys=Fake::destroys;w.reset();assert(Fake::contexts==1 && Fake::destroys==destroys);
  FSRDSubmission::pending.reset();assert(!Fake::contexts && Fake::destroys==destroys+1); // No cycle.
 }
 for(unsigned mode=0;mode<5;++mode){
  Fake::events.clear();Fake::retainFail=mode==0;Fake::convertFail=mode==1;
  Fake::dispatchResult=mode==2?FFX_API_RETURN_ERROR_RUNTIME_ERROR:FFX_API_RETURN_OK;
  Fake::dispatchThrow=mode==3;Fake::composeFail=mode==4;
  auto w=Prepare(device.Get(),p);assert(w);assert(!w->Record(list.Get()) && !w->Recorded() && !w->Error().empty());
  assert(!w->Outputs().composed);
  if(mode==0)assert(Fake::events==std::vector<std::string>{"retain"});
  if(mode==2||mode==3)assert(Fake::events.back()=="srv");
  if(mode<4)assert(std::find(Fake::events.begin(),Fake::events.end(),"compose")==Fake::events.end());
  auto destroys=Fake::destroys;w.reset();
  assert(Fake::contexts==(mode?1u:0u));FSRDSubmission::pending.reset();
  assert(!Fake::contexts && Fake::destroys==destroys+1);
 }
 Fake::retainFail=Fake::convertFail=Fake::dispatchThrow=Fake::composeFail=false;Fake::dispatchResult=0;
 {auto w=Prepare(device.Get(),p);list->type=D3D12_COMMAND_LIST_TYPE_COMPUTE;assert(!w->Record(list.Get()));
  list->type=0;assert(!w->Record(list.Get()));assert(!FSRDSubmission::pending);}
 assert(!Fake::contexts);
}
'''
        if temporal:
            # Reuse the exact old device/input/parameter fixture, not the old test body.
            harness = harness.split(' auto rejected=', 1)[0] + r'''
 Fake::temporal=true;p.maxRenderSize={1280,720};p.settings.stabilityBias=1.5f;
 SessionDesc desc{p.maxRenderSize,p.providerId,p.settings,77,32};
 const FrameAdmission admission{77,0x12340,true};
 auto parameters=[&](unsigned ordinal){auto q=p;q.dispatch.frameIndex=1000+ordinal;
  q.dispatch.flags=ordinal?2:3;q.conversion.Flags=ordinal?37:69;
  q.dispatch.deltaTime=17.25f+float(ordinal)*.01f;
  q.dispatch.cameraPositionDelta={float(ordinal),0,0};
  q.conversion.PrevViewMatrix.m[0][0]=float(ordinal+1);return q;};
 auto create=[&]{const char* error=nullptr;auto s=CreateSession(device.Get(),desc,&error);
  assert(s && error && !*error && !s->Stopped() && !s->Complete());return s;};
 for(unsigned kind=0;kind<6;++kind){auto d=desc;
  if(kind==0)d.epoch=0;if(kind==1)d.frameLimit=0;if(kind==2)d.frameLimit=SessionDesc::MaxFrames+1;
  if(kind==3)d.maxRenderSize={};if(kind==4)d.settings.maxRadiance=0;if(kind==5)d.providerId=43;
  const char* error=nullptr;assert(!CreateSession(device.Get(),d,&error)&&error&&*error&&!Fake::contexts);
 }
 {
  const auto creations=Fake::creates,queries=Fake::defaultQueries,configs=Fake::configurationCalls;
  const auto dispatches=Fake::dispatches,destroys=Fake::destroys;auto s=create();
  assert(Fake::creates==creations+1&&Fake::defaultQueries==queries+6&&Fake::configurationCalls==configs+1);
  std::shared_ptr<Work> previous;
  for(unsigned i=0;i<desc.frameLimit;++i){const auto q=parameters(i);const char* error=nullptr;
   auto w=PrepareFrame(s,q,admission,&error);assert(w&&error&&!*error);
   assert(w->EffectiveParameters().dispatch.flags==q.dispatch.flags&&w->EffectiveParameters().conversion.Flags==q.conversion.Flags);
   assert(w->EffectiveParameters().dispatch.frameIndex==q.dispatch.frameIndex);
   assert(!s->AcknowledgeExecuted(*w,admission.canonicalDirectQueue));
   assert(!PrepareFrame(s,q,admission)&&!s->Stopped()); // No second pending reservation.
   if(previous){assert(previous->Outputs().radiance.Get()!=w->Outputs().radiance.Get());
    assert(previous->Outputs().depth.Get()!=w->Outputs().depth.Get());}
   assert(w->Record(list.Get())&&w->Recorded()&&Fake::lastConversionFlags==q.conversion.Flags);
   assert(FfxApiProxy::lastDispatch.frameIndex==q.dispatch.frameIndex&&FfxApiProxy::lastDispatch.flags==q.dispatch.flags);
   assert(FfxApiProxy::lastDispatch.cameraPositionDelta.x==float(i));
   assert(FfxApiProxy::lastDispatch.linearDepth.resource==w->Outputs().depth.Get());
   assert(!w->Record(list.Get()));assert(!PrepareFrame(s,parameters(i+1),admission));
   assert(!s->AcknowledgeExecuted(*w,0)&&!s->AcknowledgeExecuted(*w,admission.canonicalDirectQueue+1));
   assert(s->AcknowledgeExecuted(*w,admission.canonicalDirectQueue));
   assert(!s->AcknowledgeExecuted(*w,admission.canonicalDirectQueue));
   assert(s->AcknowledgedFrames()==i+1&&!s->Stopped());
   previous.reset();previous=w;FSRDSubmission::pending.reset();
  }
  assert(s->Complete()&&!PrepareFrame(s,parameters(desc.frameLimit),admission)&&!s->Stopped());
  assert(Fake::creates==creations+1&&Fake::defaultQueries==queries+6&&Fake::configurationCalls==configs+1);
  assert(Fake::dispatches==dispatches+desc.frameLimit);
  for(auto* identity:Fake::dispatchedContexts)assert(identity==Fake::dispatchedContexts.front());
  previous.reset();assert(Fake::contexts==1);s.reset();assert(!Fake::contexts&&Fake::destroys==destroys+1);
 }
 // Admission refusals do not reserve/poison; accepted resource failures do.
 for(unsigned kind=0;kind<12;++kind){auto s=create();auto q=parameters(0);auto a=admission;
  if(kind==0)a.epoch=78;if(kind==1)a.canonicalDirectQueue=0;if(kind==2)a.sameViewAndCoordinateOrigin=false;
  if(kind==3)q.providerId=43;if(kind==4)q.maxRenderSize.width=1281;if(kind==5)q.settings.stabilityBias=1.6f;
  if(kind==6)q.dispatch.flags=2;if(kind==7)q.conversion.Flags=37;
  if(kind==8)q.dispatch.deltaTime=0;if(kind==9)q.settings.gaussianKernelRelaxation=-0.f;
  if(kind==10)q.dispatch.header.pNext=reinterpret_cast<ffxHeader*>(uintptr_t(1));
  if(kind==11)q.dispatch.deltaTime=std::bit_cast<float>(0x7fc12345u);
  const char* error=nullptr;assert(!PrepareFrame(s,q,a,&error)&&error&&*error&&!s->Stopped());
  auto w=PrepareFrame(s,parameters(0),admission);assert(w);w.reset();assert(s->Stopped());
  assert(!PrepareFrame(s,parameters(0),admission));s.reset();assert(!Fake::contexts);
 }
 for(unsigned kind=0;kind<7;++kind){auto s=create();auto w=PrepareFrame(s,parameters(0),admission);assert(w);
  assert(w->Record(list.Get())&&s->AcknowledgeExecuted(*w,admission.canonicalDirectQueue));
  auto q=parameters(1);auto a=admission;
  if(kind==0)q.dispatch.frameIndex=1000;if(kind==1)q.dispatch.frameIndex=1002;
  if(kind==2)q.dispatch.flags=3;if(kind==3)q.conversion.Flags=101;
  if(kind==4)a.canonicalDirectQueue++;if(kind==5)q.dispatch.renderSize.width=1279;
  if(kind==6)q.settings.maxRadiance=65000;
  assert(!PrepareFrame(s,q,a)&&!s->Stopped());w.reset();FSRDSubmission::pending.reset();s.reset();assert(!Fake::contexts);
 }
 // Counter wrap is never a temporal join. Explicit Stop cannot be cleared.
 {auto s=create();auto q=parameters(0);q.dispatch.frameIndex=UINT32_MAX;
  auto w=PrepareFrame(s,q,admission);assert(w&&w->Record(list.Get()));
  assert(s->AcknowledgeExecuted(*w,admission.canonicalDirectQueue));q=parameters(1);q.dispatch.frameIndex=0;
  assert(!PrepareFrame(s,q,admission));s->Stop();assert(s->Stopped());
  w.reset();FSRDSubmission::pending.reset();s.reset();assert(!Fake::contexts);}
 // Exact session/Work identity is required, not just matching numeric frames.
 {auto a=create(),b=create();auto wa=PrepareFrame(a,parameters(0),admission),wb=PrepareFrame(b,parameters(0),admission);
  assert(wa&&wb&&wa->Record(list.Get())&&wb->Record(list.Get()));
  assert(!a->AcknowledgeExecuted(*wb,admission.canonicalDirectQueue));
  assert(a->AcknowledgeExecuted(*wa,admission.canonicalDirectQueue));
  assert(b->AcknowledgeExecuted(*wb,admission.canonicalDirectQueue));
  wa.reset();wb.reset();a.reset();b.reset();assert(Fake::contexts==2);FSRDSubmission::pending.reset();assert(!Fake::contexts);}
 // A successful Work abandoned before the host's actual consumer acknowledgement poisons.
 {auto s=create();auto w=PrepareFrame(s,parameters(0),admission);assert(w&&w->Record(list.Get()));
  w.reset();assert(s->Stopped()&&!PrepareFrame(s,parameters(1),admission));
  s.reset();assert(Fake::contexts==1);FSRDSubmission::pending.reset();assert(!Fake::contexts);}
 // Retain-failure, conversion/provider/composition failures, and throws stop history permanently.
 for(unsigned kind=0;kind<7;++kind){auto s=create();auto w=PrepareFrame(s,parameters(0),admission);assert(w);
  Fake::retainFail=kind==0;Fake::convertFail=kind==1;Fake::dispatchResult=kind==2?FFX_API_RETURN_ERROR_RUNTIME_ERROR:0;
  Fake::dispatchThrow=kind==3;Fake::composeFail=kind==4;if(kind==5)list->type=2;if(kind==6)s->Stop();
  assert(!w->Record(list.Get())&&!w->Recorded()&&s->Stopped());
  assert(!s->AcknowledgeExecuted(*w,admission.canonicalDirectQueue)&&!PrepareFrame(s,parameters(1),admission));
  list->type=0;Fake::retainFail=Fake::convertFail=Fake::dispatchThrow=Fake::composeFail=false;Fake::dispatchResult=0;
  w.reset();s.reset();FSRDSubmission::pending.reset();assert(!Fake::contexts);}
 {auto s=create();auto q=parameters(0);q.conversion.Resources.InColor=nullptr;
  assert(!PrepareFrame(s,q,admission)&&s->Stopped());s.reset();assert(!Fake::contexts);}
 {auto s=create();Fake::allocationFail=true;assert(!PrepareFrame(s,parameters(0),admission)&&s->Stopped());
  Fake::allocationFail=false;s.reset();assert(!Fake::contexts);}
 // Concurrent preparation has one winner; concurrently retrying one Record cannot enter provider twice.
 {auto s=create();std::array<std::shared_ptr<Work>,2> contenders;std::array<std::thread,2> threads;
  for(unsigned i=0;i<2;++i)threads[i]=std::thread([&,i]{contenders[i]=PrepareFrame(s,parameters(0),admission);});
  for(auto& thread:threads)thread.join();assert(bool(contenders[0])!=bool(contenders[1]));
  auto w=contenders[0]?contenders[0]:contenders[1];const auto dispatches=Fake::dispatches;
  bool results[2]{};for(unsigned i=0;i<2;++i)threads[i]=std::thread([&,i]{results[i]=w->Record(list.Get());});
  for(auto& thread:threads)thread.join();assert(results[0]!=results[1]&&Fake::dispatches==dispatches+1);
  assert(s->AcknowledgeExecuted(*w,admission.canonicalDirectQueue));
  w.reset();contenders={};s.reset();FSRDSubmission::pending.reset();assert(!Fake::contexts);}
 // Reentrant Stop does not deadlock provider recording or preparation, and no success is published.
 {auto s=create();static Session* stopping;stopping=s.get();
  Fake::duringAllocation=[](){stopping->Stop();};assert(!PrepareFrame(s,parameters(0),admission)&&s->Stopped());
  Fake::duringAllocation=nullptr;s.reset();assert(!Fake::contexts);}
 {auto s=create();auto w=PrepareFrame(s,parameters(0),admission);static Session* stopping;stopping=s.get();
  Fake::duringDispatch=[](){stopping->Stop();};assert(!w->Record(list.Get())&&s->Stopped()&&!w->Recorded());
  Fake::duringDispatch=nullptr;w.reset();s.reset();assert(Fake::contexts==1);
  FSRDSubmission::pending.reset();assert(!Fake::contexts);}
 // Five retained frame leases fit this fixture; the sixth exceeds fixed aggregate budget.
 // A returned Execute acknowledgement is deliberately NOT a resource-retirement event.
 {auto s=create();std::vector<std::shared_ptr<Work>> retained;
  for(unsigned i=0;i<5;++i){auto w=PrepareFrame(s,parameters(i),admission);assert(w&&w->Record(list.Get()));
   assert(s->AcknowledgeExecuted(*w,admission.canonicalDirectQueue));retained.push_back(w);}
  const char* error=nullptr;assert(!PrepareFrame(s,parameters(5),admission,&error)&&s->Stopped());
  assert(std::string(error)=="session retained resource budget exceeded");
  retained.clear();std::weak_ptr<Session> weak=s;s.reset();assert(weak.expired()&&Fake::contexts==1);
  // The ticket may represent unsubmitted/failed-signal work. No lifetime shortcut exists.
  assert(Fake::contexts==1);FSRDSubmission::pending.reset();assert(!Fake::contexts);}
 assert(!Fake::contexts&&!FSRDSubmission::pending);
}
'''
            harness = '#include <thread>\n' + harness
            if visual:
                # Same real Work/provider/ownership assertions, extended for the visual cap.
                harness = harness.replace('SessionDesc desc{p.maxRenderSize,p.providerId,p.settings,77,32};',
                                          'SessionDesc desc{p.maxRenderSize,p.providerId,p.settings,77,18000};')
                harness = harness.replace('17.25f+float(ordinal)*.01f', '17.25f+float(ordinal%32)*.01f')
        with tempfile.TemporaryDirectory(prefix='fsrd-private-denoise-') as directory:
            tmp = Path(directory)
            files = {'pch.h': '#pragma once\n#include "d3d12.h"\n', 'd3d12.h': windows,
                     'wrl/client.h': '#pragma once\n#include "d3d12.h"\n',
                     'ffx_api.h': api, 'ffx_api_types.h': types,
                     'resource_tracking/FSRDSubmission.h': submission,
                     'shaders/fsrd_preprocess/FSRDPreprocessor_Dx12.h': converter,
                     'proxies/FfxApi_Proxy.h': proxy, 'main.cpp': harness}
            for name, text in files.items():
                path = tmp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            for number, optimization in enumerate((['-O0'], ['-O3', '-ffast-math', '-ffp-contract=fast'])):
                binary = tmp / f'test-{number}'
                result = subprocess.run([compiler, '-std=c++20', '-pthread', *optimization, '-I', str(tmp), '-I', str(BASE),
                    '-I', str(ROOT / 'OptiScaler/include'), str(tmp / 'main.cpp'), '-o', str(binary)],
                    capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()

"""Inactive-until-marker RGB identity host admission and recording adapters.

Compiles actual host functions with CPU/D3D recording mocks at O0/O3-fast.
This is not shader execution, native state restoration, or GPU identity proof.
"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'OptiScaler/upscalers/ffx'
HOST = (BASE / 'FSRDCyberpunkFogProbe.cpp').read_text()


def section(start, end):
    return HOST[HOST.index(start):HOST.index(end, HOST.index(start))]


class FogRgbIdentityHost(unittest.TestCase):
    def test_explicit_marker_exclusive_transaction_and_closed_reader(self):
        arm = section('void ArmRgbIdentity(', 'void ArmPrivateReset(')
        self.assertIn('L"FSRRR-prefog-rgb-identity.request"', arm)
        self.assertIn('!controls.is_object() || controls.size() != 1', arm)
        self.assertIn('controls.at("mode") != "rgb_identity_only"', arm)
        for required in ('fog.queued', 'fog.busy', 'fog.attempted', 'guides.queued', 'guides.busy',
                         'guides.attempted', 'captureStarted.load()', 'lightingRequested.load()',
                         'privateResetPacket.load', 'rgbIdentityPacket.load', 'file_size(path) > 4096'):
            self.assertIn(required, arm)
        self.assertLess(arm.index('captureRequestMutex'), arm.index('privateResetArming.exchange(true'))
        self.assertLess(arm.index('privateResetArming.exchange(true'), arm.index('FSRDFogLayerCapture::Request()'))
        self.assertLess(arm.index('FSRDFogLayerCapture::Request()'), arm.index('DeleteFileW(path.c_str())'))
        self.assertLess(arm.index('DeleteFileW(path.c_str())'), arm.index('rgbIdentityPacket.store('))
        self.assertIn('~FinishArming() { privateResetArming.store(false, std::memory_order_release); }', arm)
        self.assertIn('if (queued) FSRDFogLayerCapture::CancelRequest();', arm)
        reader = re.search(r'\{\s*(?://[^\n]*\n\s*)*std::ifstream file\(path, std::ios::binary\);\s*file >> controls;\s*\}', arm)
        self.assertIsNotNone(reader)
        self.assertLess(reader.end(), arm.index('DeleteFileW(path.c_str())'))
        for forbidden in ('RequestEarlyGuides(', 'AllocateTargets(', 'PrivateDenoise::', 'lightingRequested.store(true)'):
            self.assertNotIn(forbidden, arm)
        poll = section('void PollRearm(', 'const Json& EarlyInput(')
        private = section('void ArmPrivateReset(', 'uint64_t AdmitPrivateResetSubmission(')
        for other in (poll, private):
            self.assertIn('captureRequestMutex', other)
            self.assertIn('rgbIdentityPacket.load(std::memory_order_acquire)', other)

    def test_host_order_ownership_budget_and_no_late_route_change(self):
        capture = section('std::shared_ptr<CapturePlan> PrepareCapture(', 'void PublishFogEndpoint(')
        self.assertLess(capture.index('IsFullRgbViewport(state,'), capture.index('captureStarted.exchange(true)'))
        self.assertIn('desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || desc.MipLevels != 1', capture)
        self.assertIn('nativeCaller != authenticatedImage.load() + FSRD::CyberpunkFogDepth::DrawReturnRva', capture)
        self.assertIn('copyCount = plan->rgbIdentity ? 3 : 2', capture)
        self.assertIn('mainBytes + copyCount * copyBytes + authoredBytes', capture)
        self.assertLess(capture.index('FSRDSubmission::Retain('), capture.index('CopyMain(list,'))
        self.assertLess(capture.index('CopyMain(list,'), capture.index('RecordRgbIdentity(list,'))
        self.assertRegex(capture, r'if \(plan->rgbIdentity\)\s*RecordRgbIdentity\([^\n]+\n\s*else\s*\{\s*RecordPrivateReset')
        plan = section('struct CapturePlan\n', 'bool SameEarlyReservation(')
        self.assertNotIn('FogRgbWrite::Work', plan)
        record = section('void RecordRgbIdentity(', 'void PrepareFogDepth(')
        self.assertIn('RecordSceneRgb(host, input, [&] { return work->Record(list); })', record)
        self.assertNotIn('RecordPrivateCompute(', record)
        self.assertNotIn('originalDraw(', record)
        self.assertLess(record.index('plan.layers.before.state ='), record.index('RecordSceneRgb('))
        self.assertLess(record.index('RecordSceneRgb('), record.index('CopyMain(list,'))
        self.assertIn('if (!privateStatesKnown) throw;', record)
        self.assertIn('if (!snapshotStarted) plan.layers.rgbIdentity = {};', record)
        self.assertIn('PrivateResetFatal();', record)
        draw = section('void WINAPI HookDraw(', 'void WINAPI HookDrawIndexed(')
        self.assertEqual(draw.count('originalDraw(list, count, instances, start, firstInstance);'), 1)
        self.assertLess(draw.index('PrepareCapture('), draw.index('originalDraw('))
        self.assertLess(draw.index('earlyFatalRecording.load()'), draw.index('originalDraw('))
        feature = (BASE / 'FSRDFeature_Dx12.cpp').read_text()
        self.assertRegex(feature, r'if \(_denoiser.IsCreated\(\)\)\s*FSRDCyberpunkFogProbe::ArmRgbIdentity\(')
        self.assertEqual(feature.count('ArmRgbIdentity('), 1)
        self.assertNotIn('rgbIdentityPacket', feature)
        project = (ROOT / 'OptiScaler/OptiScaler.vcxproj').read_text()
        for suffix in ('.h', '.cpp'):
            self.assertEqual(project.count('FSRDCyberpunkFogRgbWrite' + suffix + '"'), 1)

    def test_exact_topology_original_preparation_body(self):
        self.assertIn('0x1f6fbc, 0x1a7, "2c52a561dbce82c04eed3c3b0cfa4ee525ae3aee61d430529b9ff1a6b579b275"', HOST)
        executable = Path('/home/synse/Games/Heroic/Games/Cyberpunk 2077/bin/x64/Cyberpunk2077.exe')
        if not executable.is_file():
            self.skipTest('Optional installed executable fixture unavailable')
        with executable.open('rb') as stream:
            stream.seek(0x1f6fbc - 0xc00)
            self.assertEqual(hashlib.sha256(stream.read(0x1a7)).hexdigest(),
                             '2c52a561dbce82c04eed3c3b0cfa4ee525ae3aee61d430529b9ff1a6b579b275')

    def test_actual_host_admission_recording_and_failure_adapters(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX for actual host adapter compilation')
        functions = section('bool IsFullRgbViewport(', 'void PrepareFogDepth(')
        mocks = r'''
#include <json.hpp>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#ifndef __fastcall
#define __fastcall
#endif
using UINT=unsigned;using LONG=int32_t;using BOOL=int;using D3D12_RESOURCE_STATES=uint32_t;
constexpr BOOL FALSE=0;constexpr uint32_t D3D12_RESOURCE_STATE_COPY_DEST=0x400;
using Json=nlohmann::json;
#define IID_PPV_ARGS(x) (x)
#define FAILED(x) ((x)<0)
struct IUnknown{};struct Resource{};struct Heap{};struct Pso{};
template<class T>struct ComPtr{T* p=nullptr;T* Get()const{return p;}T* operator->()const{return p;}
 T** operator&(){return &p;}explicit operator bool()const{return p!=nullptr;}};
struct Device{IUnknown* identity=nullptr;bool bad=false;int QueryInterface(IUnknown** out){*out=identity;return bad?-1:0;}};
using ID3D12Resource=Resource;using ID3D12Device=Device;struct ID3D12GraphicsCommandList{};
struct View{uint32_t Format=10,ViewDimension=4;struct{uint32_t MipSlice=0,PlaneSlice=0;}Texture2D;};
struct Handle{uintptr_t ptr=0;};
struct Viewport{float TopLeftX=0,TopLeftY=0,Width=4,Height=4,MinDepth=0,MaxDepth=1;};
struct Rect{LONG left=0,top=0,right=4,bottom=4;};
struct ListState{uint64_t generation=12;bool known=true,predicated=false,renderPass=false;UINT queryCount=0,viewportCount=1,scissorCount=1;
 std::array<Viewport,1> viewports;std::array<Rect,1> scissors;};
struct BoundRtv{bool known=true;ComPtr<Resource> resource;ComPtr<Heap> heap;View view;};
struct Scope{ID3D12GraphicsCommandList* rtvList=nullptr;std::array<Handle,1> rtvs;BoundRtv boundRtv;};
Scope current;Scope* scope=&current;
struct DepthSnapshot{uint32_t handle=7;uintptr_t native=0x1000;
 struct{uint64_t serial=2,recordingGeneration=12;uintptr_t engine=0x2000,list=0,view=0x3000;}scope;};
struct Endpoint{ComPtr<IUnknown> list;};
struct Texture{ComPtr<Resource> resource;D3D12_RESOURCE_STATES state=0x400;};
struct CapturePlan{bool rgbIdentity=true,rgbIdentityPrepared=true,depthPrepared=true,fatalEarlyRecording=false;
 ComPtr<Device> device;ComPtr<Resource> main;ComPtr<Pso> originalPso;ComPtr<Heap> privateHeap,sourceHeap;
 Handle frozenOriginalRtv{100},originalRtv{200};View originalView;ListState drawState;DepthSnapshot depthSnapshot;
 uintptr_t depthFrameObject=0x4000;uint32_t depthFrame=99;
 struct{Texture before,rgbIdentity;}layers;std::shared_ptr<Endpoint> endpoint=std::make_shared<Endpoint>();Json provenance;};
struct RgbIdentityPacket{ComPtr<IUnknown> deviceIdentity;UINT width=4,height=4;};
std::atomic<RgbIdentityPacket*> rgbIdentityPacket{nullptr};std::atomic<void*> privateResetPacket{nullptr};
std::atomic<bool> privateResetArming{false};std::atomic<uintptr_t> authenticatedImage{0x140000000};
struct Registry{std::mutex mutex;std::unordered_map<IUnknown*,ListState> lists;};Registry registry;
Registry& Data(){return registry;}
bool sameDepth=true,codeValid=true,readValid=true;uint32_t topologyValue=4;constexpr int FogTopologyCode=5;
bool SameFogDepthSource(const CapturePlan&)noexcept{return sameDepth&&scope;}
bool MatchLiveCode(uintptr_t image,int code){assert(image==0x140000000&&code==5);return codeValid;}
bool ReadEarlyAt(uintptr_t engine,uintptr_t offset,uint32_t& value){assert(engine==0x2000&&offset==0x628);value=topologyValue;return readValid;}
std::vector<std::string> events;unsigned transitionCount=0;int failTransition=-1;bool failCopy=false,prepareFailure=false,recordFailure=false,recordThrow=false;
unsigned sceneMode=0; // 1 refusal,2 fatal,3 missing actual OM restore.
void NativeRtv(ID3D12GraphicsCommandList* list,UINT count,const Handle* h,BOOL contiguous,const void* dsv){
 assert(list&&count==1&&h&&h->ptr==100&&!contiguous&&!dsv);events.push_back("restoreOM");}
void NativePso(){};auto originalSetRtv=&NativeRtv;auto originalSetPso=&NativePso;
struct FogDepthEngineHost{const CapturePlan& plan;};
namespace FSRD::CyberpunkFogDenoiseAccess{
struct Input{uintptr_t image=0,list=0,originalPso=0;uint64_t originalFogScope=0;struct{uint32_t handle=0;uintptr_t native=0;}depth;bool copySource=false;};
enum class SceneOutcome:uint8_t{Refused,SceneFailedRestored,SceneRecordedRestored,ScopeLostAfterMutation};
struct Result{SceneOutcome outcome=SceneOutcome::Refused;bool callbackEntered=false,bindingsRestored=false;};
template<class H,class F>Result RecordSceneRgb(H& host,const Input& input,F callback){
 events.push_back("admit");assert(input.image==0x140000000&&input.originalFogScope==2&&input.depth.handle==7&&input.depth.native==0x1000&&!input.copySource);
 if(sceneMode==1||!host.IsAdmittedFogRgbScope(input))return{};
 bool success=false;try{success=callback();}catch(...){}
 if(sceneMode==2)return{SceneOutcome::ScopeLostAfterMutation,true,false};
 if(sceneMode!=3)host.RestoreOriginalFogTarget(input.list);
 return{success?SceneOutcome::SceneRecordedRestored:SceneOutcome::SceneFailedRestored,true,true};
}}
namespace FSRD::CyberpunkFogRgbWrite{
struct Work{bool recorded=false;bool Record(ID3D12GraphicsCommandList*){events.push_back("rgb");if(recordThrow)throw std::runtime_error("record");recorded=!recordFailure;return recorded;}
 bool Recorded()const{return recorded;}};
std::shared_ptr<Work> Prepare(Device* device,UINT width,UINT height,const ComPtr<Resource>& source,const ComPtr<Resource>& target,
 const View&,const Viewport&,const Rect&,const char** error){assert(device&&width==4&&height==4&&source&&target&&source.Get()!=target.Get());
 events.push_back("prepare");if(prepareFailure){*error="prepare";return{};}return std::make_shared<Work>();}
}
void Transition(ID3D12GraphicsCommandList*,Resource* resource,uint32_t before,uint32_t after){assert(resource&&before==0x400&&after==0xc0);
 events.push_back("transition");if(int(transitionCount++)==failTransition)throw std::runtime_error("transition");}
void CopyMain(ID3D12GraphicsCommandList*,const CapturePlan& plan,Resource* target){assert(target&&target!=plan.main.Get());events.push_back("snapshot");if(failCopy)throw std::runtime_error("copy");}
[[noreturn]]void PrivateResetFatal()noexcept{std::_Exit(79);}
'''
        harness = r'''
IUnknown deviceIdentity,listIdentity,wrongIdentity;Device device{&deviceIdentity};Resource mainResource,beforeResource,snapshotResource,wrongResource;
Heap privateHeap,sourceHeap,wrongHeap;Pso pso;ID3D12GraphicsCommandList list,wrongList;RgbIdentityPacket packet;
void Setup(CapturePlan& plan){
 plan=CapturePlan{};events.clear();transitionCount=0;failTransition=-1;failCopy=prepareFailure=recordFailure=recordThrow=false;sceneMode=0;
 sameDepth=codeValid=readValid=true;topologyValue=4;privateResetArming=false;privateResetPacket=nullptr;scope=&current;
 originalSetRtv=&NativeRtv;originalSetPso=&NativePso;device.identity=&deviceIdentity;device.bad=false;
 packet={ComPtr<IUnknown>{&deviceIdentity},4,4};rgbIdentityPacket=&packet;
 plan.device={&device};plan.main={&mainResource};plan.layers.before.resource={&beforeResource};plan.layers.rgbIdentity.resource={&snapshotResource};
 plan.privateHeap={&privateHeap};plan.sourceHeap={&sourceHeap};plan.originalPso={&pso};plan.endpoint->list={&listIdentity};plan.depthSnapshot.scope.list=uintptr_t(&list);
 current=Scope{};current.rtvList=&list;current.rtvs[0]=plan.originalRtv;current.boundRtv.resource=plan.main;current.boundRtv.heap=plan.sourceHeap;
 registry.lists.clear();registry.lists[&listIdentity]=plan.drawState;
}
int main(int argc,char** argv){
 CapturePlan plan;Setup(plan);
 const std::string mode=argc>1?argv[1]:"normal";
 if(mode=="fatal"||mode=="missing-OM"){sceneMode=mode=="fatal"?2:3;RecordRgbIdentity(&list,plan);assert(false);}
 assert(IsFullRgbViewport(plan.drawState,4,4));
 for(unsigned field=0;field<6;++field)for(uint32_t bits:{0x7f800000u,0xff800000u,0x7fc00001u,0x7f800001u,0x80000000u,1u}){
  Setup(plan);auto& v=plan.drawState.viewports[0];float* fields[]={&v.TopLeftX,&v.TopLeftY,&v.Width,&v.Height,&v.MinDepth,&v.MaxDepth};
  *fields[field]=std::bit_cast<float>(bits);assert(!IsFullRgbViewport(plan.drawState,4,4));
 }
 for(unsigned bad=0;bad<37;++bad){
  Setup(plan);plan.layers.before.state=0xc0;assert(SameFogRgbSource(plan));
  switch(bad){
   case 0:rgbIdentityPacket=nullptr;break;case 1:privateResetPacket=&packet;break;case 2:privateResetArming=true;break;
   case 3:plan.rgbIdentity=false;break;case 4:plan.rgbIdentityPrepared=false;break;case 5:plan.privateHeap={};break;
   case 6:plan.frozenOriginalRtv.ptr=0;break;case 7:plan.layers.before.resource={};break;case 8:plan.layers.before.state=0x400;break;
   case 9:plan.layers.rgbIdentity.resource={};break;case 10:originalSetRtv=nullptr;break;case 11:originalSetPso=nullptr;break;
   case 12:sameDepth=false;break;case 13:codeValid=false;break;case 14:readValid=false;break;case 15:topologyValue=5;break;
   case 16:current.rtvList=&wrongList;break;case 17:++current.rtvs[0].ptr;break;case 18:current.boundRtv.known=false;break;
   case 19:current.boundRtv.resource={&wrongResource};break;case 20:current.boundRtv.heap={&wrongHeap};break;
   case 21:++current.boundRtv.view.Format;break;case 22:++current.boundRtv.view.ViewDimension;break;
   case 23:++current.boundRtv.view.Texture2D.MipSlice;break;case 24:++current.boundRtv.view.Texture2D.PlaneSlice;break;
   case 25:device.bad=true;break;case 26:device.identity=nullptr;break;case 27:device.identity=&wrongIdentity;break;
   case 28:registry.lists.clear();break;case 29:registry.lists[&listIdentity].known=false;break;
   case 30:registry.lists[&listIdentity].predicated=true;break;case 31:registry.lists[&listIdentity].renderPass=true;break;
   case 32:registry.lists[&listIdentity].queryCount=1;break;case 33:++registry.lists[&listIdentity].generation;break;
   case 34:registry.lists[&listIdentity].viewportCount=0;break;case 35:registry.lists[&listIdentity].scissorCount=0;break;
   case 36:registry.lists[&listIdentity].scissors[0].right=3;break;
  }
  assert(!SameFogRgbSource(plan));
 }
 Setup(plan);RecordRgbIdentity(&list,plan);
 assert((events==std::vector<std::string>{"prepare","transition","admit","rgb","restoreOM","snapshot","transition"}));
 auto evidence=plan.provenance["rgb_identity_control"];
 assert(evidence["outcome"]==2&&evidence["draw_recorded"]==true&&evidence["identity_passed"].is_null());
 assert(evidence["original_target_restored"]==true&&evidence["scope"]==2&&evidence["frame_source_value"]==99);
 assert(plan.layers.before.state==0xc0&&plan.layers.rgbIdentity.state==0xc0);
 for(unsigned bad=0;bad<5;++bad){
  Setup(plan);switch(bad){case 0:prepareFailure=true;break;case 1:sceneMode=1;break;case 2:plan.depthPrepared=false;break;
   case 3:plan.layers.before.state=0xc0;break;case 4:rgbIdentityPacket=nullptr;break;}
  RecordRgbIdentity(&list,plan);assert(!plan.layers.rgbIdentity.resource);
  for(const auto& e:events)assert(e!="rgb"&&e!="snapshot");
 }
 for(bool throws:{false,true}){
  Setup(plan);recordFailure=!throws;recordThrow=throws;RecordRgbIdentity(&list,plan);
  assert(plan.layers.rgbIdentity.resource&&plan.layers.rgbIdentity.state==0xc0);
  const auto& e=plan.provenance["rgb_identity_control"];assert(e["outcome"]==1&&e["draw_recorded"]==false&&e["original_target_restored"]==true);
 }
 for(unsigned where=0;where<3;++where){
  Setup(plan);if(where==0)failTransition=0;else if(where==1)failCopy=true;else failTransition=1;
  bool threw=false;try{RecordRgbIdentity(&list,plan);}catch(const std::runtime_error&){threw=true;}
  assert(threw&&plan.layers.rgbIdentity.resource); // Never drop an owner after a possibly recorded copy.
 }
 Setup(plan);plan.rgbIdentity=false;RecordRgbIdentity(&list,plan);assert(events.empty());
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-rgb-identity-host-') as temporary:
            source = Path(temporary) / 'test.cpp'
            source.write_text(mocks + functions + harness)
            for flags in (['-O0'], ['-O3', '-ffast-math', '-ffp-contract=fast']):
                binary = Path(temporary) / 'test'
                subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags,
                                '-I', str(ROOT / 'external/nlohmann'), str(source), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)
                for mode in ('fatal', 'missing-OM'):
                    self.assertEqual(subprocess.run([str(binary), mode]).returncode, 79)


if __name__ == '__main__':
    unittest.main()

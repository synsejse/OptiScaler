"""One-shot scene RESET host gates and actual recording adapters.

Compiles extracted production functions with bounded CPU/D3D mocks at O0/O3.
This tests admission/control flow, not shader output or native GPU restoration.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'OptiScaler/upscalers/ffx'
HOST = (BASE / 'FSRDCyberpunkFogProbe.cpp').read_text()


def section(start, end):
    return HOST[HOST.index(start):HOST.index(end, HOST.index(start))]


class SceneResetHost(unittest.TestCase):
    def test_explicit_fixed_route_marker_and_no_identity_capture(self):
        arm = section('void ArmPrivateReset(', 'uint64_t AdmitPrivateResetSubmission(')
        self.assertIn('L"FSRRR-prefog-reset.request"', arm)
        self.assertIn('const bool sceneReset = controls.at("mode") == "scene_reset_once"', arm)
        self.assertIn('if (sceneReset && (!FSRD::PreFogSession::LateSrOnly() ||', arm)
        self.assertIn('!MatchLiveCode(authenticatedImage.load(), FogTopologyCode)', arm)
        self.assertLess(arm.index('if (sceneReset &&'), arm.index('AllocateTargets('))
        self.assertLess(arm.index('packet->sceneResetOnce = sceneReset'), arm.index('privateResetPacket.store('))
        self.assertLess(arm.index('DeleteFileW(path.c_str())'), arm.index('privateResetPacket.store('))
        capture = section('std::shared_ptr<CapturePlan> PrepareCapture(', 'void PublishFogEndpoint(')
        self.assertIn('plan->sceneResetOnce = resetPacket && resetPacket->sceneResetOnce', capture)
        self.assertIn('((plan->sceneResetOnce || plan->temporal) && (rgbPacket || !FSRD::PreFogSession::LateSrOnly()))', capture)
        self.assertLess(capture.index('IsFullRgbViewport(state,'), capture.index('captureStarted.exchange(true)'))
        self.assertIn('if (plan->rgbIdentity)\n    {\n        allocate(beforeDesc,', capture)
        scene = section('void RecordSceneReset(', 'void RecordPrivateReset(')
        for forbidden in ('Transition(', 'ResourceBarrier(', 'CopyMain(', 'rgb_identity_control',
                          'layers.rgbIdentity', 'originalDraw(', 'RecordPrivateCompute('):
            self.assertNotIn(forbidden, scene)
        self.assertIn('plan.layers.privateResetSceneWrite = true', scene)
        self.assertIn('"native_target_alpha_unwritten"', scene)
        self.assertIn('"independent_one_shot_RESET_only"', scene)
        plan = section('struct CapturePlan\n', 'bool SameEarlyReservation(')
        for forbidden in ('PrivateDenoise::Work', 'FogRgbWrite::Work', 'shared_ptr<PrivateResetPacket>',
                          'unique_ptr<PrivateResetPacket>'):
            self.assertNotIn(forbidden, plan)
        self.assertIn('PrivateResetPacket* packet', plan)

    def test_actual_private_then_scene_order_and_complete_consumer_guard(self):
        record = section('void RecordPrivateReset(', 'std::shared_ptr<CapturePlan> PrepareCapture(')
        points = ('packet->denoise = work', 'policy.EmbedConsumer(', 'work->Record(list)',
                  'plan.layers.privateReset =', 'RecordSceneReset(list, plan, *packet)',
                  '["scene_modified"] = plan.sceneResetWritten', 'attempted.complete = true')
        indices = [record.index(point) for point in points]
        self.assertEqual(indices, sorted(indices))
        self.assertIn('~RefuseIncomplete() { if (!complete) FailPacket(packet); }', record)
        self.assertNotIn('catch (', record)  # No partial-write fallback can complete the obligation.
        finish = section('void FinishCapture(', 'bool MatchesFinalLightingDraw(')
        disk = finish[finish.index('    CopyMain(list, *plan, plan->layers.after.resource.Get());'):]
        self.assertLess(disk.index('FSRDFogLayerCapture::Record('), disk.index('policy.SealConsumer('))
        self.assertIn('!recorded || ((packet->sceneResetOnce || packet->temporal) && !plan->sceneResetWritten) ||', disk)
        draw = section('void WINAPI HookDraw(', 'void WINAPI HookDrawIndexed(')
        self.assertEqual(draw.count('originalDraw(list, count, instances, start, firstInstance);'), 1)
        self.assertLess(draw.index('PrepareCapture('), draw.index('originalDraw('))
        self.assertLess(draw.index('originalDraw('), draw.index('FinishCapture('))

    def test_actual_scene_admission_recording_marker_and_seal_at_o0_o3(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX for the actual scene RESET adapter compilation')
        functions = section('struct FogRgbEngineHost', 'void RecordRgbIdentity(')
        functions += section('struct FogSceneResetEngineHost', 'void RecordPrivateReset(')
        marker = section('const bool sceneReset = controls.at("mode")', '            ComPtr<IUnknown> identity;')
        # Use the exact production rejection branch and append only its test return.
        functions += '\nbool Marker(const Json& controls) {\n' + marker + '\nreturn sceneReset;\n}\n'
        finish = section('void FinishCapture(', 'bool MatchesFinalLightingDraw(')
        seal_start = finish.index('    if (plan->layers.privateReset[0].resource)')
        seal_end = finish.index('    {\n        const auto status', seal_start)
        functions += '\nvoid Seal(const std::shared_ptr<CapturePlan>& plan, bool recorded) {\n'
        functions += 'auto* selected = plan->packet ? plan->packet : privateResetPacket.load();\n'
        functions += finish[seal_start:seal_end] + '\n}\n'
        guard = section('    struct RefuseIncomplete\n', '    if (!plan.depthPrepared')
        functions += '\nvoid ControlledScene(ID3D12GraphicsCommandList* list, CapturePlan& plan, PrivateResetPacket& value) {\nauto* packet = &value;\n'
        functions += guard + '\nRecordSceneReset(list, plan, *packet);\nattempted.complete = true;\n}\n'
        mocks = r'''
#include <json.hpp>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef __fastcall
#define __fastcall
#endif
using UINT=unsigned;using BOOL=int;using D3D12_RESOURCE_STATES=uint32_t;using Json=nlohmann::json;
constexpr BOOL FALSE=0;
struct IUnknown{};struct Resource{};struct Device{};struct Pso{};struct ID3D12GraphicsCommandList{};
template<class T>struct ComPtr{T* p=nullptr;T* Get()const{return p;}T* operator->()const{return p;}
 explicit operator bool()const{return p!=nullptr;}};
struct Handle{uintptr_t ptr=100;};struct View{};struct Viewport{};struct Rect{};
struct Texture{ComPtr<Resource> resource;D3D12_RESOURCE_STATES state=0xc0;};
struct Endpoint{ComPtr<IUnknown> list;};
struct PrivateResetPacket;
namespace FSRDFogLayerCapture{struct Layers{enum class PrivateOutputMode{IndependentReset,TemporalWindowFinalSceneControl32};};}
struct CapturePlan{
 bool rgbIdentity=false,sceneResetOnce=true,sceneResetWritten=false;
 bool temporal=false,captureFinal=false;PrivateResetPacket* packet=nullptr; // Legacy fixture keeps temporal disabled.
 ComPtr<Device> device;ComPtr<Resource> main;ComPtr<Pso> originalPso;Handle frozenOriginalRtv;View originalView;
 struct{std::array<Viewport,1> viewports;std::array<Rect,1> scissors;uint64_t generation=12;}drawState;
 struct{uint32_t handle=7;uintptr_t native=0x1000;
  struct{uint64_t serial=2,recordingGeneration=12;uintptr_t list=0,view=0x3000;}scope;}depthSnapshot;
 uintptr_t depthFrameObject=0x4000;uint32_t depthFrame=99;
 struct{std::array<Texture,3> privateReset;bool privateResetSceneWrite=false;
  FSRDFogLayerCapture::Layers::PrivateOutputMode privateOutputMode=FSRDFogLayerCapture::Layers::PrivateOutputMode::IndependentReset;}layers;
 std::shared_ptr<Endpoint> endpoint=std::make_shared<Endpoint>();Json provenance;
};
struct Recording{uintptr_t list;uint64_t generation;};
struct Policy{bool failed=false,sealAccepted=true;unsigned seals=0;
 bool Failed()const{return failed;}
 bool SealConsumer(Recording value,uint64_t point,bool restored,bool complete){
  assert(value.list&&value.generation==12&&point==18&&restored&&complete);++seals;return !failed&&sealAccepted;}};
struct DenoiseWork{bool recorded=true;struct Data{ComPtr<Resource> composed;}data;
 bool Recorded()const{return recorded;}const Data& Outputs()const{return data;}};
struct PrivateResetPacket{
 std::mutex mutex;bool sceneResetOnce=true,fogClaimed=true;UINT width=4,height=4;
 void* temporal=nullptr;bool sceneRecorded=false,consumerSealed=false;
 std::shared_ptr<DenoiseWork> denoise=std::make_shared<DenoiseWork>();ComPtr<IUnknown> consumerIdentity,deviceIdentity;Policy policy;
};
std::atomic<PrivateResetPacket*> privateResetPacket{nullptr};std::atomic<void*> rgbIdentityPacket{nullptr};
std::atomic<uintptr_t> authenticatedImage{0x140000000};
bool route=true,targetValid=true,codeValid=true;constexpr int FogTopologyCode=5;
namespace FSRD::PreFogSession{bool LateSrOnly()noexcept{return route;}}
bool MatchLiveCode(uintptr_t image,int code){assert(image==0x140000000&&code==5);return codeValid;}
bool SameFogRgbSource(const CapturePlan&)noexcept{return false;} // Identity-only gate must not authorize RESET.
bool SameFogRgbTarget(const CapturePlan&,UINT width,UINT height,IUnknown* identity)noexcept{
 assert(width==4&&height==4&&identity);return targetValid;}
std::vector<std::string> events;bool prepareFailure=false,recordFailure=false,recordThrow=false;
unsigned sceneMode=0,failedPackets=0; // 1 refusal; 2 fatal; 3 missing OM; 4 false-restored; 5 false-positive Recorded; 6 callback not entered.
void NativeRtv(ID3D12GraphicsCommandList* list,UINT count,const Handle* handle,BOOL contiguous,const void* depth){
 assert(list&&count==1&&handle&&handle->ptr==100&&!contiguous&&!depth);events.push_back("restoreOM");}
auto originalSetRtv=&NativeRtv;
struct FogDepthEngineHost{const CapturePlan& plan;};
namespace FSRD::CyberpunkFogDenoiseAccess{
struct Input{uintptr_t image=0,list=0,originalPso=0;uint64_t originalFogScope=0;
 struct{uint32_t handle=0;uintptr_t native=0;}depth;bool copySource=false;};
enum class SceneOutcome:uint8_t{Refused,SceneFailedRestored,SceneRecordedRestored,ScopeLostAfterMutation};
struct Result{SceneOutcome outcome=SceneOutcome::Refused;bool callbackEntered=false,bindingsRestored=false;};
template<class H,class F>Result RecordSceneRgb(H& host,const Input& input,F callback){
 events.push_back("admit");assert(input.image==0x140000000&&input.originalFogScope==2&&input.depth.handle==7&&input.depth.native==0x1000&&!input.copySource);
 if(sceneMode==1||!host.IsAdmittedFogRgbScope(input))return{};
 bool success=false;try{if(sceneMode!=6)success=callback();}catch(...){}
 if(sceneMode==2)return{SceneOutcome::ScopeLostAfterMutation,true,false};
 if(sceneMode!=3)host.RestoreOriginalFogTarget(input.list);
 if(sceneMode==5)return{SceneOutcome::SceneRecordedRestored,true,true};
 return{success?SceneOutcome::SceneRecordedRestored:SceneOutcome::SceneFailedRestored,sceneMode!=6,sceneMode!=4};
}}
namespace FSRD::CyberpunkFogRgbWrite{
struct Work{bool recorded=false;bool Record(ID3D12GraphicsCommandList*){
 events.push_back("rgb");if(recordThrow)throw std::runtime_error("record");recorded=!recordFailure;return recorded;}
 bool Recorded()const{return recorded;}};
std::shared_ptr<Work> Prepare(Device* device,UINT width,UINT height,const ComPtr<Resource>& source,const ComPtr<Resource>& target,
 const View&,const Viewport&,const Rect&,const char** error){
 assert(device&&width==4&&height==4&&source&&target&&source.Get()!=target.Get());events.push_back("prepare");
 if(prepareFailure){*error="prepare";return{};}return std::make_shared<Work>();}
}
uint64_t PrivateResetPoint(IUnknown* list,uint64_t generation){assert(list&&generation==12);return 18;}
void FailPrivateReset(){++failedPackets;if(auto* packet=privateResetPacket.load())packet->policy.failed=true;}
void FailPacket(PrivateResetPacket* packet){++failedPackets;assert(packet);packet->policy.failed=true;}
[[noreturn]]void PrivateResetFatal()noexcept{std::_Exit(79);}
'''
        harness = r'''
IUnknown deviceIdentity,listIdentity,wrongIdentity;Device device;Resource mainResource,composedResource,wrongResource;
Pso pso;ID3D12GraphicsCommandList list;PrivateResetPacket packet;
void Setup(CapturePlan& plan){
 plan=CapturePlan{};events.clear();route=targetValid=codeValid=true;prepareFailure=recordFailure=recordThrow=false;sceneMode=failedPackets=0;
 privateResetPacket=&packet;rgbIdentityPacket=nullptr;packet.sceneResetOnce=packet.fogClaimed=true;packet.policy=Policy{};
 packet.temporal=nullptr;packet.sceneRecorded=packet.consumerSealed=false;
 packet.denoise=std::make_shared<DenoiseWork>();packet.denoise->data.composed={&composedResource};
 packet.consumerIdentity={&listIdentity};packet.deviceIdentity={&deviceIdentity};
 plan.device={&device};plan.main={&mainResource};plan.originalPso={&pso};plan.endpoint->list={&listIdentity};
 plan.depthSnapshot.scope.list=uintptr_t(&list);for(auto& texture:plan.layers.privateReset)texture.resource={&composedResource};
}
int main(int argc,char** argv){
 CapturePlan plan;Setup(plan);const std::string mode=argc>1?argv[1]:"normal";
 if(mode=="fatal"||mode=="missing-OM"){sceneMode=mode=="fatal"?2:3;ControlledScene(&list,plan,packet);assert(false);}
 Json controls={{"mode","private_reset_only"},{"delta_source","explicit_reset_control_not_captured_duration"}};
 assert(!Marker(controls));controls["mode"]="scene_reset_once";assert(Marker(controls));
 for(unsigned bad=0;bad<6;++bad){
  Setup(plan);auto altered=controls;
  switch(bad){case 0:route=false;break;case 1:codeValid=false;break;case 2:altered["mode"]="unknown";break;
   case 3:altered["delta_source"]="previous_frame";break;case 4:altered.erase("mode");break;case 5:altered.erase("delta_source");break;}
  bool refused=false;try{(void)Marker(altered);}catch(const std::exception&){refused=true;}assert(refused);
 }
 Setup(plan);route=codeValid=false;controls["mode"]="private_reset_only";assert(!Marker(controls));
 for(unsigned bad=0;bad<16;++bad){
  Setup(plan);FSRD::CyberpunkFogDenoiseAccess::Input input;FogSceneResetEngineHost host{plan};assert(host.IsAdmittedFogRgbScope(input));
  switch(bad){
   case 0:input.copySource=true;break;case 1:route=false;break;case 2:privateResetPacket=nullptr;break;
   case 3:packet.sceneResetOnce=false;break;case 4:rgbIdentityPacket=&packet;break;case 5:plan.rgbIdentity=true;break;
   case 6:plan.sceneResetOnce=false;break;case 7:plan.sceneResetWritten=true;break;case 8:packet.denoise.reset();break;
   case 9:packet.denoise->recorded=false;break;case 10:plan.layers.privateReset[2].resource={};break;
   case 11:plan.layers.privateReset[2].state=0x8;break;case 12:plan.layers.privateReset[2].resource={&wrongResource};break;
   case 13:packet.consumerIdentity={&wrongIdentity};break;case 14:packet.fogClaimed=false;break;case 15:packet.policy.failed=true;break;
  }
  assert(!host.IsAdmittedFogRgbScope(input));
 }
 Setup(plan);targetValid=false;FSRD::CyberpunkFogDenoiseAccess::Input input;FogSceneResetEngineHost host{plan};assert(!host.IsAdmittedFogRgbScope(input));
 Setup(plan);ControlledScene(&list,plan,packet);
 assert((events==std::vector<std::string>{"prepare","admit","rgb","restoreOM"}));
 assert(plan.sceneResetWritten&&plan.layers.privateResetSceneWrite&&!failedPackets);
 const auto& receipt=plan.provenance["scene_reset_control"];
 assert(receipt["mode"]=="scene_reset_once"&&receipt["source"]=="same_consumer_private_RESET_composed");
 assert(receipt["source_address"]==uintptr_t(&composedResource)&&receipt["target_address"]==uintptr_t(&mainResource));
 assert(receipt["frame_source_value"]==99&&receipt["list"]==uintptr_t(&list)&&receipt["original_target_restored"]==true);
 assert(!plan.provenance.contains("rgb_identity_control"));
 auto shared=std::make_shared<CapturePlan>(plan);Seal(shared,true);assert(packet.policy.seals==1);
 // Duplicate attempts are rejected rather than silently writing a second frame.
 bool duplicate=false;try{ControlledScene(&list,plan,packet);}catch(const std::runtime_error&){duplicate=true;}
 assert(duplicate&&failedPackets==1&&packet.policy.failed);
 for(unsigned bad=0;bad<10;++bad){
  Setup(plan);
  switch(bad){case 0:prepareFailure=true;break;case 1:sceneMode=1;break;case 2:recordFailure=true;break;
   case 3:recordThrow=true;break;case 4:sceneMode=4;break;case 5:sceneMode=5;recordFailure=true;break;
   case 6:sceneMode=6;break;case 7:route=false;break;case 8:plan.rgbIdentity=true;break;case 9:plan.sceneResetOnce=false;break;}
  bool refused=false;try{ControlledScene(&list,plan,packet);}catch(const std::runtime_error&){refused=true;}
  assert(refused&&!plan.sceneResetWritten&&!plan.layers.privateResetSceneWrite&&failedPackets==1&&packet.policy.failed);
  assert(!plan.provenance.contains("scene_reset_control"));
 }
 Setup(plan);packet.sceneResetOnce=false;ControlledScene(&list,plan,packet);assert(events.empty()&&!failedPackets&&!plan.sceneResetWritten);
 for(unsigned bad=0;bad<3;++bad){
  Setup(plan);shared=std::make_shared<CapturePlan>(plan);shared->sceneResetWritten=bad!=0;packet.policy.sealAccepted=bad!=2;
  bool refused=false;try{Seal(shared,bad!=1);}catch(const std::runtime_error&){refused=true;}
  assert(refused&&packet.policy.seals==(bad==2?1u:0u));
 }
 Setup(plan);packet.sceneResetOnce=false;shared=std::make_shared<CapturePlan>(plan);Seal(shared,true);assert(packet.policy.seals==1);
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-scene-reset-host-') as temporary:
            source = Path(temporary) / 'test.cpp'
            source.write_text(mocks + functions + harness)
            for flags in (['-O0'], ['-O3', '-ffast-math', '-ffp-contract=fast']):
                binary = Path(temporary) / 'test'
                result = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags,
                                         '-I', str(ROOT / 'external/nlohmann'), str(source), '-o', str(binary)],
                                        text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for mode in ('normal', 'fatal', 'missing-OM'):
                    result = subprocess.run([str(binary), mode], text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0 if mode == 'normal' else 79,
                                     mode + result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()

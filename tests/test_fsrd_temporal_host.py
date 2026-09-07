"""Bounded opt-in temporal host wiring; real submission adapters with CPU mocks.

These tests establish control flow and durable receipts, not native camera
provenance, GPU execution, shader identity, or visual quality.
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


def section(first, following):
    return HOST[HOST.index(first):HOST.index(following, HOST.index(first))]


class TemporalHost(unittest.TestCase):
    def test_explicit_transaction_and_no_default_activation(self):
        poll = section('void PollTemporalWindow(', 'uint64_t AdmitPrivateResetSubmission(')
        self.assertIn('FSRD::PreFogSession::LateSrOnly()', poll)
        self.assertIn('L"FSRRR-prefog-temporal.request"', poll)
        self.assertIn('"temporal_window_32"', poll)
        self.assertIn('"selected_Fog_draw_CPU_interval_not_native_delta"', poll)
        self.assertIn('"experimental_software_epoch_stable_view_no_native_origin_proof"', poll)
        self.assertLess(poll.index('privateResetArming.exchange('), poll.index('CreateSession('))
        self.assertLess(poll.index('CreateSession('), poll.index('AllocateTemporalTargets('))
        self.assertLess(poll.index('AllocateTemporalTargets('), poll.index('DeleteFileW('))
        self.assertLess(poll.index('DeleteFileW('), poll.index('temporalWindow.store('))
        self.assertLess(poll.index('temporalWindow.store('), poll.index('lightingRequested.store(true)'))
        self.assertIn('~Finish() { privateResetArming.store(false, std::memory_order_release); }', poll)
        self.assertNotIn('FSRDFogLayerCapture::Request(', poll)
        self.assertNotIn('FSRDFogLayerCapture::RequestEarlyGuides(', poll)
        for signature, following in [('void ArmPrivateReset(', 'namespace\n{\nconstexpr uint64_t TemporalToken'),
                                     ('void ArmRgbIdentity(', 'void ArmPrivateReset(')]:
            self.assertGreaterEqual(section(signature, following).count('TemporalRecordingRequested()'), 2)
        feature = (BASE / 'FSRDFeature_Dx12.cpp').read_text()
        self.assertIn('FSRDCyberpunkFogProbe::PollTemporalWindow(Device, RenderWidth(), RenderHeight(), _denoiserProviderId, &settings)', feature)

    def test_raw_producers_real_fog_clock_and_owned_history(self):
        bind = section('void WINAPI HookSetRtv(', 'void LogDraw(')
        self.assertIn('TemporalRecordingRequested() && !lightingAttempted.load()', bind)
        self.assertLess(bind.index('bound.resource = slot->resource'), bind.index('originalSetRtv(list,'))
        select = section('PrivateResetPacket* SelectTemporalFrame(', 'void FailPacket(')
        self.assertIn('ParseRawTemporal(metadata)', select)
        self.assertNotIn('ParseTemporal(', select)
        self.assertIn('window.policy->ClaimRole(source.current.frame, role)', select)
        self.assertIn('created->policy.key = key', select)
        observe = section('PrivateResetPacket* ObserveTemporalFog(', 'void WINAPI HookDraw(')
        self.assertLess(observe.index('window->policy->Complete()'), observe.index('s.boundRtv'))
        self.assertIn('ResTrack_Dx12::PrepareSubmission(', observe)
        self.assertIn('observed.Get() != identity.Get()', observe)
        self.assertIn('state->second.generation', observe)
        self.assertIn('nativeList != uintptr_t(list)', observe)
        self.assertIn('topology != 4', observe)
        self.assertIn('raw.current.frame != window->lastFog->frame + 1', observe)
        self.assertIn('frame {}->{} view {:#x}->{:#x}', observe)
        self.assertIn('object {:#x}->{:#x} list {:#x} generation {} committed {}', observe)
        self.assertIn('const double delta = timestamp - previousTime', observe)
        self.assertIn('ParseTemporal(metadata, measured)', observe)
        self.assertLess(observe.index('SelectTemporalFrame('), observe.index('window->lastFog = raw.current'))
        record = section('void RecordPrivateReset(', 'std::shared_ptr<CapturePlan> PrepareCapture(')
        self.assertIn('window.policy->CommittedFrames() != packet->temporalKey.index', record)
        self.assertIn('previous = window.previous', record)
        self.assertIn('TemporalCamera::Build(', record)
        self.assertIn('PrivateDenoise::PrepareFrame(packet->temporal->session', record)
        self.assertIn('d.cameraPositionDelta = { c.cameraPositionDelta[0]', record)
        self.assertLess(record.index('packet->denoise = work'), record.index('policy.EmbedConsumer('))
        self.assertLess(record.index('policy.EmbedConsumer('), record.index('work->Record(list)'))
        self.assertLess(record.index('work->Record(list)'), record.index('RecordSceneReset('))
        self.assertLess(record.index('RecordSceneReset('), record.index('attempted.complete = true'))

    def test_intermediate_seals_and_final_only_disk(self):
        finish = section('void FinishCapture(', 'bool MatchesFinalLightingDraw(')
        intermediate = finish.split('CopyMain(list,', 1)[0]
        self.assertIn('if (plan->temporal && !plan->captureFinal)', intermediate)
        self.assertIn('selected->policy.SealConsumer(', intermediate)
        self.assertIn('selected->consumerSealed = true', intermediate)
        self.assertNotIn('FSRDFogLayerCapture::Record(', intermediate)
        self.assertIn('return;', intermediate)
        final = finish[finish.index('CopyMain(list,'):]
        self.assertLess(final.index('FSRDFogLayerCapture::Record('), final.index('policy.SealConsumer('))
        prepare = section('std::shared_ptr<CapturePlan> PrepareCapture(', 'void PublishFogEndpoint(')
        self.assertIn('if (TemporalRecordingRequested() && !temporal) return {}', prepare)
        self.assertIn('const bool diskCapture = !temporal || plan->captureFinal', prepare)
        self.assertIn('if (diskCapture) PrepareBoundCb12Target(', prepare)
        self.assertIn('selectedTemporal->finalTicket = std::move(finalTicket)', prepare)
        self.assertLess(prepare.index('FSRDSubmission::Retain('), prepare.index('CopyMain(list,'))
        lighting = section('void FinishLightingCapture(', 'bool HasBoundCb12Psos(')
        self.assertIn('const bool recorded = intermediate || FSRDFogLayerCapture::RecordEarlyGuides(', lighting)
        self.assertLess(lighting.index('RecordPrivateCompute('), lighting.index('policy.SealProducer('))
        rgb = section('void RecordSceneReset(', 'void RecordPrivateReset(')
        self.assertIn('if (plan.temporal && plan.captureFinal)', rgb)
        self.assertIn('TemporalWindowFinalSceneControl32', rgb)
        self.assertNotIn('layers.rgbIdentity', rgb)
        draw = section('void WINAPI HookDraw(', 'void WINAPI HookDrawIndexed(')
        self.assertEqual(draw.count('originalDraw(list, count, instances, start, firstInstance);'), 1)
        self.assertLess(draw.index('originalDraw(list,'), draw.index('FinishCapture('))
        self.assertIn('if (!plan->temporal || plan->captureFinal)', draw)

    def test_fenced_retirement_and_acyclic_plans(self):
        maintain = section('void MaintainTemporalWindow(', 'uint64_t AdmitTemporalSubmission(')
        self.assertIn('FSRDSubmission::Complete(ticket)', maintain)
        self.assertLess(maintain.index('FSRDSubmission::Complete(ticket)'), maintain.index('std::move(frame->denoise)'))
        self.assertIn('frame->retired = true', maintain)
        for forbidden in ('WaitForSingleObject', 'Sleep(', 'SetEventOnCompletion', 'WaitForGpu'):
            self.assertNotIn(forbidden, maintain)
        for first, following in [('struct CapturePlan\n', 'bool SameEarlyReservation('),
                                 ('struct LightingCapturePlan\n', 'bool ReadEarly(')]:
            if following not in HOST[HOST.index(first):]:
                continue
            plan = section(first, following)
            self.assertNotIn('shared_ptr<PrivateResetPacket>', plan)
            self.assertNotIn('shared_ptr<TemporalWindow>', plan)
        packet = section('struct PrivateResetPacket\n', 'struct TemporalTargets\n')
        self.assertIn('std::shared_ptr<FSRDSubmission::Ticket> finalTicket', packet)
        charge = section('struct TemporalCharge\n', 'struct PrivateResetPacket\n')
        self.assertNotIn('shared_ptr<PrivateResetPacket>', charge)
        self.assertNotIn('shared_ptr<FSRDSubmission::Ticket>', charge)

    def test_actual_warmup_submission_return_and_camera_commit_o0_o3(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX for compiled temporal host checks')
        functions = section('uint64_t AdmitTemporalSubmission(', '} // namespace\n\nTemporalTestStatus GetTemporalTestStatus(')
        mocks = r'''
#include "FSRDCyberpunkTemporalWindowPolicy.h"
#include "FSRDCyberpunkTemporalCamera.h"
#include "FSRDCyberpunkPrivateResetSource.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
using UINT=unsigned;using HRESULT=int;using Json=nlohmann::json;
std::atomic<uint64_t> temporalLastReturn{0};
uint64_t GetTickCount64(){return 1000;}
#define FAILED(x) ((x)<0)
#define IID_PPV_ARGS(x) (x)
#define LOG_INFO(...) ((void)0)
constexpr int D3D12_COMMAND_LIST_TYPE_DIRECT=0;
constexpr uint64_t TemporalToken=1ull<<63,WarmupToken=1ull<<62;
struct IUnknown{unsigned refs=0;bool fail=false;virtual ~IUnknown()=default;
 void AddRef(){++refs;}void Release(){assert(refs);--refs;}
 HRESULT QueryInterface(IUnknown** out){if(fail)return -1;*out=this;AddRef();return 0;}};
template<class T>struct ComPtr{T* value=nullptr;ComPtr()=default;ComPtr(T* p):value(p){if(value)value->AddRef();}
 ComPtr(const ComPtr& p):value(p.value){if(value)value->AddRef();}~ComPtr(){if(value)value->Release();}
 T* Get()const{return value;}explicit operator bool()const{return value!=nullptr;}
 T** operator&(){assert(!value);return &value;}
 ComPtr& operator=(const ComPtr& p){if(p.value)p.value->AddRef();if(value)value->Release();value=p.value;return *this;}};
struct ID3D12CommandList:IUnknown{};
struct ID3D12CommandQueue:IUnknown{IUnknown* device=nullptr;bool deviceFail=false;int type=0;
 HRESULT GetDevice(IUnknown** out){if(deviceFail)return -1;*out=device;device->AddRef();return 0;}
 struct Desc{int Type;};Desc GetDesc(){return{type};}};
namespace ResetPolicy=FSRD::CyberpunkPrivateResetPolicy;
namespace WindowPolicy=FSRD::CyberpunkTemporalWindowPolicy;
namespace TemporalCamera=FSRD::CyberpunkTemporalCamera;
namespace ResetSource=FSRD::CyberpunkPrivateResetSource;
namespace FSRD::PrivateDenoise{
 struct Work{uint32_t frame=0;bool recorded=true;bool Recorded()const{return recorded;}
 struct Parameters{struct{std::array<uint32_t,16> PrevViewMatrix{};std::array<uint32_t,4> PreviousDepthProjection{};
 uint32_t Flags=37;}conversion;struct{uint32_t flags=2;struct{float x=0,y=0,z=0;}cameraPositionDelta;}dispatch;}parameters;
 const Parameters& EffectiveParameters()const{return parameters;}};
 struct Session{unsigned acknowledgements=0;uintptr_t expectedQueue=0;const Work* last=nullptr;bool refuse=false;
 bool AcknowledgeExecuted(const Work& work,uintptr_t queue){
  if(refuse||!work.recorded||queue!=expectedQueue)return false;
  assert(work.frame==101+acknowledgements);last=&work;++acknowledgements;return true;}};
}
struct PrivateResetPacket{
 std::mutex mutex;bool returned=false,consumerSealed=true,sceneRecorded=true;
 ResetPolicy::Recording consumer{};
 std::shared_ptr<FSRD::PrivateDenoise::Work> denoise;
 std::optional<ResetSource::TemporalSource> timedSource;
 float delta=16;double fogTimestamp=100,previousFogTimestamp=84;};
struct TemporalWindow{
 bool visual=false;
 std::mutex mutex;uint64_t epoch=7,warmupSerial=0;bool warmupReturned=false;std::atomic<bool> stopped{false};
 ComPtr<IUnknown> deviceIdentity,queueIdentity;ResetPolicy::Recording warmupRecording{};
 WindowPolicy::Receipt pendingWarmup{};
 std::array<WindowPolicy::Receipt,WindowPolicy::Window::MaxPendingCalls> pendingCalls{};
 std::unique_ptr<WindowPolicy::Window> policy;
 std::array<std::unique_ptr<PrivateResetPacket>,32> frames;
 std::shared_ptr<FSRD::PrivateDenoise::Session> session=std::make_shared<FSRD::PrivateDenoise::Session>();
 std::optional<TemporalCamera::PreviousFrame> previous;std::array<Json,32> ledger;
 uint32_t returnEvidencePending=0;bool ledgerEvidenceLost=false;
};
std::atomic<bool> captureTrackingValid{true};
struct ListState{bool known=true;uint64_t generation=0;};
struct Registry{std::mutex mutex;std::unordered_map<IUnknown*,ListState> lists;};
Registry& Data(){static Registry value;return value;}
[[noreturn]]void PrivateResetFatal()noexcept{std::_Exit(79);}
'''
        harness = r'''
int main(int argc,char**argv){
 const std::string mode=argc>1?argv[1]:"separate";
 IUnknown device;ID3D12CommandQueue queue,otherQueue;queue.device=otherQueue.device=&device;
 ID3D12CommandList warmup,producer,consumer,other;TemporalWindow window;window.deviceIdentity=&device;
 window.warmupRecording={uintptr_t(&warmup),9};Data().lists[&warmup]={true,9};
 ID3D12CommandList* unrelated[]{&other};assert(AdmitTemporalSubmission(window,&queue,1,unrelated)==0);
 ID3D12CommandList* warmupLists[]{&warmup};
 const auto bootstrap=AdmitTemporalSubmission(window,&queue,1,warmupLists);
 assert((bootstrap&TemporalToken)&&(bootstrap&WarmupToken)&&!window.warmupReturned);
 assert(window.queueIdentity.Get()==&queue&&!window.previous);
 ReturnedTemporalSubmission(window,bootstrap);assert(window.warmupReturned&&!window.pendingWarmup.Valid());
 if(mode=="bad-warmup-return"){ReturnedTemporalSubmission(window,bootstrap);assert(false);}
 window.session->expectedQueue=uintptr_t(&queue);
 window.policy=std::make_unique<WindowPolicy::Window>(7,ResetPolicy::Queue{uintptr_t(&queue),uintptr_t(&device),true},101);
 for(unsigned index=0;index<32;++index){
  const auto frame=101+index;
  const auto key=window.policy->ClaimRole(frame,WindowPolicy::Role::Ray);assert(key.Valid());
  assert(window.policy->ClaimRole(frame,WindowPolicy::Role::Guides)==key);
  assert(window.policy->ClaimRole(frame,WindowPolicy::Role::Fog)==key);
  ResetPolicy::Recording p{uintptr_t(&producer),10+index*2},c{uintptr_t(&consumer),11+index*2};
  Data().lists[&producer]={true,p.generation};Data().lists[&consumer]={true,c.generation};
  assert(window.policy->DeclareProducer(key,p));
  assert(window.policy->SealProducer(key,{p,p,1,2,3,4,true,true,true,true,true,true,true,true}));
  assert(window.policy->EmbedConsumer(key,c,1,true));assert(window.policy->SealConsumer(key,c,2,true,true));
  auto packet=std::make_unique<PrivateResetPacket>();packet->denoise=std::make_shared<FSRD::PrivateDenoise::Work>();
  packet->denoise->frame=frame;packet->timedSource.emplace();packet->timedSource->current.frame=frame;
  packet->timedSource->current.rawSnapshot.width=1280;packet->timedSource->current.motionScale={1,1};
  window.frames[index]=std::move(packet);ID3D12CommandList* both[]{&producer,&consumer};
  if(mode=="missing-producer"){AdmitTemporalSubmission(window,&queue,1,both+1);assert(false);}
  if(mode=="unknown-reset"){captureTrackingValid=false;AdmitTemporalSubmission(window,&queue,2,both);assert(false);}
  if(mode=="wrong-queue"){AdmitTemporalSubmission(window,&otherQueue,2,both);assert(false);}
  if(mode=="bad-order"){ID3D12CommandList* reverse[]{&consumer,&producer};AdmitTemporalSubmission(window,&queue,2,reverse);assert(false);}
  if(mode=="bad-return"){ReturnedTemporalSubmission(window,TemporalToken|999);assert(false);}
  uint64_t token=0;
  if(mode=="batch")token=AdmitTemporalSubmission(window,&queue,2,both);
  else{
   token=AdmitTemporalSubmission(window,&queue,1,both);assert(token&TemporalToken);
   ReturnedTemporalSubmission(window,token);assert(window.session->acknowledgements==index);
   assert(!window.frames[index]->returned&&window.policy->CommittedFrames()==index);
   token=AdmitTemporalSubmission(window,&queue,1,both+1);
  }
  if(mode=="missing-scene")window.frames[index]->sceneRecorded=false;
  if(mode=="missing-seal")window.frames[index]->consumerSealed=false;
  if(mode=="session-refused")window.session->refuse=true;
  if(mode=="stopped-drain"){window.stopped=true;window.policy->Stop();}
  ReturnedTemporalSubmission(window,token);
  assert(window.frames[index]->returned&&window.session->last==window.frames[index]->denoise.get());
  assert(window.previous&&window.previous->frameIndex==frame&&window.previous->sessionEpoch==7);
  assert(window.session->acknowledgements==index+1);
  if(mode=="stopped-drain"){assert(window.policy->CommittedFrames()==0);return 0;}
  assert(window.policy->CommittedFrames()==index+1);
 }
 assert(window.policy->Complete()&&window.session->acknowledgements==32);
 if(mode=="duplicate-generation"){
  Data().lists[&consumer].generation=11;ID3D12CommandList* old[]{&consumer};
  AdmitTemporalSubmission(window,&queue,1,old);assert(false);
 }
 assert(window.ledger[31]["ordinal"]==32);
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-temporal-host-') as temporary:
            temp = Path(temporary)
            (temp / 'test.cpp').write_text(mocks + functions + harness)
            for flags in (['-O0'], ['-O3', '-ffast-math']):
                result = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', *flags,
                                         '-I', str(BASE), '-I', str(ROOT / 'external/nlohmann'),
                                         str(temp / 'test.cpp'), '-o', str(temp / 'test')], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for mode in ('separate', 'batch', 'stopped-drain', 'bad-warmup-return', 'missing-producer',
                             'unknown-reset', 'wrong-queue', 'bad-order', 'bad-return', 'missing-scene',
                             'missing-seal', 'session-refused', 'duplicate-generation'):
                    result = subprocess.run([str(temp / 'test'), mode], text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0 if mode in ('separate', 'batch', 'stopped-drain') else 79,
                                     mode + result.stdout + result.stderr)

    def test_actual_frame_selector_fixed_targets_and_final_slot_transaction(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX for compiled frame selector checks')
        implementation = section('PrivateResetPacket* SelectTemporalFrame(', 'void FailPacket(')
        mocks = r'''
#include "FSRDCyberpunkTemporalWindowPolicy.h"
#include <json.hpp>
#include <array>
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <stdexcept>
using UINT=unsigned;using Json=nlohmann::json;
#define FAILED(x) ((x)<0)
#define IID_PPV_ARGS(x) (x)
struct IUnknown{};struct ID3D12Device:IUnknown{bool fail=false;
 int QueryInterface(IUnknown** value){if(fail)return -1;*value=this;return 0;}};
template<class T>struct ComPtr{T* p=nullptr;ComPtr()=default;ComPtr(T* v):p(v){}T* Get()const{return p;}
 explicit operator bool()const{return p!=nullptr;}T** operator&(){return &p;}};
namespace WindowPolicy=FSRD::CyberpunkTemporalWindowPolicy;
namespace ResetPolicy=FSRD::CyberpunkPrivateResetPolicy;
namespace ResetSource{
 struct RawSource{uint32_t width=1280,height=720,frame=101;uintptr_t view=1000,object=2000;};
 struct RawTemporalSource{RawSource current;bool nativeResetRequested=false;};
 RawTemporalSource ParseRawTemporal(const Json& j){RawTemporalSource value;
 value.current.frame=j.at("frame").get<uint32_t>();value.current.view=j.value("view",uintptr_t(1000));
 value.current.object=j.value("object",uintptr_t(2000));value.current.width=j.value("width",1280u);
 value.nativeResetRequested=j.value("reset",false);return value;}
}
struct TemporalWindow;
struct Targets{std::shared_ptr<int> guides,rays,charge;};
struct PrivateResetPacket{explicit PrivateResetPacket(uintptr_t){}
 ComPtr<ID3D12Device> device;ComPtr<IUnknown> deviceIdentity;UINT width=0,height=0;
 uint64_t provider=0;int settings=0;TemporalWindow* temporal=nullptr;WindowPolicy::FrameKey temporalKey{};
 struct{TemporalWindow* window=nullptr;WindowPolicy::FrameKey key{};}policy;
 std::shared_ptr<int> guides,rays,charge;};
struct TemporalWindow{
 std::mutex mutex;ComPtr<ID3D12Device> device;ComPtr<IUnknown> deviceIdentity,queueIdentity;
 UINT width=1280,height=720;uint64_t epoch=8,provider=1;int settings=0;
 uint32_t frameCount=32;std::vector<PrivateResetPacket*> liveFrames;
 uint32_t warmupSkippedThrough=0;
 bool warmupReturned=true,stopped=false,finalCaptureQueued=false;
 std::optional<ResetSource::RawSource> lastFog=ResetSource::RawSource{};
 std::unique_ptr<WindowPolicy::Window> policy;std::array<std::optional<Targets>,2> freeTargets;
 std::array<std::unique_ptr<PrivateResetPacket>,32> frames;
};
void StopTemporalWindow(TemporalWindow& value,const char*){value.stopped=true;}
namespace FSRDFogLayerCapture{
 unsigned guideRequests=0,fogRequests=0,cancels=0;bool guides=true,fog=true;
 bool RequestEarlyGuides(){++guideRequests;return guides;}bool Request(){++fogRequests;return fog;}
 void CancelEarlyGuideRequest(){++cancels;}
}
'''
        harness = r'''
int main(){
 ID3D12Device device;IUnknown queue;
 auto setup=[&](TemporalWindow& window){window.device=&device;window.deviceIdentity=&device;window.queueIdentity=&queue;
 window.lastFog->frame=100;for(auto& free:window.freeTargets)free=Targets{std::make_shared<int>(1),std::make_shared<int>(2),std::make_shared<int>(3)};};
 for(unsigned bad=0;bad<6;++bad){TemporalWindow window;setup(window);Json j={{"frame",101}};
  switch(bad){case 0:window.warmupReturned=false;break;case 1:window.queueIdentity={};break;
   case 2:j["view"]=999;break;case 3:j["object"]=888;break;case 4:j["width"]=2560;break;case 5:j["frame"]=103;break;}
  assert(!SelectTemporalFrame(window,j,&device,WindowPolicy::Role::Ray));assert(!window.frames[0]);
 }
 for(unsigned failFinal=0;failFinal<3;++failFinal){
  FSRDFogLayerCapture::guideRequests=FSRDFogLayerCapture::fogRequests=FSRDFogLayerCapture::cancels=0;
  FSRDFogLayerCapture::guides=true;FSRDFogLayerCapture::fog=true;
  TemporalWindow window;setup(window);
  for(unsigned i=0;i<32;++i){
   Json j={{"frame",101+i}};if(i==31){FSRDFogLayerCapture::guides=failFinal!=1;FSRDFogLayerCapture::fog=failFinal!=2;}
   // Fog may be first on CPU. It takes fixed outputs, never current/last mutable resources.
   auto* fog=SelectTemporalFrame(window,j,&device,WindowPolicy::Role::Fog);
   if(i==31&&failFinal){assert(!fog&&window.stopped&&!window.frames[i]);break;}
   assert(fog&&fog==window.frames[i].get()&&fog->temporalKey.index==i&&fog->temporalKey.frame==101+i);
   assert(fog->guides&&fog->rays&&fog->charge&&fog->policy.window==&window);
   auto* ray=SelectTemporalFrame(window,j,&device,WindowPolicy::Role::Ray);
   auto* guides=SelectTemporalFrame(window,j,&device,WindowPolicy::Role::Guides);
   assert(fog==ray&&fog==guides);
   ResetPolicy::Recording p{0x1100+i*0x100,1},c{0x1200+i*0x100,2};
   auto key=fog->temporalKey;
   assert(window.policy->DeclareProducer(key,p));
   assert(window.policy->SealProducer(key,{p,p,1,2,3,4,true,true,true,true,true,true,true,true}));
   assert(window.policy->EmbedConsumer(key,c,1,true));assert(window.policy->SealConsumer(key,c,2,true,true));
   std::array<ResetPolicy::Recording,2> both{p,c};auto submit=window.policy->BeforeExecute({uintptr_t(&queue),uintptr_t(&device),true},both);
   assert(submit.allowed);assert(window.policy->AfterExecute(submit.receipt).consumer==key);assert(window.policy->CommitConsumer(key));
   window.lastFog->frame=101+i;
   if(i<31)assert(FSRDFogLayerCapture::guideRequests==0&&FSRDFogLayerCapture::fogRequests==0);
   // Emulate fresh allocation by maintenance, not reuse of claimed Targets.
   for(auto& free:window.freeTargets)if(!free)free=Targets{std::make_shared<int>(1),std::make_shared<int>(2),std::make_shared<int>(3)};
  }
  assert(FSRDFogLayerCapture::guideRequests==1);
  assert(FSRDFogLayerCapture::fogRequests==(failFinal==1?0u:1u));
  assert(FSRDFogLayerCapture::cancels==(failFinal==2?1u:0u));
  if(!failFinal)assert(window.policy->Complete()&&window.finalCaptureQueued);
 }
 TemporalWindow reset;setup(reset);auto* first=SelectTemporalFrame(reset,{{"frame",101}},&device,WindowPolicy::Role::Ray);assert(first);
 assert(!SelectTemporalFrame(reset,{{"frame",102},{"reset",true}},&device,WindowPolicy::Role::Ray));assert(reset.stopped);
 TemporalWindow skipped;setup(skipped);skipped.warmupReturned=false;
 assert(!SelectTemporalFrame(skipped,{{"frame",101}},&device,WindowPolicy::Role::Ray));
 skipped.warmupReturned=true;
 assert(!SelectTemporalFrame(skipped,{{"frame",101}},&device,WindowPolicy::Role::Fog));
 assert(!SelectTemporalFrame(skipped,{{"frame",101}},&device,WindowPolicy::Role::Guides));
 assert(!skipped.policy&&!skipped.stopped);skipped.lastFog->frame=101;
 assert(SelectTemporalFrame(skipped,{{"frame",102}},&device,WindowPolicy::Role::Fog));
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-temporal-selector-') as temporary:
            temp = Path(temporary)
            (temp / 'test.cpp').write_text(mocks + implementation + harness)
            for flags in (['-O0'], ['-O3', '-ffast-math']):
                result = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', *flags,
                                         '-I', str(BASE), '-I', str(ROOT / 'external/nlohmann'),
                                         str(temp / 'test.cpp'), '-o', str(temp / 'test')], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(temp / 'test')], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()

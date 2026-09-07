"""Compile actual host reclamation/restart code; GPU completion itself is mocked."""
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
    begin = HOST.index(start)
    return HOST[begin:HOST.index(end, begin)]


class ContinuousLifetime(unittest.TestCase):
    def test_cpu_pins_never_enter_gpu_owner_graph(self):
        for start, end in [('struct RayCopyBundle\n', 'struct Registry\n'),
                           ('struct CapturePlan\n', 'bool SameEarlyReservation('),
                           ('struct LightingCapturePlan\n', 'struct ExposureMemory\n')]:
            body = section(start, end)
            self.assertNotIn('shared_ptr<PrivateResetPacket>', body)
            self.assertNotIn('shared_ptr<TemporalWindow>', body)
        self.assertIn('shared_ptr<PrivateResetPacket> packetLease', section('struct RayScope\n', 'thread_local RayScope*'))
        draw = section('void WINAPI HookDraw(', 'void WINAPI HookDrawIndexed(')
        self.assertIn('shared_ptr<PrivateResetPacket> lightingLease, fogLease', draw)
        self.assertLess(draw.index('lightingLease, fogLease'), draw.index('originalDraw(list,'))
        self.assertIn('diagnostic, lightingLease)', draw)
        self.assertIn('temporalTimestamp, &fogLease)', draw)
        maintain = section('void MaintainTemporalWindow(', 'uint64_t AdmitTemporalSubmission(')
        self.assertIn('FSRDSubmission::Complete(producerTicket)', maintain)
        self.assertIn('!window.policy->ConsumerEmbedded(frame->temporalKey)', maintain)
        self.assertIn('slot.get() == frame.get() && slot.use_count() == 2', maintain)
        self.assertLess(maintain.index('slot.use_count() == 2'), maintain.index('frame->retired = true'))
        self.assertLess(maintain.index('FSRDSubmission::Complete(producerTicket)'), maintain.index('frame->retired = true'))
        self.assertNotIn('WaitForSingleObject', maintain)

    def test_selected_consumer_drains_but_new_fog_is_not_admitted(self):
        embed = section('bool PacketPolicy::EmbedConsumer(', 'bool PacketPolicy::SealConsumer(')
        self.assertIn('window->continuous && window->restartRequested', embed)
        self.assertIn('EmbedConsumer(key, recording, first, owners, drain)', embed)
        record = section('void RecordPrivateReset(', 'std::shared_ptr<CapturePlan> PrepareCapture(')
        self.assertIn('window.stopped && !(window.continuous && window.restartRequested)', record)
        observe = section('PrivateResetPacket* ObserveTemporalFog(', 'void WINAPI HookDraw(')
        self.assertIn('if (window->stopped) return nullptr;', observe)

    def test_restart_required_checkbox_keeps_latched_observers(self):
        for path in ('OptiScaler/hooks/D3D12_Hooks.cpp', 'OptiScaler/hooks/Streamline_Hooks.cpp'):
            source = (ROOT / path).read_text()
            self.assertIn('#include <upscalers/ffx/FSRDPreFogSession.h>', source)
            self.assertIn('FSRD::PreFogSession::Continuous()', source)

    def test_warmup_retry_and_feature_release_keep_submission_guards(self):
        stop = section('void StopTemporalWindow(', 'bool TemporalRecordingRequested(')
        self.assertIn('window.continuous && (restart || !window.policy)', stop)
        notify = section('void NotifyFeatureReleased(', 'void PollTemporalWindow(')
        self.assertIn('device == window->device.Get()', notify)
        self.assertIn('StopTemporalWindow(*window, "RR feature recreated or disabled", true)', notify)
        feature = (BASE / 'FSRDFeature_Dx12.cpp').read_text()
        destructor = feature.split('FSRDFeatureDx12::~FSRDFeatureDx12()', 1)[1].split('\n}', 1)[0]
        self.assertLess(destructor.index('NotifyFeatureReleased(Device)'), destructor.index('DestroyDenoiserContext()'))
        admit = section('uint64_t AdmitTemporalSubmission(', 'void ReturnedTemporalSubmission(')
        self.assertIn('if (window.stopped) return 0;', admit)
        self.assertLess(admit.index('if (window.stopped) return 0;'), admit.index('window.pendingWarmup ='))

    def test_actual_reclamation_and_restart_o0_o3(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX for compiled host reclamation checks')
        functions = section('void RetireVisualWatch(', 'void MaintainTemporalWindow(')
        functions += section('bool CanRestartTemporalWindow(', 'void NotifyFeatureReleased(')
        fixture = r'''
#include "FSRDCyberpunkTemporalWindowPolicy.h"
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <unordered_map>
namespace WindowPolicy=FSRD::CyberpunkTemporalWindowPolicy;
namespace ResetPolicy=FSRD::CyberpunkPrivateResetPolicy;
struct IUnknown{};
struct PrivateResetPacket{
 std::mutex mutex;bool retired=false,returned=true,unusedProducer=false;
 bool sceneRecorded=false,consumerSealed=false;
 uint64_t rayTerminal=0,guideBegin=0;
 std::shared_ptr<int> rayCopy,finalTicket,denoise;
 ResetPolicy::Recording producer{},consumer{};WindowPolicy::FrameKey temporalKey{};
};
struct TemporalWindow{
 std::mutex mutex;bool visual=true,continuous=true,stopped=false,restartRequested=false,allocating=false;
 unsigned returnEvidencePending=0,drainedFrames=0;
 WindowPolicy::Receipt pendingWarmup{};
 std::array<WindowPolicy::Receipt,4> pendingCalls{};
 std::unique_ptr<WindowPolicy::Window> policy;
 std::array<std::shared_ptr<PrivateResetPacket>,64> frames;
 std::vector<PrivateResetPacket*> liveFrames;
};
struct ListState{bool known=true;uint64_t generation=0;};
struct Registry{std::mutex mutex;std::unordered_map<IUnknown*,ListState> lists;};
Registry& Data(){static Registry data;return data;}
std::atomic<bool> captureTrackingValid{true};
constexpr ResetPolicy::Queue queue{1,2,true};
'''
        checks = r'''
int main(){
 TemporalWindow w;w.policy=std::make_unique<WindowPolicy::Window>(9,queue,500,0);
 std::weak_ptr<PrivateResetPacket> last;
 for(unsigned i=0;i<1024;++i){
  assert(last.expired());
  auto frame=std::make_shared<PrivateResetPacket>();last=frame;
  auto key=w.policy->ClaimRole(500+i,WindowPolicy::Role::Ray);assert(key.Valid());
  assert(w.policy->ClaimRole(500+i,WindowPolicy::Role::Guides)==key);
  assert(w.policy->ClaimRole(500+i,WindowPolicy::Role::Fog)==key);
  auto p=ResetPolicy::Recording{100,10+uint64_t(i)*2};
  auto c=ResetPolicy::Recording{101,11+uint64_t(i)*2};
  assert(w.policy->DeclareProducer(key,p));
  assert(w.policy->SealProducer(key,{p,p,1,2,3,4,true,true,true,true,true,true,true,true}));
  assert(w.policy->EmbedConsumer(key,c,5,true));assert(w.policy->SealConsumer(key,c,6,true,true));
  std::array lists{p,c};auto submitted=w.policy->BeforeExecute(queue,lists);assert(submitted.allowed);
  assert(w.policy->AfterExecute(submitted.receipt).consumer==key);assert(w.policy->CommitConsumer(key));
  frame->temporalKey=key;frame->producer=p;frame->consumer=c;
  w.frames[i%64]=frame;w.liveFrames.push_back(frame.get());
  Data().lists[reinterpret_cast<IUnknown*>(p.list)]={true,p.generation+2};
  Data().lists[reinterpret_cast<IUnknown*>(c.list)]={true,c.generation+2};
  RetireVisualWatch(w,*frame);assert(w.liveFrames.size()==1); // Fence not yet attested.
  frame->retired=true;
  {auto cpuReader=frame;RetireVisualWatch(w,*frame);assert(w.liveFrames.size()==1);}
  w.returnEvidencePending=1;RetireVisualWatch(w,*frame);assert(w.liveFrames.size()==1);w.returnEvidencePending=0;
  captureTrackingValid=false;RetireVisualWatch(w,*frame);assert(w.liveFrames.size()==1);captureTrackingValid=true;
  Data().lists[reinterpret_cast<IUnknown*>(c.list)].generation=c.generation;
  RetireVisualWatch(w,*frame);assert(w.liveFrames.size()==1);
  Data().lists[reinterpret_cast<IUnknown*>(c.list)].generation=c.generation+2;
  RetireVisualWatch(w,*frame);assert(w.liveFrames.empty()&&!w.frames[i%64]);
  assert(w.policy->SubmissionWatches()==0&&w.policy->StorageFrames()==64);
  assert(w.policy->FrameFailed(key)&&w.drainedFrames==i+1);
 }
 assert(last.expired());assert(!CanRestartTemporalWindow(w));
 w.stopped=true;w.restartRequested=true;w.policy->Stop();assert(CanRestartTemporalWindow(w));
 w.allocating=true;assert(!CanRestartTemporalWindow(w));w.allocating=false;
 w.returnEvidencePending=1;assert(!CanRestartTemporalWindow(w));w.returnEvidencePending=0;
 w.pendingWarmup={9,1,1};assert(!CanRestartTemporalWindow(w));w.pendingWarmup={};
 w.pendingCalls[0]={9,2,1};assert(!CanRestartTemporalWindow(w));w.pendingCalls[0]={};
 w.frames[0]=std::make_shared<PrivateResetPacket>();assert(!CanRestartTemporalWindow(w));w.frames[0].reset();
 w.continuous=false;assert(!CanRestartTemporalWindow(w));w.continuous=true;
 w.restartRequested=false;assert(!CanRestartTemporalWindow(w));w.restartRequested=true;
 assert(CanRestartTemporalWindow(w));
 // Producer-only drain on a scene transition does not manufacture a consumer.
 TemporalWindow unused;unused.policy=std::make_unique<WindowPolicy::Window>(10,queue,900,0);
 auto frame=std::make_shared<PrivateResetPacket>();auto key=unused.policy->ClaimRole(900,WindowPolicy::Role::Ray);
 assert(unused.policy->ClaimRole(900,WindowPolicy::Role::Guides)==key);
 ResetPolicy::Recording p{200,10};assert(unused.policy->DeclareProducer(key,p));
 assert(unused.policy->SealProducer(key,{p,p,1,2,3,4,true,true,true,true,true,true,true,true}));
 auto submitted=unused.policy->BeforeExecute(queue,std::span(&p,1));assert(submitted.allowed);
 assert(unused.policy->AfterExecute(submitted.receipt).allowed);
 unused.stopped=unused.restartRequested=true;unused.policy->Stop();
 frame->temporalKey=key;frame->producer=p;frame->returned=false;frame->unusedProducer=true;frame->retired=true;
 unused.frames[0]=frame;unused.liveFrames.push_back(frame.get());
 Data().lists[reinterpret_cast<IUnknown*>(p.list)]={true,11};
 RetireVisualWatch(unused,*frame);assert(unused.liveFrames.empty());
 assert(unused.policy->CommittedFrames()==0&&CanRestartTemporalWindow(unused));
 // A preparation failure with NO private recording cannot strand the session.
 // Exercise the real cleanup with every independent host proof withheld.
 TemporalWindow empty;empty.policy=std::make_unique<WindowPolicy::Window>(11,queue,1000,0);
 auto abandoned=std::make_shared<PrivateResetPacket>();abandoned->returned=false;
 auto emptyKey=empty.policy->ClaimRole(1000,WindowPolicy::Role::Ray);
 abandoned->temporalKey=emptyKey;empty.frames[0]=abandoned;empty.liveFrames.push_back(abandoned.get());
 empty.stopped=true;empty.policy->FailFrame(emptyKey);
 {auto reader=abandoned;assert(!RetireUnrecordedTemporalFrame(empty,abandoned));}
 for(auto* flag:{&abandoned->sceneRecorded,&abandoned->consumerSealed,&abandoned->returned,&abandoned->retired}){
  *flag=true;assert(!RetireUnrecordedTemporalFrame(empty,abandoned));*flag=false;}
 for(auto* value:{&abandoned->rayTerminal,&abandoned->guideBegin}){
  *value=1;assert(!RetireUnrecordedTemporalFrame(empty,abandoned));*value=0;}
 for(auto* recording:{&abandoned->producer,&abandoned->consumer}){
  *recording={100,1};assert(!RetireUnrecordedTemporalFrame(empty,abandoned));*recording={};}
 for(auto* owner:{&abandoned->rayCopy,&abandoned->finalTicket,&abandoned->denoise}){
  *owner=std::make_shared<int>(0);assert(!RetireUnrecordedTemporalFrame(empty,abandoned));owner->reset();}
 empty.returnEvidencePending=1;assert(!RetireUnrecordedTemporalFrame(empty,abandoned));empty.returnEvidencePending=0;
 empty.continuous=false;assert(!RetireUnrecordedTemporalFrame(empty,abandoned));empty.continuous=true;
 empty.stopped=false;assert(!RetireUnrecordedTemporalFrame(empty,abandoned));empty.stopped=true;
 assert(RetireUnrecordedTemporalFrame(empty,abandoned));
 assert(empty.liveFrames.empty()&&!empty.frames[0]&&empty.policy->CommittedFrames()==0);
 assert(CanRestartTemporalWindow(empty));
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-continuous-lifetime-') as folder:
            folder = Path(folder)
            source = folder / 'test.cpp'
            source.write_text(fixture + functions + checks)
            for flags in (['-O0'], ['-O3', '-ffast-math']):
                binary = folder / 'test'
                result = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags,
                                         '-I', str(BASE), str(source), '-o', str(binary)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()

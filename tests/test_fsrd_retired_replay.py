"""Compile production replay guards/host migration; COM and native tracking are mocked."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

BASE = Path(__file__).resolve().parents[1] / 'OptiScaler/upscalers/ffx'
HOST = (BASE / 'FSRDCyberpunkFogProbe.cpp').read_text()


class RetiredReplay(unittest.TestCase):
    def test_post_admission_guard_closes_watch_transfer_race(self):
        entry = HOST.split('uint64_t AdmitPrivateResetSubmission(', 1)[1].split('void ReturnedPrivateResetSubmission(', 1)[0]
        self.assertLess(entry.index('AdmitTemporalSubmission('), entry.index('AllowsRetiredSubmission('))
        self.assertLess(entry.index('AllowsRetiredSubmission('), entry.index('return token;'))
        self.assertLess(entry.index('if (!AllowsRetiredSubmission', entry.index('return token;')),
                        entry.index('if (!packet) return 0;'))

    def test_actual_guard_transfer_and_replay_veto(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX for compiled replay guards')
        begin = HOST.index('constexpr size_t MaxRetiredReplayGuards')
        functions = HOST[begin:HOST.index('void LogStoppedTemporalFrames(', begin)]
        fixture = r'''
#include "FSRDCyberpunkTemporalWindowPolicy.h"
#include "FSRDReplayGuardPolicy.h"
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <stdexcept>
namespace WindowPolicy=FSRD::CyberpunkTemporalWindowPolicy;
namespace ResetPolicy=FSRD::CyberpunkPrivateResetPolicy;
using UINT=unsigned;
#define FAILED(x) ((x)<0)
#define IID_PPV_ARGS(x) (x)
template<class T> struct ComPtr {
 std::shared_ptr<T> value;
 T* Get() const{return value.get();}
 explicit operator bool()const{return bool(value);}
};
struct IUnknown:std::enable_shared_from_this<IUnknown>{
 bool fail=false;
 int QueryInterface(ComPtr<IUnknown>* out){if(fail)return -1;out->value=shared_from_this();return 0;}
};
using ID3D12CommandList=IUnknown;
struct PrivateResetPacket{
 std::mutex mutex;bool retired=false,returned=false,unusedProducer=false;
 ResetPolicy::Recording producer{},consumer{};WindowPolicy::FrameKey temporalKey{};
 ComPtr<IUnknown> producerIdentity,consumerIdentity;
};
struct TemporalWindow{
 std::mutex mutex;bool continuous=true,stopped=false,restartRequested=false;
 unsigned returnEvidencePending=0,drainedFrames=0;
 std::unique_ptr<WindowPolicy::Window> policy;
 std::array<std::shared_ptr<PrivateResetPacket>,64> frames;
 std::vector<PrivateResetPacket*> liveFrames;
};
struct ListState{bool known=true;uint64_t generation=0;};
struct Registry{std::mutex mutex;std::unordered_map<IUnknown*,ListState> lists;};
Registry& Data(){static Registry data;return data;}
std::atomic<bool> captureTrackingValid{true};
[[noreturn]] void PrivateResetFatal(){throw std::runtime_error("fatal");}
constexpr ResetPolicy::Queue queue{1,2,true};
'''
        checks = r'''
void clear(){auto& g=ReplayGuards();g.policy={};g.identities={};g.pending=false;Data().lists.clear();}
std::shared_ptr<PrivateResetPacket> prepare(TemporalWindow& w,bool unused=false){
 w.policy=std::make_unique<WindowPolicy::Window>(9,queue,500,0);
 auto f=std::make_shared<PrivateResetPacket>();
 f->producerIdentity.value=std::make_shared<IUnknown>();
 f->producer={uintptr_t(f->producerIdentity.Get()),100};
 auto k=w.policy->ClaimRole(500,WindowPolicy::Role::Ray);f->temporalKey=k;
 assert(w.policy->ClaimRole(500,WindowPolicy::Role::Guides)==k);
 auto p=f->producer;assert(w.policy->DeclareProducer(k,p));
 assert(w.policy->SealProducer(k,{p,p,1,2,3,4,true,true,true,true,true,true,true,true}));
 if(!unused){
  f->consumerIdentity.value=std::make_shared<IUnknown>();
  f->consumer={uintptr_t(f->consumerIdentity.Get()),101};
  assert(w.policy->ClaimRole(500,WindowPolicy::Role::Fog)==k);
  assert(w.policy->EmbedConsumer(k,f->consumer,5,true));
  assert(w.policy->SealConsumer(k,f->consumer,6,true,true));
 }
 std::array lists{f->producer,f->consumer};
 auto s=w.policy->BeforeExecute(queue,std::span(lists.data(),unused?1:2));assert(s.allowed);
 assert(w.policy->AfterExecute(s.receipt).allowed);
 if(!unused)assert(w.policy->CommitConsumer(k));
 f->unusedProducer=unused;f->returned=!unused;
 w.frames[0]=f;w.liveFrames.push_back(f.get());return f;
}
int main(){
 // Scalar capacity/identity/generation contract, including transactional failure.
 {FSRD::ReplayGuardPolicy::Guards<2> g;std::array<ResetPolicy::Recording,2> a{{{1,5},{2,7}}};
  assert(g.Remember(a));assert(!g.Allows(a));
  for(auto v:{0u,4u,5u}){ResetPolicy::Recording r{1,v};assert(!g.Allows(std::span(&r,1)));assert(!g.Retire(r));}
  std::array<ResetPolicy::Recording,2> bad{{{1,99},{3,1}}};assert(!g.Remember(bad));
  assert(g.At(g.Find(1)).generation==5);bad[1]={2,0};assert(!g.Remember(bad));
  assert(g.At(g.Find(1)).generation==5);
  std::array<ResetPolicy::Recording,2> dup{{{1,8},{1,6}}};assert(g.Remember(dup));
  assert(g.At(g.Find(1)).generation==8);assert(g.Retire({1,9}));assert(g.Retire({2,8}));assert(g.Empty());
 }
 for(bool unused:{false,true}){
  clear();TemporalWindow w;auto f=prepare(w,unused);
  assert(!TransferCompletedTemporalWatch(w,f));
  w.stopped=w.restartRequested=true;w.policy->Stop();
  assert(!TransferCompletedTemporalWatch(w,f));f->retired=true;
  {auto cpuReader=f;assert(!TransferCompletedTemporalWatch(w,f));}
  w.returnEvidencePending=1;assert(!TransferCompletedTemporalWatch(w,f));w.returnEvidencePending=0;
  w.restartRequested=false;assert(!TransferCompletedTemporalWatch(w,f));w.restartRequested=true;
  const auto key=f->temporalKey;auto p=f->producer,c=f->consumer;
  assert(!w.policy->TransferReplayGuard(key,p,c,unused,false));
  assert(!w.policy->CanTransferReplayGuard(key,{p.list,p.generation+1},c,unused));
  assert(!w.policy->CanTransferReplayGuard(key,p,c,!unused));
  // Unowned/wrong identity cannot publish guards or erase the old watch.
  auto identity=f->producerIdentity;f->producerIdentity={};assert(!TransferCompletedTemporalWatch(w,f));
  f->producerIdentity=identity;identity={};
  std::weak_ptr<PrivateResetPacket> oldFrame=f;
  std::weak_ptr<IUnknown> oldIdentity=f->producerIdentity.value;
  assert(TransferCompletedTemporalWatch(w,f));assert(w.liveFrames.empty()&&!w.frames[0]);
  assert(w.policy->SubmissionWatches()==0&&w.policy->CommittedFrames()==(unused?0u:1u));
  f.reset();w.policy.reset();assert(oldFrame.expired()&&!oldIdentity.expired());
  auto liveIdentity=oldIdentity.lock();ID3D12CommandList* list=liveIdentity.get();
  // Same exact recording is still refused after the entire old root is gone.
  assert(!AllowsRetiredSubmission(1,&list));
  Data().lists[list]={true,p.generation};assert(!AllowsRetiredSubmission(1,&list));
  Data().lists[list]={true,p.generation-1};assert(!AllowsRetiredSubmission(1,&list));
  Data().lists[list]={true,p.generation+1};captureTrackingValid=false;
  assert(!AllowsRetiredSubmission(1,&list));captureTrackingValid=true;
  // A successful newer Reset retires only this identity, not any other guard.
  assert(AllowsRetiredSubmission(1,&list));liveIdentity.reset();assert(oldIdentity.expired());
  assert(ReplayGuards().pending.load()==!unused);
 }
 clear();
 // Registry saturation fails closed: the frame/root and old watch remain owned.
 {TemporalWindow w;auto f=prepare(w);w.stopped=w.restartRequested=f->retired=true;w.policy->Stop();
  std::array<ResetPolicy::Recording,MaxRetiredReplayGuards> full;
  for(size_t i=0;i<full.size();++i)full[i]={i+1,1};
  assert(ReplayGuards().policy.Remember(full));assert(!TransferCompletedTemporalWatch(w,f));
  assert(w.frames[0]==f&&w.liveFrames.size()==1&&w.policy->SubmissionWatches()==1);
 }
 clear();
 // A hard failed frame cannot be migrated, even with a forged host retired flag.
 {TemporalWindow w;auto f=prepare(w);w.stopped=w.restartRequested=f->retired=true;
  w.policy->FailFrame(f->temporalKey);assert(!TransferCompletedTemporalWatch(w,f));}
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-retired-replay-') as folder:
            source = Path(folder) / 'test.cpp'
            source.write_text(fixture + functions + checks)
            for flags in (['-O0'], ['-O3', '-ffast-math']):
                binary = Path(folder) / 'test'
                result = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags,
                                         '-I', str(BASE), str(source), '-o', str(binary)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()

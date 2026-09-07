"""Actual value-only window policy; no native queue, frame, camera or GPU claims."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "OptiScaler/upscalers/ffx"
HEADER = BASE / "FSRDCyberpunkTemporalWindowPolicy.h"


class TemporalWindowPolicy(unittest.TestCase):
    def test_narrow_value_contract_and_existing_frame_policy(self):
        source = HEADER.read_text()
        for forbidden in ("ID3D", "ComPtr", "shared_ptr", "operator new", "chrono", "MillisecondsNow",
                          "QueryPerformance", "ReadProcessMemory", "Config::", "GetLast"):
            self.assertNotIn(forbidden, source)
        for required in ("FSRDCyberpunkPrivateResetPolicy.h", "std::optional<FramePolicy::Policy>",
                         "FrameCount = 32", "MaxFramesPerCall = 2", "index == 31",
                         "policy->BeforeExecute", "policy.AfterExecute", "CommitConsumer",
                         "HOST COMMIT ATTESTATION", "completed tombstones", "not an independent observation"):
            self.assertIn(required, source)
        self.assertNotIn("FSRDPrivateDenoise.h", source)

    def test_actual_window_routes_submissions_stops_and_tombstones(self):
        compiler = os.environ.get("CXX") or shutil.which("c++")
        if not compiler:
            self.skipTest("Set CXX for actual window policy compilation")
        harness = r'''
#include "FSRDCyberpunkTemporalWindowPolicy.h"
#include <cassert>
#include <iostream>
#include <vector>
using namespace FSRD::CyberpunkTemporalWindowPolicy;
constexpr Queue queue{71,81,true};
constexpr uint64_t epoch=991;
constexpr uint32_t first=1000;
Recording producer(unsigned i){return {100+uintptr_t(i)*10,2000+i};}
Recording consumer(unsigned i){return {101+uintptr_t(i)*10,3000+i};}
ProducerSeal seal(Recording recording){return {recording,recording,10,20,30,40,true,true,true,true,true,true,true,true};}
FrameKey producers(Window& w,unsigned i,Recording recording={}){
 if(!recording.Valid())recording=producer(i);
 const auto key=w.ClaimRole(first+i,Role::Ray);assert(key.Valid()&&key.index==i&&key.frame==first+i&&key.epoch==epoch);
 assert(w.DeclareProducer(key,recording));assert(w.ClaimRole(first+i,Role::Guides)==key);
 assert(w.SealProducer(key,seal(recording)));return key;
}
FrameKey fog(Window& w,unsigned i,Recording recording={}){
 if(!recording.Valid())recording=consumer(i);
 const auto key=w.ClaimRole(first+i,Role::Fog);assert(key.Valid());
 assert(w.EmbedConsumer(key,recording,50,true));assert(w.SealConsumer(key,recording,60,true,true));return key;
}
SubmissionDecision submit(Window& w,std::initializer_list<Recording> recordings,Queue q=queue){
 return w.BeforeExecute(q,std::span<const Recording>(recordings.begin(),recordings.size()));
}
void commit(Window& w,FrameKey key,Receipt receipt){
 const auto returned=w.AfterExecute(receipt);assert(returned.allowed&&returned.consumer==key);
 assert(w.ConsumerReturned(key));assert(w.CommitConsumer(key));
}
int main(){
 // All32 consecutive frames, one look-ahead producer, only final index is captured.
 {Window w(epoch,queue,first);unsigned finalCaptures=0;
  for(unsigned i=0;i<32;++i){FrameKey key{epoch,i,first+i};
   if(i==0)assert(producers(w,i)==key);
   assert(fog(w,i)==key);finalCaptures+=key.CaptureFinal();
   if(i+1<32)producers(w,i+1);
   const auto submitted=submit(w,{producer(i),consumer(i)});assert(submitted.allowed&&submitted.receipt.Valid());
   commit(w,key,submitted.receipt);assert(w.CommittedFrames()==i+1&&!w.Stopped());
  }
  assert(w.Complete()&&finalCaptures==1);
  assert(submit(w,{{999,0}},Queue{5,6,false}).allowed); // Unrelated unknown generation/queue is harmless.
  assert(w.BeforeExecute(queue,{}).allowed);
  const auto duplicate=submit(w,{producer(0)});
  assert(!duplicate.allowed&&duplicate.frameFailure==FramePolicy::Failure::ProducerAlreadySubmitted);
  assert(w.ConsumerEmbedded({epoch,0,first})); // Completed records never disappear.
 }
 // Separate actual producer return is not consumer return and cannot commit history.
 {Window w(epoch,queue,first);const auto key=producers(w,0);fog(w,0);
  auto p=submit(w,{producer(0)});assert(p.allowed);
  auto returned=w.AfterExecute(p.receipt);assert(returned.allowed&&!returned.consumer.Valid()&&returned.producersReturned==1);
  assert(!w.ConsumerReturned(key)&&w.CommittedFrames()==0);
  auto c=submit(w,{consumer(0)});assert(c.allowed);commit(w,key,c.receipt);assert(w.CommittedFrames()==1);
 }
 // Producer-only attempted commit stops admission, but preserves the embedded consumer.
 {Window w(epoch,queue,first);const auto key=producers(w,0);fog(w,0);
  auto p=submit(w,{producer(0)});assert(w.AfterExecute(p.receipt).allowed);
  assert(!w.CommitConsumer(key)&&w.Stopped()&&w.ConsumerEmbedded(key));
  auto c=submit(w,{consumer(0)});assert(c.allowed);assert(w.AfterExecute(c.receipt).consumer==key);
  assert(!w.CommitConsumer(key));
 }
 // Missing/overlapping producer return is not inferred from entry or a submitted flag.
 {Window w(epoch,queue,first);const auto key=producers(w,0);fog(w,0);
  auto p=submit(w,{producer(0)});assert(p.allowed);
  auto c=submit(w,{consumer(0)});assert(!c.allowed&&c.frameFailure==FramePolicy::Failure::ProducerCallNotReturned);
  assert(w.Stopped()&&w.ConsumerEmbedded(key));
 }
 // Current and look-ahead producer share one actual array: demux local token1/token1.
 {Window w(epoch,queue,first);auto current=producers(w,0);producers(w,1);fog(w,0);
  auto p=submit(w,{producer(0),producer(1)});assert(p.allowed);
  auto r=w.AfterExecute(p.receipt);assert(r.allowed&&r.producersReturned==2&&!r.consumer.Valid());
  auto c=submit(w,{consumer(0)});assert(c.allowed);commit(w,current,c.receipt);
  auto next=fog(w,1);c=submit(w,{consumer(1)});assert(c.allowed);commit(w,next,c.receipt);
 }
 // Different related original calls can return in reverse CPU order without exchanging tokens.
 {Window w(epoch,queue,first);producers(w,0);producers(w,1);
  auto a=submit(w,{producer(0)}),b=submit(w,{producer(1)});assert(a.allowed&&b.allowed&&a.receipt!=b.receipt);
  assert(w.AfterExecute(b.receipt).producersReturned==1);assert(w.AfterExecute(a.receipt).producersReturned==1);
  assert(!w.AfterExecute(a.receipt).allowed&&w.Stopped());
 }
 // Exact same-list local insertion is supported through the existing frame policy.
 {Window w(epoch,queue,first);auto key=producers(w,0);fog(w,0,producer(0));
  auto s=submit(w,{producer(0)});assert(s.allowed);commit(w,key,s.receipt);}
 // Fog may record first on CPU, but actual producer must be sealed before native submission.
 {Window w(epoch,queue,first);auto key=fog(w,0);assert(w.ConsumerEmbedded(key));
  producers(w,0);auto s=submit(w,{producer(0),consumer(0)});assert(s.allowed);commit(w,key,s.receipt);}
 {Window w(epoch,queue,first);auto key=fog(w,0);auto s=submit(w,{consumer(0)});
  assert(!s.allowed&&s.frameFailure==FramePolicy::Failure::ProducerNotSealed&&w.ConsumerEmbedded(key));}
 // Wrong array order, duplicate entries, changed generations and fixed queue/device gate.
 for(unsigned bad=0;bad<7;++bad){Window w(epoch,queue,first);auto key=producers(w,0);fog(w,0);
  SubmissionDecision d;
  if(bad==0)d=submit(w,{consumer(0),producer(0)});
  if(bad==1)d=submit(w,{producer(0),consumer(0),consumer(0)});
  if(bad==2)d=submit(w,{{producer(0).list,producer(0).generation+1},consumer(0)});
  if(bad==3)d=submit(w,{producer(0),{consumer(0).list,0}});
  if(bad==4)d=submit(w,{producer(0),consumer(0)},Queue{queue.queue+1,queue.device,true});
  if(bad==5)d=submit(w,{producer(0),consumer(0)},Queue{queue.queue,queue.device+1,true});
  if(bad==6)d=submit(w,{producer(0),consumer(0)},Queue{queue.queue,queue.device,false});
  assert(!d.allowed&&w.Stopped()&&w.ConsumerEmbedded(key));
 }
 // An old returned recording survives Reset only as immutable evidence, not as the new generation.
 {Window w(epoch,queue,first);auto key=producers(w,0);fog(w,0);auto a=submit(w,{producer(0),consumer(0)});commit(w,key,a.receipt);
  auto changed=producer(0);++changed.generation;assert(submit(w,{changed}).allowed&&!w.Stopped());
  changed.generation=0;assert(!submit(w,{changed}).allowed&&w.ConsumerEmbedded(key));}
 // Explicit stop still permits existing producer closures and valid recorded consumers to drain.
 {Window w(epoch,queue,first);auto key=fog(w,0);w.Stop();assert(w.Stopped()&&w.ConsumerEmbedded(key));
  assert(!w.FrameFailed(key)&&w.FrameFailed({})&&w.FrameFailed({epoch+1,0,first}));
  assert(!w.ClaimRole(first+1,Role::Ray).Valid());assert(!w.ClaimRole(first,Role::Fog).Valid());
  assert(producers(w,0)==key);auto s=submit(w,{producer(0),consumer(0)});assert(s.allowed);
  auto r=w.AfterExecute(s.receipt);assert(r.allowed&&r.consumer==key&&!w.CommitConsumer(key));}
 {Window w(epoch,queue,first);auto key=w.ClaimRole(first,Role::Ray);assert(w.DeclareProducer(key,producer(0)));
  w.Stop();assert(w.ClaimRole(first,Role::Guides)==key);assert(w.SealProducer(key,seal(producer(0))));
  auto p=submit(w,{producer(0)});assert(p.allowed&&w.AfterExecute(p.receipt).producersReturned==1);
  assert(!w.ClaimRole(first,Role::Fog).Valid());}
 // Failure after recording does not clear an obligation or authorize its native submission.
 {Window w(epoch,queue,first);auto key=producers(w,0);fog(w,0);w.FailFrame(key);
  assert(w.Stopped()&&w.FrameFailed(key)&&w.ConsumerEmbedded(key)&&!submit(w,{producer(0),consumer(0)}).allowed);}
 {Window w(epoch,queue,first);auto key=producers(w,0);assert(w.ClaimRole(first,Role::Fog)==key);
  assert(w.EmbedConsumer(key,consumer(0),50,true));assert(!w.SealConsumer(key,consumer(0),60,false,true));
  assert(w.ConsumerEmbedded(key)&&!submit(w,{producer(0),consumer(0)}).allowed);}
 // Repeated roles, future Fog, frame gaps/old frames and forged keys cannot allocate new work.
 for(unsigned bad=0;bad<6;++bad){Window w(epoch,queue,first);auto key=w.ClaimRole(first,Role::Ray);assert(key.Valid());
  FrameKey invalid;
  if(bad==0)invalid=w.ClaimRole(first,Role::Ray);
  if(bad==1)invalid=w.ClaimRole(first+1,Role::Fog);
  if(bad==2)invalid=w.ClaimRole(first+2,Role::Ray);
  if(bad==3)invalid=w.ClaimRole(first-1,Role::Ray);
  if(bad==4)invalid=w.ClaimRole(first+32,Role::Ray);
  if(bad==5)invalid=w.ClaimRole(first,static_cast<Role>(255));
  assert(!invalid.Valid()&&w.Stopped());}
 for(unsigned bad=0;bad<3;++bad){Window w(epoch,queue,first);auto key=w.ClaimRole(first,Role::Ray);
  if(bad==0)++key.epoch;
  if(bad==1)++key.frame;
  if(bad==2)key.index=32;
  assert(!w.DeclareProducer(key,producer(0))&&w.Stopped());}
 // Tampered, unknown, cross-epoch and duplicated native return receipts fail closed.
 for(unsigned bad=0;bad<4;++bad){Window w(epoch,queue,first);auto key=producers(w,0);fog(w,0);
  auto s=submit(w,{producer(0),consumer(0)});auto receipt=s.receipt;
  if(bad==0)++receipt.epoch;
  if(bad==1)++receipt.serial;
  if(bad==2)++receipt.queue;
  if(bad==3)receipt={};
  assert(!w.AfterExecute(receipt).allowed&&w.Stopped()&&w.ConsumerEmbedded(key));}
 // Oversized unknown arrays are not certified unrelated, even after a soft stop.
 {Window w(epoch,queue,first);auto key=fog(w,0);w.Stop();std::vector<Recording> many(257);
  auto d=w.BeforeExecute(queue,many);assert(!d.allowed&&d.frameFailure==FramePolicy::Failure::ArrayTooLarge&&w.ConsumerEmbedded(key));}
 // All constructor guards and native counter wrap: zero source frame is valid.
 {Window w(epoch,queue,0);assert(w.ClaimRole(0,Role::Ray).Valid());}
 for(unsigned bad=0;bad<5;++bad){auto q=queue;auto e=epoch;auto f=first;
  if(bad==0)e=0;
  if(bad==1)q.queue=0;
  if(bad==2)q.device=0;
  if(bad==3)q.direct=false;
  if(bad==4)f=UINT32_MAX-30;
  Window w(e,q,f);assert(w.Stopped()&&!w.Complete()&&!w.ClaimRole(f,Role::Ray).Valid());}
 // Long visual pass: one source sequence, no final dump, stable keys, bounded
 // scanning only after BOTH authenticated newer list Resets and actual returns.
 {Window w(epoch,queue,first,Window::VisualFrameCount);
  for(unsigned i=0;i<Window::VisualFrameCount;++i){
   const Recording p{100,10+uint64_t(i)*2},c{101,11+uint64_t(i)*2};
   const auto key=producers(w,i,p);assert(key.frameCount==18000&&!key.CaptureFinal());fog(w,i,c);
   assert(!w.RetireSubmissionWatch(key,{100,p.generation+2},{101,c.generation+2}));
   auto s=submit(w,{p,c});assert(s.allowed);commit(w,key,s.receipt);
   const auto count=w.SubmissionWatches();assert(count==1);
   assert(!w.RetireSubmissionWatch(key,p,{101,c.generation+2})); // One Reset is insufficient.
   assert(!w.RetireSubmissionWatch(key,{100,p.generation+2},c));
   assert(!w.RetireSubmissionWatch(key,{100,0},{101,c.generation+2}));
   assert(!w.RetireSubmissionWatch(key,{999,p.generation+2},{101,c.generation+2}));
   auto forged=key;forged.frameCount=32;assert(!w.RetireSubmissionWatch(forged,{100,p.generation+2},{101,c.generation+2}));
   assert(w.RetireSubmissionWatch(key,{100,p.generation+2},{101,c.generation+2}));
   assert(!w.RetireSubmissionWatch(key,{100,p.generation+2},{101,c.generation+2}));
   assert(w.SubmissionWatches()==0&&w.ConsumerReturned(key)&&w.ConsumerEmbedded(key));
   assert(!w.Stopped()&&w.CommittedFrames()==i+1);
  }
  assert(w.Complete());
 }
 // Unknown/unreset lists never get evicted merely to keep a visual pass running.
 {Window w(epoch,queue,first,Window::VisualFrameCount);
  for(unsigned i=0;i<Window::MaxVisualWatches;++i){auto key=producers(w,i);fog(w,i);
   auto s=submit(w,{producer(i),consumer(i)});assert(s.allowed);commit(w,key,s.receipt);}
  assert(w.SubmissionWatches()==Window::MaxVisualWatches);
  assert(!w.ClaimRole(first+Window::MaxVisualWatches,Role::Ray).Valid()&&w.Stopped());
  auto duplicate=submit(w,{consumer(0)});assert(!duplicate.allowed); // Retained duplicate guard.
 }
 {Window w(epoch,queue,first,Window::VisualFrameCount);auto key=producers(w,0);fog(w,0);
  auto s=submit(w,{producer(0),consumer(0)});commit(w,key,s.receipt);
  assert(!submit(w,{producer(0)}).allowed); // Before either Reset, duplicates still refuse.
 }
 // Continuous history reuses only retired slots; stale keys never alias a new frame.
 {Window w(epoch,queue,first,Window::ContinuousFrameCount);FrameKey stale{};
  assert(w.Continuous()&&w.StorageFrames()==64);
  for(unsigned i=0;i<100000;++i){
   const Recording p{100,10+uint64_t(i)*2},c{101,11+uint64_t(i)*2};
   const auto key=producers(w,i,p);fog(w,i,c);if(i==0)stale=key;
   auto s=submit(w,{p,c});assert(s.allowed);commit(w,key,s.receipt);
   assert(!w.Complete()&&!key.CaptureFinal());
   assert(!w.RetireSubmissionWatch(key,p,{101,c.generation+2}));
   assert(w.RetireSubmissionWatch(key,{100,p.generation+2},{101,c.generation+2}));
   assert(w.FrameFailed(stale)&&!w.ConsumerReturned(stale));
   assert(w.StorageFrames()==64&&w.SubmissionWatches()==0&&!w.Stopped());
  }
  assert(w.CommittedFrames()==100000&&!w.Complete());
 }
 // A stopped continuous controller can drain an already sealed consumer, but
 // the same opt-in does not weaken the existing bounded experiment contract.
 {Window w(epoch,queue,first,0);const auto key=producers(w,0);fog(w,0);
  auto s=submit(w,{producer(0),consumer(0)});w.Stop();
  auto r=w.AfterExecute(s.receipt);assert(r.consumer==key);
  assert(w.CommitConsumer(key,true)&&w.CommittedFrames()==1&&w.Stopped());}
 // Submitted look-ahead work with no consumer can retire only after its actual
 // return and a newer producer Reset; this never commits denoiser history.
 {Window w(epoch,queue,first,0);const auto key=producers(w,0);w.Stop();
  assert(!w.RetireUnusedProducer(key,{producer(0).list,producer(0).generation+1}));
  auto s=submit(w,{producer(0)});assert(s.allowed);assert(w.AfterExecute(s.receipt).allowed);
  assert(!w.RetireUnusedProducer(key,producer(0)));
  assert(!w.RetireUnusedProducer(key,{producer(0).list,0}));
  assert(w.RetireUnusedProducer(key,{producer(0).list,producer(0).generation+1}));
  assert(w.CommittedFrames()==0&&w.SubmissionWatches()==0);}
 {Window w(epoch,queue,first,0);const auto key=producers(w,0);fog(w,0);w.Stop();
  auto s=submit(w,{producer(0)});assert(s.allowed);assert(w.AfterExecute(s.receipt).allowed);
  assert(!w.RetireUnusedProducer(key,{producer(0).list,producer(0).generation+1}));}
 {Window w(epoch,queue,first,0);
  for(unsigned i=0;i<64;++i){auto key=producers(w,i);fog(w,i);
   auto s=submit(w,{producer(i),consumer(i)});commit(w,key,s.receipt);}
  assert(!w.ClaimRole(first+64,Role::Ray).Valid()&&w.Stopped());}
 for(auto count:{31u,33u,17999u,18001u,UINT32_MAX}){
  Window w(epoch,queue,first,count);assert(w.Stopped()&&!w.ClaimRole(first,Role::Ray).Valid());}
 {Window w(epoch,queue,UINT32_MAX-17998,18000);assert(w.Stopped());}
 std::cout<<"32-frame control and 18000-frame visual routing, actual-return demux and guarded watch retirement passed\n";
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-temporal-window-") as directory:
            source = Path(directory) / "test.cpp"
            source.write_text(harness)
            outputs = []
            for number, flags in enumerate((["-O0"], ["-O3", "-ffast-math", "-ffp-contract=fast"])):
                binary = Path(directory) / f"test-{number}"
                result = subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", *flags,
                                         "-I", str(BASE), str(source), "-o", str(binary)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                outputs.append(result.stdout)
            self.assertEqual(outputs[0], outputs[1])


if __name__ == "__main__":
    unittest.main()

"""Actual portable one-packet submission admission; no native/GPU ordering guesses."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'OptiScaler/upscalers/ffx/FSRDCyberpunkPrivateResetPolicy.h'


class PrivateResetPolicy(unittest.TestCase):
    def test_actual_policy_order_failure_and_durable_obligation(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX to compile the actual private RESET policy')
        harness = r'''
#include "FSRDCyberpunkPrivateResetPolicy.h"
#include <array>
#include <cassert>
#include <type_traits>
using namespace FSRD::CyberpunkPrivateResetPolicy;
constexpr uintptr_t device=0x1000;
constexpr Queue queue{0x2000,device,true};
constexpr Recording producer{0x3000,11},consumer{0x4000,12},other{0x5000,0};
ProducerSeal Seal(Recording p=producer) {
 return {p,p,1,2,3,4,true,true,true,true,true,true,true,true};
}
void Ready(Policy& p, Recording c=consumer,uint64_t firstRead=1) {
 assert(p.DeclareProducer(producer));
 assert(p.SealProducer(Seal()));
 assert(p.EmbedConsumer(c,firstRead,true));
 assert(p.SealConsumer(c,firstRead+1,true,true));
}
Decision Submit(Policy& p,Recording r,Queue q=queue) {
 const std::array a{r};return p.BeforeExecute(q,a);
}
static_assert(noexcept(Policy(device)));
static_assert(!std::is_copy_constructible_v<Policy> && !std::is_move_constructible_v<Policy>);
static_assert(noexcept(std::declval<Policy&>().BeforeExecute(queue,{})));
static_assert(noexcept(std::declval<Policy&>().AfterExecute(1)));
int main() {
 // Producer/consumer in one actual array: ordering is its index, not CPU entry order.
 {
  Policy p(device);Ready(p);const std::array a{other,producer,consumer};
  auto d=p.BeforeExecute(queue,a);assert(d.admission==Admission::EarlierInArray && d.token);
  assert(p.ConsumerEmbedded() && p.ConsumerAdmitted() && !p.ConsumerReturned() && !p.ProducerReturned());
  assert(p.AfterExecute(d.token));assert(p.ConsumerReturned() && p.ProducerReturned());
 }
 // The consumer can be embedded/sealed before either producer is CPU-recorded.
 {
  Policy p(device);assert(p.EmbedConsumer(consumer,1,true));assert(p.SealConsumer(consumer,2,true,true));
  assert(p.DeclareProducer(producer));assert(p.SealProducer(Seal()));
  const std::array a{producer,consumer};assert(p.BeforeExecute(queue,a).admission==Admission::EarlierInArray);
 }
 // Previously returned original producer call on exactly the same queue/device.
 {
  Policy p(device);assert(p.DeclareProducer(producer));assert(p.SealProducer(Seal()));
  auto first=Submit(p,producer);assert(first.admission==Admission::ProducerOnly && first.token);
  assert(!p.ProducerReturned());assert(p.AfterExecute(first.token));
  // Reusable producer-list Reset does not erase already-submitted work.
  auto reused=Submit(p,{producer.list,producer.generation+1});
  assert(reused.admission==Admission::Unrelated && !reused.token && p.ProducerReturned());
  assert(p.EmbedConsumer(consumer,1,true));assert(p.SealConsumer(consumer,2,true,true));
  auto second=Submit(p,consumer);assert(second.admission==Admission::ReturnedProducer && second.token!=first.token);
  assert(p.AfterExecute(second.token));assert(p.ConsumerReturned());
  assert(Submit(p,{consumer.list,consumer.generation+1}).admission==Admission::Unrelated);
 }
 // Same native recording requires producer terminal barriers before consumer reads.
 {
  Policy p(device);Ready(p,producer,5);auto d=Submit(p,producer);
  assert(d.admission==Admission::SameRecording);assert(p.AfterExecute(d.token));
 }
 for(uint64_t firstRead=1;firstRead<=4;++firstRead) {
  Policy p(device);Ready(p,producer,firstRead);auto d=Submit(p,producer);
  assert(d.admission==Admission::Refused && d.failure==Failure::WrongLocalOrder);
  assert(p.ConsumerEmbedded() && !p.ConsumerAdmitted() && p.Failed());
 }
 // Earlier callback/entry serial is not a returned predecessor.
 {
  Policy p(device);Ready(p);auto first=Submit(p,producer);assert(first.Allowed());
  auto second=Submit(p,consumer);assert(second.failure==Failure::ProducerCallNotReturned);
  assert(p.ConsumerEmbedded() && !p.ConsumerAdmitted() && p.Failed());
  assert(p.AfterExecute(first.token));
  // A later return cannot recover an already rejected consumer submission.
  assert(!Submit(p,consumer).Allowed());
 }
 {
  Policy p(device);Ready(p);assert(Submit(p,consumer).failure==Failure::MissingProducerSubmission);
 }
 {
  Policy p(device);Ready(p);const std::array a{consumer,producer};
  assert(p.BeforeExecute(queue,a).failure==Failure::WrongArrayOrder);
 }
 // Duplicate entries are refused even when the duplicate itself is unrelated.
 for(unsigned which=0;which<3;++which) {
  Policy p(device);Ready(p);
  const Recording duplicated=which==0?producer:which==1?consumer:other;
  const std::array a{producer,consumer,duplicated,duplicated};
  assert(p.BeforeExecute(queue,a).failure==Failure::DuplicateEntry);
 }
 // Exact recorded commands may not be submitted again, during or after return.
 for(bool returned:{false,true}) {
  Policy p(device);Ready(p);auto d=Submit(p,producer);assert(d.Allowed());
  if(returned) assert(p.AfterExecute(d.token));
  assert(Submit(p,producer).failure==Failure::ProducerAlreadySubmitted);
 }
 {
  Policy p(device);Ready(p);auto d=Submit(p,producer);assert(p.AfterExecute(d.token));
  auto c=Submit(p,consumer);assert(c.Allowed());assert(p.AfterExecute(c.token));
  assert(Submit(p,consumer).failure==Failure::ConsumerAlreadySubmitted);
 }
 // The current generation is supplied by observed Reset, not an old saved key.
 for(unsigned which=0;which<4;++which) {
  Policy p(device);Ready(p);Recording changed=which<2?producer:consumer;
  changed.generation=(which&1)?0:changed.generation+1;
  assert(Submit(p,changed).failure==Failure::GenerationChanged);
  assert(p.ConsumerEmbedded());
 }
 {
  Policy p(device);Ready(p);auto d=Submit(p,producer);assert(p.AfterExecute(d.token));
  assert(Submit(p,{producer.list,0}).failure==Failure::GenerationChanged);
 }
 // An unknown/non-direct/different-device/different canonical queue is not ordered.
 for(unsigned which=0;which<4;++which) {
  Policy p(device);Ready(p);Queue q=queue;
  if(which==0)q.queue=0;if(which==1)q.direct=false;if(which==2)q.device=0;if(which==3)++q.device;
  const std::array a{producer,consumer};assert(p.BeforeExecute(q,a).failure==Failure::WrongQueue);
 }
 {
  Policy p(device);Ready(p);auto d=Submit(p,producer);assert(p.AfterExecute(d.token));
  Queue q=queue;++q.queue;assert(Submit(p,consumer,q).failure==Failure::WrongQueue);
 }
 // Every individual producer success/barrier/restoration/retention proof is required.
 for(unsigned bad=0;bad<16;++bad) {
  Policy p(device);assert(p.DeclareProducer(producer));assert(p.EmbedConsumer(consumer,1,true));
  auto s=Seal();
  if(bad==0)s.raySucceeded=false;if(bad==1)s.guidesSucceeded=false;
  if(bad==2)s.rayReadBarriers=false;if(bad==3)s.guideReadBarriers=false;
  if(bad==4)s.rayRestored=false;if(bad==5)s.guidesRestored=false;
  if(bad==6)s.rayOwnersRetained=false;if(bad==7)s.guideOwnersRetained=false;
  if(bad==8)s.rayTerminal=0;if(bad==9)s.rayTerminal=s.guideBegin;
  if(bad==10)s.guideBegin=s.guideTerminal+1;if(bad==11)s.sealed=s.guideTerminal-1;
  if(bad==12)++s.ray.generation;if(bad==13)++s.guides.generation;
  if(bad==14)++s.guides.list;if(bad==15)s.ray.list=0;
  assert(!p.SealProducer(s));assert(p.ConsumerEmbedded() && p.Failed());
  assert(!p.SealProducer(Seal())); // No successful retry can resurrect the packet.
 }
 {
  Policy p(device);assert(!p.SealProducer(Seal()));assert(p.LastFailure()==Failure::ProducerNotDeclared);
 }
 {
  Policy p(device);assert(p.DeclareProducer(producer));
  assert(!Submit(p,producer).Allowed());assert(p.LastFailure()==Failure::ProducerNotSealed);
 }
 // An obligation is not a completed consumer. It survives every later failure.
 {
  Policy p(device);assert(p.DeclareProducer(producer));assert(p.SealProducer(Seal()));
  assert(p.EmbedConsumer(consumer,1,true));const std::array a{producer,consumer};
  assert(p.BeforeExecute(queue,a).failure==Failure::ConsumerNotSealed);assert(p.ConsumerEmbedded());
 }
 for(unsigned bad=0;bad<4;++bad) {
  Policy p(device);assert(p.EmbedConsumer(consumer,2,true));Recording c=consumer;
  if(bad==0)++c.generation;
  assert(!p.SealConsumer(c,bad==1?1:3,bad!=2,bad!=3));assert(p.ConsumerEmbedded());
 }
 {
  Policy p(device);Ready(p);p.Fail();assert(p.ConsumerEmbedded());
  assert(!Submit(p,consumer).Allowed());assert(!p.EmbedConsumer(consumer,2,true));
  assert(Submit(p,other).admission==Admission::Unrelated); // Original unrelated work is not dropped.
 }
 // Invalid/repeated declarations cannot replace packet identities/resources.
 {
  Policy p(device);assert(p.DeclareProducer(producer));assert(!p.DeclareProducer(producer));
 }
 {
  Policy p(device);Ready(p);assert(!p.SealProducer(Seal()));assert(p.ConsumerEmbedded());
 }
 {
  Policy p(device);Ready(p);assert(!p.EmbedConsumer(consumer,2,true));assert(p.ConsumerEmbedded());
 }
 {
  Policy p(device);Ready(p);assert(!p.SealConsumer(consumer,3,true,true));assert(p.ConsumerEmbedded());
 }
 for(Recording invalid:std::array{Recording{},Recording{1,0},Recording{0,1}}) {
  Policy p(device);assert(!p.DeclareProducer(invalid));
  Policy c(device);assert(!c.EmbedConsumer(invalid,1,true));assert(!c.ConsumerEmbedded());
 }
 {
  Policy p(device);assert(!p.EmbedConsumer(consumer,1,false));assert(!p.ConsumerEmbedded());
  Policy q(device);assert(!q.EmbedConsumer(consumer,0,true));
  Policy invalid(0);assert(invalid.Failed());assert(!invalid.DeclareProducer(producer));
 }
 {
  Policy p(device);assert(p.EmbedConsumer({producer.list,producer.generation+1},1,true));
  assert(!p.DeclareProducer(producer));assert(p.ConsumerEmbedded());
  Policy q(device);assert(q.DeclareProducer(producer));
  assert(!q.EmbedConsumer({producer.list,producer.generation+1},1,true));
 }
 // Return tokens are one-use receipts, not arbitrary numerical ordering hints.
 for(unsigned wrong=0;wrong<3;++wrong) {
  Policy p(device);Ready(p);auto d=Submit(p,producer);assert(d.Allowed());
  if(wrong==2)assert(p.AfterExecute(d.token));
  assert(!p.AfterExecute(wrong==0?0:wrong==1?d.token+1:d.token));assert(p.Failed());
 }
 // Bounded admission: oversize arrays poison, rather than silently missing an obligation.
 {
  Policy p(device);Ready(p);std::array<Recording,Policy::MaxExecuteLists+1> a{};
  a.back()=consumer;assert(p.BeforeExecute(queue,a).failure==Failure::ArrayTooLarge);
  assert(p.ConsumerEmbedded() && p.Failed());
 }
 {
  Policy p(device);assert(p.BeforeExecute({},{}).admission==Admission::Unrelated);
  assert(Submit(p,other,{}).admission==Admission::Unrelated); // No packet list, no invented queue proof.
  Ready(p);auto d=Submit(p,producer);assert(p.AfterExecute(d.token));
  const std::array a{Recording{producer.list,producer.generation+1},consumer};
  assert(p.BeforeExecute(queue,a).admission==Admission::ReturnedProducer);
 }
}
'''
        # Independent statements intentionally occupy one line in the bounded
        # fixture; do not confuse fixture style with policy warnings.
        with tempfile.TemporaryDirectory(prefix='fsrd-private-reset-policy-') as tmp:
            source, binary = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
            source.write_text(harness)
            for flags in (['-O0'], ['-O3', '-ffast-math']):
                subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                                '-Wno-misleading-indentation', *flags,
                                '-I', str(HEADER.parent), str(source), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)

    def test_policy_has_no_engine_d3d_wait_or_allocation_dependency(self):
        source = HEADER.read_text()
        for forbidden in ('#include <windows', '#include <d3d', '#include <mutex', '#include <vector',
                          '#include <map', 'new ', 'delete ', 'Sleep(', 'WaitFor', 'GetTickCount',
                          'GetCompletedValue', 'ReadProcessMemory', 'reinterpret_cast', 'AddRef(', 'Release('):
            self.assertNotIn(forbidden, source)
        for required in ('host serializes ALL', 'No\n// lock may be held across the original Execute call',
                         'terminal transitions', 'CPU callback', 'first potentially failing',
                         'Poison(Failure::FailedPacket)', 'if (!_consumerReturned) changed = true',
                         '_producerTerminal >= _consumerFirstRead', 'producerIndex >= consumerIndex',
                         '_producerSubmitted && !_producerReturned', 'uint64_t token'):
            if required == 'CPU callback':
                self.assertIn('global callback counters', source)
            else:
                self.assertIn(required, source)


if __name__ == '__main__': unittest.main()

"""One-shot private RESET host wiring and actual submission-adapter checks.

Submission adapters compile unchanged against narrow COM/registry mocks and the
production policy. This is CPU admission coverage, not real GPU/frame validation.
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
HOST = (BASE / 'FSRDCyberpunkFogProbe.cpp').read_text()
QUEUE = (ROOT / 'OptiScaler/resource_tracking/ResTrack_dx12.cpp').read_text()


def body(source, signature, following):
    return source.split(signature, 1)[1].split(following, 1)[0]


class PrivateResetHost(unittest.TestCase):
    def test_both_native_execute_paths_are_vetoed_before_bookkeeping(self):
        execute = body(QUEUE, 'void ResTrack_Dx12::hkExecuteCommandLists(', '#pragma region Heap hooks')
        sequence = re.compile(
            r'const auto privateResetSubmission = FSRDCyberpunkFogProbe::AdmitPrivateResetSubmission\(This, NumCommandLists, ppCommandLists\);\s*'
            r'const auto fogSubmission = FSRDCyberpunkFogProbe::PreparingSubmission\(This, NumCommandLists, ppCommandLists\);\s*'
            r'const auto fsrdSubmission = FSRDSubmission::Preparing\(NumCommandLists, ppCommandLists\);\s*'
            r'o_ExecuteCommandLists\(This, NumCommandLists, ppCommandLists\);\s*'
            r'FSRDCyberpunkFogProbe::ReturnedPrivateResetSubmission\(privateResetSubmission\);')
        self.assertEqual(len(sequence.findall(execute)), 2)
        self.assertEqual(execute.count('o_ExecuteCommandLists('), 2)
        gate = body(HOST, 'uint64_t AdmitPrivateResetSubmission(', 'void ReturnedPrivateResetSubmission(')
        self.assertNotIn('Metadata([', gate)
        self.assertIn('catch (...) { PrivateResetFatal(); }', gate)
        self.assertIn('found->second.generation', gate)
        self.assertNotIn('recordings[i].generation = packet->', gate)

    def test_arming_is_an_explicit_transaction_before_any_capture_can_consume(self):
        arm = body(HOST, 'void ArmPrivateReset(', 'uint64_t AdmitPrivateResetSubmission(')
        self.assertLess(arm.index('privateResetArming.exchange(true'), arm.index('AllocateTargets('))
        self.assertLess(arm.index('privateResetArming.exchange(true'), arm.index('FSRDFogLayerCapture::Request()'))
        self.assertIn('~FinishArming() { privateResetArming.store(false, std::memory_order_release); }', arm)
        self.assertLess(arm.index('privateResetPacket.store(packet.release(), std::memory_order_release)'),
                        arm.index('lightingRequested.store(true)'))
        self.assertIn('"private_reset_only"', arm)
        self.assertIn('"explicit_reset_control_not_captured_duration"', arm)
        prepare = body(HOST, 'std::shared_ptr<CapturePlan> PrepareCapture(', 'void CaptureBoundCb12(')
        self.assertLess(prepare.index('privateResetArming.load'), prepare.index('WantsCapture()'))
        self.assertIn('privateResetArming.load(std::memory_order_acquire)', prepare.split('WantsCapture()', 1)[1])
        self.assertLess(prepare.rindex('privateResetArming.load'), prepare.index('captureStarted.exchange(true)'))
        draw = body(HOST, 'void WINAPI HookDraw(', 'void WINAPI HookDrawIndexed(')
        self.assertIn('!privateResetArming.load(std::memory_order_acquire)', draw)
        ray = body(HOST, 'std::shared_ptr<RayCopyBundle> PrepareRayCopy(', 'void FinishRayCopy(')
        self.assertIn('privateResetArming.load(std::memory_order_acquire)', ray)

    def test_owned_targets_and_consumer_obligation_precede_private_commands(self):
        record = body(HOST, 'void RecordPrivateReset(', 'std::shared_ptr<CapturePlan> PrepareCapture(')
        self.assertIn('conversion.Resources.InColor = plan.layers.before.resource.Get()', record)
        self.assertIn('conversion.Resources.InDepth = plan.layers.hardwareDepth.resource.Get()', record)
        self.assertIn('const auto& rays = packet->rays->Outputs()', record)
        self.assertIn('const auto& guides = packet->guides->Outputs()', record)
        self.assertLess(record.index('PrivateDenoise::Prepare('), record.index('SameFrame(repeated)'))
        self.assertLess(record.index('SameFrame(repeated)'), record.index('policy.EmbedConsumer('))
        self.assertLess(record.index('packet->denoise = work'), record.index('policy.EmbedConsumer('))
        self.assertLess(record.index('policy.EmbedConsumer('), record.index('Transition(list, plan.layers.before'))
        self.assertLess(record.index('plan.layers.before.state = readable'), record.index('work->Record(list)'))
        self.assertIn('if (!complete) FailPacket(packet)', record)
        self.assertIn('auto* packet = plan.packet ? plan.packet : privateResetPacket.load', record)
        self.assertIn('ScopeLostAfterMutation)', record)
        self.assertIn('PrivateResetFatal();', record)
        self.assertNotIn('originalDraw(', record)
        capture_plan = body(HOST, 'struct CapturePlan\n', '\n};')
        self.assertNotIn('PrivateDenoise::Work', capture_plan)
        self.assertIn('PrivateResetPacket* packet', capture_plan)
        self.assertNotIn('shared_ptr<PrivateResetPacket>', capture_plan)
        self.assertNotIn('unique_ptr<PrivateResetPacket>', capture_plan)
        feature = (BASE / 'FSRDFeature_Dx12.cpp').read_text()
        poll = body(feature, 'void FSRDFeatureDx12::PollPreFogExperiments()',
                    'bool FSRDFeatureDx12::EvaluatePreFogSrOnly(')
        self.assertRegex(poll, r'if \(_denoiser.IsCreated\(\)\)\s*FSRDCyberpunkFogProbe::ArmPrivateReset\(')
        self.assertEqual(feature.count('PollPreFogExperiments();'), 2)
        fixed_route = body(feature, 'bool FSRDFeatureDx12::EvaluatePreFogSrOnly(',
                           'bool FSRDFeatureDx12::Evaluate(')
        self.assertIn('PollPreFogExperiments();', fixed_route)
        self.assertLess(record.index('work->Record(list)'), record.index('RecordSceneReset(list, plan, *packet)'))
        self.assertLess(record.index('RecordSceneReset(list, plan, *packet)'), record.index('attempted.complete = true'))

    def test_request_reader_lifetime_ends_before_windows_delete(self):
        arm = body(HOST, 'void ArmPrivateReset(', 'uint64_t AdmitPrivateResetSubmission(')
        # CRT ifstream does not share DELETE access on Windows. Preserve the
        # actual lexical reader lifetime; successful JSON parsing alone cannot
        # make DeleteFileW succeed while that stream remains alive.
        reader = re.search(r'\{\s*(?://[^\n]*\n\s*)*std::ifstream file\(path, std::ios::binary\);\s*file >> controls;\s*\}', arm)
        self.assertIsNotNone(reader)
        self.assertLess(reader.end(), arm.index('DeleteFileW(path.c_str())'))
        self.assertIn('const auto error = GetLastError();', arm)

    def test_producer_and_consumer_are_sealed_only_after_restored_readbacks(self):
        lighting = body(HOST, 'void FinishLightingCapture(', 'bool HasBoundCb12Psos(')
        self.assertLess(lighting.index('FSRDSubmission::Retain('), lighting.index('RecordPrivateCompute('))
        self.assertLess(lighting.index('RecordEarlyGuides('), lighting.index('policy.SealProducer('))
        self.assertIn('seal.raySucceeded = pairedRayCopies', lighting)
        self.assertIn('seal.guidesRestored = result.bindingsRestored', lighting)
        self.assertIn('seal.rayTerminal = packet->rayTerminal', lighting)
        fog = body(HOST, 'void FinishCapture(', 'bool MatchesFinalLightingDraw(')
        disk = fog[fog.index('    CopyMain(list, *plan, plan->layers.after.resource.Get());'):]
        self.assertLess(disk.index('FSRDFogLayerCapture::Record('), disk.index('policy.SealConsumer('))
        self.assertIn('((packet->sceneResetOnce || packet->temporal) && !plan->sceneResetWritten)', disk)
        self.assertIn('if (plan->temporal && !plan->captureFinal)', fog[:fog.index('    CopyMain(')])
        draw = body(HOST, 'void WINAPI HookDraw(', 'void WINAPI HookDrawIndexed(')
        self.assertEqual(draw.count('originalDraw(list, count, instances, start, firstInstance);'), 1)
        self.assertLess(draw.index('PrepareCapture('), draw.index('originalDraw('))
        self.assertLess(draw.index('originalDraw('), draw.index('FinishCapture('))

    def test_actual_submission_adapter_valid_paths_and_fatal_refusals(self):
        compiler = os.environ.get('CXX') or shutil.which('c++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('Set CXX for the actual host submission-adapter compilation')
        implementation = HOST[HOST.index('uint64_t AdmitPrivateResetSubmission('):HOST.index('CaptureCandidate ObserveNgxInput(')]
        mocks = r'''
#include "FSRDCyberpunkPrivateResetPolicy.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
using UINT=unsigned;using HRESULT=int;
#define FAILED(x) ((x)<0)
#define IID_PPV_ARGS(x) (x)
#define LOG_ERROR(...) ((void)0)
#define LOG_INFO(...) ((void)0)
constexpr int D3D12_COMMAND_LIST_TYPE_DIRECT=0;
struct IUnknown{unsigned refs=0;bool fail=false;virtual ~IUnknown()=default;
 void AddRef(){++refs;}void Release(){assert(refs);--refs;}
 HRESULT QueryInterface(IUnknown** out){if(fail)return -1;*out=this;AddRef();return 0;}};
template<class T>struct ComPtr{T* value=nullptr;ComPtr()=default;ComPtr(const ComPtr& p):value(p.value){if(value)value->AddRef();}
 ~ComPtr(){if(value)value->Release();}T* Get()const{return value;}T** operator&(){assert(!value);return &value;}
 ComPtr& operator=(const ComPtr& p){if(p.value)p.value->AddRef();if(value)value->Release();value=p.value;return *this;}};
struct ID3D12CommandList:IUnknown{};
struct ID3D12CommandQueue:IUnknown{IUnknown* device=nullptr;bool deviceFail=false;int type=0;
 HRESULT GetDevice(IUnknown** out){if(deviceFail)return -1;*out=device;device->AddRef();return 0;}
 struct Desc{int Type;};Desc GetDesc(){return{type};}};
// This fixture preserves the ordinary one-shot path. Temporal routing is
// separately compiled against real Window policy in test_fsrd_temporal_host.
struct TemporalWindow{};std::atomic<TemporalWindow*> temporalWindow{nullptr};
constexpr uint64_t TemporalToken=1ull<<63;
bool replayAllowed=true;
bool AllowsRetiredSubmission(UINT,ID3D12CommandList* const*){return replayAllowed;}
// Simulate a guard being published while this call waits for old-root admission.
uint64_t AdmitTemporalSubmission(TemporalWindow&,ID3D12CommandQueue*,UINT,ID3D12CommandList* const*){
 replayAllowed=false;return TemporalToken|123;}
void ReturnedTemporalSubmission(TemporalWindow&,uint64_t){assert(false);}
namespace ResetPolicy=FSRD::CyberpunkPrivateResetPolicy;
struct PrivateResetPacket{std::mutex mutex;ResetPolicy::Policy policy;ComPtr<IUnknown> queueIdentity;
 explicit PrivateResetPacket(uintptr_t device):policy(device){}};
std::atomic<PrivateResetPacket*> privateResetPacket{nullptr};std::atomic<bool> captureTrackingValid{true};
struct ListState{bool known=true;uint64_t generation=0;};
struct Registry{std::mutex mutex;std::unordered_map<IUnknown*,ListState> lists;};
Registry& Data(){static Registry registry;return registry;}
[[noreturn]] void PrivateResetFatal() noexcept {std::_Exit(79);}
'''
        harness = r'''
int main(int argc,char** argv){
 assert(AdmitPrivateResetSubmission(nullptr,1,nullptr)==0);
 IUnknown device;ID3D12CommandList producer,consumer,other;ID3D12CommandQueue queue;queue.device=&device;
 PrivateResetPacket packet{uintptr_t(&device)};privateResetPacket.store(&packet);
 auto& policy=packet.policy;ResetPolicy::Recording p{uintptr_t(&producer),11},c{uintptr_t(&consumer),12};
 Data().lists[&producer]={true,11};Data().lists[&consumer]={true,12};
 assert(AdmitPrivateResetSubmission(nullptr,0,nullptr)==0);ReturnedPrivateResetSubmission(0);
 ID3D12CommandList* unrelated[]{&other};assert(AdmitPrivateResetSubmission(&queue,1,unrelated)==0);
 assert(policy.DeclareProducer(p));
 ResetPolicy::ProducerSeal seal{p,p,1,2,3,4,true,true,true,true,true,true,true,true};
 assert(policy.SealProducer(seal));assert(policy.EmbedConsumer(c,1,true));assert(policy.SealConsumer(c,2,true,true));
 const std::string mode=argc>1?argv[1]:"separate";ID3D12CommandList* both[]{&producer,&consumer};
 if(mode=="retired-gap"){privateResetPacket.store(nullptr);replayAllowed=false;AdmitPrivateResetSubmission(&queue,2,both);assert(false);}
 if(mode=="retired-transfer"){TemporalWindow old;temporalWindow.store(&old);AdmitPrivateResetSubmission(&queue,2,both);assert(false);}
 if(mode=="bad-order"){ID3D12CommandList* reverse[]{&consumer,&producer};AdmitPrivateResetSubmission(&queue,2,reverse);assert(false);}
 if(mode=="missing-producer"){ID3D12CommandList* one[]{&consumer};AdmitPrivateResetSubmission(&queue,1,one);assert(false);}
 if(mode=="unknown-reset"){captureTrackingValid=false;AdmitPrivateResetSubmission(&queue,2,both);assert(false);}
 if(mode=="wrong-queue"){queue.type=2;AdmitPrivateResetSubmission(&queue,2,both);assert(false);}
 if(mode=="lost-list"){Data().lists.erase(&consumer);AdmitPrivateResetSubmission(&queue,2,both);assert(false);}
 if(mode=="bad-return"){ReturnedPrivateResetSubmission(99);assert(false);}
 if(mode=="batch"){
  const auto token=AdmitPrivateResetSubmission(&queue,2,both);assert(token&&!policy.ConsumerReturned());
  ReturnedPrivateResetSubmission(token);assert(policy.ConsumerReturned()&&policy.ProducerReturned());
 }else{
  const auto producerToken=AdmitPrivateResetSubmission(&queue,1,both);assert(producerToken&&!policy.ProducerReturned());
  if(mode=="in-flight"){AdmitPrivateResetSubmission(&queue,1,both+1);assert(false);}
  ReturnedPrivateResetSubmission(producerToken);assert(policy.ProducerReturned());
  const auto consumerToken=AdmitPrivateResetSubmission(&queue,1,both+1);assert(consumerToken&&!policy.ConsumerReturned());
  ReturnedPrivateResetSubmission(consumerToken);assert(policy.ConsumerReturned());
 }
 assert(packet.queueIdentity.Get()==&queue);
 // Subsequent reset generations no longer contain the completed packet commands.
 Data().lists[&producer].generation=21;Data().lists[&consumer].generation=22;
 assert(AdmitPrivateResetSubmission(&queue,2,both)==0);
 privateResetPacket.store(nullptr);
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-private-reset-host-') as temporary:
            temp = Path(temporary)
            (temp / 'test.cpp').write_text(mocks + implementation + harness)
            for flags in (['-O0'], ['-O3', '-ffast-math']):
                result = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', *flags,
                                         '-I', str(BASE), str(temp / 'test.cpp'), '-o', str(temp / 'test')],
                                        text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for mode in ('separate', 'batch', 'bad-order', 'missing-producer', 'unknown-reset',
                             'wrong-queue', 'lost-list', 'bad-return', 'in-flight', 'retired-gap', 'retired-transfer'):
                    result = subprocess.run([str(temp / 'test'), mode], text=True, capture_output=True)
                    expected = 0 if mode in ('separate', 'batch') else 79
                    self.assertEqual(result.returncode, expected, mode + result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()

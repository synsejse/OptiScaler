"""Bounded visual-pass wiring and actual host watch retirement with CPU mocks.

Not native Reset/fence evidence or a GPU image-quality test.
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


class VisualTest(unittest.TestCase):
    def test_bounded_owners_final_fence_and_no_visual_disk_capture(self):
        window = section('struct TemporalWindow\n', 'auto& temporalWindow')
        self.assertIn('std::vector<std::shared_ptr<PrivateResetPacket>> frames', window)
        self.assertIn('std::array<Json, 32> ledger', window)
        retirement = section('void RetireVisualWatch(', 'void MaintainTemporalWindow(')
        self.assertNotRegex(retirement, r'window\.frames\.(clear|erase|resize|reset)\(')
        self.assertIn('if (window.continuous) slot.reset()', retirement)
        self.assertIn('std::erase(window.liveFrames, &frame)', retirement)
        self.assertIn('!frame.retired || (!frame.returned && !frame.unusedProducer)', retirement)
        self.assertIn('!window.returnEvidencePending', retirement)
        maintain = section('void MaintainTemporalWindow(', 'uint64_t AdmitTemporalSubmission(')
        self.assertLess(maintain.index('FSRDSubmission::Complete(ticket)'), maintain.index('frame->retired = true'))
        self.assertLess(maintain.index('frame->retired = true'), maintain.index('RetireVisualWatch(window, *frame)'))
        self.assertIn('frames.push_back(window.frames[frame->temporalKey.index % window.frames.size()])', maintain)
        self.assertIn('last_32_returned_frames_ring_not_full_history', maintain)
        policy = (BASE / 'FSRDCyberpunkTemporalWindowPolicy.h').read_text()
        self.assertIn('frameCount == 32 && index == 31', policy)
        self.assertIn('MaxVisualWatches = 64', policy)
        self.assertIn('_watches.size() == MaxVisualWatches', policy)
        poll = section('void PollTemporalWindow(', 'uint64_t AdmitPrivateResetSubmission(')
        self.assertIn('"visual_test_18000"', poll)
        self.assertIn('window->frames.resize(continuous ? WindowPolicy::Window::MaxVisualWatches : window->frameCount)', poll)
        self.assertIn('window->liveFrames.reserve(', poll)
        self.assertIn('window->epoch, window->frameCount', poll)

    def test_gui_reports_inactive_stopped_complete_and_requests_no_native_work(self):
        gui = (ROOT / 'OptiScaler/menu/menu_common.cpp').read_text()
        for text in ('Start pre-Fog visual test', 'Denoised frames: %u / %u',
                     'STOPPED - late SR only', 'Test finished - late SR only',
                     'ACTIVE - AMD denoising before original fog', 'Restart the game to test again'):
            self.assertIn(text, gui)
        request = section('bool RequestVisualTest(', 'void PollTemporalWindow(')
        self.assertIn('visualTestRequested.exchange(true)', request)
        for forbidden in ('CreateSession', 'Dispatch', 'Execute', 'Config::', 'WriteFile', 'DeleteFile'):
            self.assertNotIn(forbidden, request)
        returned = section('void ReturnedTemporalSubmission(', '} // namespace\n\nTemporalTestStatus')
        self.assertLess(returned.index('AcknowledgeExecuted'), returned.index('temporalLastReturn.store'))
        self.assertIn('returned.consumer.index % window.ledger.size()', returned)

    def test_actual_retirement_keeps_packets_and_requires_all_evidence(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX for actual visual host retirement checks')
        function = section('void RetireVisualWatch(', 'bool RetireUnrecordedTemporalFrame(')
        mocks = r'''
#include "FSRDCyberpunkTemporalWindowPolicy.h"
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <unordered_map>
struct IUnknown{};
namespace WindowPolicy=FSRD::CyberpunkTemporalWindowPolicy;
namespace ResetPolicy=FSRD::CyberpunkPrivateResetPolicy;
struct PrivateResetPacket{
 std::mutex mutex;bool retired=true,returned=true,unusedProducer=false;
 ResetPolicy::Recording producer{},consumer{};WindowPolicy::FrameKey temporalKey{};
};
struct TemporalWindow{
 std::mutex mutex;bool visual=true,continuous=false;unsigned returnEvidencePending=0,drainedFrames=0;
 std::unique_ptr<WindowPolicy::Window> policy;
 std::vector<std::shared_ptr<PrivateResetPacket>> frames;
 std::vector<PrivateResetPacket*> liveFrames;
};
std::atomic<bool> captureTrackingValid{true};
struct ListState{bool known=true;uint64_t generation=0;};
struct Registry{std::mutex mutex;std::unordered_map<IUnknown*,ListState> lists;};
Registry& Data(){static Registry value;return value;}
'''
        harness = r'''
int main(){
 for(unsigned failure=0;failure<13;++failure){
  TemporalWindow window;IUnknown producer,consumer;
  const ResetPolicy::Queue queue{1,2,true};
  window.policy=std::make_unique<WindowPolicy::Window>(7,queue,100,18000);
  auto owned=std::make_shared<PrivateResetPacket>();auto* frame=owned.get();
  frame->producer={uintptr_t(&producer),10};frame->consumer={uintptr_t(&consumer),11};
  auto& policy=*window.policy;
  const auto key=policy.ClaimRole(100,WindowPolicy::Role::Ray);frame->temporalKey=key;
  assert(policy.DeclareProducer(key,frame->producer));
  assert(policy.ClaimRole(100,WindowPolicy::Role::Guides)==key);
  assert(policy.SealProducer(key,{frame->producer,frame->producer,1,2,3,4,true,true,true,true,true,true,true,true}));
  assert(policy.ClaimRole(100,WindowPolicy::Role::Fog)==key);
  assert(policy.EmbedConsumer(key,frame->consumer,1,true));assert(policy.SealConsumer(key,frame->consumer,2,true,true));
  std::array<ResetPolicy::Recording,2> both{frame->producer,frame->consumer};
  const auto call=policy.BeforeExecute(queue,both);assert(call.allowed);
  if(failure!=11){assert(policy.AfterExecute(call.receipt).consumer==key);assert(policy.CommitConsumer(key));}
  window.frames.push_back(std::move(owned));window.liveFrames.push_back(frame);
  Data().lists.clear();Data().lists[&producer]={true,12};Data().lists[&consumer]={true,13};captureTrackingValid=true;
  switch(failure){
   case 1:frame->retired=false;break; // New Reset never replaces the final GPU fence.
   case 2:frame->returned=false;break;
   case 3:Data().lists[&producer].generation=10;break;
   case 4:Data().lists[&consumer].generation=11;break;
   case 5:Data().lists.erase(&producer);break;
   case 6:Data().lists[&consumer].known=false;break;
   case 7:captureTrackingValid=false;break;
   case 8:window.returnEvidencePending=1;break;
   case 9:window.visual=false;break;
   case 10:Data().lists[&producer].generation=0;break;
   case 12:++frame->temporalKey.epoch;break;
  }
  RetireVisualWatch(window,*frame);
  assert(window.frames.size()==1&&window.frames[0].get()==frame); // CPU pointer still alive.
  assert(window.liveFrames.size()==(failure?1u:0u));
  assert(policy.SubmissionWatches()==(failure?1u:0u));
  assert(window.drainedFrames==(failure?0u:1u));
  RetireVisualWatch(window,*frame);assert(window.drainedFrames==(failure?0u:1u));
 }
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-visual-retirement-') as directory:
            temp = Path(directory)
            (temp / 'test.cpp').write_text(mocks + function + harness)
            for flags in (['-O0'], ['-O3', '-ffast-math']):
                result = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags,
                                         '-I', str(BASE), str(temp / 'test.cpp'), '-o', str(temp / 'test')],
                                        text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(temp / 'test')], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()

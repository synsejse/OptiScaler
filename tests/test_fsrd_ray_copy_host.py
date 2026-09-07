"""Bounded original-ray copy and exact later-lighting companion admission."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'OptiScaler/upscalers/ffx/FSRDCyberpunkFogProbe.cpp').read_text()


def function(name):
    start = SOURCE.index(name + '(')
    opening = SOURCE.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[SOURCE.rfind('\n', 0, start) + 1:end]


class RayCopyHost(unittest.TestCase):
    def test_original_once_native_borrow_and_no_flattened_extent_guess(self):
        hook = function('HookDispatchRays')
        original = 'originalDispatchRays(list, description);'
        self.assertEqual(hook.count(original), 1)
        self.assertLess(hook.index('CyberpunkRayBindings::Observe('), hook.index('PrepareRayCopy('))
        self.assertLess(hook.index('PrepareRayCopy('), hook.index(original))
        self.assertLess(hook.index(original), hook.index('FinishRayCopy('))
        prepare = function('PrepareRayCopy')
        for required in ('rayCopyAttempted.exchange(true)', 'snapshot.scope.view, 0x34',
                         'snapshot.scope.view, 0x38', 'SameRayCopyScope(*plan)',
                         'list4Identity.Get() != plan->listIdentity.Get()',
                         'snapshot.textures[i ? 2 : 0].native', 'PrivateRayCopy::Prepare(',
                         'D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS'):
            self.assertIn(required, prepare)
        self.assertLess(prepare.index('SameRayCopyScope(*plan)'), prepare.index('plan->sources[i] ='))
        for forbidden in ('dispatch.Width !=', 'dispatch.Height !=', 'width = dispatch.Width',
                          'RecordCopy(', '->Record(', 'RequestState(', 'ResourceBarrier('):
            self.assertNotIn(forbidden, prepare)

    def test_current_scope_and_refcounts_not_accidentally_an_extra_tracker(self):
        same = function('SameRayCopyScope')
        for required in ('CurrentRayConstantScope() != expected.scope', 'plan.frameSourceObject',
                         'found->second.generation != expected.scope.recordingGeneration',
                         'found->second.predicated', 'found->second.renderPass', 'found->second.queryCount',
                         'rayScope->bindings[i].valid', 'receipt.phase !=',
                         'CyberpunkRayBindings::Detail::ReadCurrent(',
                         'current.textures[i].refs = expected.textures[i].refs', 'current != expected'):
            self.assertIn(required, same)
        self.assertNotIn('CyberpunkRayBindings::Observe(', same)
        self.assertNotIn('lightingAttempted.load()', same)
        host = SOURCE.split('struct RayCopyEngineHost\n', 1)[1].split('std::shared_ptr<RayCopyBundle> PrepareRayCopy', 1)[0]
        for required in ('plan.originalReturned', 'SameRayCopyScope(plan)',
                         'CyberpunkLightingSource::Code', 'ObserveRegisteredResidency(', '0x68'):
            self.assertIn(required, host)
        for forbidden in ('0x538', '0x528', 'Reenter(', 'RestorePso(', 'GetGPUVirtualAddress('):
            self.assertNotIn(forbidden, host)

    def test_restore_fatal_guard_and_private_companion_ownership(self):
        finish = function('FinishRayCopy')
        self.assertIn('CyberpunkRayAccess::RecordCopy(', finish)
        self.assertIn('plan->work->Record(', finish)
        fatal = finish.split('Outcome::ScopeLostAfterMutation', 1)[1].split('plan->recorded =', 1)[0]
        self.assertLess(fatal.index('earlyFatalRecording.store(true)'), fatal.index('LOG_ERROR('))
        for required in ('active.load() && captureEnabled.load()', 'authenticatedImage.load()',
                         'GetModuleHandleW(nullptr)', 'TerminateProcess(GetCurrentProcess()', 'RaiseFailFastException('):
            self.assertIn(required, fatal)
        self.assertIn('result.hitRestored && plan->work->Recorded()', finish)
        self.assertIn('if (plan->recorded) data.rayCopy = plan;', finish)
        attach = function('AttachRayCopies')
        for required in ('"CPU_value_present"', '"repeated_source_fields_equal"',
                         '"object_address"', '"source_value"', 'currentObject, 0x1b0',
                         'CurrentLightingConstantScope()', 'plan.rayCopy = std::move(candidate)',
                         'D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE'):
            self.assertIn(required, attach)
        lighting = function('FinishLightingCapture')
        self.assertIn('pairedRayCopies ? &rayCopies : nullptr', lighting)
        self.assertIn('plan->provenance.dump(), plan,', lighting)
        self.assertLess(lighting.index('AttachRayCopies('), lighting.index('RecordEarlyGuides('))

    def test_actual_recording_join_rejects_stale_or_unrecorded_pairs(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX to compile actual join')
        harness = r'''
#include <array>
#include <cstdint>
#include <memory>
#include <cassert>
struct Identity {uintptr_t address=7;uintptr_t Get()const{return address;}};
struct Work {bool good=true;bool Recorded()const{return good;}};
struct Scope {uintptr_t list=100,view=200;uint64_t serial=10,recordingGeneration=30;uint32_t frameSource=0;};
struct RayCopyBundle {
 struct {struct {Scope scope;} dispatch;} input;
 bool recorded=true;std::shared_ptr<Work> work=std::make_shared<Work>();Identity listIdentity;
 uint32_t width=1280,height=720;uintptr_t frameSourceObject=300;
};
struct LightingCapturePlan {
 uintptr_t list=100,view=200;uint64_t serial=11;Identity listIdentity;
 struct{uint64_t generation=30;} drawState;
};
'''
        harness += function('RayCopyRecordingMatches')
        harness += r'''
int main(){
 for(unsigned bad=0;bad<14;++bad){
  RayCopyBundle b;LightingCapturePlan p;uintptr_t object=300;uint32_t frame=0;
  std::array<uint32_t,2> dimensions{1280,720};
  switch(bad){case 0:break;case 1:b.recorded=false;break;case 2:b.work.reset();break;
  case 3:b.work->good=false;break;case 4:++p.list;break;case 5:++p.listIdentity.address;break;
  case 6:++p.drawState.generation;break;case 7:++p.view;break;case 8:p.serial=10;break;
  case 9:p.serial=9;break;case 10:++dimensions[0];break;case 11:++dimensions[1];break;
  case 12:++object;break;case 13:++frame;break;}
  assert(RayCopyRecordingMatches(b,p,object,frame,dimensions)==(bad==0));
 }
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-ray-copy-host-') as tmp:
            source = Path(tmp) / 'test.cpp'
            source.write_text(harness)
            for flags in (['-O0'], ['-O3', '-ffast-math']):
                binary = Path(tmp) / 'test'
                subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags,
                                str(source), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()

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
        self.assertNotIn('FinishRayCopy(', hook)
        self.assertLess(hook.index('current->pendingCopy = rayCopy'), hook.index(original))
        self.assertLess(hook.index(original), hook.index('pending->originalReturned = true'))
        self.assertIn('++pending->completedDispatches', hook)
        prepare = function('PrepareRayCopy')
        for required in ('!originalRayCleanup', 'rayCopyAttempted.exchange(true)', 'snapshot.scope.view, 0x34',
                         'snapshot.scope.view, 0x38', 'SameRayCopyScope(*plan)',
                         'list4Identity.Get() != plan->listIdentity.Get()',
                         'snapshot.textures[i ? 2 : 0].native', 'PrivateRayCopy::Prepare(',
                         'D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS', 'snapshot.scope.view, 0x1d70'):
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
                         'CyberpunkRayBindings::SameBindingIdentity(current, expected)'):
            self.assertIn(required, same)
        endpoint = same.split('if (plan.cleanupEntered)\n', 1)[1].split('const FSRD::CyberpunkRayBindings::TextureHandles', 1)[0]
        for required in ('rayScope->pendingCopy.get() != &plan', '!plan.originalReturned',
                         '!plan.completedDispatches', 'rayScope->nativeDispatchDepth',
                         'Detail::ReadTexture(', 'current.native != source.native',
                         'current.descriptor != source.descriptor', 'current.compact != source.compact'):
            self.assertIn(required, endpoint)
        for forbidden in ('ReadCurrent(', 'receipt.', 'expected.b6', 'expected.layout', 'expected.cache'):
            self.assertNotIn(forbidden, endpoint)
        self.assertIn('if (plan.cleanupEntered && i == 1) continue;', same)
        self.assertIn('owner != plan.rayOwner', same)
        self.assertIn('hitHandle != expected.textures[2].handle', same)
        self.assertNotIn('CyberpunkRayBindings::Observe(', same)
        self.assertNotIn('lightingAttempted.load()', same)
        host = SOURCE.split('struct RayCopyEngineHost\n', 1)[1].split('std::shared_ptr<RayCopyBundle> PrepareRayCopy', 1)[0]
        for required in ('plan.originalReturned', 'SameRayCopyScope(plan)',
                         'CyberpunkLightingSource::Code', 'ObserveRegisteredResidency(', '0x68'):
            self.assertIn(required, host)
        for forbidden in ('0x538', '0x528', 'Reenter(', 'RestorePso(', 'GetGPUVirtualAddress('):
            self.assertNotIn(forbidden, host)

    def test_exact_cleanup_entry_and_authentication_before_detour(self):
        cleanup = function('HookRayCleanup')
        original = 'originalRayCleanup(context, flags);'
        self.assertEqual(cleanup.count(original), 1)
        self.assertLess(cleanup.index('FinishRayCopy(plan)'), cleanup.index(original))
        self.assertLess(cleanup.index('earlyFatalRecording.load()'), cleanup.index(original))
        self.assertLess(cleanup.index(original), cleanup.index('current->pendingCopy.reset()'))
        for required in ('CyberpunkRayAccess::CleanupReturnRva', 'context == current->context',
                         'flags == 0', '!plan->cleanupConsumed', '!plan->invalidated',
                         'plan->originalReturned', '!current->nativeDispatchDepth',
                         'plan->cleanupConsumed = true', 'plan->cleanupEntered = false',
                         '"primary_b6_u0_layout_asserted_current", false'):
            self.assertIn(required, cleanup)
        init = function('Initialize')
        self.assertLess(init.index('MatchLiveCode(image, FSRD::CyberpunkRayAccess::CleanupCode)'),
                        init.index('DetourTransactionBegin()'))
        self.assertIn('DetourAttach(reinterpret_cast<PVOID*>(&originalRayCleanup), HookRayCleanup)', init)
        self.assertIn('originalRayCleanup = nullptr;', init.split('if (error != NO_ERROR)', 1)[1])
        node = function('HookRayNode')
        self.assertIn('parent->pendingCopy->invalidated = true', node)
        self.assertIn('original ray node returned without the admitted common cleanup endpoint', node)

    def test_actual_cleanup_hook_original_once_refusal_and_order(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX to compile actual cleanup hook')
        harness = r'''
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <json.hpp>
#include "FSRDCyberpunkRayAccess.h"
#ifndef __fastcall
#define __fastcall
#endif
using Json=nlohmann::json;
constexpr uintptr_t Image=0x140000000,Context=0x2000;
thread_local bool inMetadata=false;
std::atomic<bool> active{true},captureEnabled{true},rayBindingsAuthenticated{true};
std::atomic<bool> earlyFatalRecording{false};
std::atomic<uintptr_t> authenticatedImage{Image};
uintptr_t fakeCaller=Image+FSRD::CyberpunkRayAccess::CleanupReturnRva;
void* _ReturnAddress(){return reinterpret_cast<void*>(fakeCaller);}
struct RayCopyBundle {
 FSRD::CyberpunkRayAccess::Input input;
 bool originalReturned=true,cleanupEntered=false,cleanupConsumed=false,invalidated=false;
 unsigned completedDispatches=2;
 Json provenance;
};
struct RayScope {void* context=reinterpret_cast<void*>(Context);unsigned nativeDispatchDepth=0;
 std::shared_ptr<RayCopyBundle> pendingCopy;};
RayScope* rayScope=nullptr;
struct Registry {std::mutex mutex;Json rayCopyStatus;};
Registry registry;Registry& Data(){return registry;}
std::vector<char> events;bool skipMetadata=false,throwOriginal=false,fatalOnFinish=false;
template<class T>bool ReadEarlyAt(uintptr_t,uintptr_t,T& value){value=1;return true;}
template<class F>void Metadata(F&& f)noexcept{if(skipMetadata)return;try{f();}catch(...){}}
void FinishRayCopy(const std::shared_ptr<RayCopyBundle>& p){
 assert(p->cleanupEntered&&p->cleanupConsumed&&p->originalReturned&&!p->invalidated);
 assert(rayScope->pendingCopy==p);events.push_back('f');
 if(fatalOnFinish)earlyFatalRecording=true;
}
void originalRayCleanup(void*,uint8_t){events.push_back('o');if(throwOriginal)throw std::runtime_error("original");}
'''
        harness += function('HookRayCleanup')
        harness += r'''
int main(){
 for(unsigned bad=0;bad<16;++bad){
  events.clear();active=true;captureEnabled=true;rayBindingsAuthenticated=true;authenticatedImage=Image;
  fakeCaller=Image+FSRD::CyberpunkRayAccess::CleanupReturnRva;inMetadata=false;skipMetadata=false;
  RayScope s;rayScope=&s;s.pendingCopy=std::make_shared<RayCopyBundle>();auto p=s.pendingCopy;
  p->input.image=Image;p->input.dispatch.scope.graphContext=Context;
  void* context=s.context;uint8_t flags=0;
  switch(bad){case 0:break;case 1:++fakeCaller;break;case 2:context=nullptr;break;case 3:flags=1;break;
   case 4:p->invalidated=true;break;case 5:p->originalReturned=false;break;case 6:p->completedDispatches=0;break;
   case 7:s.nativeDispatchDepth=1;break;case 8:active=false;break;case 9:captureEnabled=false;break;
   case 10:rayBindingsAuthenticated=false;break;case 11:authenticatedImage=Image+1;break;
   case 12:inMetadata=true;break;case 13:p->cleanupConsumed=true;break;
   case 14:skipMetadata=true;break;case 15:p->input.dispatch.scope.graphContext++;break;}
  HookRayCleanup(context,flags);
  assert((events==(bad==0?std::vector<char>{'f','o'}:std::vector<char>{'o'})));
  assert(!s.pendingCopy&&!p->cleanupEntered&&p->cleanupConsumed);
  // A repeated callback cannot record a second copy from a completed receipt.
  HookRayCleanup(context,flags);assert(events.back()=='o');
 }
 // If guarded process termination cannot complete, do not resume native cleanup
 // with an unverified hit restoration. This is not an ordinary refusal.
 events.clear();active=true;captureEnabled=true;rayBindingsAuthenticated=true;authenticatedImage=Image;
 fakeCaller=Image+FSRD::CyberpunkRayAccess::CleanupReturnRva;inMetadata=false;skipMetadata=false;
 RayScope s;rayScope=&s;s.pendingCopy=std::make_shared<RayCopyBundle>();
 s.pendingCopy->input.image=Image;s.pendingCopy->input.dispatch.scope.graphContext=Context;
 fatalOnFinish=true;HookRayCleanup(s.context,0);
 assert((events==std::vector<char>{'f'})&&s.pendingCopy&&!s.pendingCopy->cleanupEntered);
 fatalOnFinish=false;earlyFatalRecording=false;
 // Original exceptions are not swallowed/replayed by the diagnostic wrapper.
 events.clear();rayScope=nullptr;throwOriginal=true;bool caught=false;
 try{HookRayCleanup(reinterpret_cast<void*>(Context),0);}catch(...){caught=true;}
 assert(caught&&(events==std::vector<char>{'o'}));
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-ray-cleanup-host-') as tmp:
            source = Path(tmp) / 'test.cpp'
            source.write_text(harness)
            for flags in (['-O0'], ['-O3', '-ffast-math']):
                binary = Path(tmp) / 'test'
                subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags,
                                '-I', str(ROOT / 'OptiScaler/upscalers/ffx'),
                                '-I', str(ROOT / 'external/nlohmann'),
                                str(source), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)

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

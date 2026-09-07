"""Original Fog hardware-depth copy: host source guards, ordering and ownership."""
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


def compiled(test, harness):
    compiler = os.environ.get('CXX') or shutil.which('c++')
    if not compiler:
        test.skipTest('Set CXX to compile actual Fog depth host bodies')
    with tempfile.TemporaryDirectory(prefix='fsrd-fog-depth-host-') as tmp:
        source = Path(tmp) / 'test.cpp'
        source.write_text(harness)
        for flags in (['-O0'], ['-O3', '-ffast-math']):
            binary = Path(tmp) / 'test'
            subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags,
                            '-I', str(ROOT / 'OptiScaler/upscalers/ffx'),
                            '-I', str(ROOT / 'external/nlohmann'),
                            str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


class FogDepthHost(unittest.TestCase):
    def test_original_scope_and_pre_detour_authentication(self):
        helper = function('HookFullscreenHelper')
        self.assertEqual(helper.count('originalFullscreenHelper(renderer, shader, flag);'), 1)
        self.assertIn('CyberpunkFogDepth::FullscreenReturnRva', helper)
        self.assertIn('~RestoreFog()', helper)
        binder = function('HookBindTextures')
        self.assertEqual(binder.count('originalBindTextures(first, count, handles, stage);'), 1)
        self.assertLess(binder.index('CyberpunkFogDepth::IsDepthBind('), binder.index('originalBindTextures('))
        self.assertLess(binder.index('originalBindTextures('), binder.index('if (fogAdmitted && fog == scope)'))
        self.assertIn('fog->depthBindCalls == 1', binder)
        self.assertIn('fogDepth == repeated && fogDepth && fogDepth <= 0x8000', binder)
        init = function('Initialize')
        self.assertLess(init.index('std::begin(FSRD::CyberpunkFogDepth::Code)'), init.index('DetourTransactionBegin()'))

    def test_original_native_format_and_owning_plan_before_commands(self):
        prepare = function('PrepareFogDepth')
        for gate in ('CyberpunkFogDepth::Observe(', 'DXGI_FORMAT_R32_FLOAT', 'desc.MipLevels != 1',
                     'desc.SampleDesc.Count != 1', 'desc.DepthOrArraySize != 1',
                     'sourceDeviceId.Get() != targetDeviceId.Get()',
                     'holder_end_event_position', 'plan.retainedTextureBytes',
                     'D3D12_RESOURCE_STATE_COPY_DEST'):
            self.assertIn(gate, prepare)
        self.assertIn('sourceId.Get() == mainId.Get()', prepare)
        self.assertIn('plan.retainedTextureBytes += sourceBytes + outputBytes', prepare)
        refusal = prepare.split('catch (const std::exception& error)', 1)[1]
        for required in ('plan.depthPrepared = false', 'plan.depthSource = {}', 'plan.depthOutput.Reset()'):
            self.assertIn(required, refusal)
        self.assertLess(prepare.index('CyberpunkFogDepth::Observe('), prepare.index('plan.depthSource.resource ='))
        self.assertLess(prepare.index('plan.depthSource.resource ='), prepare.index('resource->GetDesc()'))
        capture = function('PrepareCapture')
        positions = [capture.index(x) for x in ('PrepareFogDepth(*plan, nativeCaller)',
                     'FSRDSubmission::Retain(plan->device.Get(), list, plan)', 'CopyMain(list, *plan', 'RecordFogDepth(list, *plan)')]
        self.assertEqual(positions, sorted(positions))
        record = function('RecordFogDepth')
        self.assertIn('input.copySource = true', record)
        self.assertEqual(record.count('CopyTextureRegion('), 1)
        self.assertIn('Transition(list, plan.depthOutput.Get()', record)
        for forbidden in ('Transition(list, plan.depthSource', 'CopyDescriptors', 'Dispatch(', 'Shader',
                          'rayCopy', 'earlyWork', 'lightingCaptureSource'):
            self.assertNotIn(forbidden, record)
        hook = function('HookDraw')
        self.assertEqual(hook.count('originalDraw(list, count, instances, start, firstInstance);'), 1)
        self.assertLess(hook.index('earlyFatalRecording.load()'), hook.index('originalDraw('))

    def test_actual_current_source_rejects_stale_scope_frame_handles_and_reset(self):
        harness = r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "FSRDCyberpunkFogDepth.h"
struct Ptr {uintptr_t value=0;uintptr_t Get()const{return value;}explicit operator bool()const{return value!=0;}};
struct Scope {bool fogHelper=true,depthBindObserved=true,hasDsv=false;unsigned depthBindCalls=1,rtvCount=1;
 uint32_t depthHandle=1;uint64_t serial=11;void* context=reinterpret_cast<void*>(0x1000);
 uintptr_t psoList=0x5000,pso=0x6000;};
Scope* scope=nullptr;
std::atomic<bool> captureTrackingValid{true},earlyHeapTrackingValid{true};
struct CapturePlan {
 FSRD::CyberpunkFogDepth::Snapshot depthSnapshot;
 struct {Ptr resource,heap;} depthSource;
 Ptr main{0x9000};uintptr_t depthFrameObject=0x7000;uint32_t depthFrame=88;
 struct Endpoint{Ptr list{0x5000};};std::shared_ptr<Endpoint> endpoint=std::make_shared<Endpoint>();
};
struct List {bool known=true,predicated=false,renderPass=false;unsigned queryCount=0;uint64_t generation=12;};
struct Registry {std::mutex mutex;std::map<uintptr_t,List> lists;};
Registry registry;Registry& Data(){return registry;}
std::map<uintptr_t,std::vector<unsigned char>> memory;
template<class T>void Put(uintptr_t p,const T& value){auto& b=memory[p];b.resize(sizeof(value));std::memcpy(b.data(),&value,sizeof(value));}
template<class T>bool ReadEarly(uintptr_t p,T& value){if(!memory.contains(p)||memory[p].size()!=sizeof(value))return false;
 std::memcpy(&value,memory[p].data(),sizeof(value));return true;}
template<class T>bool ReadEarlyAt(uintptr_t p,uintptr_t offset,T& value){return ReadEarly(p+offset,value);}
uintptr_t __readgsqword(unsigned){return 0x8000;}
'''
        harness += function('SameFogDepthSource')
        harness += r'''
int main(){
 for(unsigned bad=0;bad<36;++bad){
  memory.clear();captureTrackingValid=true;earlyHeapTrackingValid=true;
  Scope s;scope=&s;CapturePlan p;auto& d=p.depthSnapshot;
  d.scope={11,12,0x1000,0x2000,0x3000,0x4000,0x5000,0x6000};
  d.handle=1;d.native=0xa000;d.slot=0xb000;d.descriptor=0xc000;d.descriptorArray=0xd000;d.descriptorIndex=7;
  d.cache=0xe000;d.layout=0xf000;
  p.depthSource.resource.value=d.native;p.depthSource.heap.value=0x10000;
  Put(uintptr_t(0x1000+0x18),d.scope.view);Put(uintptr_t(0x1000),p.depthFrameObject);
  Put(p.depthFrameObject+0x1b0,p.depthFrame);Put(d.slot-8,int32_t(5));Put(d.slot,d.native);
  Put(d.slot+0x30,d.descriptor);Put(d.slot+0x4e,d.compact);Put(d.descriptorArray+7*8,d.descriptor);
  Put(uintptr_t(0x8000),d.scope.tls);Put(d.scope.tls+0x14,uint8_t(1));Put(d.scope.tls+0x188,d.scope.engine);
  Put(d.scope.engine+0x30,d.scope.list);Put(d.scope.engine+0x60,d.cache);Put(d.cache+0x68,d.layout);
  Put(d.scope.engine+0x3d0,d.scope.pso);
  Put(d.cache+0x28,d.descriptorArray);Put(d.scope.graphContext+0x30,uint8_t(2));
  registry.lists.clear();registry.lists[0x5000]={};
  switch(bad){case 0:break;case 1:scope=nullptr;break;case 2:s.fogHelper=false;break;
   case 3:s.depthBindObserved=false;break;case 4:++s.depthBindCalls;break;case 5:++s.depthHandle;break;
   case 6:++s.serial;break;case 7:s.context=nullptr;break;case 8:s.hasDsv=true;break;case 9:s.rtvCount=2;break;
   case 10:++s.psoList;break;case 11:++s.pso;break;case 12:captureTrackingValid=false;break;
   case 13:earlyHeapTrackingValid=false;break;case 14:p.depthSource.resource.value=0;break;
   case 15:p.depthSource.heap.value=0;break;case 16:p.main.value=d.native;break;
   case 17:Put(d.scope.graphContext+0x18,uintptr_t(9));break;case 18:Put(p.depthFrameObject+0x1b0,uint32_t(89));break;
   case 19:Put(d.slot-8,int32_t(0));break;case 20:Put(d.slot,uintptr_t(9));break;
   case 21:Put(d.slot+0x30,uintptr_t(9));break;case 22:Put(d.descriptorArray+7*8,uintptr_t(9));break;
   case 23:++registry.lists[0x5000].generation;break;case 24:registry.lists[0x5000].predicated=true;break;
   case 25:registry.lists[0x5000].renderPass=true;break;case 26:registry.lists[0x5000].queryCount=1;break;
   case 27:Put(uintptr_t(0x8000),uintptr_t(9));break;case 28:Put(d.scope.tls+0x14,uint8_t(0));break;
   case 29:Put(d.scope.tls+0x188,uintptr_t(9));break;case 30:Put(d.scope.engine+0x30,uintptr_t(9));break;
   case 31:Put(d.scope.engine+0x3d0,uintptr_t(9));break;case 32:Put(d.scope.engine+0x60,uintptr_t(9));break;
   case 33:Put(d.cache+0x68,uintptr_t(9));break;case 34:Put(d.cache+0x28,uintptr_t(9));break;
   case 35:Put(d.scope.graphContext+0x30,uint8_t(0));break;}
  assert(SameFogDepthSource(p)==(bad==0));
  if(bad==0){Put(d.slot-8,int32_t(999));assert(SameFogDepthSource(p));}
 }
}
'''
        compiled(self, harness)

    def test_actual_copy_wrapper_private_output_publication_and_fatal_latch(self):
        harness = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>
#include <json.hpp>
#include "FSRDCyberpunkFogDepth.h"
using Json=nlohmann::json;
struct Ptr{uintptr_t value=0;uintptr_t Get()const{return value;}};
constexpr unsigned D3D12_RESOURCE_STATE_COPY_DEST=0x400,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE=0x40,
 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE=0x80,DXGI_FORMAT_R32_FLOAT=41;
struct CD3DX12_TEXTURE_COPY_LOCATION{uintptr_t resource;unsigned subresource;
 CD3DX12_TEXTURE_COPY_LOCATION(uintptr_t r,unsigned s):resource(r),subresource(s){}};
std::vector<char> events;
struct ID3D12GraphicsCommandList {
 void CopyTextureRegion(const CD3DX12_TEXTURE_COPY_LOCATION* dst,unsigned x,unsigned y,unsigned z,
                        const CD3DX12_TEXTURE_COPY_LOCATION* src,const void* box){
  assert(dst->resource==0xb000&&src->resource==0xa000&&!x&&!y&&!z&&!box&&!dst->subresource&&!src->subresource);
  events.push_back('c');
 }
};
void Transition(ID3D12GraphicsCommandList*,uintptr_t r,unsigned before,unsigned after){
 assert(r==0xb000&&before==0x400&&after==0xc0);events.push_back('t');}
struct CapturePlan {
 bool depthPrepared=true,fatalEarlyRecording=false;Ptr originalPso{0x6000};
 FSRD::CyberpunkFogDepth::Snapshot depthSnapshot;
 struct{Ptr resource{0xa000};}depthSource;Ptr depthOutput{0xb000};
 struct{struct{Ptr resource;unsigned viewFormat=0,state=0;}hardwareDepth;}layers;
 Json provenance;
};
struct FogDepthEngineHost{const CapturePlan& plan;};
std::atomic<uintptr_t> authenticatedImage{0x140000000};
std::atomic<bool> active{true},captureEnabled{true},earlyFatalRecording{false};
void* GetModuleHandleW(const void*){return reinterpret_cast<void*>(uintptr_t(authenticatedImage));}
unsigned terminated=0;int GetCurrentProcess(){return 4;}
void TerminateProcess(int process,uint32_t code){assert(process==4&&code==0xf51d0001);++terminated;events.push_back('k');}
void RaiseFailFastException(const void*,const void*,unsigned){events.push_back('!');}
namespace FSRD::CyberpunkFogDenoiseAccess {
struct Input{uintptr_t image=0,list=0,originalPso=0;uint64_t originalFogScope=0;
 CyberpunkEngineAccess::TextureBorrow depth;bool copySource=false;};
enum class Outcome{Refused,PrivateFailedRestored,PrivateRecordedRestored,ScopeLostAfterMutation};
struct Result{Outcome outcome=Outcome::Refused;unsigned requestsIssued=0;bool bindingsRestored=false;};
unsigned mode=0,calls=0;
template<class H,class F>Result RecordPrivateCompute(H&,const Input& in,F&& fn){
 ++calls;assert(in.image==0x140000000&&in.originalPso==0x6000&&in.originalFogScope==11);
 assert(in.depth.handle==1&&in.depth.native==0xa000&&in.copySource);
 if(mode==1)return {};
 if(mode==3)return {Outcome::ScopeLostAfterMutation,1,false};
 assert(fn());return {mode==2?Outcome::PrivateFailedRestored:Outcome::PrivateRecordedRestored,1,true};
}
}
'''
        harness += function('RecordFogDepth')
        harness += r'''
int main(){
 namespace F=FSRD::CyberpunkFogDenoiseAccess;
 for(unsigned mode=0;mode<5;++mode){
  events.clear();earlyFatalRecording=false;terminated=0;F::calls=0;F::mode=mode;
  CapturePlan p;p.depthSnapshot.scope.serial=11;p.depthSnapshot.handle=1;p.depthSnapshot.native=0xa000;
  p.depthPrepared=mode!=4;ID3D12GraphicsCommandList list;RecordFogDepth(&list,p);
  if(mode==4){assert(!F::calls&&events.empty());continue;}
  assert(F::calls==1);
  if(mode==0){assert((events==std::vector<char>{'c','t'}));assert(p.layers.hardwareDepth.resource.value==0xb000);
   assert(p.layers.hardwareDepth.viewFormat==41&&p.layers.hardwareDepth.state==0xc0);}
  else assert(!p.layers.hardwareDepth.resource.value);
  if(mode==3){assert(earlyFatalRecording&&p.fatalEarlyRecording&&terminated==1);assert((events==std::vector<char>{'k','!'}));}
  else assert(!earlyFatalRecording&&!p.fatalEarlyRecording&&!terminated);
 }
}
'''
        compiled(self, harness)


if __name__ == '__main__':
    unittest.main()

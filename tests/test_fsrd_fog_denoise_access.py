"""Actual one-native-depth/private-input Fog protocol, compiled without D3D mocks."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'OptiScaler/upscalers/ffx/FSRDCyberpunkFogDenoiseAccess.h'


class FogDenoiseAccess(unittest.TestCase):
    def test_one_depth_no_fabricated_four_inputs_or_state_tracker(self):
        text = HEADER.read_text()
        for forbidden in ('input.textures', 'Engine::Input ', 'ResourceBarrier(',
                          'GetDesc(', 'AddRef(', 'RequestBufferState(', '0x538', '0x528',
                          'Config::', 'State::', 'ExecuteCommandLists', 'WaitFor'):
            self.assertNotIn(forbidden, text)
        for required in ('Engine::TextureBorrow depth', 'Engine::Detail::ReadTexture',
                         'IsAdmittedFogDenoiseScope(input)', '0x1a8e988',
                         'bool copySource = false', 'CopySourceState = 0x800',
                         'Engine::InputReadState | (input.copySource ? CopySourceState : 0u)',
                         'ScopeLostAfterMutation', 'Engine::BufferCode[1]',
                         'original Fog draw exactly once', 'No source undo barrier'):
            self.assertIn(required, text)
        self.assertEqual(text.count('host.RequestState(input.image'), 1)
        self.assertEqual(text.count('host.Flush(input.image'), 1)
        self.assertEqual(text.count('host.Reenter(input.image'), 1)
        self.assertEqual(text.count('host.RestorePso(input.list'), 1)
        self.assertEqual(text.count('host.FlushGraphicsTables(input.image'), 1)
        record = text.split('Result RecordWork', 1)[1].split('} // namespace Detail', 1)[0]
        self.assertLess(record.index('Detail::Prepare'), record.index('host.RequestState(input.image'))
        self.assertLess(record.index('catch (...)'), record.index('host.Reenter(input.image'))
        self.assertLess(record.index('host.Reenter(input.image'), record.index('host.RestorePso(input.list'))
        self.assertLess(record.index('host.RestorePso(input.list'), record.index('host.FlushGraphicsTables(input.image'))
        self.assertLess(record.index('host.FlushGraphicsTables(input.image'), record.index('result.bindingsRestored = true'))

    def test_actual_protocol_O0_O3_fast_refusals_restoration_and_scope_loss(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX to compile the actual Fog denoise protocol')
        harness = r'''
#include "FSRDCyberpunkFogDenoiseAccess.h"
#include <cassert>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>
namespace F=FSRD::CyberpunkFogDenoiseAccess;
namespace E=FSRD::CyberpunkEngineAccess;
constexpr uintptr_t Image=0x140000000,Tls=0x1000,Engine=0x2000,Cache=0x3000,
 List=0x4000,Pso=0x5000,Registry=0x10000000,Depth=0x6000;
constexpr uintptr_t Slot=Registry+0x2f1d8;
bool copyMode=false;
struct Host {
 std::map<uintptr_t,std::vector<unsigned char>> memory;
 std::vector<char> events;
 uint32_t thread=33;uintptr_t tls=Tls,currentList=List,nativePso=Pso;
 uint32_t expectedReadState=0xc0;
 bool image=true,scope=true,direct=true,readable=true,throwRead=false,resident=false;
 bool actualDepth=true,bundle=true,camera=true,reset=true,noAliases=true,noPredicate=true;
 bool retainKind3=false,loseOnGetter=false,cacheDirty=false,repairTables=true,originalTableReceipt=true;
 unsigned codeChecks=0,badCode=99,operations=0,loseAfter=0,getters=0,residencyChecks=0;
 template<class T>void Put(uintptr_t a,const T& value){
  auto& bytes=memory[a];bytes.resize(sizeof(value));std::memcpy(bytes.data(),&value,sizeof(value));
 }
 bool Read(uintptr_t a,void* out,size_t n){
  if(throwRead)throw std::runtime_error("read");
  if(!readable||!memory.contains(a)||memory[a].size()!=n)return false;
  std::memcpy(out,memory[a].data(),n);return true;
 }
 uint32_t ThreadId()noexcept{return thread;}
 bool ReadTlsSlotZero(uintptr_t& out)noexcept{out=tls;return out!=0;}
 bool ExactImageAuthenticated(uintptr_t a,uintptr_t bytes,uint32_t stamp,std::string_view sha)noexcept{
  assert(a==Image&&bytes==E::ImageBytes&&stamp==E::ImageTimestamp&&sha==E::ExeSha256);return image;
 }
 bool LiveCodeMatches(uintptr_t a,const E::CodeRange& code)noexcept{
  assert(a==Image&&codeChecks<F::Code.size());
  assert(code.rva==F::Code[codeChecks].rva&&code.bytes==F::Code[codeChecks].bytes&&code.sha256==F::Code[codeChecks].sha256);
  return codeChecks++!=badCode;
 }
 bool IsAdmittedFogDenoiseScope(const F::Input& input)noexcept{
  assert(input.originalFogScope==77&&input.depth.handle==1&&input.depth.native==Depth);
  // Original-use receipt gate, not a check that private compute left nativePSO alone.
  return scope&&actualDepth&&bundle&&camera&&reset&&noAliases&&noPredicate;
 }
 bool ListIsDirect(uintptr_t list)noexcept{assert(list==List);return direct;}
 uintptr_t CurrentNativeList(uintptr_t entry)noexcept{
  assert(entry==Image+E::GetCurrentListRva);++getters;if(loseOnGetter)scope=false;return currentList;
 }
 bool IsTextureResidencyAdmitted(uintptr_t registry,const E::TextureBorrow& depth)noexcept{
  assert(registry==Registry&&depth.handle==1&&depth.native==Depth);++residencyChecks;return resident;
 }
 void Step()noexcept{if(++operations==loseAfter)scope=false;}
 void RequestState(uintptr_t entry,uintptr_t context,uint32_t handle,uint32_t state,uint32_t subresource)noexcept{
  assert(entry==Image+E::RequestStateRva&&context==Engine&&handle==1&&state==expectedReadState&&subresource==0xffffffff);
  events.push_back('d');
  if(retainKind3){auto& bytes=memory[Slot-8];int32_t refs;std::memcpy(&refs,bytes.data(),4);
   ++refs;std::memcpy(bytes.data(),&refs,4);}
  Step();
 }
 void Flush(uintptr_t entry,uintptr_t context)noexcept{
  assert(entry==Image+E::FlushRva&&context==Engine);events.push_back('f');Step();
 }
 void Reenter(uintptr_t entry,uintptr_t list)noexcept{
  assert(entry==Image+E::ReenterRva&&list==List);events.push_back('r');cacheDirty=true;Step();
 }
 void RestorePso(uintptr_t list,uintptr_t pso)noexcept{
  assert(list==List&&pso==Pso);events.push_back('p');nativePso=pso;Step();
 }
 void FlushGraphicsTables(uintptr_t entry,uintptr_t cache,uintptr_t context)noexcept{
  assert(entry==Image+0x1f22e4&&cache==Cache&&context==Engine);
  assert(cacheDirty&&nativePso==Pso);events.push_back('g');if(repairTables)cacheDirty=false;Step();
 }
 bool OriginalFogDepthTableRestored(uintptr_t cache,uintptr_t context)noexcept{
  assert(cache==Cache&&context==Engine);return originalTableReceipt&&!cacheDirty;
 }
};
F::Input Setup(Host& h){
 h=Host{};F::Input input{Image,List,Pso,77,{1,Depth}};
 input.copySource=copyMode;h.expectedReadState=copyMode?0x8c0:0xc0;
 h.Put(Tls+0x14,uint8_t(1));h.Put(Tls+0x188,Engine);
 h.Put(Engine+0x30,List);h.Put(Engine+0x60,Cache);h.Put(Engine+0x68,uint32_t(0));
 h.Put(Engine+0x3d0,Pso);h.Put(Image+E::RegistryRva,Registry);
 h.Put(Registry+0x1a8e988,uint8_t(0));h.Put(Slot-8,int32_t(5));h.Put(Slot,Depth);
 h.Put(Slot+0x56,uint8_t(0));h.Put(Slot+0x68,uintptr_t(0));return input;
}
// An old adapter without residency proof still compiles, but cannot admit a
// nonzero underlying resource merely because that address equals native Depth.
struct LegacyHost {
 Host& h;
 bool Read(uintptr_t a,void* p,size_t n){return h.Read(a,p,n);}
 uint32_t ThreadId()noexcept{return h.ThreadId();}
 bool ReadTlsSlotZero(uintptr_t& p)noexcept{return h.ReadTlsSlotZero(p);}
 bool ExactImageAuthenticated(uintptr_t a,uintptr_t n,uint32_t t,std::string_view s)noexcept{return h.ExactImageAuthenticated(a,n,t,s);}
 bool LiveCodeMatches(uintptr_t a,const E::CodeRange& c)noexcept{return h.LiveCodeMatches(a,c);}
 bool IsAdmittedFogDenoiseScope(const F::Input& i)noexcept{return h.IsAdmittedFogDenoiseScope(i);}
 bool ListIsDirect(uintptr_t l)noexcept{return h.ListIsDirect(l);}
 uintptr_t CurrentNativeList(uintptr_t a)noexcept{return h.CurrentNativeList(a);}
 void RequestState(uintptr_t a,uintptr_t c,uint32_t hnd,uint32_t s,uint32_t r)noexcept{h.RequestState(a,c,hnd,s,r);}
 void Flush(uintptr_t a,uintptr_t c)noexcept{h.Flush(a,c);}
 void Reenter(uintptr_t a,uintptr_t l)noexcept{h.Reenter(a,l);}
 void RestorePso(uintptr_t l,uintptr_t p)noexcept{h.RestorePso(l,p);}
 void FlushGraphicsTables(uintptr_t a,uintptr_t c,uintptr_t e)noexcept{h.FlushGraphicsTables(a,c,e);}
 bool OriginalFogDepthTableRestored(uintptr_t c,uintptr_t e)noexcept{return h.OriginalFogDepthTableRestored(c,e);}
};
int main(){
 assert(!F::Input{}.copySource);
 // Run every success/refusal/false/throw/fatal case in both modes. Only the
 // native depth read mask changes; all ownership and restoration gates remain.
 for(bool mode:{false,true}){copyMode=mode;
 Host h;auto input=Setup(h);
 auto work=[&]{h.events.push_back('c');h.nativePso=0xabcd;h.Step();return true;};
 auto r=F::RecordPrivateCompute(h,input,work);
 assert(r.outcome==F::Outcome::PrivateRecordedRestored&&r.requestsIssued==1&&r.callbackEntered&&r.bindingsRestored);
 assert(h.getters==1&&h.codeChecks==14&&h.nativePso==Pso&&!h.cacheDirty);
 assert((h.events==std::vector<char>{'d','f','c','r','p','g'}));
 for(bool throws:{false,true}){
  input=Setup(h);r=F::RecordPrivateCompute(h,input,[&]()->bool{
   h.events.push_back('c');h.nativePso=0xabcd;if(throws)throw std::runtime_error("partial");return false;});
  assert(r.outcome==F::Outcome::PrivateFailedRestored&&r.callbackEntered&&r.bindingsRestored&&h.nativePso==Pso);
  assert(!h.cacheDirty);
  assert((h.events==std::vector<char>{'d','f','c','r','p','g'}));
 }
 // A native call returning is not a binding receipt. Reject both missing
 // dynamic-table repair and a changed original pixel-t0 descriptor receipt.
 for(bool missingRepair:{false,true}){
  input=Setup(h);h.repairTables=!missingRepair;h.originalTableReceipt=missingRepair;
  r=F::RecordPrivateCompute(h,input,work);
  assert(r.outcome==F::Outcome::ScopeLostAfterMutation&&!r.bindingsRestored&&r.callbackEntered);
  assert((h.events==std::vector<char>{'d','f','c','r','p','g'}));
 }
 for(unsigned bad=0;bad<34;++bad){
  input=Setup(h);
  switch(bad){case 0:input.image=0;break;case 1:input.image=UINTPTR_MAX;break;
  case 2:input.list=0;break;case 3:input.originalPso=0;break;case 4:input.originalFogScope=0;break;
  case 5:input.depth.handle=0;break;case 6:input.depth.handle=0x8001;break;case 7:input.depth.native=0;break;
  case 8:h.image=false;break;case 9:h.badCode=7;break;case 10:h.scope=false;break;
  case 11:h.actualDepth=false;break;case 12:h.bundle=false;break;case 13:h.camera=false;break;
  case 14:h.reset=false;break;case 15:h.noAliases=false;break;case 16:h.noPredicate=false;break;
  case 17:h.thread=0;break;case 18:h.tls=0;break;case 19:h.Put(Tls+0x14,uint8_t(0));break;
  case 20:h.Put(Tls+0x188,uintptr_t(0));break;case 21:h.Put(Engine+0x60,uintptr_t(0));break;
  case 22:h.Put(Engine+0x30,uintptr_t(7));break;case 23:h.Put(Engine+0x3d0,uintptr_t(7));break;
  case 24:h.Put(Slot,uintptr_t(7));break;case 25:h.Put(Slot-8,int32_t(0));break;
  case 26:h.Put(Slot+0x56,uint8_t(0x40));break;case 27:h.Put(Slot+0x68,Depth);break;
  case 28:h.Put(Engine+0x68,uint32_t(4));h.Put(Registry+0x1a8e988,uint8_t(1));break;
  case 29:h.readable=false;break;case 30:h.throwRead=true;break;case 31:h.direct=false;break;
  case 32:h.currentList=7;break;case 33:h.loseOnGetter=true;break;
  }
  r=F::RecordPrivateCompute(h,input,work);
  assert(r.outcome==F::Outcome::Refused&&!r.requestsIssued&&!r.callbackEntered&&!r.bindingsRestored&&h.events.empty());
  if(bad==19)assert(h.getters==0);
 }
 // All authenticated bodies are checked before any mutation.
 for(unsigned bad=0;bad<F::Code.size();++bad){input=Setup(h);h.badCode=bad;
  r=F::RecordPrivateCompute(h,input,work);assert(r.outcome==F::Outcome::Refused&&h.events.empty()&&h.getters==0);}
 input=Setup(h);h.Put(Engine+0x68,uint32_t(4));r=F::RecordPrivateCompute(h,input,work);assert(r.bindingsRestored);
 input=Setup(h);h.Put(Engine+0x68,uint32_t(3));h.retainKind3=true;
 r=F::RecordPrivateCompute(h,input,work);assert(r.outcome==F::Outcome::PrivateRecordedRestored);
 input=Setup(h);h.Put(Slot+0x68,Depth);h.resident=true;
 r=F::RecordPrivateCompute(h,input,work);assert(r.bindingsRestored&&h.residencyChecks>0);
 input=Setup(h);{LegacyHost legacy{h};r=F::RecordPrivateCompute(legacy,input,work);assert(r.bindingsRestored);}
 input=Setup(h);h.Put(Slot+0x68,Depth);{LegacyHost legacy{h};r=F::RecordPrivateCompute(legacy,input,work);
  assert(r.outcome==F::Outcome::Refused&&h.events.empty());}
 // Scope loss after EACH request/flush/private/reentry/PSO/table operation is fatal,
 // with no later restore calls aimed at an unverified replacement context.
 for(unsigned step=1;step<=6;++step){input=Setup(h);h.loseAfter=step;
  r=F::RecordPrivateCompute(h,input,work);
  assert(r.outcome==F::Outcome::ScopeLostAfterMutation&&!r.bindingsRestored&&r.requestsIssued==1);
  assert(h.events.size()==step&&r.callbackEntered==(step>=3));}
 for(unsigned bad=0;bad<14;++bad){input=Setup(h);
  r=F::RecordPrivateCompute(h,input,[&]{h.events.push_back('c');
   switch(bad){case 0:h.scope=false;break;case 1:++h.thread;break;case 2:++h.tls;break;
   case 3:h.Put(Engine+0x30,uintptr_t(7));break;case 4:h.Put(Engine+0x60,uintptr_t(7));break;
   case 5:h.Put(Engine+0x3d0,uintptr_t(7));break;case 6:h.Put(Engine+0x68,uint32_t(2));break;
   case 7:h.Put(Slot,uintptr_t(7));break;case 8:h.Put(Slot+0x56,uint8_t(0x40));break;
   case 9:h.Put(Slot-8,int32_t(0));break;case 10:h.Put(Slot+0x68,Depth);break;
   case 11:h.throwRead=true;break;case 12:h.bundle=false;break;case 13:h.camera=false;break;}
   return true;});
  assert(r.outcome==F::Outcome::ScopeLostAfterMutation&&!r.bindingsRestored&&h.events.size()==3);
 }
 }
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-fog-denoise-access-') as temporary:
            source = Path(temporary) / 'test.cpp'
            source.write_text(harness)
            for flags in (['-O0'], ['-O3', '-ffast-math']):
                binary = Path(temporary) / 'test'
                subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags,
                                '-I', str(HEADER.parent), str(source), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()

"""Trusted native post-ray copy state protocol, without a second state tracker."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'OptiScaler/upscalers/ffx/FSRDCyberpunkRayAccess.h'


class RayAccess(unittest.TestCase):
    def test_narrow_native_contract_and_no_graphics_reentry(self):
        text = HEADER.read_text()
        for forbidden in ('host.Reenter(', 'host.RestorePso(', 'host.CurrentNativeList(',
                          'ResourceBarrier(', 'GetDesc(', 'AddRef(', '0x538', '0x528'):
            self.assertNotIn(forbidden, text)
        for required in ('Engine::Detail::ReadTexture', 'Bindings::Detail::ReadTexture',
                         'IsAdmittedPostRayScope(input)', '0x1a8e988',
                         'motion.requestedSrvState != 0x40 && motion.requestedSrvState != 0xc0',
                         'CopySource, Engine::AllSubresources', 'HitUav, Engine::AllSubresources',
                         'catch (...)', 'ScopeLostAfterMutation', 'Engine::BufferCode[1]'):
            self.assertIn(required, text)
        self.assertEqual(text.count('host.RequestState(input.image'), 3)
        self.assertEqual(text.count('host.Flush(input.image'), 2)

    def test_actual_protocol_O0_O3_fast_failures_and_mandatory_restore(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX to compile the actual protocol')
        harness = r'''
#include "FSRDCyberpunkRayAccess.h"
#include <cassert>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
namespace R=FSRD::CyberpunkRayAccess;
namespace E=FSRD::CyberpunkEngineAccess;
constexpr uintptr_t Image=0x140000000, Graph=0x1000, View=0x2000, Tls=0x3000,
 Engine=0x4000,List=0x5000,List4=0x5010,Cache=0x6000,Descriptor=0x7000,Registry=0x10000000;
struct Host {
 std::map<uintptr_t,std::vector<unsigned char>> memory;
 std::vector<char> events;
 std::vector<uint32_t> states;
 uint32_t thread=33;uintptr_t currentTls=Tls;
 bool scope=true,image=true,direct=true,readable=true,throwRead=false,residency=false;
 unsigned codeChecks=0,badCode=99,operations=0,loseAfter=0,residencyChecks=0;
 bool kind3Retain=false;
 template<class T>void Put(uintptr_t a,const T& value) {
  auto& bytes=memory[a];bytes.resize(sizeof(value));std::memcpy(bytes.data(),&value,sizeof(value));
 }
 bool Read(uintptr_t a,void* out,size_t n) {
  if(throwRead)throw std::runtime_error("read");
  if(!readable||!memory.contains(a)||memory[a].size()!=n)return false;
  std::memcpy(out,memory[a].data(),n);return true;
 }
 uint32_t ThreadId()noexcept{return thread;}
 bool ReadTlsSlotZero(uintptr_t& out)noexcept{out=currentTls;return out!=0;}
 bool ExactImageAuthenticated(uintptr_t a,uintptr_t size,uint32_t stamp,std::string_view sha)noexcept {
  assert(a==Image&&size==E::ImageBytes&&stamp==E::ImageTimestamp&&sha==E::ExeSha256);return image;
 }
 bool LiveCodeMatches(uintptr_t a,const E::CodeRange& code)noexcept {
  assert(a==Image&&codeChecks<R::Code.size());
  assert(code.rva==R::Code[codeChecks].rva&&code.sha256==R::Code[codeChecks].sha256);
  return codeChecks++!=badCode;
 }
 bool IsAdmittedPostRayScope(const R::Input& in)noexcept {
  assert(in.dispatch.scope.serial==11&&in.dispatch.scope.recordingGeneration==12);
  return scope;
 }
 bool ListIsDirect(uintptr_t list)noexcept{assert(list==List);return direct;}
 bool IsTextureResidencyAdmitted(uintptr_t registry,const E::TextureBorrow& source)noexcept {
  assert(registry==Registry&&(source.handle==1||source.handle==2));++residencyChecks;return residency;
 }
 void Step()noexcept{if(++operations==loseAfter)scope=false;}
 void RequestState(uintptr_t entry,uintptr_t engine,uint32_t handle,uint32_t state,uint32_t subresource)noexcept {
  assert(entry==Image+E::RequestStateRva&&engine==Engine&&subresource==0xffffffff);
  assert((handle==1&&(state==0x8c0||state==0x840))||(handle==2&&(state==0x800||state==8)));
  events.push_back(handle==1?'m':state==8?'u':'h');states.push_back(state);
  if(kind3Retain){
   auto& bytes=memory[Registry+0x2f1d0+uintptr_t(handle-1)*0xb0];int32_t refs;
   std::memcpy(&refs,bytes.data(),4);++refs;std::memcpy(bytes.data(),&refs,4);
  }
  Step();
 }
 void Flush(uintptr_t entry,uintptr_t engine)noexcept {
  assert(entry==Image+E::FlushRva&&engine==Engine);events.push_back('f');Step();
 }
};
R::Input Setup(Host& h) {
 h=Host{};R::Input input;input.image=Image;
 auto& d=input.dispatch;d.scope={11,12,Graph,View,Tls,Engine,List,0};
 d.callerRva=FSRD::CyberpunkRayBindings::DispatchReturnRva;
 d.list4=List4;d.cache=Cache;d.registry=Registry;d.b6.descriptor=Descriptor;
 h.Put(Tls+0x14,uint8_t(1));h.Put(Tls+0x188,Engine);h.Put(Graph+0x18,View);
 h.Put(Engine+0x30,List);h.Put(Engine+0x40,List4);h.Put(Engine+0x60,Cache);
 h.Put(Engine+0x68,uint32_t(0));h.Put(Engine+0x90,Descriptor);
 h.Put(Image+E::RegistryRva,Registry);h.Put(Registry+0x1a8e988,uint8_t(0));
 for(unsigned i=0;i<2;++i){
  auto& t=d.textures[i?2:0];t.handle=i+1;t.native=0x8000+i*0x100;t.descriptor=0x9000+i*0x100;
  t.requestedSrvState=0xc0;t.refs=5;t.slot=Registry+0x2f1d8+uintptr_t(i)*0xb0;
  t.compact[4]=1;t.compact[6]=0x10;t.compact[7]=i?0x0d:0x11;t.compact[8]=1;
  t.binding.shaderRegister=i?8:4;t.binding.descriptor=t.descriptor;
  h.Put(t.slot-8,t.refs);h.Put(t.slot,t.native);h.Put(t.slot+0x48,t.requestedSrvState);
  h.Put(t.slot+0x4e,t.compact);h.Put(t.slot+0x56,uint8_t(1));h.Put(t.slot+0x68,uintptr_t(0));
  if(i){t.extra=0xa000;t.uavArray=0xb000;h.Put(t.slot+0x40,t.extra);h.Put(t.extra+0x28,t.uavArray);h.Put(t.uavArray,t.descriptor);}
  else h.Put(t.slot+0x30,t.descriptor);
 }
 return input;
}
int main(){
 Host h;auto input=Setup(h);
 auto copy=[&]{h.events.push_back('c');h.Step();return true;};
 auto result=R::RecordCopy(h,input,copy);
 assert(result.outcome==R::Outcome::CopyRecordedRestored&&result.hitRestored&&result.callbackEntered&&result.requestsIssued==3);
 assert((h.events==std::vector<char>{'m','h','f','c','u','f'}));assert(h.codeChecks==4);
 assert((h.states==std::vector<uint32_t>{0x8c0,0x800,8}));
 for(unsigned failed=0;failed<2;++failed){
  input=Setup(h);result=R::RecordCopy(h,input,[&]()->bool{h.events.push_back('c');if(failed)throw std::runtime_error("partial");return false;});
  assert(result.outcome==R::Outcome::CopyFailedRestored&&result.hitRestored&&result.callbackEntered);
  assert((h.events==std::vector<char>{'m','h','f','c','u','f'}));
 }
 // Every original-use/scope/state-skip refusal must precede any mutation.
 for(unsigned bad=0;bad<22;++bad){
  input=Setup(h);auto& d=input.dispatch;auto& m=d.textures[0];auto& t=d.textures[2];
  switch(bad){
  case 0:input.image=0;break;case 1:d.callerRva++;break;case 2:d.list4=0;break;
  case 3:m.requestedSrvState=8;break;case 4:h.image=false;break;case 5:h.badCode=2;break;
  case 6:h.scope=false;break;case 7:h.direct=false;break;case 8:h.readable=false;break;
  case 9:h.throwRead=true;break;case 10:h.thread=0;break;case 11:h.currentTls=0;break;
  case 12:h.Put(Tls+0x14,uint8_t(0));break;case 13:h.Put(Engine+0x40,uintptr_t(7));break;
  case 14:h.Put(t.slot+0x56,uint8_t(0x40));break;case 15:h.Put(t.slot-8,int32_t(0));break;
  case 16:h.Put(t.slot,uintptr_t(7));break;case 17:h.Put(m.slot+0x30,uintptr_t(7));break;
  case 18:h.Put(t.slot+0x68,t.native);break;
  case 19:h.Put(Engine+0x68,uint32_t(4));h.Put(Registry+0x1a8e988,uint8_t(1));break;
  case 20:m.native=t.native;break;case 21:t.binding.descriptor++;break;
  }
  result=R::RecordCopy(h,input,copy);assert(result.outcome==R::Outcome::Refused&&!result.requestsIssued&&!result.callbackEntered&&!result.hitRestored&&h.events.empty());
 }
 // Existing registered residency opt-in, not inferred from a matching pointer.
 input=Setup(h);h.residency=true;
 for(auto i:{0,2})h.Put(input.dispatch.textures[i].slot+0x68,input.dispatch.textures[i].native);
 result=R::RecordCopy(h,input,copy);assert(result.hitRestored&&h.residencyChecks>0);
 // Native context-kind3 retention may increase refcounts; it is not slot reuse.
 input=Setup(h);h.Put(Engine+0x68,uint32_t(3));h.kind3Retain=true;
 result=R::RecordCopy(h,input,copy);assert(result.outcome==R::Outcome::CopyRecordedRestored);
 input=Setup(h);h.Put(Engine+0x68,uint32_t(4));result=R::RecordCopy(h,input,copy);assert(result.hitRestored);
 input=Setup(h);input.dispatch.textures[0].requestedSrvState=0x40;
 h.Put(input.dispatch.textures[0].slot+0x48,uint32_t(0x40));
 result=R::RecordCopy(h,input,copy);assert(result.hitRestored&&h.states[0]==0x840);
 // Scope loss after EACH state call/flush/callback stops further engine calls.
 for(unsigned step=1;step<=6;++step){
  input=Setup(h);h.loseAfter=step;result=R::RecordCopy(h,input,copy);
  assert(result.outcome==R::Outcome::ScopeLostAfterMutation&&!result.hitRestored&&h.events.size()==step);
  assert(result.callbackEntered==(step>=4));
 }
 // Resource admission and thread/TLS/cache/view identity are rechecked after a
 // partial private callback too. No restore against an unverified reused scope.
 for(unsigned bad=0;bad<8;++bad){
  input=Setup(h);result=R::RecordCopy(h,input,[&]{
   switch(bad){case 0:h.scope=false;break;case 1:++h.thread;break;case 2:++h.currentTls;break;
   case 3:h.Put(Engine+0x68,uint32_t(2));break;case 4:h.Put(Graph+0x18,uintptr_t(9));break;
   case 5:h.Put(Engine+0x90,uintptr_t(9));break;case 6:h.Put(input.dispatch.textures[2].slot,uintptr_t(9));break;
   case 7:h.throwRead=true;break;}return true;});
  assert(result.outcome==R::Outcome::ScopeLostAfterMutation&&!result.hitRestored&&result.requestsIssued==2);
 }
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-ray-access-') as tmp:
            source = Path(tmp) / 'test.cpp'
            source.write_text(harness)
            for flags in (['-O0'], ['-O3', '-ffast-math']):
                binary = Path(tmp) / 'test'
                subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags,
                                '-I', str(HEADER.parent), str(source), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()

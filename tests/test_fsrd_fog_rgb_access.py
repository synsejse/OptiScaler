"""Explicit scene-RGB protocol: actual shared header, with no production caller."""
import ast
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'OptiScaler/upscalers/ffx/FSRDCyberpunkFogDenoiseAccess.h'


def existing_native_host_fixture():
    """Reuse the original-depth protocol's CPU fixture, not a second ABI model."""
    tree = ast.parse((ROOT / 'tests/test_fsrd_fog_denoise_access.py').read_text())
    values = [node.value.value for node in ast.walk(tree) if isinstance(node, ast.Assign)
              and any(isinstance(target, ast.Name) and target.id == 'harness' for target in node.targets)
              and isinstance(node.value, ast.Constant) and isinstance(node.value.value, str)]
    assert len(values) == 1
    return values[0].split('int main(){', 1)[0]


class FogRgbAccess(unittest.TestCase):
    def test_explicit_scene_contract_and_compile_time_private_separation(self):
        text = HEADER.read_text()
        private = text.split('Result RecordPrivateCompute(', 1)[1].split('// Explicit, separately admitted', 1)[0]
        self.assertIn('Detail::RecordWork<false>', private)
        self.assertNotIn('IsAdmittedFogRgbScope', private)
        scene = text.split('SceneResult RecordSceneRgb(', 1)[1]
        self.assertIn('Detail::RecordWork<true>', scene)
        self.assertIn('SceneOutcome::SceneFailedRestored', scene)
        self.assertIn('SceneOutcome::SceneRecordedRestored', scene)
        admitted = text.split('bool AdmittedScope(', 1)[1].split('template<bool SceneRgb, class Host> bool SameScope', 1)[0]
        self.assertIn('if constexpr (SceneRgb)', admitted)
        self.assertIn('input.copySource || !host.IsAdmittedFogRgbScope(input)', admitted)
        self.assertIn('return host.IsAdmittedFogDenoiseScope(input)', admitted)
        same = text.split('bool SameScope(', 1)[1].split('template<bool SceneRgb, class Host> bool Prepare', 1)[0]
        self.assertIn('AdmittedScope<SceneRgb>(host, input)', same)
        prepare = text.split('bool Prepare(', 1)[1].split('} // namespace Detail', 1)[0]
        self.assertLess(prepare.index('AdmittedScope<SceneRgb>'), prepare.index('host.CurrentNativeList'))
        work = text.split('Result RecordWork(', 1)[1].split('} // namespace Detail', 1)[0]
        self.assertEqual(work.count('host.RestoreOriginalFogTarget(input.list)'), 1)
        self.assertLess(work.index('host.FlushGraphicsTables('), work.index('host.RestoreOriginalFogTarget('))
        self.assertLess(work.index('host.RestoreOriginalFogTarget('), work.index('result.bindingsRestored = true'))
        self.assertIn('Detail::SameScope<SceneRgb>', work.split('host.RestoreOriginalFogTarget(', 1)[1])
        for required in ('one target and NO DSV', 'preserves target alpha, IA/RS/VRS',
                         'No other OM changes are authorized', 'not rollback, success,',
                         'retry permission', 'installs no hooks and never forwards the draw'):
            self.assertIn(required, text)

    def test_actual_scene_protocol_O0_O3_fast(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX to compile actual scene recorder')
        harness = existing_native_host_fixture() + r'''
struct SceneHost:Host {
 bool rgbPermission=true,exactMain=true,frozenRtv=true,noDsv=true,ownedSource=true;
 bool coverage=true,alphaMask=true,originalTarget=true,repairTarget=true;
 bool throwSceneGate=false,loseSceneAfterGetter=false;
 unsigned rgbChecks=0,rgbLoseAfter=0,targetRestores=0,rgbWrites=0;
 bool IsAdmittedFogRgbScope(const F::Input& input){
  assert(input.originalFogScope==77&&input.list==List);++rgbChecks;
  if(throwSceneGate)throw std::runtime_error("scene gate");
  if(loseSceneAfterGetter&&getters)return false;
  if(rgbLoseAfter&&events.size()>=rgbLoseAfter)return false;
  // Same native target/equivalent frozen RTV is admitted during private work;
  // the original OM binding must additionally be installed after restoration.
  return rgbPermission&&exactMain&&frozenRtv&&noDsv&&ownedSource&&coverage&&alphaMask&&
   (!targetRestores||originalTarget);
 }
 void RestoreOriginalFogTarget(uintptr_t list)noexcept{
  assert(list==List&&nativePso==Pso&&!cacheDirty);
  events.push_back('o');++targetRestores;if(repairTarget)originalTarget=true;Step();
 }
};
F::Input SetupScene(SceneHost& h){h=SceneHost{};copyMode=false;return Setup(h);}
int main(){
 SceneHost h;auto input=SetupScene(h);
 auto draw=[&]{h.events.push_back('c');h.nativePso=0xabcd;h.originalTarget=false;++h.rgbWrites;h.Step();return true;};
 auto result=F::RecordSceneRgb(h,input,draw);
 assert(result.outcome==F::SceneOutcome::SceneRecordedRestored&&result.requestsIssued==1&&
  result.callbackEntered&&result.bindingsRestored);
 assert((h.events==std::vector<char>{'d','f','c','r','p','g','o'}));
 assert(h.rgbChecks==10&&h.targetRestores==1&&h.originalTarget&&h.rgbWrites==1&&!h.cacheDirty);
 assert(h.codeChecks==F::Code.size());
 // Partial scene writes are not rolled back, but ordinary false/throw MUST
 // restore the exact target as well as roots/heaps/PSO/dynamic tables.
 for(bool throws:{false,true}){
  input=SetupScene(h);
  result=F::RecordSceneRgb(h,input,[&]()->bool{
   h.events.push_back('c');h.nativePso=0xabcd;h.originalTarget=false;++h.rgbWrites;
   if(throws)throw std::runtime_error("after scene commands");
   return false;
  });
  assert(result.outcome==F::SceneOutcome::SceneFailedRestored&&result.bindingsRestored&&result.callbackEntered);
  assert(h.rgbWrites==1&&h.originalTarget&&h.targetRestores==1&&!h.cacheDirty&&h.nativePso==Pso);
  assert((h.events==std::vector<char>{'d','f','c','r','p','g','o'}));
 }
 // Scene admission is initial and fail-closed, BEFORE even the native getter.
 for(unsigned bad=0;bad<10;++bad){
  input=SetupScene(h);
  switch(bad){case 0:h.rgbPermission=false;break;case 1:h.exactMain=false;break;
   case 2:h.frozenRtv=false;break;case 3:h.noDsv=false;break;case 4:h.ownedSource=false;break;
   case 5:h.coverage=false;break;case 6:h.alphaMask=false;break;case 7:input.copySource=true;break;
   case 8:h.throwSceneGate=true;break;case 9:h.scope=false;break;}
  result=F::RecordSceneRgb(h,input,draw);
  assert(result.outcome==F::SceneOutcome::Refused&&!result.requestsIssued&&!result.callbackEntered&&
   !result.bindingsRestored&&h.events.empty()&&!h.getters&&!h.targetRestores&&!h.rgbWrites);
 }
 input=SetupScene(h);h.loseSceneAfterGetter=true;
 result=F::RecordSceneRgb(h,input,draw);
 assert(result.outcome==F::SceneOutcome::Refused&&h.getters==1&&h.events.empty());
 // Loss of ONLY scene permission is caught at each of the seven original
 // scope rechecks, not merely at entry or around the callback.
 for(unsigned step=1;step<=7;++step){
  input=SetupScene(h);h.rgbLoseAfter=step;
  result=F::RecordSceneRgb(h,input,draw);
  assert(result.outcome==F::SceneOutcome::ScopeLostAfterMutation&&!result.bindingsRestored&&
   result.requestsIssued==1&&result.callbackEntered==(step>=3));
  assert(h.events.size()==step&&h.targetRestores==(step==7));
 }
 // Common scope loss is still fatal on each native/private operation.
 for(unsigned step=1;step<=7;++step){
  input=SetupScene(h);h.loseAfter=step;
  result=F::RecordSceneRgb(h,input,draw);
  assert(result.outcome==F::SceneOutcome::ScopeLostAfterMutation&&!result.bindingsRestored&&h.events.size()==step);
 }
 input=SetupScene(h);h.repairTarget=false;
 result=F::RecordSceneRgb(h,input,draw);
 assert(result.outcome==F::SceneOutcome::ScopeLostAfterMutation&&!result.bindingsRestored&&
  h.targetRestores==1&&!h.originalTarget);
 input=SetupScene(h);h.repairTables=false;
 result=F::RecordSceneRgb(h,input,draw);
 assert(result.outcome==F::SceneOutcome::ScopeLostAfterMutation&&!h.targetRestores&&h.events.size()==6);
 input=SetupScene(h);h.originalTableReceipt=false;
 result=F::RecordSceneRgb(h,input,draw);
 assert(result.outcome==F::SceneOutcome::ScopeLostAfterMutation&&!h.targetRestores&&h.events.size()==6);
 input=SetupScene(h);
 result=F::RecordSceneRgb(h,input,[&]{h.events.push_back('c');h.throwSceneGate=true;return true;});
 assert(result.outcome==F::SceneOutcome::ScopeLostAfterMutation&&h.events.size()==3&&!h.targetRestores);
 for(unsigned bad=0;bad<F::Code.size();++bad){
  input=SetupScene(h);h.badCode=bad;result=F::RecordSceneRgb(h,input,draw);
  assert(result.outcome==F::SceneOutcome::Refused&&h.events.empty()&&!h.getters);
 }
 // Existing private API does not consult scene permission, change OM, or
 // require the new methods. The inherited CPU fixture's LegacyHost has neither.
 input=SetupScene(h);h.rgbPermission=false;h.throwSceneGate=true;
 auto privateResult=F::RecordPrivateCompute(h,input,[&]{h.events.push_back('c');return true;});
 assert(privateResult.outcome==F::Outcome::PrivateRecordedRestored&&h.rgbChecks==0&&!h.targetRestores);
 assert((h.events==std::vector<char>{'d','f','c','r','p','g'}));
 Host legacy;input=Setup(legacy);LegacyHost old{legacy};
 privateResult=F::RecordPrivateCompute(old,input,[&]{legacy.events.push_back('c');return true;});
 assert(privateResult.outcome==F::Outcome::PrivateRecordedRestored&&privateResult.bindingsRestored);
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-fog-rgb-access-') as temporary:
            source = Path(temporary) / 'test.cpp'
            source.write_text(harness)
            for flags in (['-O0'], ['-O3', '-ffast-math']):
                binary = Path(temporary) / 'test'
                subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags,
                                '-I', str(HEADER.parent), str(source), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()

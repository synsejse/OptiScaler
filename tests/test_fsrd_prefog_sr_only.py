"""Process-fixed late-SR route and actual Feature scalar/polling bodies, CPU only."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / 'OptiScaler/upscalers/ffx'
SOURCE = DIRECTORY / 'FSRDFeature_Dx12.cpp'


def function(signature):
    text = SOURCE.read_text()
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def compile_and_run(test, harness):
    compiler = os.environ.get('CXX') or shutil.which('c++')
    if not compiler:
        test.skipTest('Set CXX for compiled production route tests')
    with tempfile.TemporaryDirectory(prefix='fsrd-prefog-sr-') as tmp:
        source, executable = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        source.write_text(harness)
        for options in (['-O0'], ['-O3', '-ffast-math']):
            result = subprocess.run([compiler, '-std=c++20', '-pthread', *options, '-I', str(DIRECTORY),
                                     str(source), '-o', str(executable)], capture_output=True, text=True)
            test.assertEqual(result.returncode, 0, result.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True)
            test.assertEqual(run.returncode, 0, run.stderr)


class PreFogSrOnly(unittest.TestCase):
    def test_default_off_fixed_route_and_no_late_rr_reentry(self):
        header = (ROOT / 'OptiScaler/Config.h').read_text()
        config = (ROOT / 'OptiScaler/Config.cpp').read_text()
        self.assertIn('FfxDenoiserCyberpunkPreFogExperiment { false }', header)
        self.assertIn('readBool("FSR-RR", "CyberpunkPreFogExperiment")', config)
        self.assertIn('ini.SetValue("FSR-RR", "CyberpunkPreFogExperiment"', config)
        text = SOURCE.read_text()
        self.assertEqual(text.count('PreFogSession::Freeze('), 1)
        constructor = text.split('FSRDFeatureDx12::FSRDFeatureDx12(', 1)[1].split('FSRDFeatureDx12::~', 1)[0]
        self.assertIn('PreFogSession::Freeze(Config::Instance()->FfxDenoiserCyberpunkPreFogExperiment', constructor)
        evaluate = function('bool FSRDFeatureDx12::EvaluateInternal(')
        self.assertLess(evaluate.index('PreFogSession::LateSrOnly()'), evaluate.index('FSRD::ValidateInputContract('))
        self.assertIn('return EvaluatePreFogSrOnly(InCommandList, InParameters);', evaluate)
        late = function('bool FSRDFeatureDx12::EvaluatePreFogSrOnly(')
        self.assertEqual(late.count('FFXFeatureDx12::EvaluateInternal('), 1)
        for forbidden in ('ValidateInputContract(', 'UpdateSize(', 'PrepareDenoiserInput(', 'ConfigureDenoiser(',
                          'DispatchDenoiser(', 'DispatchComposition(', 'CommitCameraHistory(', 'ShowNativeDebugOutput(',
                          'FSRDResearch::', 'State::Instance()', 'changeBackend', 'parameters->Set(', '++_frameCount'):
            self.assertNotIn(forbidden, late)
        self.assertIn('PollPreFogExperiments();', late)
        self.assertIn('PollPreFogExperiments();', evaluate)
        self.assertLess(late.index('VerticalFov('), late.index('FFXFeatureDx12::EvaluateInternal('))
        self.assertIn('_contextDesc.maxRenderSize.width', late)
        self.assertNotIn('_denoiserCtxDesc.maxRenderSize', late)
        init = function('bool FSRDFeatureDx12::InitFFX(')
        self.assertIn('CreateDenoiserContext()', init)
        self.assertNotIn('PreFogSession', init)  # Do not trigger generic initialization fallback for missing early work.

    def test_actual_header_route_history_projection_fast_math(self):
        compile_and_run(self, r'''
#include "FSRDPreFogSession.h"
#include <cassert>
#include <thread>
#include <vector>
using namespace FSRD::PreFogSession;
int main(){
 RouteLatch off;assert(!off.LateSrOnly());assert(!off.Freeze(false));assert(!off.Freeze(true));
 RouteLatch on;assert(on.Freeze(true));assert(on.Freeze(false));
 std::vector<std::thread> threads;
 for(int i=0;i<16;++i)threads.emplace_back([&,i]{for(int j=0;j<1000;++j){assert(on.Freeze(i&1));assert(!off.Freeze(i&1));}});
 for(auto& t:threads)t.join();
 assert(!LateSrOnly());Freeze(true);assert(LateSrOnly());Freeze(false);assert(LateSrOnly());
 assert(StatusText().find("NOT guaranteed for this frame")!=std::string_view::npos);
 LateSrHistory h;
 assert(h.Begin(1280,720,1920,1080,false));h.Complete(true);
 assert(!h.Begin(1280,720,1920,1080,false));h.Complete(true);
 assert(h.Begin(1280,720,2560,1440,false));h.Complete(true);
 assert(h.Begin(1920,1080,2560,1440,false));h.Complete(true);
 assert(h.Begin(1920,1080,2560,1440,true));h.Complete(true);
 assert(!h.Begin(1920,1080,2560,1440,false));h.Complete(false);
 assert(h.Begin(1920,1080,2560,1440,false));h.Complete(true);
 h.Invalidate();assert(h.Begin(1920,1080,2560,1440,false));h.Complete(true);
 h.Complete(true);assert(h.Begin(1920,1080,2560,1440,false)); // No pending dispatch cannot publish history.
 h.Complete(true);assert(h.Begin(0,1080,2560,1440,false));h.Complete(true);
 assert(h.Begin(1920,1080,2560,1440,false));
 // Current Cyberpunk-style native projection layout, including jitter/lens offsets.
 std::array<float,16> p{1.3f,0,0,0, 0,2.2f,0,0, -.001f,.002f,1.000001f,1, 0,0,-.02f,0};
 float fov=0;assert(VerticalFov(p,fov));assert(std::abs(fov-float(2*std::atan(1.0/2.2)))<1e-6f);
 for(float handedness:{-1.0f,1.0f}){auto q=p;q[11]=handedness;assert(VerticalFov(q,fov));}
 for(uint32_t bits:{0x7f800000u,0xff800000u,0x7fc00001u,0xffc00001u}){
  const float bad=std::bit_cast<float>(bits);assert(!Finite(bad));
  for(size_t i=0;i<16;++i){auto q=p;q[i]=bad;assert(!VerticalFov(q,fov));assert(fov==0);}
 }
 for(size_t i:{1u,2u,3u,4u,6u,7u,12u,13u,15u}){auto q=p;q[i]=.01f;assert(!VerticalFov(q,fov));}
 for(size_t i:{0u,5u,11u,14u}){auto q=p;q[i]=0;assert(!VerticalFov(q,fov));}
 for(size_t i:{0u,5u}){auto q=p;q[i]=-1;assert(!VerticalFov(q,fov));}
 for(size_t i=0;i<16;++i){auto q=p;q[i]=1e30f;assert(!VerticalFov(q,fov));}
}
''')

    def test_actual_feature_branch_poll_and_scalar_history_bodies(self):
        # Compile the actual Feature methods against a narrow base-SR/NGX boundary.
        # Base SR is an explicit stub; source policy separately proves real base call.
        override = function('void FSRDFeatureDx12::OverrideUpscaleDispatch(')
        poll = function('void FSRDFeatureDx12::PollPreFogExperiments(')
        late = function('bool FSRDFeatureDx12::EvaluatePreFogSrOnly(')
        entry = function('bool FSRDFeatureDx12::EvaluateInternal(').split('    auto& state = State::Instance();', 1)[0]
        entry += '    ++ordinaryRrCalls; return true;\n}'
        harness = r'''
#include "FSRDPreFogSession.h"
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string>
#define LOG_FUNC(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
constexpr int NVSDK_NGX_Result_Success=0;
constexpr auto NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX="projection";
constexpr auto NVSDK_NGX_Parameter_FrameTimeDeltaInMsec="delta";
constexpr auto NVSDK_NGX_Parameter_Reset="reset";
constexpr int FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ=192;
struct ID3D12GraphicsCommandList{};struct ID3D12Resource{};
int memcpy_s(void* dst,size_t cap,const void* src,size_t size){assert(cap>=size);std::memcpy(dst,src,size);return 0;}
template<class T>struct Option{T v{};T value_or_default()const{return v;}void set_volatile_value(T x){v=x;}};
struct Config{
 Option<float> FfxDenoiserCrossBlNormStr{1},FfxDenoiserStabilityBias{1},FfxDenoiserMaxRadiance{65504},
 FfxDenoiserRadianceClip{50},FfxDenoiserGaussKernRelax{0},FfxDenoiserDisocclusionThreshold{.01f};
 Option<bool> FfxDenoiserCyberpunkFogProbe{true},FfxDenoiserCyberpunkFogCapture{true},FsrUseFsrInputValues{},RcasEnabled{true},OutputScalingEnabled{true};
 static Config* Instance(){static Config c;return &c;}
};
namespace Util{inline double now=100;double MillisecondsNow(){return now;}}
namespace FSRD{struct DenoiserSettings{float crossBilateralNormalStrength,stabilityBias,maxRadiance,
 radianceClipStdK,gaussianKernelRelaxation,disocclusionThreshold;};}
namespace FSRDCyberpunkFogProbe{inline int resets=0,identities=0,temporal=0;
void ArmPrivateReset(void*,unsigned,unsigned){++resets;}void ArmRgbIdentity(void*,unsigned,unsigned){++identities;}
void PollTemporalWindow(void*,unsigned,unsigned,uint64_t provider,const FSRD::DenoiserSettings* settings){
 assert(provider==42&&settings&&settings->maxRadiance==65504);++temporal;}}
struct NVSDK_NGX_Parameter{
 std::array<float,16> projection{1.3f,0,0,0,0,2.2f,0,0,0,0,1.000001f,1,0,0,-.02f,0};
 bool hasProjection=true,hasDelta=true,hasExplicit=false,failBefore=false,failAfter=false,throws=false;
 float delta=16,explicitDelta=7;unsigned reset=0,width=1280,height=720,outWidth=1920,outHeight=1080;
 ID3D12Resource originalColor; // No albedo, normal, hit, early-success receipt, or RR texture fields exist.
 int Get(const char* key,void** out){assert(std::string(key)=="projection");*out=hasProjection?projection.data():nullptr;return hasProjection?0:1;}
 int Get(const char* key,float* out){if(std::string(key)=="FSR.frameTimeDelta"){*out=explicitDelta;return hasExplicit?0:1;}assert(std::string(key)=="delta");*out=delta;return hasDelta?0:1;}
 int Get(const char* key,unsigned* out){assert(std::string(key)=="reset");*out=reset;return 0;}
};
struct ffxDispatchDescUpscale{
 ID3D12Resource* color=nullptr;float cameraFovAngleVertical=0,frameTimeDelta=0;bool reset=false;
 struct Extent{unsigned width=0,height=0;}renderSize,upscaleSize;
};
ID3D12Resource* ffxApiGetResourceDX12(ID3D12Resource* p,int){return p;}
struct FFXFeatureDx12{
 unsigned _frameCount=0,baseCalls=0;ffxDispatchDescUpscale dispatched;
 virtual void OverrideUpscaleDispatch(ffxDispatchDescUpscale&){}
 bool EvaluateInternal(ID3D12GraphicsCommandList*,NVSDK_NGX_Parameter* p){
  ++baseCalls;if(p->failBefore)return false;
  ffxDispatchDescUpscale d;d.color=&p->originalColor;d.renderSize={p->width,p->height};d.upscaleSize={p->outWidth,p->outHeight};d.reset=p->reset==1;
  OverrideUpscaleDispatch(d);dispatched=d;if(p->throws)throw std::runtime_error("base");if(p->failAfter)return false;++_frameCount;return true;
 }
};
struct FSRDFeatureDx12:FFXFeatureDx12{
 uint64_t _denoiserProviderId=42;
 FSRD::PreFogSession::LateSrHistory _preFogSrHistory;bool _preFogSrScalarOverride=false,_preFogSrGameReset=false;
 bool _loggedPreFogRoute=false,_loggedPreFogScalarFailure=false,_frameShowNativeDebug=true;
 bool _hasCameraHistory=true,_isInReset=false,_diagnosticUpscaleReset=false,inited=true;
 ID3D12Resource* _upscaleColorOverride=nullptr;float _upscaleFovVertical=0,_upscaleDeltaTime=0;
 double _preFogSrLastFrameTime=80;unsigned width=1280,height=720,ordinaryRrCalls=0;
 void* Device=nullptr;
 struct Context{ffxDispatchDescUpscale::Extent maxRenderSize{1920,1080};}_contextDesc;
 struct Denoiser{bool created=true;bool IsCreated(){return created;}}_denoiser;
 struct Helper{bool ready=true;bool IsInit(){return ready;}}rcas,output;
 Helper* RCAS=&rcas;Helper* OutputScaler=&output;
 bool IsInited(){return inited;}unsigned RenderWidth(){return width;}unsigned RenderHeight(){return height;}
 void GetRenderResolution(NVSDK_NGX_Parameter* p,unsigned* w,unsigned* h){*w=width=p->width;*h=height=p->height;}
 void OverrideUpscaleDispatch(ffxDispatchDescUpscale&)override;
 void PollPreFogExperiments();
 bool EvaluatePreFogSrOnly(ID3D12GraphicsCommandList*,NVSDK_NGX_Parameter*);
 bool EvaluateInternal(ID3D12GraphicsCommandList*,NVSDK_NGX_Parameter*);
};
'''
        harness += override + '\n' + poll + '\n' + late + '\n' + entry
        harness += r'''
int main(){
 ID3D12GraphicsCommandList list;NVSDK_NGX_Parameter p;FSRDFeatureDx12 normal;
 // Unfrozen/default-off still enters the untouched ordinary RR body.
 assert(normal.EvaluateInternal(&list,&p)&&normal.ordinaryRrCalls==1&&normal.baseCalls==0);
 FSRD::PreFogSession::Freeze(true);FSRDFeatureDx12 f;
 assert(f.EvaluateInternal(&list,&p));assert(f.baseCalls==1&&f._frameCount==1&&!f.ordinaryRrCalls);
 assert(f.dispatched.color==&p.originalColor&&f.dispatched.reset&&f.dispatched.frameTimeDelta==16);
 assert(std::abs(f.dispatched.cameraFovAngleVertical-float(2*std::atan(1.0/2.2)))<1e-6f);
 assert(!f._preFogSrScalarOverride&&!f._upscaleColorOverride&&!f._frameShowNativeDebug);
 assert(f._hasCameraHistory); // RR camera state was not committed/consumed by late SR.
 assert(FSRDCyberpunkFogProbe::resets==1&&FSRDCyberpunkFogProbe::identities==1&&FSRDCyberpunkFogProbe::temporal==1);
 assert(f.EvaluateInternal(&list,&p)&&!f.dispatched.reset&&f._frameCount==2);
 p.reset=7;assert(f.EvaluateInternal(&list,&p)&&f.dispatched.reset);p.reset=0;
 p.width=1000;assert(f.EvaluateInternal(&list,&p)&&f.dispatched.reset);
 assert(f.EvaluateInternal(&list,&p)&&!f.dispatched.reset);
 p.outWidth=2000;assert(f.EvaluateInternal(&list,&p)&&f.dispatched.reset);
 for(bool before:{false,true}){
  p.failBefore=before;p.failAfter=!before;assert(!f.EvaluateInternal(&list,&p));
  assert(!f._preFogSrScalarOverride&&!f._upscaleColorOverride);p.failBefore=p.failAfter=false;
  assert(f.EvaluateInternal(&list,&p)&&f.dispatched.reset);
 }
 p.throws=true;try{f.EvaluateInternal(&list,&p);assert(false);}catch(const std::runtime_error&){}
 assert(!f._preFogSrScalarOverride&&!f._upscaleColorOverride);p.throws=false;
 assert(f.EvaluateInternal(&list,&p)&&f.dispatched.reset);
 // Actual-current scalar failures happen before base SR (and reset next success).
 for(unsigned bad=0;bad<8;++bad){
  auto q=p;switch(bad){case 0:q.hasProjection=false;break;case 1:q.projection[5]=0;break;
   case 2:q.delta=0;break;case 3:q.delta=-1;break;case 4:q.delta=std::bit_cast<float>(0x7fc00001u);break;
   case 5:q.width=2000;break;case 6:q.height=0;break;case 7:q.projection[14]=std::bit_cast<float>(0x7f800000u);break;}
  const auto calls=f.baseCalls,frames=f._frameCount;assert(!f.EvaluateInternal(&list,&q));
  assert(f.baseCalls==calls&&f._frameCount==frames&&!f._preFogSrScalarOverride);
  assert(f.EvaluateInternal(&list,&p)&&f.dispatched.reset);
 }
 // Explicit FSR duration precedence is opt-in; present-invalid is not silently replaced.
 auto& cfg=*Config::Instance();cfg.FsrUseFsrInputValues.v=true;p.hasExplicit=true;p.explicitDelta=5;
 assert(f.EvaluateInternal(&list,&p)&&f.dispatched.frameTimeDelta==5);
 p.explicitDelta=-1;const auto calls=f.baseCalls;assert(!f.EvaluateInternal(&list,&p)&&f.baseCalls==calls);
 cfg.FsrUseFsrInputValues.v=false;p.hasDelta=false;Util::now+=13;
 assert(f.EvaluateInternal(&list,&p)&&f.dispatched.frameTimeDelta==13&&f.dispatched.reset);
 // Missing early provider/disabled controls affect polling only, never select late RR.
 f._denoiser.created=false;auto polls=FSRDCyberpunkFogProbe::resets;Util::now+=16;
 const auto temporalPolls=FSRDCyberpunkFogProbe::temporal;
 assert(f.EvaluateInternal(&list,&p)&&FSRDCyberpunkFogProbe::resets==polls&&!f.ordinaryRrCalls);
 assert(FSRDCyberpunkFogProbe::temporal==temporalPolls);
 FSRD::PreFogSession::Freeze(false);FSRDFeatureDx12 recreated;p.hasDelta=true;
 assert(recreated.EvaluateInternal(&list,&p)&&recreated.dispatched.reset&&!recreated.ordinaryRrCalls);
 f.inited=false;const auto previous=f.baseCalls;assert(!f.EvaluateInternal(&list,&p)&&f.baseCalls==previous);
 f.inited=true;assert(f.EvaluateInternal(&list,&p)&&f.dispatched.reset);
 // Normal color override retains exactly the previous RR descriptor behavior.
 FSRDFeatureDx12 rr;ID3D12Resource converted;ffxDispatchDescUpscale desc;desc.color=&p.originalColor;
 rr.OverrideUpscaleDispatch(desc);assert(desc.color==&p.originalColor&&desc.frameTimeDelta==0&&!desc.reset);
 rr._upscaleColorOverride=&converted;rr._upscaleDeltaTime=11;rr._upscaleFovVertical=.8f;rr._isInReset=true;
 rr.OverrideUpscaleDispatch(desc);assert(desc.color==&converted&&desc.frameTimeDelta==11&&desc.cameraFovAngleVertical==.8f&&desc.reset);
}
'''
        compile_and_run(self, harness)


if __name__ == '__main__':
    unittest.main()

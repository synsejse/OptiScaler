"""Actual pure temporal builder: explicit owned predecessor, no native access.

Synthetic consecutive poses exercise history math; the native word fixture
exercises compatibility with the already-tested RESET builder, not a live join.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'OptiScaler/upscalers/ffx'
HEADER = BASE / 'FSRDCyberpunkTemporalCamera.h'


class TemporalCamera(unittest.TestCase):
    def test_explicit_contract_no_native_reads_or_clock_defaults(self):
        source = HEADER.read_text()
        for forbidden in ('State::', 'Config::', 'NVSDK_', 'ID3D12', 'ReadProcessMemory', 'GetModuleHandle',
                          'MillisecondsNow', 'chrono::', 'GetLast', '1.0f / 60', '16.67'):
            self.assertNotIn(forbidden, source)
        for required in ('enum class NativeReset', 'NativeReset nativeReset', 'const PreviousFrame* previous',
                         'const Continuity& continuity', 'sameViewAndCoordinateOrigin', 'previousDispatchAccepted',
                         'CyberpunkResetCamera::Build(current,', 'CyberpunkResetCamera::Build(previous->source,',
                         'result.previousView = previous->source.nativeView',
                         'result.previousDepthProjection = prior.previousDepthProjection',
                         'volatile float difference = before - now', 'optimize("no-fast-math", "fp-contract=off")',
                         'float_control(precise, on, push)', 'Every refusal leaves output unchanged'):
            self.assertIn(required, source)
        self.assertNotIn('result.motionScale =', source)
        self.assertIn('result.dispatchFlags &= ~DispatchReset', source)

    def test_actual_current_previous_reset_and_refusals_at_o0_o3(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX for the actual temporal camera compilation')
        harness = r'''
#include "FSRDCyberpunkTemporalCamera.h"
#include "FSRDDepthMotion.h"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace FSRD::CyberpunkTemporalCamera;
uint32_t bits(float x){return std::bit_cast<uint32_t>(x);}
float f(uint32_t x){return std::bit_cast<float>(x);}
Snapshot captured(){
 Snapshot s;
 s.nativeView={1052756518,977788298,1064131678,0,3211615332,966924020,1052756514,0,
  898473216,1065353210,3126307840,0,3302796298,3262674531,1161996442,1065353216};
 s.inverseNativeView={1052756518,3211615332,898473216,0,977788298,966924020,1065353210,0,
  1064131678,1052756514,3126307840,0,3305842410,3308206053,1115773974,1065353216};
 s.nativeProjection={1066856116,0,0,0,0,1074145668,0,0,975385395,982935142,1065353226,1065353216,
  0,0,3164854039,0};
 s.depthProjection={1066856116,0,0,0,0,1074145668,0,0,975385395,982935142,3047161856,1065353216,
  0,0,1017370391,0};
 s.lensOffset={0,0};s.jitterPixels={1053556736,1054048256};
 s.width=s.jitterWidth=1280;s.height=s.jitterHeight=720;s.projectionFlags=4;return s;
}
Snapshot synthetic(std::array<float,3> position,float near,float far,bool reverse=true){
 Snapshot s;
 s.width=s.jitterWidth=64;s.height=s.jitterHeight=32;s.projectionFlags=reverse?4:0;
 for(unsigned i:{0u,5u,10u,15u})s.nativeView[i]=s.inverseNativeView[i]=bits(1);
 for(unsigned i=0;i<3;++i){s.nativeView[12+i]=bits(-position[i]);s.inverseNativeView[12+i]=bits(position[i]);}
 s.nativeProjection[0]=bits(2);s.nativeProjection[5]=bits(3);s.nativeProjection[11]=bits(1);
 s.nativeProjection[10]=bits(far/(far-near));s.nativeProjection[14]=bits(-(near*far)/(far-near));
 s.depthProjection=s.nativeProjection;
 if(reverse){s.depthProjection[10]=bits(1-f(s.nativeProjection[10]));s.depthProjection[14]=bits(-f(s.nativeProjection[14]));}
 return s;
}
void same(const Parameters& a,const Parameters& b){
 assert(a.inverseView==b.inverseView&&a.inverseProjection==b.inverseProjection&&a.previousView==b.previousView);
 assert(a.renderSize==b.renderSize&&a.previousDepthProjection==b.previousDepthProjection);
 assert(a.cameraRight==b.cameraRight&&a.cameraUp==b.cameraUp&&a.cameraForward==b.cameraForward&&a.cameraPositionDelta==b.cameraPositionDelta);
 assert(a.motionScale==b.motionScale&&a.jitterNdc==b.jitterNdc&&a.nearPlane==b.nearPlane&&a.farPlane==b.farPlane);
 assert(a.aspectRatio==b.aspectRatio&&a.verticalFovRadians==b.verticalFovRadians&&a.deltaMilliseconds==b.deltaMilliseconds);
 assert(a.frameIndex==b.frameIndex&&a.conversionFlags==b.conversionFlags&&a.dispatchFlags==b.dispatchFlags&&a.infinitePlanePolicy==b.infinitePlanePolicy);
}
int main(){
 auto live=captured();const std::array<float,2> scale={1.25f,-.5f};Parameters reset,first;
 Continuity context{7,true,true};
 assert(FSRD::CyberpunkResetCamera::Build(live,scale,17.25f,209225,reset));
 assert(Build(live,scale,17.25f,209225,NativeReset::NotRequested,nullptr,context,first));same(reset,first);
 PreviousFrame invalid;invalid.frameIndex=UINT32_MAX;
 Continuity firstOnly{7,false,false};
 assert(Build(live,scale,17.25f,209225,NativeReset::Requested,&invalid,firstOnly,first));same(reset,first);
 assert(Build(live,scale,17.25f,209225,NativeReset::NotRequested,nullptr,firstOnly,first));same(reset,first);
 auto current=synthetic({12,19,31.5f},.25f,100);
 PreviousFrame previous{synthetic({10,20,30},.5f,40),scale,15,201,7};
 const auto originalCurrent=current;const auto originalPrevious=previous;
 Parameters result,currentReset;
 assert(Build(current,scale,17.25f,202,NativeReset::NotRequested,&previous,context,result));
 assert(FSRD::CyberpunkResetCamera::Build(current,scale,17.25f,202,currentReset));
 assert(result.inverseView==currentReset.inverseView&&result.inverseProjection==currentReset.inverseProjection);
 assert(result.previousView==previous.source.nativeView&&result.previousView!=current.nativeView);
 assert(result.previousDepthProjection[0]==f(previous.source.depthProjection[10]));
 assert(result.previousDepthProjection[1]==f(previous.source.depthProjection[14])&&result.previousDepthProjection[2]==1);
 assert(result.previousDepthProjection!=currentReset.previousDepthProjection);
 assert((result.cameraPositionDelta==std::array<float,3>{-2,1,-1.5f}));
 assert((result.motionScale==std::array<float,3>{1.25f,-.5f,1}));
 assert(result.jitterNdc==currentReset.jitterNdc&&result.cameraForward==currentReset.cameraForward);
 assert(result.conversionFlags==37&&result.dispatchFlags==2&&result.frameIndex==202&&result.deltaMilliseconds==17.25f);
 assert(std::memcmp(&current,&originalCurrent,sizeof(current))==0);
 assert(std::memcmp(&previous,&originalPrevious,sizeof(previous))==0);
 // Historical geometry Z uses previous depth coefficients, not the new projection.
 const auto hw=[](const Snapshot& s,float z){return f(s.depthProjection[10])+f(s.depthProjection[14])/z;};
 const float nowDepth=hw(current,8),priorDepth=hw(previous.source,10),encoded=(priorDepth-nowDepth)*1000;
 float delta=0;
 assert(FSRD::DecodeCyberpunkDepthMotion(nowDepth,encoded,1,8,result.previousDepthProjection[0],
  result.previousDepthProjection[1],result.previousDepthProjection[2],delta));
 assert(std::abs(delta-2)<1e-4f);
 auto reject=[&](const Snapshot& c,const std::array<float,2>& mv,float dt,uint32_t frame,NativeReset native,
                 const PreviousFrame* p,const Continuity& link){
  Parameters sentinel=result;sentinel.frameIndex=0xabcdef01;
  std::array<unsigned char,sizeof(Parameters)> before{};std::memcpy(before.data(),&sentinel,sizeof(sentinel));
  assert(!Build(c,mv,dt,frame,native,p,link,sentinel));assert(std::memcmp(before.data(),&sentinel,sizeof(sentinel))==0);
 };
 for(unsigned bad=0;bad<15;++bad){
  auto c=current;auto p=previous;auto link=context;auto mv=scale;float dt=17.25f;uint32_t frame=202;auto native=NativeReset::NotRequested;
  switch(bad){
   case 0:link.sessionEpoch=0;break;case 1:link.sameViewAndCoordinateOrigin=false;break;
   case 2:link.previousDispatchAccepted=false;break;case 3:++p.sessionEpoch;break;case 4:frame=201;break;
   case 5:frame=203;break;case 6:p.frameIndex=UINT32_MAX;frame=0;break;
   case 7:p.source.width=p.source.jitterWidth=65;break;case 8:p.source.height=p.source.jitterHeight=33;break;
   case 9:p.source=synthetic({10,20,30},.5f,40,false);break;case 10:p.source.inverseNativeView[0]^=1;break;
   case 11:p.deltaMilliseconds=0;break;case 12:native=NativeReset::Unavailable;break;
   case 13:native=static_cast<NativeReset>(255);break;case 14:c.jitterWidth=63;break;
  }
  reject(c,mv,dt,frame,native,&p,link);
 }
 reject(current,scale,17.25f,202,NativeReset::Unavailable,nullptr,context);
 for(uint32_t value:{0x7f800000u,0xff800000u,0x7fc12345u,0x7f800001u,1u}){
  auto p=previous;p.source.nativeView[0]=value;reject(current,scale,17.25f,202,NativeReset::NotRequested,&p,context);
  p=previous;p.motionScale[0]=f(value);reject(current,scale,17.25f,202,NativeReset::NotRequested,&p,context);
  p=previous;p.deltaMilliseconds=f(value);reject(current,scale,17.25f,202,NativeReset::NotRequested,&p,context);
  reject(current,{f(value),1},17.25f,202,NativeReset::NotRequested,&previous,context);
  reject(current,scale,f(value),202,NativeReset::NotRequested,&previous,context);
 }
 reject(current,scale,0,202,NativeReset::NotRequested,&previous,context);
 reject(current,scale,-1,202,NativeReset::NotRequested,&previous,context);
 // Canonical normal inputs can subtract into unsupported subnormal or overflow.
 {
  auto c=synthetic({f(0x00800001),0,0},.25f,100);
  auto p=previous;p.source=synthetic({f(0x00800000),0,0},.5f,40);
  reject(c,scale,17.25f,202,NativeReset::NotRequested,&p,context);
 }
 {
  auto c=synthetic({-std::numeric_limits<float>::max(),0,0},.25f,100);
  auto p=previous;p.source=synthetic({std::numeric_limits<float>::max(),0,0},.5f,40);
  reject(c,scale,17.25f,202,NativeReset::NotRequested,&p,context);
 }
 // Paired forward-depth history is also supported; do not force reverse-Z.
 {
  auto c=synthetic({12,19,31.5f},.25f,100,false);auto p=previous;p.source=synthetic({10,20,30},.5f,40,false);
  Parameters q;assert(Build(c,scale,17.25f,202,NativeReset::NotRequested,&p,context,q));
  assert(q.previousDepthProjection[0]==f(p.source.depthProjection[10])&&q.conversionFlags==37);
 }
 const int rounding=std::fegetround();assert(std::fesetround(FE_DOWNWARD)==0);
 reject(current,scale,17.25f,202,NativeReset::NotRequested,&previous,context);
 assert(std::fegetround()==FE_DOWNWARD);assert(std::fesetround(rounding)==0);
 // Cross-optimization output comparison includes all newly temporal words.
 for(uint32_t word:result.previousView)std::cout<<word<<' ';
 for(float word:result.previousDepthProjection)std::cout<<bits(word)<<' ';
 for(float word:result.cameraPositionDelta)std::cout<<bits(word)<<' ';
 for(float word:result.motionScale)std::cout<<bits(word)<<' ';
 std::cout<<result.conversionFlags<<' '<<result.dispatchFlags<<'\n';
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-temporal-camera-') as temporary:
            source = Path(temporary) / 'test.cpp'
            source.write_text(harness)
            outputs = []
            for flags in (['-O0'], ['-O3', '-ffast-math', '-ffp-contract=fast']):
                binary = Path(temporary) / 'test'
                result = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags,
                                         '-I', str(BASE), str(source), '-o', str(binary)],
                                        text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(binary)], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                outputs.append(result.stdout)
            self.assertEqual(outputs[0], outputs[1], 'Temporal words changed with caller fast math')


if __name__ == '__main__':
    unittest.main()

"""Compile the actual pure current-view reset builder with no Windows/NGX mocks.

Fixture is the native camera words from early-guides-20260907-092958-835Z-324;
it does not assert a live frame association or that any resource is GPU-ready.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'OptiScaler/upscalers/ffx'
HEADER = BASE / 'FSRDCyberpunkResetCamera.h'


class ResetCamera(unittest.TestCase):
    def test_no_native_access_defaults_or_late_history(self):
        source = HEADER.read_text()
        for forbidden in ('State::', 'Config::', 'NVSDK_', 'ID3D12', 'ReadProcessMemory', 'GetModuleHandle',
                          'MillisecondsNow', 'chrono::', 'GetLast', '1.0f / 60', '16.67'):
            self.assertNotIn(forbidden, source)
        self.assertIn('FSRD::GetViewPlanes(', source)
        self.assertIn('currentMotionScale', source)
        self.assertIn('float measuredDeltaMilliseconds, uint32_t frameIndex', source)
        self.assertIn('output = result;', source)
        self.assertIn('not silently', source)
        self.assertIn('pre-Fog', source)

    def test_fast_math_and_reset_contract_are_explicit(self):
        source = HEADER.read_text()
        self.assertIn('float_control(precise, on, push)', source)
        self.assertIn('optimize("no-fast-math", "fp-contract=off")', source)
        self.assertIn('exponent != 0x7f800000u', source)
        self.assertIn('std::fegetround() != FE_TONEAREST', source)
        self.assertIn('result.inverseView = source.inverseNativeView', source)
        self.assertIn('result.previousView = source.nativeView', source)
        self.assertIn('result.cameraPositionDelta = { 0, 0, 0 }', source)
        self.assertIn('ConversionNonGammaPackedReset = (1u << 0) | (1u << 2) | (1u << 6)', source)
        self.assertIn('DispatchResetNonGamma = (1u << 0) | (1u << 1)', source)

    def test_compiled_capture_and_projection_rejections(self):
        compiler = os.environ.get('CXX') or shutil.which('c++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('Set CXX for the actual current-camera builder tests')
        harness = r'''
#include <cassert>
#include <cstring>
#include <iostream>
#include "FSRDCyberpunkResetCamera.h"
using namespace FSRD::CyberpunkResetCamera;
float f(uint32_t bits){return std::bit_cast<float>(bits);}
uint32_t bits(float value){return std::bit_cast<uint32_t>(value);}
Snapshot fixture(){
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
 s.width=s.jitterWidth=1280;s.height=s.jitterHeight=720;s.projectionFlags=4;
 return s;
}
void checkInverse(const Snapshot& s,const Parameters& p){
 std::array<float,16> a{},b{};
 for(unsigned i=0;i<16;++i){a[i]=f(s.depthProjection[i]);b[i]=f(p.inverseProjection[i]);}
 a[8]=f(s.lensOffset[0]);a[9]=f(s.lensOffset[1]);
 for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col){
  double value=0,magnitude=0;
  for(unsigned k=0;k<4;++k){double x=double(a[row*4+k])*b[k*4+col];value+=x;magnitude+=std::abs(x);}
  assert(std::abs(value-(row==col?1.0:0.0))<2e-6*(1+magnitude));
 }
}
int main(){
 auto s=fixture();Parameters p;
 assert(Build(s,{1.25f,-.5f},17.25f,209225,p));
 assert(p.inverseView==s.inverseNativeView && p.previousView==s.nativeView);
 assert(p.renderSize[0]==1280 && p.renderSize[1]==720);
 assert(bits(p.nearPlane)==1017370378 && bits(p.farPlane)==1182995065u);
 assert(std::abs(p.aspectRatio-1.777777910232544f)<3e-7f);
 assert(std::abs(p.verticalFovRadians-.890214204788208f)<3e-7f);
 assert(bits(p.jitterNdc[0])==975385395 && bits(p.jitterNdc[1])==982935142);
 assert(p.motionScale[0]==1.25f && p.motionScale[1]==-.5f && p.motionScale[2]==1);
 assert((p.cameraPositionDelta==std::array<float,3>{}));
 assert(p.conversionFlags==69 && p.dispatchFlags==3 && !p.infinitePlanePolicy);
 assert(p.frameIndex==209225 && p.deltaMilliseconds==17.25f);
 assert(p.previousDepthProjection[0]==f(s.depthProjection[10]) &&
  p.previousDepthProjection[1]==f(s.depthProjection[14]) && p.previousDepthProjection[2]==1);
 assert(p.cameraForward[0]>.9f); // Physical +Z forward; never apply the sample's RH flip.
 for(const auto& v:{p.cameraRight,p.cameraUp,p.cameraForward}){
  const double length=double(v[0])*v[0]+double(v[1])*v[1]+double(v[2])*v[2];assert(std::abs(length-1)<3e-7);}
 checkInverse(s,p);
 auto reject=[&](const Snapshot& bad,std::array<float,2> scale=std::array<float,2>{1,1},float dt=17.25f){
  Parameters sentinel=p;sentinel.frameIndex=0xabcdef01;
  std::array<unsigned char,sizeof(Parameters)> before{};std::memcpy(before.data(),&sentinel,sizeof(sentinel));
  assert(!Build(bad,scale,dt,4,sentinel));assert(std::memcmp(before.data(),&sentinel,sizeof(sentinel))==0);
 };
 {auto t=s;t.jitterWidth=2560;reject(t);}
 {auto t=s;t.jitterPixels[1]^=0x80000000;reject(t);} // Incorrect double/native Y negation.
 {auto t=s;t.nativeProjection[8]^=1;reject(t);} // A one-ULP current source mismatch is not fitted.
 {auto t=s;t.projectionFlags=0;reject(t);} // Forward projection and reverse depth disagree.
 {auto t=s;t.projectionFlags=0x84;reject(t);}
 {auto t=s;t.nativeProjection[3]=bits(.01f);reject(t);} // Oblique.
 {auto t=s;t.nativeProjection[11]=0;t.nativeProjection[15]=bits(1);reject(t);} // Orthographic.
 {auto t=s;t.nativeProjection[11]=bits(-1);reject(t);} // Unsupported handedness, not guessed flip.
 {auto t=s;t.inverseNativeView[0]^=1;reject(t);} // Native rigid-inverse rotation correspondence.
 {auto t=s;t.inverseNativeView[12]=bits(f(t.inverseNativeView[12])+10);reject(t);}
 {auto t=s;t.width=t.jitterWidth=0;reject(t);}
 {auto t=s;t.width=t.jitterWidth=8193;reject(t);}
 reject(s,{1,1},0);reject(s,{1,1},-1);
 for(uint32_t value:{0x7f800000u,0xff800000u,0x7fc12345u,0x7f800001u,1u}){
  auto t=s;t.nativeProjection[0]=value;reject(t);
  t=s;t.jitterPixels[0]=value;reject(t);
  reject(s,{f(value),1});reject(s,{1,1},f(value));
 }
 // Nonzero authored offsets survive removing temporal jitter. Input Pd remains
 // unchanged; only the private analytic inverse uses the authored no-jitter form.
 {
  auto t=s;t.lensOffset={bits(.125f),bits(-.0625f)};
  volatile float nx=f(t.jitterPixels[0])/float(t.width),ny=f(t.jitterPixels[1])/float(t.height);
  volatile float jx=float(nx)*2, jy=float(ny)*2;
  t.nativeProjection[8]=bits(.125f+float(jx));t.nativeProjection[9]=bits(-.0625f+float(jy));
  t.depthProjection[8]=t.nativeProjection[8];t.depthProjection[9]=t.nativeProjection[9];
  const auto before=t;Parameters q;assert(Build(t,{1,1},11,0,q));assert(q.frameIndex==0);
  assert(t.nativeProjection==before.nativeProjection && t.depthProjection==before.depthProjection);
  assert(f(q.inverseProjection[12])<0 && f(q.inverseProjection[13])>0);checkInverse(t,q);
  auto bad=t;bad.lensOffset[0]=bits(.12f);reject(bad);
 }
 // Authored non-reversed perspective and existing infinite-plane policy.
 {auto t=s;t.depthProjection=t.nativeProjection;t.projectionFlags=0;Parameters q;
  assert(Build(t,{1,1},13,1,q));checkInverse(t,q);assert(std::abs(q.farPlane-p.farPlane)<.01f);}
 {auto t=s;t.nativeProjection[10]=bits(1);t.depthProjection[10]=0;Parameters q;
  assert(Build(t,{1,1},13,1,q));assert(q.infinitePlanePolicy && q.farPlane==std::numeric_limits<float>::max());checkInverse(t,q);}
 // Restore the original rounding mode; the builder never changes it for us.
 const int mode=std::fegetround();assert(std::fesetround(FE_DOWNWARD)==0);reject(s);
 assert(std::fegetround()==FE_DOWNWARD);assert(std::fesetround(mode)==0);
 // Emit key result words for cross-optimization equality, not just loose tolerances.
 for(uint32_t word:p.inverseProjection)std::cout<<word<<' ';
 for(float word:p.jitterNdc)std::cout<<bits(word)<<' ';
 std::cout<<bits(p.nearPlane)<<' '<<bits(p.farPlane)<<' '<<bits(p.verticalFovRadians)<<'\n';
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-reset-camera-') as directory:
            tmp = Path(directory)
            path = tmp / 'main.cpp'
            path.write_text(harness)
            outputs = []
            for number, options in enumerate((['-O0'], ['-O3', '-ffast-math', '-ffp-contract=fast'])):
                binary = tmp / f'test-{number}'
                result = subprocess.run([compiler, '-std=c++20', *options, '-I', str(BASE), str(path), '-o', str(binary)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                outputs.append(result.stdout)
            self.assertEqual(outputs[0], outputs[1], 'Precise builder changed with caller fast-math optimization')


if __name__ == '__main__':
    unittest.main()
